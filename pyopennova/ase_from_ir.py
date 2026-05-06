"""ASE writer built directly from host-neutral OpenNova IR data.

This module is intentionally free of Blender/PyMXS imports.  DCC hosts can
build their editable scene for artists, then call this shared writer for the
batch/output path instead of each host maintaining its own ASE export logic.
"""
from __future__ import annotations

import ctypes
import math
import os
from typing import Any, Sequence

from pyopennova import ase_ffi, coords
from pyopennova.scene_naming import (
    build_occlusion_name as _build_occlusion_name,
    build_volume_name as _build_volume_name,
    format_duplicate_suffix as _format_duplicate_suffix,
)
from pyopennova.materials import ase_texture_names, describe_material
from pyopennova.mesh_build import flatten_lod
from pyopennova.mesh_primitives import cube_mesh
from pyopennova.mesh_utils import mtrx_to_center_rotation
from pyopennova.polyhedron import compute_polyhedron_controlled

SUBS_PER_SLOT = 64
MAX_TEX_FILENAME = 15


def write_ase_from_ir(
    ir,
    filepath: str,
    *,
    include_collisions: bool = True,
    include_occlusion: bool = True,
    include_lights: bool = True,
    bad_file=None,
) -> list[str]:
    """Write primary, additional LOD, and BulletLOD ASE files from an IR."""
    writer = IrAseWriter(
        ir,
        include_collisions=include_collisions,
        include_occlusion=include_occlusion,
        include_lights=include_lights,
        bad_file=bad_file,
    )
    return writer.write(filepath)


class IrAseWriter:
    def __init__(
        self,
        ir,
        *,
        include_collisions: bool,
        include_occlusion: bool,
        include_lights: bool,
        bad_file,
    ) -> None:
        self.ir = ir
        self.include_collisions = include_collisions
        self.include_occlusion = include_occlusion
        self.include_lights = include_lights
        self.bad_file = bad_file
        self._used_tex_names: dict[str, str] = {}
        self._face_normal_arrays: list[Any] = []

    def write(self, filepath: str) -> list[str]:
        os.makedirs(os.path.dirname(os.path.abspath(filepath)), exist_ok=True)
        stem, ext = os.path.splitext(filepath)
        written: list[str] = []

        primary_objects = self._primary_objects()
        primary_lights = self._lights() if self.include_lights else []
        self._write_doc(filepath, primary_objects, primary_lights, skinned=self._is_skinned())
        written.append(filepath)

        for lod_idx in range(1, int(self.ir.lod_count)):
            objects = self._lod_objects(lod_idx)
            if not objects:
                continue
            lod_path = f"{stem}_lod{lod_idx}{ext}"
            self._write_doc(lod_path, objects, [], skinned=self._is_skinned())
            written.append(lod_path)

        if self.include_collisions:
            bullet_objects = self._bullet_lod_objects()
            if bullet_objects:
                bullet_path = f"{stem}_bullet{ext}"
                self._write_doc(bullet_path, bullet_objects, [], skinned=False)
                written.append(bullet_path)

        return written

    def _write_doc(self, filepath: str, objects: list[dict[str, Any]], lights, *, skinned: bool) -> None:
        mat_slots = math.ceil(int(self.ir.material_count) / SUBS_PER_SLOT) if int(self.ir.material_count) > 0 else 0
        doc = ase_ffi.create_document(len(objects), mat_slots, len(lights))
        doc.flags = 1 if skinned else 0
        doc.skinned_flags = 1 if skinned else 0
        try:
            self._populate_materials(doc)
            for idx, spec in enumerate(objects):
                self._populate_object(doc.objects[idx], spec)
            for idx, light in enumerate(lights):
                self._populate_light(doc.lights[idx], light)
            ase_ffi.write_file(filepath, doc)
        finally:
            for i in range(int(doc.object_count)):
                doc.objects[i].face_normals = None
                doc.objects[i].face_normal_count = 0
            ase_ffi.free_document(doc)
            self._face_normal_arrays.clear()

    def _primary_objects(self) -> list[dict[str, Any]]:
        objects: list[dict[str, Any]] = []
        if self._is_skinned():
            objects.extend(self._bone_objects())
        objects.extend(self._lod_mesh_objects(0))
        objects.extend(self._center_marker_objects(0))
        objects.extend(self._attach_marker_objects(0))
        objects.extend(self._userpoint_objects())
        if self.include_collisions:
            objects.extend(self._collision_helper_objects())
        if self.include_occlusion:
            objects.extend(self._occlusion_helper_objects())
        return objects

    def _lod_objects(self, lod_idx: int) -> list[dict[str, Any]]:
        if lod_idx >= int(self.ir.lod_count):
            return []
        if int(self.ir.lods[lod_idx].part_count) == 0:
            return []
        objects = self._lod_mesh_objects(lod_idx)
        objects.extend(self._center_marker_objects(lod_idx))
        objects.extend(self._attach_marker_objects(lod_idx))
        return objects

    def _lod_mesh_objects(self, lod_idx: int) -> list[dict[str, Any]]:
        lod = self.ir.lods[lod_idx]
        part_abs = [
            coords.render_space(lod.parts[i].abs_position)
            for i in range(int(lod.part_count))
        ]
        flat_meshes = flatten_lod(
            self.ir,
            lod_idx,
            include_empty_parts=True,
            track_bone_data=self._is_skinned() and lod_idx == 0,
        )
        objects = []
        for fm in flat_meshes:
            parent = f"PN{fm.part_index + 1:02d}"
            origin = part_abs[fm.part_index] if fm.part_index < len(part_abs) else (0.0, 0.0, 0.0)
            world_verts = [_vec_add(origin, v) for v in fm.vertices]
            objects.append({
                "name": fm.name,
                "parent": _export_name(parent),
                "tm": _tm(origin),
                "vertices": world_verts,
                "faces": fm.faces,
                "uvs": fm.face_uvs0,
                "face_material_ids": fm.face_material_ids,
                "smoothing_groups": fm.smoothing_groups,
                "material_id_set": fm.material_id_set,
                "normals": fm.face_normals,
                "weights": fm.vertex_bone_data if self._is_skinned() and lod_idx == 0 else [],
            })
        return objects

    def _center_marker_objects(self, lod_idx: int) -> list[dict[str, Any]]:
        lod = self.ir.lods[lod_idx]
        verts, faces = cube_mesh(0.015)
        objects = []
        for i in range(int(lod.part_count)):
            origin = coords.render_space(lod.parts[i].abs_position)
            rot = None
            zero_axis = False
            if i < int(lod.part_animation_count) and int(self.ir.matrix_count) > 0:
                mi = int(lod.part_animations[i].matrix_index)
                if mi != 0xFF and mi < int(self.ir.matrix_count):
                    rot = mtrx_to_center_rotation(self.ir.matrices[mi].m)
                else:
                    zero_axis = True
            tm = _tm(origin, rot=rot)
            if zero_axis:
                tm = (
                    (0.0, 0.0, 0.0),
                    (0.0, 0.0, 0.0),
                    (0.0, 0.0, 0.0),
                    (origin[0], origin[1], origin[2]),
                )
            objects.append({
                "name": f"_{i + 1:02d} center",
                "parent": _export_name(f"PN{i + 1:02d}"),
                "tm": tm,
                "vertices": [_vec_add(origin, v) for v in verts],
                "faces": _triangulate(faces),
                "material_ref": -1,
                "face_material_ids": [0] * (len(faces) * 2),
                "smoothing_groups": [0] * (len(faces) * 2),
            })
        return objects

    def _attach_marker_objects(self, lod_idx: int) -> list[dict[str, Any]]:
        lod = self.ir.lods[lod_idx]
        verts, faces = cube_mesh(0.012)
        tri_faces = _triangulate(faces)
        counters: dict[int, int] = {}
        objects = []
        for i in range(int(lod.part_count)):
            pi = int(lod.parts[i].parent_index)
            if i == 0 or pi < 0:
                continue
            origin = coords.render_space(lod.parts[i].abs_position)
            count = counters.get(pi, 0)
            counters[pi] = count + 1
            suffix = chr(ord("a") + (count % 26))
            objects.append({
                "name": f"~{pi + 1:02d}{suffix} attach",
                "parent": _export_name(f"PN{i + 1:02d}"),
                "tm": _tm(origin),
                "vertices": [_vec_add(origin, v) for v in verts],
                "faces": tri_faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(tri_faces),
                "smoothing_groups": [0] * len(tri_faces),
            })
        return objects

    def _userpoint_objects(self) -> list[dict[str, Any]]:
        if int(self.ir.lod_count) == 0:
            return []
        lod0 = self.ir.lods[0]
        part_abs = [
            coords.render_space(lod0.parts[i].abs_position)
            for i in range(int(lod0.part_count))
        ]
        verts, faces = cube_mesh(0.015)
        tri_faces = _triangulate(faces)
        objects = []
        for i in range(int(self.ir.userpoint_count)):
            up = self.ir.userpoints[i]
            display_idx = int(up.part_index) if int(up.part_index) >= 0 else 0
            type_code = int(up.type_code)
            prefix = f"UP{chr(type_code)}" if type_code and 32 <= type_code <= 126 else "USR"
            marker_name = f"{prefix}{display_idx + 1:02d}"
            source_name = _decode(up.name)
            if source_name:
                marker_name += f" {source_name}"
            pos = (float(up.position[0]), -float(up.position[2]), float(up.position[1]))
            parent = ""
            if 0 <= int(up.part_index) < len(part_abs):
                parent = _export_name(f"PN{int(up.part_index) + 1:02d}")
            objects.append({
                "name": marker_name,
                "parent": parent,
                "tm": _tm(pos, rot=_direction_rot(up.direction)),
                "vertices": [_vec_add(pos, v) for v in verts],
                "faces": tri_faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(tri_faces),
                "smoothing_groups": [0] * len(tri_faces),
            })
        return objects

    def _collision_helper_objects(self) -> list[dict[str, Any]]:
        coll = self.ir.collision
        if not coll or int(self.ir.lod_count) == 0:
            return []
        lod0 = self.ir.lods[0]
        part_count = int(lod0.part_count)
        counters: dict[tuple[int, int], int] = {}
        objects = []
        for vol_idx in range(int(coll.contents.volume_count)):
            vol = coll.contents.volumes[vol_idx]
            parent = ""
            if 0 <= int(vol.object_index) < part_count:
                parent = _export_name(f"PN{int(vol.object_index) + 1:02d}")
            elif 0 <= int(vol.part_index) < part_count:
                parent = _export_name(f"PN{int(vol.part_index) + 1:02d}")

            world_min = coords.collision_space(vol.min)
            world_max = coords.collision_space(vol.max)
            world_center = _vec_scale(_vec_add(world_min, world_max), 0.5)
            name_index = int(vol.object_index) if int(vol.object_index) >= 0 else int(vol.part_index)
            key_index = name_index if name_index >= 0 else -1
            key = (int(vol.type), key_index)
            counters[key] = counters.get(key, 0) + 1
            mesh_name = _build_volume_name(int(vol.type), int(vol.flags), name_index, counters[key]) + "-colonly"

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
                vertices, faces = compute_polyhedron_controlled(halfspaces, world_min, world_max)
            if vertices is not None and faces is not None and len(vertices) >= 4:
                local_vertices = [_vec_sub(v, world_center) for v in vertices]
                tri_faces = _triangulate(faces)
            else:
                half = (
                    abs((world_max[0] - world_min[0]) * 0.5),
                    abs((world_max[1] - world_min[1]) * 0.5),
                    abs((world_max[2] - world_min[2]) * 0.5),
                )
                if half[0] <= 0.0 and half[1] <= 0.0 and half[2] <= 0.0:
                    continue
                local_vertices = [
                    (s0 * half[0], s1 * half[1], s2 * half[2])
                    for s0 in (-1, 1) for s1 in (-1, 1) for s2 in (-1, 1)
                ]
                tri_faces = _triangulate([
                    (0, 2, 3, 1),
                    (4, 5, 7, 6),
                    (0, 1, 5, 4),
                    (2, 6, 7, 3),
                    (0, 4, 6, 2),
                    (1, 3, 7, 5),
                ])
            world_vertices = [_vec_add(world_center, v) for v in local_vertices]
            objects.append({
                "name": mesh_name,
                "parent": parent,
                "tm": _tm(world_center),
                "vertices": world_vertices,
                "faces": tri_faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(tri_faces),
                "smoothing_groups": [1] * len(tri_faces),
            })
        return objects

    def _occlusion_helper_objects(self) -> list[dict[str, Any]]:
        occ = self.ir.occlusion
        if not occ or int(occ.contents.object_count) == 0 or int(self.ir.lod_count) == 0:
            return []
        lod0 = self.ir.lods[0]
        counters: dict[tuple[int, int, int], int] = {}
        objects = []
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
            parent = ""
            if 0 <= occ_parent < int(lod0.part_count):
                part_abs = coords.render_space(lod0.parts[occ_parent].abs_position)
                parent = _export_name(f"PN{occ_parent + 1:02d}")
            vertices = []
            for j in range(vert_count):
                v = occ.contents.vertices[vert_start + j]
                vertices.append(coords.render_space(v.position))
            faces = []
            smoothing_groups = []
            for j in range(face_count):
                face = occ.contents.faces[face_start + j]
                raw = int(face.raw_indices)
                v1 = raw & 0xFF
                v2 = (raw >> 8) & 0xFF
                v3 = (raw >> 16) & 0xFF
                if v1 < len(vertices) and v2 < len(vertices) and v3 < len(vertices):
                    faces.append((v1, v2, v3))
                    smoothing_groups.append(((raw >> 24) & 0xFF) + 1)
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
            counters[key] = counters.get(key, 0) + 1
            objects.append({
                "name": base_name + _format_duplicate_suffix(counters[key]) + "-occonly",
                "parent": parent,
                "tm": _tm(part_abs),
                "vertices": vertices,
                "faces": faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(faces),
                "smoothing_groups": smoothing_groups,
            })
        return objects

    def _bullet_lod_objects(self) -> list[dict[str, Any]]:
        coll = self.ir.collision
        if not coll:
            return []
        obj_count = int(coll.contents.object_count)
        face_count_total = int(coll.contents.face_count)
        vert_count_total = int(coll.contents.vertex_count)
        if obj_count == 0 or vert_count_total == 0:
            return []
        objects = []
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

        obj_positions = []
        for oi in range(obj_count):
            obj = coll.contents.objects[oi]
            obj_positions.append((
                float(obj.offset[0]) / 65536.0,
                float(obj.offset[1]) / 65536.0,
                float(obj.offset[2]) / 65536.0,
            ))

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
            ordered_mat_ids = []
            for key in face_mat_keys:
                mat_id = flag_to_mat.get(key, 0)
                if mat_id not in ordered_mat_ids:
                    ordered_mat_ids.append(mat_id)
            if not ordered_mat_ids:
                ordered_mat_ids = [0]
            face_material_ids = [flag_to_mat.get(key, ordered_mat_ids[0]) for key in face_mat_keys]
            parent = _export_name(f"PN{oi + 1:02d}")
            origin = coords.collision_space(obj_positions[oi])
            world_verts = [_vec_add(origin, v) for v in verts]
            objects.append({
                "name": f"{oi + 1:02d} Mesh0",
                "parent": parent,
                "tm": _tm(origin),
                "vertices": world_verts,
                "faces": faces,
                "face_material_ids": face_material_ids,
                "material_id_set": ordered_mat_ids,
                "smoothing_groups": [1] * len(faces),
            })

        objects.extend(self._bullet_center_objects(obj_positions))
        objects.extend(self._bullet_attach_objects(coll, obj_positions))
        return objects

    def _bullet_center_objects(self, obj_positions) -> list[dict[str, Any]]:
        verts, faces = cube_mesh(0.015)
        tri_faces = _triangulate(faces)
        return [
            {
                "name": f"_{oi + 1:02d} center",
                "parent": _export_name(f"PN{oi + 1:02d}"),
                "tm": _tm(coords.collision_space(obj_positions[oi])),
                "vertices": [
                    _vec_add(coords.collision_space(obj_positions[oi]), v)
                    for v in verts
                ],
                "faces": tri_faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(tri_faces),
                "smoothing_groups": [0] * len(tri_faces),
            }
            for oi in range(len(obj_positions))
        ]

    def _bullet_attach_objects(self, coll, obj_positions) -> list[dict[str, Any]]:
        verts, faces = cube_mesh(0.012)
        tri_faces = _triangulate(faces)
        trans_count = int(coll.contents.translation_count)
        child_parts = []
        for oi in range(int(coll.contents.object_count)):
            if oi == 0:
                continue
            pi = int(coll.contents.objects[oi].parent_subobject_index)
            if pi >= 0:
                child_parts.append((oi, pi))
        counters: dict[int, int] = {}
        attach_specs = []
        for child_oi, parent_oi in child_parts:
            count = counters.get(parent_oi, 0)
            counters[parent_oi] = count + 1
            suffix = chr(ord("a") + (count % 26))
            attach_specs.append((f"~{parent_oi + 1:02d}{suffix} attach", child_oi))
        extra_needed = max(0, trans_count - len(attach_specs))
        for extra_idx in range(extra_needed):
            suffix = chr(ord("a") + (extra_idx % 26))
            attach_specs.append((f"~99{suffix} attach", -1))
        attach_specs.sort(key=lambda it: it[0].lower())
        objects = []
        for ti, (attach_name, child_oi) in enumerate(attach_specs[:trans_count]):
            trans = coll.contents.translations[ti]
            eng_pos = (
                float(trans.translation[0]) / 65536.0,
                float(trans.translation[1]) / 65536.0,
                float(trans.translation[2]) / 65536.0,
            )
            world_pos = coords.collision_space(eng_pos)
            parent = ""
            if child_oi >= 0:
                parent = _export_name(f"PN{child_oi + 1:02d}")
            objects.append({
                "name": attach_name,
                "parent": parent,
                "tm": _tm(world_pos),
                "vertices": [_vec_add(world_pos, v) for v in verts],
                "faces": tri_faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(tri_faces),
                "smoothing_groups": [0] * len(tri_faces),
            })
        return objects

    def _bone_objects(self) -> list[dict[str, Any]]:
        if self.bad_file is not None and int(self.bad_file.num_bones) > 0:
            bone_count = int(self.bad_file.num_bones)
            names = []
            parents = []
            positions = []
            for i in range(bone_count):
                bone = self.bad_file.bones[i]
                names.append(_decode(bone.name).replace(":", "") or f"BN{i + 1:02d}")
                pi = int(bone.parent_index)
                parents.append(pi if 0 <= pi < bone_count and pi != i else -1)
                positions.append(coords.bone_space(bone.position))
            names.append("root_motion")
            parents.append(-1)
            positions.append((0.0, 0.0, 0.0))
        else:
            lod0 = self.ir.lods[0]
            bone_count = int(lod0.part_count)
            names = [f"BN{i + 1:02d}" for i in range(bone_count)]
            parents = []
            positions = []
            for i in range(bone_count):
                pi = int(lod0.parts[i].parent_index)
                parents.append(pi if 0 <= pi < bone_count and pi != i else -1)
                positions.append(coords.render_space(lod0.parts[i].abs_position))

        verts, faces = _bone_mesh()
        objects = []
        for i, name in enumerate(names):
            parent = ""
            if parents[i] >= 0:
                parent = _bone_export_name(names[parents[i]])
            elif name != "root_motion" and "root_motion" in names:
                parent = "root_motion"
            objects.append({
                "name": _bone_export_name(name),
                "parent": parent,
                "tm": _tm(positions[i]),
                "vertices": [_vec_add(positions[i], v) for v in verts],
                "faces": faces,
                "material_ref": -1,
                "face_material_ids": [0] * len(faces),
                "smoothing_groups": [0] * len(faces),
                "node_id": _bone_node_id(name),
            })
        return objects

    def _lights(self) -> list[dict[str, Any]]:
        if int(self.ir.lod_count) == 0:
            part_abs = []
        else:
            lod0 = self.ir.lods[0]
            part_abs = [
                coords.render_space(lod0.parts[i].abs_position)
                for i in range(int(lod0.part_count))
            ]
        lights = []
        for i in range(int(self.ir.light_count)):
            light = self.ir.lights[i]
            light_idx = int(light.part_index) if int(light.part_index) >= 0 else i
            pos = coords.render_space(light.offset)
            if 0 <= int(light.part_index) < len(part_abs):
                pos = _vec_add(part_abs[int(light.part_index)], pos)
            row2 = coords.render_space(light.rotation)
            lights.append({
                "name": f"LP{light_idx + 1:02d}",
                "type": int(light.light_type),
                "pos": pos,
                "color": (
                    float(light.color_start[0]),
                    float(light.color_start[1]),
                    float(light.color_start[2]),
                ),
                "atten_start": float(light.attenuation_start),
                "atten_end": float(light.attenuation_end),
                "falloff": float(light.falloff),
                "tm_row2": (-row2[0], -row2[1], -row2[2]),
            })
        return lights

    def _populate_materials(self, doc) -> None:
        if int(self.ir.material_count) <= 0:
            return
        for slot_idx in range(int(doc.material_count)):
            start = slot_idx * SUBS_PER_SLOT
            end = min(start + SUBS_PER_SLOT, int(self.ir.material_count))
            count = end - start
            parent = doc.materials[slot_idx]
            parent.name = f"Scene_Materials_{slot_idx}".encode("utf-8")[:31]
            ase_ffi.alloc_submaterials(parent, count)
        for i in range(int(self.ir.material_count)):
            ir_mat = self.ir.materials[i]
            desc = describe_material(ir_mat)
            slot_idx = i // SUBS_PER_SLOT
            local_idx = i % SUBS_PER_SLOT
            sub = doc.materials[slot_idx].submaterials[local_idx]
            shader = desc.shader
            sub.name = desc.name.encode("utf-8")[:31]
            diffuse, detail = ase_texture_names(desc, self._used_tex_names)
            if diffuse:
                sub.maps[0].value = diffuse.encode("utf-8")[:31]
            if detail:
                sub.maps[1].value = detail.encode("utf-8")[:31]
            sub.uv_u_tiling[0] = 1.0
            sub.uv_v_tiling[0] = 1.0
            sub.uv_u_tiling[1] = 1.0
            sub.uv_v_tiling[1] = 1.0
            sub.diffuse[0] = sub.diffuse[1] = sub.diffuse[2] = 0.5882
            sub.ambient[0] = sub.ambient[1] = sub.ambient[2] = 0.0588
            sub.specular[0] = sub.specular[1] = sub.specular[2] = 0.9
            sub.shine = 0.1
            sub.shine_strength = 0.0
            sub.transparency = 0.0
            sub.wiresize = 1.0
            if int(ir_mat.flags) & 0x04:
                sub.extra_flags |= 1
            if any(s in shader for s in ("PHONGT", "PHONGO", "BUMPPHONG", "ENVPHONG")):
                sub.shading = 1
            else:
                sub.shading = 0

    def _populate_object(self, ase_obj, spec: dict[str, Any]) -> None:
        vertices = spec.get("vertices", [])
        faces = spec.get("faces", [])
        uvs = spec.get("uvs", [])
        weights = spec.get("weights", [])
        ase_ffi.alloc_object(
            ase_obj,
            len(vertices),
            len(uvs),
            len(faces),
            0,
            len(vertices) if weights else 0,
        )
        ase_obj.name = fixup_name(spec["name"]).encode("utf-8")[:63]
        parent = spec.get("parent", "")
        if parent:
            ase_obj.parent_name = fixup_name(parent).encode("utf-8")[:63]
        ase_obj.node_id = int(spec.get("node_id", -1))
        ase_obj.material_ref = int(spec.get("material_ref", 0))

        mat_ids = spec.get("material_id_set", [])
        face_material_ids = spec.get("face_material_ids", [])
        if mat_ids:
            first = int(mat_ids[0])
            ase_obj.material_ref = first // SUBS_PER_SLOT

        _copy_tm(ase_obj, spec.get("tm", _tm((0.0, 0.0, 0.0))))

        for i, v in enumerate(vertices):
            ase_obj.verts[i * 3] = float(v[0])
            ase_obj.verts[i * 3 + 1] = float(v[1])
            ase_obj.verts[i * 3 + 2] = float(v[2])

        for i, face_indices in enumerate(faces):
            face = ase_obj.faces[i]
            face.vert[0] = int(face_indices[0])
            face.vert[1] = int(face_indices[1])
            face.vert[2] = int(face_indices[2])
            face.edge_visibility[0] = 1
            face.edge_visibility[1] = 1
            face.edge_visibility[2] = 1
            if i < len(face_material_ids):
                face.material_id = int(face_material_ids[i]) % SUBS_PER_SLOT
            else:
                face.material_id = 0
            smoothing = spec.get("smoothing_groups", [])
            face.smoothing_mask = int(smoothing[i]) if i < len(smoothing) else 1

        for i, uv in enumerate(uvs):
            ase_obj.uvs[i].u = float(uv[0])
            ase_obj.uvs[i].v = float(uv[1])
            ase_obj.uvs[i].w = 0.0
        if uvs:
            for i in range(len(faces)):
                base = i * 3
                ase_obj.faces[i].uv[0] = base
                ase_obj.faces[i].uv[1] = base + 1
                ase_obj.faces[i].uv[2] = base + 2

        if weights:
            ase_obj.skinned = 1
            for i in range(len(vertices)):
                entries = weights[i] if i < len(weights) else []
                packed = _pack_weights(entries)
                for j in range(4):
                    ase_obj.weights[i].bone_index[j] = packed[0][j]
                    ase_obj.weights[i].weight[j] = packed[1][j]

        normals = spec.get("normals", [])
        if normals and len(normals) == len(faces) * 3:
            NormalArray = ctypes.c_float * (len(faces) * 9)
            arr = NormalArray()
            for i, normal in enumerate(normals):
                arr[i * 3] = float(normal[0])
                arr[i * 3 + 1] = float(normal[1])
                arr[i * 3 + 2] = float(normal[2])
            ase_obj.face_normal_count = len(faces)
            ase_obj.face_normals = ctypes.cast(arr, ctypes.POINTER(ctypes.c_float))
            self._face_normal_arrays.append(arr)

    def _populate_light(self, ase_light, spec: dict[str, Any]) -> None:
        ase_light.name = fixup_name(spec["name"]).encode("utf-8")[:63]
        ase_light.type = int(spec.get("type", 0))
        pos = spec.get("pos", (0.0, 0.0, 0.0))
        ase_light.pos[0] = float(pos[0])
        ase_light.pos[1] = float(pos[1])
        ase_light.pos[2] = float(pos[2])
        color = spec.get("color", (1.0, 1.0, 1.0))
        ase_light.color[0] = float(color[0])
        ase_light.color[1] = float(color[1])
        ase_light.color[2] = float(color[2])
        ase_light.intensity = 0.0
        ase_light.atten_start = float(spec.get("atten_start", 0.0))
        ase_light.atten_end = float(spec.get("atten_end", 0.0))
        ase_light.falloff = float(spec.get("falloff", 0.0))
        row2 = spec.get("tm_row2", (0.0, 0.0, 1.0))
        ase_light.tm_row2[0] = float(row2[0])
        ase_light.tm_row2[1] = float(row2[1])
        ase_light.tm_row2[2] = float(row2[2])

    def _material_texture_names(self, ir_mat) -> tuple[str, str]:
        diffuse = ""
        detail = ""
        for t_idx in range(int(ir_mat.texture_count)):
            tex = ir_mat.textures[t_idx]
            tex_name = _decode(tex.name).strip()
            if not tex_name:
                continue
            if int(tex.slot) == 1 or (t_idx == 0 and not diffuse):
                diffuse = tex_name
            elif int(tex.slot) == 2:
                detail = tex_name
        return _fit_texture_name(self._used_tex_names, _fix_tex_ext(diffuse)), _fit_texture_name(
            self._used_tex_names,
            _fix_tex_ext(detail),
        )

    def _collision_material_lookup(self) -> dict[tuple[int, int], int]:
        flag_to_mat: dict[tuple[int, int], int] = {}
        for ir_idx in range(int(self.ir.material_count)):
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
            flag_to_mat.setdefault((mflags, surface_type), ir_idx)
        return flag_to_mat

    def _is_skinned(self) -> bool:
        return int(getattr(self.ir, "mesh_type", 0)) == 3


def _copy_tm(ase_obj, tm) -> None:
    for r in range(4):
        for c in range(3):
            ase_obj.tm_row[r][c] = float(tm[r][c])


def _tm(
    translation: Sequence[float],
    *,
    rot: Sequence[Sequence[float]] | None = None,
) -> tuple[tuple[float, float, float], ...]:
    if rot is None:
        rows = (
            (1.0, 0.0, 0.0),
            (0.0, 1.0, 0.0),
            (0.0, 0.0, 1.0),
        )
    else:
        rows = (
            (float(rot[0][0]), float(rot[1][0]), float(rot[2][0])),
            (float(rot[0][1]), float(rot[1][1]), float(rot[2][1])),
            (float(rot[0][2]), float(rot[1][2]), float(rot[2][2])),
        )
    return (
        rows[0],
        rows[1],
        rows[2],
        (float(translation[0]), float(translation[1]), float(translation[2])),
    )


def _decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def _vec_add(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (float(a[0]) + float(b[0]), float(a[1]) + float(b[1]), float(a[2]) + float(b[2]))


def _vec_sub(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (float(a[0]) - float(b[0]), float(a[1]) - float(b[1]), float(a[2]) - float(b[2]))


def _vec_scale(a: Sequence[float], scalar: float) -> tuple[float, float, float]:
    return (float(a[0]) * scalar, float(a[1]) * scalar, float(a[2]) * scalar)


def _vec_len_sq(v: Sequence[float]) -> float:
    return float(v[0]) * float(v[0]) + float(v[1]) * float(v[1]) + float(v[2]) * float(v[2])


def _vec_normalize(v: Sequence[float]) -> tuple[float, float, float]:
    length = math.sqrt(_vec_len_sq(v))
    if length <= 1e-12:
        return (0.0, 0.0, 0.0)
    return (float(v[0]) / length, float(v[1]) / length, float(v[2]) / length)


def _vec_cross(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (
        float(a[1]) * float(b[2]) - float(a[2]) * float(b[1]),
        float(a[2]) * float(b[0]) - float(a[0]) * float(b[2]),
        float(a[0]) * float(b[1]) - float(a[1]) * float(b[0]),
    )


def _vec_dot(a: Sequence[float], b: Sequence[float]) -> float:
    return float(a[0]) * float(b[0]) + float(a[1]) * float(b[1]) + float(a[2]) * float(b[2])


def _direction_rot(source_direction) -> tuple[tuple[float, float, float], ...] | None:
    z_axis = _vec_normalize((float(source_direction[0]), -float(source_direction[2]), float(source_direction[1])))
    if _vec_len_sq(z_axis) <= 1e-12:
        return None
    world_up = (0.0, 0.0, 1.0)
    if abs(_vec_dot(z_axis, world_up)) > 0.999:
        world_up = (0.0, 1.0, 0.0)
    x_axis = _vec_normalize(_vec_cross(world_up, z_axis))
    y_axis = _vec_normalize(_vec_cross(z_axis, x_axis))
    return (x_axis, y_axis, z_axis)


def _triangulate(faces: Sequence[Sequence[int]]) -> list[tuple[int, int, int]]:
    out = []
    for face in faces:
        if len(face) < 3:
            continue
        for i in range(1, len(face) - 1):
            out.append((int(face[0]), int(face[i]), int(face[i + 1])))
    return out


def _pack_weights(entries) -> tuple[list[int], list[float]]:
    clean = [
        (int(bone_idx), float(weight))
        for bone_idx, weight in entries
        if int(bone_idx) >= 0 and float(weight) > 0.0
    ]
    clean.sort(key=lambda item: item[0])
    clean = clean[:4]
    indices = [-1, -1, -1, -1]
    weights = [0.0, 0.0, 0.0, 0.0]
    for idx, (bone_idx, weight) in enumerate(clean):
        indices[idx] = bone_idx
        weights[idx] = weight
    return indices, weights


def _bone_mesh() -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    base = 0.01
    tip = 0.001
    base_z = 0.01
    tip_z = 0.10
    verts = [
        ( base,  base, base_z),
        ( base, -base, base_z),
        (-base, -base, base_z),
        (-base,  base, base_z),
        ( tip,  tip,  tip_z),
        ( tip, -tip,  tip_z),
        (-tip, -tip,  tip_z),
        (-tip,  tip,  tip_z),
        (0.0, 0.0, 0.0),
    ]
    faces = [
        (8, 0, 1), (8, 1, 2), (8, 2, 3), (8, 3, 0),
        (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
        (4, 5, 6), (4, 6, 7),
    ]
    return verts, faces


def _bone_export_name(name: str) -> str:
    if name.startswith("BN") and len(name) >= 4 and name[2:4].isdigit():
        return name[:4]
    return name


def _bone_node_id(name: str) -> int:
    trimmed = _bone_export_name(name)
    if trimmed.startswith("BN") and len(trimmed) >= 4 and trimmed[2:4].isdigit():
        return int(trimmed[2:4]) - 1
    return -1


def _export_name(name: str) -> str:
    if name.startswith("PN") and len(name) >= 4 and name[2:4].isdigit():
        return name[2:]
    return name


def fixup_name(name: str) -> str:
    result = []
    for ch in name:
        if ch == '"':
            result.append("'")
        elif ord(ch) <= 31:
            result.append("_")
        else:
            result.append(ch)
    return "".join(result)


def _fix_tex_ext(name: str) -> str:
    if not name:
        return name
    name = os.path.basename(name)
    root, ext = os.path.splitext(name)
    if ext.upper() in (".TGA", ".PCX", ".MDT"):
        return root + ext.upper()
    return root + ".TGA"


def _fit_texture_name(used: dict[str, str], name: str) -> str:
    if not name or len(name) <= MAX_TEX_FILENAME:
        if name:
            used[name.lower()] = name.lower()
        return name
    root, ext = os.path.splitext(name)
    max_stem = MAX_TEX_FILENAME - len(ext)
    candidate = root[:max_stem] + ext
    key = candidate.lower()
    if key in used and used[key] != name.lower():
        for i in range(2, 100):
            suffix = str(i)
            candidate = root[:max_stem - len(suffix)] + suffix + ext
            key = candidate.lower()
            if key not in used:
                break
    used[key] = name.lower()
    return candidate
