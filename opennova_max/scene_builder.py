"""3ds Max scene builder for OpenNova .3di assets."""
from __future__ import annotations

import math
from typing import Any, Sequence

from pyopennova import coords
from pyopennova.mesh_primitives import cube_mesh
from pyopennova.mesh_utils import mtrx_to_center_rotation
from pyopennova.polyhedron import compute_polyhedron_controlled
from pyopennova.scene_naming import (
    CollisionType,
    OcclusionType,
    build_occlusion_name as _build_occlusion_name,
    build_volume_name as _build_volume_name,
    format_duplicate_suffix as _format_duplicate_suffix,
)


class MaxSceneBuilder:
    """Build a Max scene from a parsed ``ThreediModelIR``."""

    def __init__(
        self,
        ir,
        bad_file=None,
        anim_context=None,
        resolver=None,
        import_collisions: bool = True,
        import_occlusion: bool = True,
        import_lights: bool = True,
    ):
        self.ir = ir
        self.bad_file = bad_file
        self.anim_context = anim_context
        self.resolver = resolver
        self.import_collisions = import_collisions
        self.import_occlusion = import_occlusion
        self.import_lights = import_lights

        self.material_dict: dict[int, Any] = {}
        self.root_object: Any | None = None
        self.part_nodes: dict[int, Any] = {}
        self.lod_roots: list[Any] = []
        self.bullet_root: Any | None = None
        self.bullet_lod_index: int = -1
        self.mesh_objects: list[Any] = []
        self.armature_object: Any | None = None
        self.root_motion_node: Any | None = None
        self.bone_nodes: list[Any] = []
        self._bone_infos: list[tuple[str, int, tuple[float, float, float]]] = []
        self._mesh_bone_data: dict[str, Any] = {}

    def build_basic_scene(self, name: str) -> bool:
        """Create the full Max scene for one model IR."""
        from .materials import create_material
        from .mesh import build_lod_meshes, build_part_hierarchy

        if int(self.ir.lod_count) == 0:
            return False

        for i in range(int(self.ir.material_count)):
            self.material_dict[i] = create_material(
                self.ir.materials[i],
                resolver=self.resolver,
                ctrl_resolver=self._resolve_ctrl_reg,
            )

        material_names = self._material_names_manifest()
        self.root_object, self.part_nodes = build_part_hierarchy(
            self.ir,
            lod_index=0,
            name=name,
            material_names=material_names,
        )
        self._store_material_diagnostics(self.root_object)

        self.mesh_objects = build_lod_meshes(
            self.ir,
            lod_index=0,
            part_nodes=self.part_nodes,
            material_dict=self.material_dict,
            include_empty_parts=True,
            track_bone_data=True,
            mesh_bone_data=self._mesh_bone_data,
        )

        self._build_additional_lods(name)
        self.create_scene_markers()
        self.create_user_points()
        if self.import_occlusion:
            self.create_occlusion_visualization()
        if self.import_collisions:
            self.create_collision_visualization()
            self.bullet_lod_index = self.create_bullet_lod(name)
        else:
            self.bullet_lod_index = -1
        if self.import_lights:
            self.create_scene_lights()

        if self.bad_file and self.anim_context:
            self.build_armature_from_bad(self.bad_file, name)
            self.bind_meshes_to_armature()
            self.build_animations_from_context(self.anim_context)
        elif int(getattr(self.ir, "mesh_type", 0)) == 3:
            self.build_armature_from_parts(name)
            self.bind_meshes_to_armature()

        return bool(self.mesh_objects)

    def merge_with_existing_scene(self, main_builder) -> bool:
        from .mesh import build_lod_meshes

        self.root_object = main_builder.root_object
        self.part_nodes = main_builder.part_nodes
        self.material_dict = main_builder.material_dict
        self.armature_object = main_builder.armature_object
        self.root_motion_node = main_builder.root_motion_node
        self.bone_nodes = main_builder.bone_nodes
        self._bone_infos = main_builder._bone_infos
        self.mesh_objects = build_lod_meshes(
            self.ir,
            lod_index=0,
            part_nodes=self.part_nodes,
            material_dict=self.material_dict,
            include_empty_parts=False,
            track_bone_data=True,
            mesh_bone_data=self._mesh_bone_data,
        )
        if self.armature_object:
            self.bind_meshes_to_armature()
        return bool(self.mesh_objects)

    def _build_additional_lods(self, name: str) -> None:
        if int(self.ir.lod_count) <= 1:
            return
        from .mesh import build_lod_meshes, build_part_hierarchy

        material_names = self._material_names_manifest()
        for lod_idx in range(1, int(self.ir.lod_count)):
            lod = self.ir.lods[lod_idx]
            if int(lod.part_count) == 0:
                continue
            lod_root, lod_part_nodes = build_part_hierarchy(
                self.ir,
                lod_index=lod_idx,
                name=f"{name}_LOD{lod_idx}",
                hidden=True,
                material_names=material_names,
            )
            self.lod_roots.append(lod_root)
            build_lod_meshes(
                self.ir,
                lod_index=lod_idx,
                part_nodes=lod_part_nodes,
                material_dict=self.material_dict,
                include_empty_parts=True,
                hidden=True,
            )
            self._create_lod_markers(lod_idx, lod_part_nodes, hidden=True)

    def create_scene_markers(self) -> None:
        if int(self.ir.lod_count) == 0:
            return
        self._create_lod_markers(0, self.part_nodes, hidden=False)

    def create_user_points(self) -> None:
        if int(self.ir.lod_count) == 0:
            return
        from .materials import create_marker_material
        from .mesh import create_mesh_node, vector_sub, world_position

        verts, faces = cube_mesh(0.015)
        mat = create_marker_material("user_point_white", (1.0, 1.0, 1.0))
        for i in range(int(self.ir.userpoint_count)):
            up = self.ir.userpoints[i]
            source_name = _decode(up.name)
            display_idx = int(up.part_index) if int(up.part_index) >= 0 else 0
            type_code = int(up.type_code)
            if type_code and 32 <= type_code <= 126:
                prefix = f"UP{chr(type_code)}"
            else:
                prefix = "USR"
            marker_name = f"{prefix}{display_idx + 1:02d}"
            if source_name:
                marker_name += f" {source_name}"

            raw = up.position
            world_pos = (float(raw[0]), -float(raw[2]), float(raw[1]))
            parent = self.root_object
            local_pos = world_pos
            if 0 <= int(up.part_index) < len(self.part_nodes):
                parent = self.part_nodes[int(up.part_index)]
                local_pos = vector_sub(world_pos, world_position(parent))

            marker = create_mesh_node(
                marker_name,
                verts,
                faces,
                material=mat,
                parent=parent,
                location=local_pos,
            )
            rt = _rt()
            _set_user_prop(rt, marker, "nl_ase_name", marker_name)
            _set_user_prop(rt, marker, "nl_raw_position", _csv(up.position, 3))
            _set_user_prop(rt, marker, "nl_raw_direction", _csv(up.direction, 3))
            _set_user_prop(rt, marker, "nl_parent_index", int(up.part_index))
            _set_user_prop(rt, marker, "nl_type_code", int(up.type_code))
            _apply_direction_rotation(marker, up.direction)

    def create_scene_lights(self) -> None:
        from .mesh import set_parent_and_local_position

        rt = _rt()
        for i in range(int(self.ir.light_count)):
            light = self.ir.lights[i]
            light_idx = int(light.part_index) if int(light.part_index) >= 0 else i
            light_name = f"LP{light_idx + 1:02d}"
            try:
                light_obj = rt.OmniLight()
            except Exception:
                light_obj = rt.Light()
            light_obj.name = light_name
            _try_set(light_obj, "rgb", rt.color(
                _byte(light.color_start[0]),
                _byte(light.color_start[1]),
                _byte(light.color_start[2]),
            ))
            _try_set(light_obj, "multiplier", 1.0)
            if float(light.attenuation_end) > 0.0:
                _try_set(light_obj, "farAttenEnd", float(light.attenuation_end))
                _try_set(light_obj, "useFarAtten", True)
            local_pos = coords.render_space(light.offset)

            _set_user_prop(rt, light_obj, "atten_start", float(light.attenuation_start))
            _set_user_prop(rt, light_obj, "falloff", float(light.falloff))
            rot = coords.render_space(light.rotation)
            _set_user_prop(rt, light_obj, "tm_row2", f"{rot[0]},{rot[1]},{rot[2]}")
            if int(light.light_type) != 0:
                _set_user_prop(rt, light_obj, "light_type", int(light.light_type))
            if int(light.style) != 0:
                _set_user_prop(rt, light_obj, "colorgen_style", int(light.style))
                _set_user_prop(rt, light_obj, "colorgen_rate", float(light.rate) / 256.0)
                _set_user_prop(rt, light_obj, "colorgen_phase", float(light.phase) / 256.0)
            ce = light.color_end
            if ce[0] != 0.0 or ce[1] != 0.0 or ce[2] != 0.0:
                _set_user_prop(rt, light_obj, "colorgen_end", f"{_byte(ce[0])},{_byte(ce[1])},{_byte(ce[2])}")
            if int(light.style) > 0x70:
                creg = self._resolve_ctrl_reg(int(light.phase))
                if creg:
                    _set_user_prop(rt, light_obj, "colorgen_ctrlreg", creg)
            flags = int(light.flags)
            if flags & 0x01:
                _set_user_prop(rt, light_obj, "disable_corona", 1)
            if flags & 0x02:
                _set_user_prop(rt, light_obj, "disable_lightterrain", 1)
            if flags & 0x04:
                _set_user_prop(rt, light_obj, "disable_lightobjects", 1)

            parent = self.root_object
            if 0 <= int(light.part_index) in self.part_nodes:
                parent = self.part_nodes[int(light.part_index)]
            set_parent_and_local_position(light_obj, parent, local_pos)

    def create_collision_visualization(self) -> None:
        coll = self.ir.collision
        if not coll:
            return
        if int(self.ir.lod_count) == 0:
            return
        lod0 = self.ir.lods[0]
        part_count = int(lod0.part_count)
        name_counters: dict[tuple[int, int], int] = {}

        for vol_idx in range(int(coll.contents.volume_count)):
            vol = coll.contents.volumes[vol_idx]
            try:
                color = CollisionType(int(vol.type)).color
            except ValueError:
                color = (1.0, 1.0, 1.0)

            parent = self.root_object
            parent_pos = (0.0, 0.0, 0.0)
            if 0 <= int(vol.object_index) < part_count:
                parent = self.part_nodes.get(int(vol.object_index), self.root_object)
                parent_pos = coords.render_space(lod0.parts[int(vol.object_index)].abs_position)
            elif 0 <= int(vol.part_index) < part_count:
                parent = self.part_nodes.get(int(vol.part_index), self.root_object)
                parent_pos = coords.render_space(lod0.parts[int(vol.part_index)].abs_position)

            world_min = coords.collision_space(vol.min)
            world_max = coords.collision_space(vol.max)
            world_center = _vec_scale(_vec_add(world_min, world_max), 0.5)
            local_center = _vec_sub(world_center, parent_pos)

            name_index = int(vol.object_index) if int(vol.object_index) >= 0 else int(vol.part_index)
            name_key = name_index if name_index >= 0 else -1
            counter_key = (int(vol.type), name_key)
            name_counters[counter_key] = name_counters.get(counter_key, 0) + 1
            mesh_name = _build_volume_name(
                int(vol.type), int(vol.flags), name_index, name_counters[counter_key]
            ) + "-colonly"

            halfspaces = []
            if (
                int(vol.plane_count) > 0
                and int(vol.plane_start) >= 0
                and int(vol.plane_start) + int(vol.plane_count) <= int(coll.contents.plane_count)
            ):
                for p_idx in range(int(vol.plane_count)):
                    plane = coll.contents.planes[int(vol.plane_start) + p_idx]
                    n = coords.collision_space(plane.normal)
                    halfspaces.append((n[0], n[1], n[2], float(plane.distance)))

            vertices = faces = None
            if halfspaces:
                vertices, faces = compute_polyhedron_controlled(
                    halfspaces,
                    world_min,
                    world_max,
                )
            if vertices is not None and faces is not None and len(vertices) >= 4:
                local_vertices = [_vec_sub(v, world_center) for v in vertices]
                self._create_colored_mesh(mesh_name, local_vertices, faces, color, parent, local_center)
            else:
                half = (
                    abs((world_max[0] - world_min[0]) * 0.5),
                    abs((world_max[1] - world_min[1]) * 0.5),
                    abs((world_max[2] - world_min[2]) * 0.5),
                )
                if half[0] <= 0.0 and half[1] <= 0.0 and half[2] <= 0.0:
                    continue
                box_pts = [
                    (s0 * half[0], s1 * half[1], s2 * half[2])
                    for s0 in (-1, 1) for s1 in (-1, 1) for s2 in (-1, 1)
                ]
                box_faces = [
                    (0, 2, 3, 1),
                    (4, 5, 7, 6),
                    (0, 1, 5, 4),
                    (2, 6, 7, 3),
                    (0, 4, 6, 2),
                    (1, 3, 7, 5),
                ]
                self._create_colored_mesh(mesh_name, box_pts, box_faces, color, parent, local_center)

    def create_occlusion_visualization(self) -> None:
        occ = self.ir.occlusion
        if not occ or int(occ.contents.object_count) == 0 or int(self.ir.lod_count) == 0:
            return
        lod0 = self.ir.lods[0]
        occ_counters: dict[tuple[int, int, int], int] = {}

        for i in range(int(occ.contents.object_count)):
            occ_obj = occ.contents.objects[i]
            if int(occ_obj.num_vertices) <= 0 or int(occ_obj.face_count) <= 0:
                continue
            vert_start = int(occ_obj.vertex_start)
            face_start = int(occ_obj.face_start)
            vert_count = int(occ_obj.num_vertices)
            face_count = int(occ_obj.face_count)
            if vert_start + vert_count > int(occ.contents.vertex_count):
                continue
            if face_start + face_count > int(occ.contents.face_count):
                continue

            occ_parent = int(occ_obj.parent_subobject_index)
            part_abs = (0.0, 0.0, 0.0)
            if 0 <= occ_parent < int(lod0.part_count):
                part_abs = coords.render_space(lod0.parts[occ_parent].abs_position)

            vertices = []
            for j in range(vert_count):
                v = occ.contents.vertices[vert_start + j]
                vertices.append(_vec_sub(coords.render_space(v.position), part_abs))

            faces = []
            face_flags = []
            for j in range(face_count):
                face = occ.contents.faces[face_start + j]
                raw = int(face.raw_indices)
                v1 = raw & 0xFF
                v2 = (raw >> 8) & 0xFF
                v3 = (raw >> 16) & 0xFF
                if v1 < len(vertices) and v2 < len(vertices) and v3 < len(vertices):
                    faces.append((v1, v2, v3))
                    face_flags.append(((raw >> 24) & 0xFF) + 1)
            if not vertices or not faces:
                continue

            base_name = _build_occlusion_name(
                int(occ_obj.type),
                int(occ_obj.parent_subobject_index),
                int(occ_obj.connecting_subobject),
            )
            key = (
                int(occ_obj.type),
                int(occ_obj.parent_subobject_index),
                int(occ_obj.connecting_subobject),
            )
            occ_counters[key] = occ_counters.get(key, 0) + 1
            mesh_name = base_name + _format_duplicate_suffix(occ_counters[key]) + "-occonly"
            try:
                color = OcclusionType(int(occ_obj.type)).color
            except ValueError:
                color = (1.0, 0.0, 0.0)
            parent = self.root_object
            if occ_parent >= 0 and occ_parent in self.part_nodes:
                parent = self.part_nodes[occ_parent]
            self._create_colored_mesh(
                mesh_name,
                vertices,
                faces,
                color,
                parent,
                (0.0, 0.0, 0.0),
                smoothing_groups=face_flags,
                alpha=0.25,
            )

    def create_bullet_lod(self, name: str) -> int:
        coll = self.ir.collision
        if not coll:
            return -1
        obj_count = int(coll.contents.object_count)
        face_count_total = int(coll.contents.face_count)
        vert_count_total = int(coll.contents.vertex_count)
        if obj_count == 0 or vert_count_total == 0:
            return -1

        from .materials import create_multimaterial
        from .mesh import create_mesh_node, set_parent_and_local_position

        rt = _rt()
        bullet_lod_idx = int(self.ir.lod_count)
        root = rt.Dummy()
        root.name = f"{name}_BulletLOD"
        _set_hidden(root, True)
        _set_user_prop(rt, root, "_lod_index", bullet_lod_idx)
        _set_user_prop(rt, root, "_material_names", self._material_names_manifest())
        self.bullet_root = root

        flag_to_mat = self._collision_material_lookup()
        vert_ranges = []
        face_ranges = []
        v_off = 0
        f_off = 0
        for oi in range(obj_count):
            obj = coll.contents.objects[oi]
            nv = int(obj.num_vertices)
            nf = int(obj.num_faces)
            vert_ranges.append((v_off, nv))
            face_ranges.append((f_off, nf))
            v_off += nv
            f_off += nf

        bullet_parts: dict[int, Any] = {}
        obj_positions = []
        for oi in range(obj_count):
            obj = coll.contents.objects[oi]
            offset_pos = (
                float(obj.offset[0]) / 65536.0,
                float(obj.offset[1]) / 65536.0,
                float(obj.offset[2]) / 65536.0,
            )
            obj_positions.append(offset_pos)
            part_obj = rt.Dummy()
            part_obj.name = f"PN{oi + 1:02d}"
            _set_hidden(part_obj, True)
            bullet_parts[oi] = part_obj

        for oi in range(obj_count):
            obj = coll.contents.objects[oi]
            part_obj = bullet_parts[oi]
            pi = int(obj.parent_subobject_index)
            abs_pos = coords.collision_space(obj_positions[oi])
            if pi >= 0 and pi < obj_count and pi != oi and pi in bullet_parts:
                parent_abs = coords.collision_space(obj_positions[pi])
                set_parent_and_local_position(part_obj, bullet_parts[pi], _vec_sub(abs_pos, parent_abs))
            else:
                set_parent_and_local_position(part_obj, root, abs_pos)

        for oi in range(obj_count):
            v_start, v_count = vert_ranges[oi]
            f_start, f_count = face_ranges[oi]
            part_pos = obj_positions[oi]
            verts = []
            for vi in range(v_count):
                gvi = v_start + vi
                if gvi >= vert_count_total:
                    break
                cv = coll.contents.vertices[gvi]
                verts.append(coords.collision_space(_vec_sub(cv.position, part_pos)))

            faces = []
            face_mat_keys = []
            for fi in range(f_count):
                gfi = f_start + fi
                if gfi >= face_count_total:
                    break
                cf = coll.contents.faces[gfi]
                v0, v1, v2 = int(cf.vert_index[0]), int(cf.vert_index[1]), int(cf.vert_index[2])
                if v0 < v_count and v1 < v_count and v2 < v_count:
                    faces.append((v0, v1, v2))
                    face_mat_keys.append((int(cf.material_flags), int(cf.poly_type)))

            mesh = create_mesh_node(
                f"{oi + 1:02d} Mesh0",
                verts,
                faces,
                parent=bullet_parts[oi],
                hidden=True,
            )
            _set_user_prop(rt, mesh, "_part_index", oi)
            if face_mat_keys:
                ordered_keys: list[tuple[int, int]] = []
                for key in face_mat_keys:
                    if key not in ordered_keys:
                        ordered_keys.append(key)
                temp_materials = {
                    idx: flag_to_mat.get(key)
                    for idx, key in enumerate(ordered_keys)
                    if flag_to_mat.get(key) is not None
                }
                multi = create_multimaterial(
                    f"{mesh.name}_CollisionMaterials",
                    list(range(len(ordered_keys))),
                    temp_materials,
                )
                if multi is not None:
                    mesh.material = multi
                slot_lookup = {key: idx + 1 for idx, key in enumerate(ordered_keys)}
                for fi, key in enumerate(face_mat_keys):
                    rt.setFaceMatID(mesh, fi + 1, slot_lookup.get(key, 1))

        self._create_lod_centers_for_parts(0, bullet_parts, hidden=True, lod_override=coll)
        self._create_bullet_attach_markers(coll, bullet_parts, root)
        return bullet_lod_idx

    def build_armature_from_parts(self, name: str) -> None:
        if int(self.ir.lod_count) == 0:
            return
        lod0 = self.ir.lods[0]
        part_count = int(lod0.part_count)
        if part_count <= 0:
            return
        rt = _rt()
        skeleton = rt.Dummy()
        skeleton.name = "Skeleton"
        if self.root_object:
            skeleton.parent = self.root_object
        self.armature_object = skeleton
        self.root_motion_node = None

        abs_positions = [coords.render_space(lod0.parts[i].abs_position) for i in range(part_count)]
        parent_indices = []
        for i in range(part_count):
            pi = int(lod0.parts[i].parent_index)
            if pi < 0 or pi >= part_count or pi == i:
                pi = -1
            parent_indices.append(pi)

        self.bone_nodes = []
        self._bone_infos = []
        for i in range(part_count):
            bone = _create_bone_or_dummy(f"BN{i + 1:02d}", abs_positions[i])
            self.bone_nodes.append(bone)
            self._bone_infos.append((bone.name, parent_indices[i], abs_positions[i]))
        for i, pi in enumerate(parent_indices):
            self.bone_nodes[i].parent = self.bone_nodes[pi] if pi >= 0 else skeleton

    def build_armature_from_bad(self, bad_file, name: str) -> None:
        rt = _rt()
        bone_count = int(bad_file.num_bones)
        self.root_motion_node = None
        if bone_count == 0:
            return
        skeleton = rt.Dummy()
        skeleton.name = "Bip001"
        if self.root_object:
            skeleton.parent = self.root_object
        self.armature_object = skeleton
        try:
            skeleton.position = rt.Point3(0.0, 0.0, 0.0)
        except Exception:
            pass
        self.root_motion_node = skeleton
        self.bone_nodes = []
        self._bone_infos = []

        for i in range(bone_count):
            bone = bad_file.bones[i]
            bone_name = _decode(bone.name).replace(":", "") or f"BN{i + 1:02d}"
            parent_idx = int(bone.parent_index)
            if parent_idx < 0 or parent_idx >= bone_count or parent_idx == i:
                parent_idx = -1
            pos = self._bad_bone_start_position(i, bone)
            rest_origin = coords.bone_space(bone.position)
            node = _create_bad_bone(rt, bone_name, bone, pos)
            self.bone_nodes.append(node)
            self._bone_infos.append((bone_name, parent_idx, rest_origin))

        for i, (_name, parent_idx, _rest_origin) in enumerate(self._bone_infos):
            self.bone_nodes[i].parent = self.bone_nodes[parent_idx] if parent_idx >= 0 else skeleton

    def _bad_bone_start_position(self, bone_index: int, bone) -> tuple[float, float, float]:
        if int(getattr(self.ir, "lod_count", 0)) > 0:
            lod0 = self.ir.lods[0]
            if 0 <= bone_index < int(lod0.part_count):
                return coords.render_space(lod0.parts[bone_index].abs_position)
        return coords.bone_space(bone.position)

    def bind_meshes_to_armature(self) -> None:
        if not self.armature_object or not self.bone_nodes:
            self._mesh_bone_data.clear()
            return
        rt = _rt()
        for mesh_obj in self.mesh_objects:
            bone_data = self._mesh_bone_data.get(mesh_obj.name)
            if not bone_data:
                continue
            _set_user_prop(rt, mesh_obj, "opennova_skin_bones", ";".join(b[0] for b in self._bone_infos))
            try:
                skin = rt.Skin()
                rt.addModifier(mesh_obj, skin)
                bone_id_by_source = _add_skin_bones(rt, skin, self.bone_nodes)
                bound_vertices = 0
                for vert_idx, entries in enumerate(bone_data, start=1):
                    bone_indices, weights = _remap_skin_weight_entries(entries, bone_id_by_source)
                    if bone_indices:
                        rt.skinOps.ReplaceVertexWeights(
                            skin,
                            vert_idx,
                            rt.Array(*bone_indices),
                            rt.Array(*weights),
                        )
                        bound_vertices += 1
                _set_user_prop(rt, mesh_obj, "opennova_skin_bone_count", len(bone_id_by_source))
                _set_user_prop(rt, mesh_obj, "opennova_skin_bound_vertices", bound_vertices)
            except Exception as exc:
                _set_user_prop(rt, mesh_obj, "opennova_skin_warning", str(exc))
        self._mesh_bone_data.clear()

    def build_animations_from_context(self, anim_context) -> None:
        """Build Max transform keys from AnimationContext."""
        if not self.armature_object or not anim_context:
            return
        rt = _rt()
        try:
            from .animation import key_animation_context

            sampled_set = key_animation_context(
                rt,
                anim_context,
                self.armature_object,
                self.bone_nodes,
                self._bone_infos,
                self.root_motion_node,
            )
            for warning in sampled_set.warnings:
                print(f"[ANIM] {warning}")
        except Exception as exc:
            _set_user_prop(rt, self.armature_object, "opennova_animation_warning", str(exc))

    def _create_lod_markers(
        self,
        lod_idx: int,
        part_nodes: dict[int, Any],
        *,
        hidden: bool,
    ) -> None:
        self._create_lod_centers_for_parts(lod_idx, part_nodes, hidden=hidden)
        self._create_attach_markers(lod_idx, part_nodes, hidden=hidden)

    def _create_lod_centers_for_parts(
        self,
        lod_idx: int,
        part_nodes: dict[int, Any],
        *,
        hidden: bool,
        lod_override=None,
    ) -> None:
        from .materials import create_marker_material
        from .mesh import create_mesh_node

        apply_matrices = lod_override is None
        lod = self.ir.lods[lod_idx] if lod_idx < int(self.ir.lod_count) else None
        verts, faces = cube_mesh(0.015)
        mat = create_marker_material("center_magenta", (1.0, 0.0, 1.0))
        for i in sorted(part_nodes.keys()):
            center = create_mesh_node(
                f"_{i + 1:02d} center",
                verts,
                faces,
                material=mat,
                parent=part_nodes[i],
                hidden=hidden,
            )
            if (
                apply_matrices
                and lod is not None
                and i < int(lod.part_animation_count)
                and int(self.ir.matrix_count) > 0
            ):
                mi = int(lod.part_animations[i].matrix_index)
                if mi != 0xFF and mi < int(self.ir.matrix_count):
                    rot = mtrx_to_center_rotation(self.ir.matrices[mi].m)
                    if rot is not None:
                        _apply_local_rotation_matrix(center, rot)
                else:
                    _set_user_prop(_rt(), center, "opennova_zero_axis", True)

    def _create_attach_markers(
        self,
        lod_idx: int,
        part_nodes: dict[int, Any],
        *,
        hidden: bool,
    ) -> None:
        from .mesh import create_mesh_node

        lod = self.ir.lods[lod_idx]
        verts, faces = cube_mesh(0.012)
        counters: dict[int, int] = {}
        for i in range(int(lod.part_count)):
            if i not in part_nodes:
                continue
            pi = int(lod.parts[i].parent_index)
            if i == 0 or pi < 0:
                continue
            count = counters.get(pi, 0)
            counters[pi] = count + 1
            suffix = chr(ord("a") + (count % 26))
            create_mesh_node(
                f"~{pi + 1:02d}{suffix} attach",
                verts,
                faces,
                parent=part_nodes[i],
                hidden=hidden,
            )

    def _create_bullet_attach_markers(self, coll, bullet_parts, lod_root) -> None:
        from .mesh import create_mesh_node, world_position

        verts, faces = cube_mesh(0.012)
        trans_count = int(coll.contents.translation_count)
        child_parts = []
        for oi in range(int(coll.contents.object_count)):
            if oi == 0:
                continue
            obj = coll.contents.objects[oi]
            pi = int(obj.parent_subobject_index)
            if pi >= 0:
                child_parts.append((oi, pi))

        counters: dict[int, int] = {}
        attach_specs = []
        for child_oi, parent_oi in child_parts:
            count = counters.get(parent_oi, 0)
            counters[parent_oi] = count + 1
            suffix = chr(ord("a") + (count % 26))
            attach_specs.append((f"~{parent_oi + 1:02d}{suffix} attach", bullet_parts[child_oi]))

        extra_needed = max(0, trans_count - len(attach_specs))
        for extra_idx in range(extra_needed):
            suffix = chr(ord("a") + (extra_idx % 26))
            attach_specs.append((f"~99{suffix} attach", lod_root))

        attach_specs.sort(key=lambda it: it[0].lower())
        for ti, (attach_name, parent_part) in enumerate(attach_specs[:trans_count]):
            trans = coll.contents.translations[ti]
            eng_pos = (
                float(trans.translation[0]) / 65536.0,
                float(trans.translation[1]) / 65536.0,
                float(trans.translation[2]) / 65536.0,
            )
            world_pos = coords.collision_space(eng_pos)
            parent_abs = (0.0, 0.0, 0.0) if parent_part == lod_root else world_position(parent_part)
            create_mesh_node(
                attach_name,
                verts,
                faces,
                parent=parent_part,
                location=_vec_sub(world_pos, parent_abs),
                hidden=True,
            )

    def _create_colored_mesh(
        self,
        name: str,
        vertices,
        faces,
        color: tuple[float, float, float],
        parent,
        location,
        *,
        smoothing_groups=None,
        alpha: float = 0.5,
    ) -> None:
        from .materials import create_marker_material
        from .mesh import create_mesh_node

        mat = create_marker_material(f"Material_{name}", color, alpha=alpha)
        create_mesh_node(
            name,
            vertices,
            faces,
            material=mat,
            parent=parent,
            location=location,
            smoothing_groups=smoothing_groups,
        )

    def _collision_material_lookup(self) -> dict[tuple[int, int], Any]:
        flag_to_mat = {}
        for ir_idx, max_mat in self.material_dict.items():
            ir_mat = self.ir.materials[ir_idx]
            flags = int(ir_mat.flags)
            pattrib = int(ir_mat.pattrib)
            surface_type = int(ir_mat.surface_type)
            rattrib = 0
            if flags & 0x04:
                rattrib |= 0x1
            if flags & 0x01:
                rattrib |= 0x400
            if flags & 0x02:
                rattrib |= 0x1000
            mflags = 0
            if rattrib & 1:
                mflags |= 1
            if pattrib & 0x100:
                mflags |= 0x100
            if pattrib & 0x2000:
                mflags |= 0x800
            if pattrib & 0x1000:
                mflags |= 0x400
            flag_to_mat.setdefault((mflags, surface_type), max_mat)
        return flag_to_mat

    def _resolve_ctrl_reg(self, reg_index: int) -> str | None:
        if reg_index < 0 or reg_index >= int(self.ir.control_register_count):
            return None
        if not self.ir.control_registers:
            return None
        return _decode(self.ir.control_registers[reg_index].name) or None

    def _material_names_manifest(self) -> str:
        names = []
        for i in range(int(self.ir.material_count)):
            shader = _decode(self.ir.materials[i].shader_name) or "FF_ST_OP"
            names.append(f"Material_{i}_{shader}")
        return ";".join(names)

    def _store_material_diagnostics(self, target) -> None:
        from pyopennova.materials import describe_materials, material_diagnostics

        rt = _rt()
        diag = material_diagnostics(describe_materials(self.ir, resolver=self.resolver))
        _set_user_prop(rt, target, "opennova_material_diffuse_paths", ";".join(diag["diffuse_paths"]))
        _set_user_prop(rt, target, "opennova_material_missing_diffuse", ";".join(diag["missing_diffuse"]))
        _set_user_prop(rt, target, "opennova_material_detail_paths", ";".join(diag["detail_paths"]))
        _set_user_prop(rt, target, "opennova_material_missing_detail", ";".join(diag["missing_detail"]))
        _set_user_prop(rt, target, "opennova_material_normal_paths", ";".join(diag["normal_paths"]))
        _set_user_prop(rt, target, "opennova_material_missing_normal", ";".join(diag["missing_normal"]))
        _set_user_prop(rt, target, "opennova_material_opacity_maps", int(diag["opacity_maps"]))


def _rt():
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; opennova_max only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime


def _decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def _byte(value: float) -> int:
    return max(0, min(255, int(float(value) * 255.0)))


def _try_set(obj, attr: str, value) -> None:
    try:
        setattr(obj, attr, value)
    except Exception:
        pass


def _set_user_prop(rt, obj, key: str, value) -> None:
    try:
        rt.setUserProp(obj, key, value)
    except Exception:
        pass


def _set_hidden(node, hidden: bool) -> None:
    if not hidden:
        return
    try:
        node.isHidden = True
    except Exception:
        try:
            _rt().hide(node)
        except Exception:
            pass


def _csv(values, count: int) -> str:
    return ",".join(str(float(values[i])) for i in range(count))


def _vec_add(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (float(a[0]) + float(b[0]), float(a[1]) + float(b[1]), float(a[2]) + float(b[2]))


def _vec_sub(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (float(a[0]) - float(b[0]), float(a[1]) - float(b[1]), float(a[2]) - float(b[2]))


def _vec_scale(a: Sequence[float], scalar: float) -> tuple[float, float, float]:
    return (float(a[0]) * scalar, float(a[1]) * scalar, float(a[2]) * scalar)


def _vec_len_sq(v: Sequence[float]) -> float:
    return float(v[0]) * float(v[0]) + float(v[1]) * float(v[1]) + float(v[2]) * float(v[2])


def _vec_normalize(v: Sequence[float]) -> tuple[float, float, float]:
    n = math.sqrt(_vec_len_sq(v))
    if n <= 1e-12:
        return (0.0, 0.0, 0.0)
    return (float(v[0]) / n, float(v[1]) / n, float(v[2]) / n)


def _vec_cross(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (
        float(a[1]) * float(b[2]) - float(a[2]) * float(b[1]),
        float(a[2]) * float(b[0]) - float(a[0]) * float(b[2]),
        float(a[0]) * float(b[1]) - float(a[1]) * float(b[0]),
    )


def _apply_direction_rotation(node, source_direction) -> None:
    z_axis = _vec_normalize((float(source_direction[0]), -float(source_direction[2]), float(source_direction[1])))
    if _vec_len_sq(z_axis) <= 1e-12:
        return
    world_up = (0.0, 0.0, 1.0)
    if abs(sum(z_axis[i] * world_up[i] for i in range(3))) > 0.999:
        world_up = (0.0, 1.0, 0.0)
    x_axis = _vec_normalize(_vec_cross(world_up, z_axis))
    y_axis = _vec_normalize(_vec_cross(z_axis, x_axis))
    _apply_local_rotation_matrix(node, (x_axis, y_axis, z_axis))


def _apply_local_rotation_matrix(node, rot: Sequence[Sequence[float]]) -> None:
    rt = _rt()
    try:
        pos = node.position
        node.transform = rt.Matrix3(
            rt.Point3(float(rot[0][0]), float(rot[0][1]), float(rot[0][2])),
            rt.Point3(float(rot[1][0]), float(rot[1][1]), float(rot[1][2])),
            rt.Point3(float(rot[2][0]), float(rot[2][1]), float(rot[2][2])),
            pos,
        )
    except Exception:
        _set_user_prop(rt, node, "opennova_rotation_matrix", ";".join(
            ",".join(str(float(rot[row][col])) for col in range(3))
            for row in range(3)
        ))


def _create_bone_or_dummy(name: str, position: Sequence[float]):
    rt = _rt()
    p = rt.Point3(float(position[0]), float(position[1]), float(position[2]))
    tail = rt.Point3(float(position[0]), float(position[1]) + 0.05, float(position[2]))
    try:
        node = rt.BoneSys.createBone(p, tail, rt.Point3(0.0, 0.0, 1.0))
    except Exception:
        node = rt.Dummy()
        node.position = p
    node.name = name
    return node


_MAX_GLOBAL_MATRIX_ROWS = (
    (0.0, 0.0, 1.0),
    (0.0, 1.0, 0.0),
    (-1.0, 0.0, 0.0),
)

_MAX_GLOBAL_CORRECTION_ROWS = (
    (1.0, 0.0, 0.0),
    (0.0, 0.0, 1.0),
    (0.0, -1.0, 0.0),
)


def _bad_rotation_rows(bone) -> tuple[tuple[float, float, float], ...]:
    rot = bone.rotation
    return (
        (float(rot[0]), float(rot[1]), float(rot[2])),
        (float(rot[3]), float(rot[4]), float(rot[5])),
        (float(rot[6]), float(rot[7]), float(rot[8])),
    )


def _max_bad_rotation_rows(bone) -> tuple[tuple[float, float, float], ...]:
    return _mul3(_mul3(_MAX_GLOBAL_MATRIX_ROWS, _bad_rotation_rows(bone)), _MAX_GLOBAL_CORRECTION_ROWS)


def _mul3(a: Sequence[Sequence[float]], b: Sequence[Sequence[float]]) -> tuple[tuple[float, float, float], ...]:
    return tuple(
        tuple(sum(float(a[row][k]) * float(b[k][col]) for k in range(3)) for col in range(3))
        for row in range(3)
    )


def _create_bad_bone(rt, name: str, bone, start: Sequence[float]):
    length = max(float(getattr(bone, "length", 0.0)), 0.05)
    start_point = rt.Point3(float(start[0]), float(start[1]), float(start[2]))
    bone_tm = _max_bad_transform(rt, bone)
    try:
        direction = bone_tm.row3
        end_point = start_point + direction * length
    except Exception:
        row3 = _max_bad_rotation_rows(bone)[2]
        end_point = rt.Point3(
            float(start[0]) + row3[0] * length,
            float(start[1]) + row3[1] * length,
            float(start[2]) + row3[2] * length,
        )
    try:
        node = rt.BoneSys.createBone(start_point, end_point, rt.Point3(0.0, 1.0, 0.0))
    except Exception:
        node = rt.Dummy()
        node.position = start_point
    node.name = name
    try:
        tm = rt.matrix3(1)
        tm.rotation = bone_tm.rotation
        tm.position = start_point
        node.transform = tm
    except Exception:
        pass
    _try_set(node, "width", 0.1 * length)
    _try_set(node, "height", 0.1 * length)
    return node


def _max_bad_transform(rt, bone):
    rows = _bad_rotation_rows(bone)
    bad_tm = rt.matrix3(
        rt.Point3(*rows[0]),
        rt.Point3(*rows[1]),
        rt.Point3(*rows[2]),
        rt.Point3(0.0, 0.0, 0.0),
    )
    return _max_global_matrix(rt) * bad_tm * _max_global_correction(rt)


def _max_global_matrix(rt):
    return rt.matrix3(
        rt.Point3(*_MAX_GLOBAL_MATRIX_ROWS[0]),
        rt.Point3(*_MAX_GLOBAL_MATRIX_ROWS[1]),
        rt.Point3(*_MAX_GLOBAL_MATRIX_ROWS[2]),
        rt.Point3(0.0, 0.0, 0.0),
    )


def _max_global_correction(rt):
    return rt.matrix3(
        rt.Point3(*_MAX_GLOBAL_CORRECTION_ROWS[0]),
        rt.Point3(*_MAX_GLOBAL_CORRECTION_ROWS[1]),
        rt.Point3(*_MAX_GLOBAL_CORRECTION_ROWS[2]),
        rt.Point3(0.0, 0.0, 0.0),
    )


def _add_skin_bones(rt, skin, bone_nodes: Sequence[object]) -> dict[int, int]:
    bone_id_by_source: dict[int, int] = {}
    for source_idx, bone in enumerate(bone_nodes):
        skin_bone_id = len(bone_id_by_source) + 1
        rt.skinOps.addBone(skin, bone, skin_bone_id)
        bone_id_by_source[source_idx] = skin_bone_id
    return bone_id_by_source


def _remap_skin_weight_entries(entries, bone_id_by_source: dict[int, int]) -> tuple[list[int], list[float]]:
    bone_indices = []
    weights = []
    for bone_idx, weight in entries:
        skin_bone_id = bone_id_by_source.get(int(bone_idx))
        if skin_bone_id is not None and float(weight) > 0.001:
            bone_indices.append(skin_bone_id)
            weights.append(float(weight))
    return bone_indices, weights
