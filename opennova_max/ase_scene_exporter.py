"""Current-scene Novalogic ASE export for 3ds Max."""
from __future__ import annotations

import math
import os
import re
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from pyopennova import ase_ffi
from pyopennova.ase_material_writer import populate_ase_submaterial
from pyopennova.materials import (
    MATERIAL_BLEND_ALPHA,
    MATERIAL_BLEND_OPAQUE,
    MaterialDescriptor,
    TextureDescriptor,
    classify_material_shader,
)


Vec3 = Tuple[float, float, float]


BONE_VERTS: Tuple[Vec3, ...] = (
    (0.01, 0.01, 0.01),
    (0.01, -0.01, 0.01),
    (-0.01, -0.01, 0.01),
    (-0.01, 0.01, 0.01),
    (0.001, 0.001, 0.10),
    (0.001, -0.001, 0.10),
    (-0.001, -0.001, 0.10),
    (-0.001, 0.001, 0.10),
    (0.0, 0.0, 0.0),
)
BONE_FACES: Tuple[Tuple[int, int, int], ...] = (
    (8, 0, 1), (8, 1, 2), (8, 2, 3), (8, 3, 0),
    (0, 1, 5), (0, 5, 4),
    (1, 2, 6), (1, 6, 5),
    (2, 3, 7), (2, 7, 6),
    (3, 0, 4), (3, 4, 7),
    (4, 5, 6), (4, 6, 7),
)


class MaterialKeeper:
    """Track Max scene materials and assign stable ASE submaterial IDs."""

    SUBS_PER_SLOT = 64

    def __init__(self) -> None:
        self._materials: List[Any] = []
        self._index_by_id: Dict[int, int] = {}
        self._index_by_key: Dict[Tuple[str, str], int] = {}

    def add_material(self, material: Any) -> None:
        if material is None:
            return
        if _source_material_index(material) is None:
            return
        key = id(material)
        if key in self._index_by_id:
            return
        stable_keys = _material_identity_keys(material)
        if any(stable_key in self._index_by_key for stable_key in stable_keys):
            return
        self._index_by_id[key] = len(self._materials)
        for stable_key in stable_keys:
            self._index_by_key.setdefault(stable_key, len(self._materials))
        self._materials.append(material)

    def add_object_materials(self, material: Any) -> None:
        if material is None:
            return
        if _is_multi_material(material):
            for mat in _multi_material_entries(material):
                self.add_material(mat)
        else:
            self.add_material(material)

    def sort_by_source_index(self) -> None:
        self._materials.sort(key=_source_material_sort_key)
        self._index_by_id = {}
        self._index_by_key = {}
        for index, material in enumerate(self._materials):
            self._index_by_id[id(material)] = index
            for stable_key in _material_identity_keys(material):
                self._index_by_key.setdefault(stable_key, index)

    def get_global_index(self, material: Any) -> int:
        key = id(material)
        if key in self._index_by_id:
            return self._index_by_id[key]
        for stable_key in _material_identity_keys(material):
            if stable_key in self._index_by_key:
                return self._index_by_key[stable_key]
        raise KeyError(key)

    def count(self) -> int:
        return len(self._materials)

    def get_material(self, index: int) -> Any:
        return self._materials[index]


class _SceneStateGuard:
    """Best-effort guard for live 3ds Max UI state touched during export."""

    def __init__(self, rt: Any) -> None:
        self.rt = rt
        self._selection: Optional[List[Any]] = None
        self._command_panel_mode: Any = None

    def __enter__(self) -> "_SceneStateGuard":
        self._selection = _capture_selection(self.rt)
        self._command_panel_mode = _capture_command_panel_mode(self.rt)
        return self

    def __exit__(self, _exc_type: Any, _exc: Any, _traceback: Any) -> bool:
        _restore_command_panel_mode(self.rt, self._command_panel_mode)
        _restore_selection(self.rt, self._selection)
        return False


class AseSceneExporter:
    """Translate visible 3ds Max scene state to an ASE document."""

    SUBS_PER_SLOT = MaterialKeeper.SUBS_PER_SLOT
    MAX_TEX_FILENAME = 15

    def __init__(self, rt: Any) -> None:
        self.rt = rt
        self.include_materials = True
        self.export_textures = False
        self.scale = 1.0
        self.material_keeper = MaterialKeeper()
        self._used_tex_names: Dict[str, str] = {}
        self._face_normal_arrays: List[Any] = []

    def export_scene(self, filepath: str) -> bool:
        with _SceneStateGuard(self.rt):
            return self._export_scene(filepath)

    def _export_scene(self, filepath: str) -> bool:
        if not filepath:
            raise ValueError("No output filepath specified")
        output_dir = os.path.dirname(filepath)
        if output_dir:
            os.makedirs(output_dir, exist_ok=True)

        nodes = _scene_nodes(self.rt)
        render_meshes = self._ordered_render_meshes(nodes)
        helper_meshes = self._ordered_helper_meshes(nodes)
        part_nodes = self._part_dummy_objects(nodes)
        bone_nodes = self._bone_objects(nodes)
        lights = self._light_objects(nodes)

        self._collect_materials(render_meshes)

        total_objects = (
            len(bone_nodes)
            + len(render_meshes)
            + len(part_nodes)
            + len(helper_meshes)
        )
        doc_materials = (
            int(math.ceil(float(self.material_keeper.count()) / self.SUBS_PER_SLOT))
            if self.material_keeper.count() > 0
            else 0
        )
        doc = ase_ffi.create_document(total_objects, doc_materials, len(lights))
        has_skin = any(self._skin_modifier(node) is not None for node in render_meshes + helper_meshes)
        doc.flags = 1 if has_skin else 0
        doc.skinned_flags = 1 if has_skin else 0

        try:
            if self.include_materials:
                self._populate_materials(doc)

            obj_idx = 0
            for bone in bone_nodes:
                self._populate_bone_object(doc.objects[obj_idx], bone)
                obj_idx += 1
            for mesh in render_meshes:
                self._populate_geometry(doc.objects[obj_idx], mesh)
                obj_idx += 1
            for part in part_nodes:
                self._populate_part_dummy_object(doc.objects[obj_idx], part)
                obj_idx += 1
            for mesh in helper_meshes:
                self._populate_geometry(doc.objects[obj_idx], mesh)
                obj_idx += 1
            for idx, light in enumerate(lights):
                self._populate_light(doc.lights[idx], light)

            ase_ffi.write_file(filepath, doc)
            return True
        finally:
            for i in range(doc.object_count):
                doc.objects[i].face_normals = None
                doc.objects[i].face_normal_count = 0
            ase_ffi.free_document(doc)

    def _collect_materials(self, render_meshes: Sequence[Any]) -> None:
        for node in render_meshes:
            self.material_keeper.add_object_materials(getattr(node, "material", None))
        self.material_keeper.sort_by_source_index()

    def _populate_materials(self, doc: Any) -> None:
        for parent_idx in range(doc.material_count):
            parent = doc.materials[parent_idx]
            start = parent_idx * self.SUBS_PER_SLOT
            end = min(start + self.SUBS_PER_SLOT, self.material_keeper.count())
            count = max(0, end - start)
            parent.name = ("Scene_Materials_%d" % parent_idx).encode("utf-8")[:31]
            parent.has_submaterials = 1
            parent.submaterial_count = count
            ase_ffi.alloc_submaterials(parent, count)
            for local_idx in range(count):
                mat = self.material_keeper.get_material(start + local_idx)
                desc = self._collect_material_descriptor(mat, start + local_idx)
                populate_ase_submaterial(
                    parent.submaterials[local_idx],
                    desc,
                    used_tex_names=self._used_tex_names,
                )

    def _collect_material_descriptor(self, material: Any, index: int) -> MaterialDescriptor:
        name = _clean_duplicate_suffix(str(getattr(material, "name", "") or ""))
        shader = _shader_from_material_name(name) or self._derive_shader(material)
        mat_name = name if name.startswith("Material_") else "Material_%d_%s" % (index, shader)
        diffuse_name = self._fit_texture_name(self._fix_tex_ext(_diffuse_bitmap_name(material)))
        detail_name = self._fit_texture_name(self._fix_tex_ext(_detail_bitmap_name(material)))
        u_tile, v_tile = _bitmap_tiling(_diffuse_bitmap(material))
        u1_tile, v1_tile = _bitmap_tiling(_detail_bitmap(material))
        alpha_threshold = 0.0
        renderer_blend = MATERIAL_BLEND_OPAQUE
        semantics = classify_material_shader(
            shader,
            alpha_test_value_byte=int(round(alpha_threshold * 255.0)),
        )
        glass = semantics.is_glass
        reflect_color = (0.0, 0.0, 0.0, 0.0)
        if glass:
            ambient_value = None
            for attr in ("ambient", "ambientColor"):
                try:
                    ambient_value = getattr(material, attr)
                except Exception:
                    ambient_value = None
                if ambient_value is not None:
                    break
            if ambient_value is not None:
                ambient = _color_tuple(ambient_value)
                if any(ambient):
                    reflect_color = (ambient[0], ambient[1], ambient[2], 0.0)
        try:
            opacity = float(getattr(material, "opacity"))
        except Exception:
            opacity = 100.0
        if opacity < 99.0:
            renderer_blend = MATERIAL_BLEND_ALPHA
        if renderer_blend == MATERIAL_BLEND_OPAQUE:
            renderer_blend = semantics.blend
        return MaterialDescriptor(
            index=index,
            shader=shader,
            name=mat_name,
            diffuse=TextureDescriptor("diffuse", diffuse_name),
            detail=TextureDescriptor("detail", detail_name),
            flags=0,
            alpha_threshold=alpha_threshold,
            renderer_blend=renderer_blend,
            alpha_test=semantics.alpha_test,
            alpha_inverted=semantics.alpha_test_invert,
            two_sided=semantics.is_two_sided or _bool_attr(material, ("twoSided", "two_sided"), False),
            emissive=semantics.is_emissive or semantics.is_luminance,
            phong_shader=semantics.uses_specular,
            bump_shader=semantics.needs_normal_map,
            known_shader=semantics.known_shader,
            shader_family=semantics.family,
            resolved_shader=semantics.resolved_shader,
            needs_normal_map=semantics.needs_normal_map,
            normal_space=semantics.normal_space,
            normal_uses_uv2=semantics.normal_uses_uv2,
            has_detail_slot=semantics.has_detail or bool(detail_name),
            uses_specular=semantics.uses_specular,
            uses_environment=semantics.uses_environment,
            glass=glass,
            reflect_color=reflect_color,
            is_luminance=semantics.is_luminance,
            is_skinned=semantics.is_skinned,
            u_tiling=0.0 if (u_tile == 1.0 and v_tile == 1.0) else u_tile,
            v_tiling=0.0 if (u_tile == 1.0 and v_tile == 1.0) else v_tile,
            effective_u_tiling=u_tile,
            effective_v_tiling=v_tile,
            effective_u1_tiling=u1_tile,
            effective_v1_tiling=v1_tile,
            has_custom_tiling=(abs(u_tile - 1.0) > 1e-6 or abs(v_tile - 1.0) > 1e-6),
        )

    def _derive_shader(self, material: Any) -> str:
        if material is None:
            return "FF_ST_OP"
        try:
            if float(getattr(material, "opacity", 100.0)) < 99.0:
                return "FF_ST_AB"
        except Exception:
            pass
        return "FF_ST_OP"

    def _node_tm_for_export(self, node: Any) -> Any:
        matrix = _node_transform(node)
        name = _clean_duplicate_suffix(str(getattr(node, "name", "")))
        if _is_center_marker_name(name):
            return _transpose_matrix3(matrix)
        return matrix

    def _vertex_world_for_export(self, node: Any, tri: Any, zero_based_index: int) -> Vec3:
        name = _clean_duplicate_suffix(str(getattr(node, "name", "")))
        marker_vertices = _marker_local_vertices(name)
        if marker_vertices and zero_based_index < len(marker_vertices):
            try:
                local = marker_vertices[zero_based_index]
                origin = _world_position(node)
                return (origin[0] + local[0], origin[1] + local[1], origin[2] + local[2])
            except Exception:
                pass
        return _point_tuple(self.rt.meshop.getVert(tri, zero_based_index + 1))

    def _populate_geometry(self, ase_obj: Any, node: Any) -> None:
        tri = self._snapshot_mesh(node)
        rt = self.rt
        vert_count = int(rt.meshop.getNumVerts(tri))
        face_count = int(rt.meshop.getNumFaces(tri))
        uv_coords, uv_face_indices = self._collect_uv_data(tri, face_count)
        weights = self._collect_weight_data(node, vert_count)
        ase_ffi.alloc_object(ase_obj, vert_count, len(uv_coords), face_count, 0, len(weights))

        export_name = self._export_name(str(getattr(node, "name", "")))
        ase_obj.name = self.fixup_name(export_name).encode("utf-8")[:63]
        parent = getattr(node, "parent", None)
        if parent is not None:
            ase_obj.parent_name = self.fixup_name(self._export_name(str(getattr(parent, "name", "")))).encode("utf-8")[:63]
        ase_obj.node_id = self._node_id_from_name(str(getattr(node, "name", "")))
        ase_obj.skinned = 1 if weights else 0
        ase_obj.material_ref = self._material_ref(node)
        self._set_object_tm(ase_obj, self._node_tm_for_export(node))

        for i in range(vert_count):
            world = self._vertex_world_for_export(node, tri, i)
            ase_obj.verts[i * 3 + 0] = world[0] * self.scale
            ase_obj.verts[i * 3 + 1] = _ase_y(world[1] * self.scale)
            ase_obj.verts[i * 3 + 2] = world[2] * self.scale

        for i, uv in enumerate(uv_coords):
            ase_obj.uvs[i].u = uv[0]
            ase_obj.uvs[i].v = uv[1]
            ase_obj.uvs[i].w = 0.0

        slot_ids = self._slot_material_ids(node)
        for fi in range(face_count):
            face = rt.meshop.getFace(tri, fi + 1)
            indices = [int(face.x) - 1, int(face.y) - 1, int(face.z) - 1]
            out = ase_obj.faces[fi]
            out.vert[0], out.vert[1], out.vert[2] = indices
            out.edge_visibility[0] = 1
            out.edge_visibility[1] = 1
            out.edge_visibility[2] = 1
            out.smoothing_mask = _safe_int(lambda: rt.getFaceSmoothGroup(tri, fi + 1), 1)
            mat_id = _safe_int(lambda: rt.getFaceMatID(tri, fi + 1), 1)
            out.material_id = slot_ids.get(mat_id, 0)
            if fi < len(uv_face_indices):
                out.uv[0], out.uv[1], out.uv[2] = uv_face_indices[fi]

        for i, entries in enumerate(weights):
            _write_weight_slots(ase_obj.weights[i], entries)

        self._populate_face_normals(ase_obj, tri, face_count, node)

    def _populate_part_dummy_object(self, ase_obj: Any, node: Any) -> None:
        half = 0.0005
        verts = [
            (-half, -half, -half),
            (half, -half, -half),
            (half, half, -half),
            (-half, half, -half),
            (-half, -half, half),
            (half, -half, half),
            (half, half, half),
            (-half, half, half),
        ]
        faces = [
            (0, 1, 2), (0, 2, 3),
            (4, 7, 6), (4, 6, 5),
            (0, 4, 5), (0, 5, 1),
            (2, 6, 7), (2, 7, 3),
            (1, 5, 6), (1, 6, 2),
            (0, 3, 7), (0, 7, 4),
        ]
        ase_ffi.alloc_object(ase_obj, len(verts), 0, len(faces), 0, 0)
        ase_obj.name = self.fixup_name(_clean_duplicate_suffix(str(getattr(node, "name", "")))).encode("utf-8")[:63]
        ase_obj.parent_name = b""
        ase_obj.node_id = -1
        ase_obj.material_ref = -1
        ase_obj.skinned = 0
        self._set_object_tm(ase_obj, _node_transform(node))
        origin = _world_position(node)
        for i, vert in enumerate(verts):
            ase_obj.verts[i * 3 + 0] = (origin[0] + vert[0]) * self.scale
            ase_obj.verts[i * 3 + 1] = _ase_y((origin[1] + vert[1]) * self.scale)
            ase_obj.verts[i * 3 + 2] = (origin[2] + vert[2]) * self.scale
        for i, face_indices in enumerate(faces):
            face = ase_obj.faces[i]
            face.vert[0], face.vert[1], face.vert[2] = face_indices
            face.smoothing_mask = 0
            face.material_id = 0
            face.edge_visibility[0] = 1
            face.edge_visibility[1] = 1
            face.edge_visibility[2] = 1

    def _populate_bone_object(self, ase_obj: Any, node: Any) -> None:
        origin = _world_position(node)
        verts, faces = _bone_marker_mesh(origin)
        ase_ffi.alloc_object(ase_obj, len(verts), 0, len(faces), 0, 0)
        bone_name = self._bone_export_name(str(getattr(node, "name", "")))
        ase_obj.name = self.fixup_name(bone_name).encode("utf-8")[:63]
        parent = getattr(node, "parent", None)
        if parent is not None and self._bone_export_name(str(getattr(parent, "name", ""))).startswith("BN"):
            ase_obj.parent_name = self.fixup_name(self._bone_export_name(str(getattr(parent, "name", "")))).encode("utf-8")[:63]
        else:
            ase_obj.parent_name = b""
        ase_obj.node_id = self._node_id_from_name(bone_name)
        ase_obj.material_ref = -1
        ase_obj.skinned = 0
        self._set_object_tm(ase_obj, _IdentityMatrix(origin))
        for i, vert in enumerate(verts):
            ase_obj.verts[i * 3 + 0] = vert[0] * self.scale
            ase_obj.verts[i * 3 + 1] = _ase_y(vert[1] * self.scale)
            ase_obj.verts[i * 3 + 2] = vert[2] * self.scale
        for i, face_indices in enumerate(faces):
            face = ase_obj.faces[i]
            face.vert[0], face.vert[1], face.vert[2] = face_indices
            face.smoothing_mask = 0
            face.material_id = 0
            face.edge_visibility[0] = 1
            face.edge_visibility[1] = 1
            face.edge_visibility[2] = 1

    def _populate_light(self, ase_light: Any, node: Any) -> None:
        ase_light.name = self.fixup_name(str(getattr(node, "name", ""))).encode("utf-8")[:63]
        class_name = _class_name(self.rt, node).lower()
        ase_light.type = 1 if "spot" in class_name else 0
        pos = _world_position(node)
        ase_light.pos[0] = pos[0] * self.scale
        ase_light.pos[1] = _ase_y(pos[1] * self.scale)
        ase_light.pos[2] = pos[2] * self.scale
        rgb = _color_tuple(getattr(node, "rgb", getattr(node, "color", None)))
        ase_light.color[0], ase_light.color[1], ase_light.color[2] = rgb
        ase_light.intensity = 0.0
        ase_light.atten_start = 0.0
        ase_light.atten_end = 0.0
        if bool(getattr(node, "useFarAtten", False)):
            ase_light.atten_end = float(getattr(node, "farAttenEnd", 0.0))
        ase_light.near_atten_start = 0.0
        ase_light.near_atten_end = 0.0
        ase_light.hotspot = float(getattr(node, "hotspot", 0.0) or 0.0)
        ase_light.falloff = float(getattr(node, "falloff", 0.0) or 0.0)
        if ase_light.type == 0:
            direction = (0.0, 0.0, 1.0)
        else:
            direction = _matrix_row(_node_transform(node), 2)
            direction = _normalize(direction) or (0.0, 0.0, -1.0)
            direction = (-direction[0], -direction[1], -direction[2])
        ase_light.tm_row2[0] = _positive_zero(direction[0])
        ase_light.tm_row2[1] = _positive_zero(direction[1])
        ase_light.tm_row2[2] = _positive_zero(direction[2])

    def _collect_uv_data(self, tri: Any, face_count: int) -> Tuple[List[Tuple[float, float]], List[Tuple[int, int, int]]]:
        rt = self.rt
        try:
            has_uv0 = int(rt.meshop.getNumMaps(tri)) > 1
        except Exception:
            has_uv0 = False
        if not has_uv0:
            return [], []
        coords: List[Tuple[float, float]] = []
        faces: List[Tuple[int, int, int]] = []
        for fi in range(face_count):
            try:
                map_face = rt.meshop.getMapFace(tri, 1, fi + 1)
            except Exception:
                faces.append((0, 0, 0))
                continue
            face_indices: List[int] = []
            for attr in ("x", "y", "z"):
                tv_idx = int(getattr(map_face, attr))
                try:
                    uv = rt.meshop.getMapVert(tri, 1, tv_idx)
                    coords.append((float(uv.x), float(uv.y)))
                except Exception:
                    coords.append((0.0, 0.0))
                face_indices.append(len(coords) - 1)
            faces.append((face_indices[0], face_indices[1], face_indices[2]))
        return coords, faces

    def _collect_weight_data(self, node: Any, vertex_count: int) -> List[List[Tuple[int, float]]]:
        skin = self._skin_modifier(node)
        if skin is None:
            return []
        rt = self.rt
        try:
            bone_count = int(rt.skinOps.GetNumberBones(skin))
        except Exception:
            return []
        slot_to_index: Dict[int, int] = {}
        for slot in range(1, bone_count + 1):
            name = ""
            try:
                name = str(rt.skinOps.GetBoneName(skin, slot, 0))
            except Exception:
                pass
            slot_to_index[slot] = _bone_number_from_name(name, slot - 1)
        out: List[List[Tuple[int, float]]] = []
        any_weights = False
        for vi in range(1, vertex_count + 1):
            pairs: List[Tuple[int, float]] = []
            try:
                count = int(rt.skinOps.GetVertexWeightCount(skin, vi))
            except Exception:
                count = 0
            for wi in range(1, count + 1):
                try:
                    bone_id = int(rt.skinOps.GetVertexWeightBoneID(skin, vi, wi))
                    weight = float(rt.skinOps.GetVertexWeight(skin, vi, wi))
                except Exception:
                    continue
                if weight > 0.0:
                    pairs.append((slot_to_index.get(bone_id, bone_id - 1), weight))
            pairs.sort(key=lambda item: item[0])
            if pairs:
                any_weights = True
            out.append(pairs[:4])
        return out if any_weights else []

    def _populate_face_normals(self, ase_obj: Any, tri: Any, face_count: int, node: Any) -> None:
        import ctypes

        rt = self.rt
        explicit_normals = self._collect_edit_normals(node, face_count)
        if not any(vec is not None for vec in explicit_normals):
            return
        normals = (ctypes.c_float * (face_count * 9))()
        for fi in range(face_count):
            geometry_normal: Optional[Vec3] = (0.0, 0.0, 1.0)
            try:
                face = rt.meshop.getFace(tri, fi + 1)
                indices = [int(face.x), int(face.y), int(face.z)]
            except Exception:
                face = None
                indices = [1, 1, 1]
            if face is not None:
                try:
                    geometry_normal = _face_export_geometry_normal(
                        ase_obj,
                        [idx - 1 for idx in indices],
                    )
                except Exception:
                    try:
                        geometry_normal = _face_geometry_normal(rt, tri, face)
                    except Exception:
                        geometry_normal = (0.0, 0.0, 1.0)
            render_normals = _face_render_normals(rt, tri, fi + 1)
            selected_normals: List[Vec3] = []
            for ci, vert_idx in enumerate(indices):
                explicit_index = fi * 3 + ci
                if explicit_index < len(explicit_normals) and explicit_normals[explicit_index] is not None:
                    vec = explicit_normals[explicit_index]
                elif ci < len(render_normals):
                    vec = render_normals[ci]
                else:
                    try:
                        normal = rt.getNormal(tri, vert_idx)
                        vec = _normalize(_point_tuple(normal)) or (0.0, 0.0, 1.0)
                    except Exception:
                        vec = (0.0, 0.0, 1.0)
                selected_normals.append(vec)
            if geometry_normal is None:
                selected_normals = [
                    (0.0, 0.0, 0.0) if _vec_close(vec, (1.0, 0.0, 0.0)) else vec
                    for vec in selected_normals
                ]
            for ci, vec in enumerate(selected_normals):
                base = fi * 9 + ci * 3
                normals[base + 0] = vec[0]
                normals[base + 1] = _ase_y(vec[1])
                normals[base + 2] = vec[2]
        ase_obj.face_normal_count = face_count
        ase_obj.face_normals = ctypes.cast(normals, ctypes.POINTER(ctypes.c_float))
        self._face_normal_arrays.append(normals)

    def _collect_edit_normals(self, node: Any, face_count: int) -> List[Optional[Vec3]]:
        normal_mod = _edit_normals_modifier(self.rt, node)
        if normal_mod is None:
            return []
        try:
            if int(normal_mod.GetNumFaces(node=node)) < face_count:
                return []
        except Exception:
            return []
        out: List[Optional[Vec3]] = []
        for fi in range(1, face_count + 1):
            for corner in range(1, 4):
                try:
                    normal_id = int(normal_mod.GetNormalID(fi, corner, node=node))
                    normal = normal_mod.GetNormal(normal_id, node=node)
                except Exception:
                    out.append(None)
                    continue
                out.append(_normalize(_point_tuple(normal)))
        return out

    def _snapshot_mesh(self, node: Any) -> Any:
        try:
            return self.rt.snapshotAsMesh(node)
        except Exception:
            return node

    def _skin_modifier(self, node: Any) -> Any:
        for mod in _modifiers(node):
            if "skin" in _class_name(self.rt, mod).lower():
                return mod
        return None

    def _material_ref(self, node: Any) -> int:
        material = getattr(node, "material", None)
        if material is None or self.material_keeper.count() == 0:
            return -1 if self._is_helper_mesh(node) else 0
        if _is_multi_material(material):
            for mat in _multi_material_entries(material):
                if mat is not None:
                    try:
                        return self.material_keeper.get_global_index(mat) // self.SUBS_PER_SLOT
                    except KeyError:
                        return 0
            return 0
        try:
            return self.material_keeper.get_global_index(material) // self.SUBS_PER_SLOT
        except KeyError:
            return -1 if self._is_helper_mesh(node) else 0

    def _slot_material_ids(self, node: Any) -> Dict[int, int]:
        material = getattr(node, "material", None)
        if material is None:
            return {}
        if not _is_multi_material(material):
            try:
                idx = self.material_keeper.get_global_index(material)
            except KeyError:
                idx = 0
            return {1: idx % self.SUBS_PER_SLOT}
        out: Dict[int, int] = {}
        for mat_id, mat in _multi_material_id_entries(material):
            if mat is None:
                continue
            try:
                out[int(mat_id)] = self.material_keeper.get_global_index(mat) % self.SUBS_PER_SLOT
            except KeyError:
                out[int(mat_id)] = 0
        return out

    def _ordered_render_meshes(self, nodes: Sequence[Any]) -> List[Any]:
        meshes = [(idx, node) for idx, node in enumerate(nodes) if self._is_render_mesh(node)]
        return [
            node
            for _, node in sorted(
                meshes,
                key=lambda item: (_leading_number_key(str(getattr(item[1], "name", ""))), str(getattr(item[1], "name", "")), item[0]),
            )
        ]

    def _ordered_helper_meshes(self, nodes: Sequence[Any]) -> List[Any]:
        meshes = [
            (idx, node)
            for idx, node in enumerate(nodes)
            if self._is_geometry(node) and self._is_visible(node) and not self._is_render_mesh(node)
            and not self._bone_export_name(str(getattr(node, "name", ""))).startswith("BN")
        ]
        return [node for _, node in sorted(meshes, key=lambda item: self._mesh_sort_key(item[1], item[0]))]

    def _part_dummy_objects(self, nodes: Sequence[Any]) -> List[Any]:
        return sorted(
            [node for node in nodes if self._is_part_dummy(node) and self._is_visible(node)],
            key=lambda node: (_leading_number_key(str(getattr(node, "name", ""))), str(getattr(node, "name", ""))),
        )

    def _bone_objects(self, nodes: Sequence[Any]) -> List[Any]:
        bones = [
            node for node in nodes
            if self._is_visible(node) and self._bone_export_name(str(getattr(node, "name", ""))).startswith("BN")
        ]
        return sorted(bones, key=lambda node: self._bone_export_name(str(getattr(node, "name", ""))))

    def _light_objects(self, nodes: Sequence[Any]) -> List[Any]:
        return [node for node in nodes if self._is_visible(node) and _is_light(self.rt, node)]

    def _is_visible(self, node: Any) -> bool:
        current = node
        while current is not None:
            for attr in ("isHidden", "isHiddenInVpt"):
                try:
                    if bool(getattr(current, attr)):
                        return False
                except Exception:
                    pass
            current = getattr(current, "parent", None)
        return True

    def _is_geometry(self, node: Any) -> bool:
        if not self._is_visible(node):
            return False
        if self._is_part_dummy(node) or _is_light(self.rt, node):
            return False
        try:
            return bool(self.rt.isKindOf(node, self.rt.GeometryClass))
        except Exception:
            pass
        try:
            tri = self._snapshot_mesh(node)
            return int(self.rt.meshop.getNumFaces(tri)) >= 0
        except Exception:
            return False

    def _is_render_mesh(self, node: Any) -> bool:
        name = _clean_duplicate_suffix(str(getattr(node, "name", "")))
        return self._is_geometry(node) and re.match(r"^\d{2} Mesh\d+$", name) is not None

    def _is_helper_mesh(self, node: Any) -> bool:
        return self._is_geometry(node) and not self._is_render_mesh(node)

    @staticmethod
    def _is_part_dummy(node: Any) -> bool:
        name = _clean_duplicate_suffix(str(getattr(node, "name", "")))
        return re.match(r"^PN\d{2}$", name) is not None

    @staticmethod
    def _mesh_sort_key(node: Any, scene_index: int = 0) -> Tuple[Any, ...]:
        name = _clean_duplicate_suffix(str(getattr(node, "name", "")))
        group = _mesh_group(name)
        if group == 4:
            return (group, _userpoint_sort_key(name), scene_index)
        if group in (3, 5, 6):
            return (group, _scene_order_key(node, scene_index), name)
        return (group, _leading_number_key(name), name)

    @staticmethod
    def _export_name(name: str) -> str:
        name = _clean_duplicate_suffix(name)
        if name.startswith("PN") and len(name) >= 4 and name[2:4].isdigit():
            return name[2:]
        return name

    @staticmethod
    def _bone_export_name(name: str) -> str:
        if name.startswith("BN") and len(name) >= 4 and name[2:4].isdigit():
            return name[:4]
        return name

    @staticmethod
    def _node_id_from_name(name: str) -> int:
        clean = _clean_duplicate_suffix(name)
        if clean.startswith("BN") and len(clean) >= 4 and clean[2:4].isdigit():
            return int(clean[2:4]) - 1
        return -1

    @staticmethod
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

    def _set_object_tm(self, ase_obj: Any, matrix: Any) -> None:
        for r in range(3):
            row = _matrix_row(matrix, r)
            for c in range(3):
                ase_obj.tm_row[r][c] = row[c]
        pos = _matrix_position(matrix)
        ase_obj.tm_row[3][0] = pos[0] * self.scale
        ase_obj.tm_row[3][1] = _ase_y(pos[1] * self.scale)
        ase_obj.tm_row[3][2] = pos[2] * self.scale

    def _fix_tex_ext(self, name: str) -> str:
        if not name:
            return name
        name = os.path.basename(name)
        root, ext = os.path.splitext(name)
        if ext.upper() in (".TGA", ".PCX", ".MDT"):
            return root + ext
        return root + ".tga"

    def _fit_texture_name(self, name: str) -> str:
        if not name:
            return name
        if len(name) <= self.MAX_TEX_FILENAME:
            self._used_tex_names[name.lower()] = name.lower()
            return name
        root, ext = os.path.splitext(name)
        max_stem = self.MAX_TEX_FILENAME - len(ext)
        candidate = root[:max_stem] + ext
        suffix = 1
        while candidate.lower() in self._used_tex_names:
            suffix_text = str(suffix)
            candidate = root[: max(1, max_stem - len(suffix_text))] + suffix_text + ext
            suffix += 1
        self._used_tex_names[candidate.lower()] = candidate.lower()
        return candidate


def export_scene_with_dialog() -> bool:
    rt = _rt()
    filepath = _prompt_save_path(
        rt,
        caption="Export Novalogic ASE",
        types="Novalogic ASE (*.ase)|*.ase|All Files (*.*)|*.*|",
        extension=".ase",
    )
    if not filepath:
        return False
    return AseSceneExporter(rt).export_scene(filepath)


def _rt() -> Any:
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; 3ds Max scene export only runs inside 3ds Max."
        ) from exc
    return pymxs.runtime


def _prompt_save_path(rt: Any, caption: str, types: str, extension: str) -> str:
    try:
        value = rt.execute(
            'getSaveFileName caption:"%s" types:"%s"'
            % (caption.replace('"', "'"), types.replace('"', "'"))
        )
    except Exception:
        try:
            value = rt.getSaveFileName(caption=caption, types=types)
        except Exception:
            value = None
    if value is None:
        return ""
    path = str(value)
    if not path:
        return ""
    root, ext = os.path.splitext(path)
    if not ext:
        path = root + extension
    return path


def _scene_nodes(rt: Any) -> List[Any]:
    try:
        return list(rt.objects)
    except Exception:
        return []


def _capture_selection(rt: Any) -> Optional[List[Any]]:
    try:
        return list(rt.selection)
    except Exception:
        return None


def _restore_selection(rt: Any, selection: Optional[Sequence[Any]]) -> None:
    if selection is None:
        return
    if selection:
        try:
            rt.select(list(selection))
            return
        except Exception:
            pass
        try:
            array = rt.Array(*selection)
            rt.select(array)
            return
        except Exception:
            pass
    try:
        rt.clearSelection()
        return
    except Exception:
        pass
    try:
        rt.execute("clearSelection()")
    except Exception:
        pass


def _capture_command_panel_mode(rt: Any) -> Any:
    try:
        return rt.getCommandPanelTaskMode()
    except Exception:
        pass
    try:
        return rt.execute("getCommandPanelTaskMode()")
    except Exception:
        return None


def _restore_command_panel_mode(rt: Any, mode: Any) -> None:
    if mode is None:
        return
    try:
        rt.setCommandPanelTaskMode(mode)
        return
    except Exception:
        pass
    try:
        rt.execute("setCommandPanelTaskMode %s" % mode)
    except Exception:
        pass


def _is_multi_material(material: Any) -> bool:
    if not hasattr(material, "materialList"):
        return False
    try:
        return int(getattr(material, "numsubs")) > 0
    except Exception:
        return False


def _material_identity_keys(material: Any) -> List[Tuple[str, str]]:
    keys: List[Tuple[str, str]] = []
    try:
        handle = getattr(material, "handle")
    except Exception:
        handle = None
    if handle not in (None, ""):
        keys.append(("handle", str(handle)))
    try:
        name = str(getattr(material, "name", "") or "")
    except Exception:
        name = ""
    if name:
        keys.append(("name", _clean_duplicate_suffix(name)))
    try:
        diffuse = str(_diffuse_bitmap_name(material) or "")
    except Exception:
        diffuse = ""
    if name and diffuse:
        keys.insert(0, ("name-diffuse", "%s|%s" % (_clean_duplicate_suffix(name), diffuse.lower())))
    return keys


def _source_material_index(material: Any) -> Optional[int]:
    try:
        name = _clean_duplicate_suffix(str(getattr(material, "name", "") or ""))
    except Exception:
        return None
    match = re.match(r"^Material_(\d+)_.+$", name)
    if not match:
        return None
    try:
        return int(match.group(1))
    except ValueError:
        return None


def _source_material_sort_key(material: Any) -> int:
    index = _source_material_index(material)
    return index if index is not None else 999999


def _multi_material_entries(material: Any) -> List[Any]:
    return [mat for _mat_id, mat in _multi_material_id_entries(material)]


def _multi_material_id_entries(material: Any) -> List[Tuple[int, Any]]:
    try:
        count = int(getattr(material, "numsubs"))
    except Exception:
        count = 0
    entries: List[Tuple[int, Any]] = []
    for idx in range(1, count + 1):
        mat = _seq_get(getattr(material, "materialList", None), idx)
        mat_id = _seq_get(getattr(material, "materialIDList", None), idx)
        if mat_id is None:
            mat_id = idx
        entries.append((int(mat_id), mat))
    return entries


def _seq_get(seq: Any, one_based_index: int) -> Any:
    if seq is None:
        return None
    for idx in (one_based_index - 1, one_based_index):
        try:
            return seq[idx]
        except Exception:
            pass
    try:
        values = getattr(seq, "values")
        return values[one_based_index - 1]
    except Exception:
        return None


def _modifiers(node: Any) -> Iterable[Any]:
    mods = getattr(node, "modifiers", None)
    if mods is None:
        return ()
    try:
        return list(mods)
    except Exception:
        return ()


def _edit_normals_modifier(rt: Any, node: Any) -> Any:
    for mod in _modifiers(node):
        class_name = _class_name(rt, mod).lower().replace(" ", "_")
        if "edit_normals" in class_name:
            return mod
    return None


def _diffuse_bitmap_name(material: Any) -> str:
    return _bitmap_filename(_diffuse_bitmap(material))


def _detail_bitmap_name(material: Any) -> str:
    return _bitmap_filename(_detail_bitmap(material))


def _diffuse_bitmap(material: Any) -> Any:
    return _material_bitmap(material, ("diffuseMap", "map1"), (2, 1))


def _detail_bitmap(material: Any) -> Any:
    return _material_bitmap(material, ("detailMap", "selfIllumMap"), (5,))


def _material_bitmap(material: Any, attrs: Sequence[str], map_slots: Sequence[int]) -> Any:
    bitmap = None
    for attr in attrs:
        try:
            candidate = getattr(material, attr)
            if candidate:
                bitmap = candidate
                break
        except Exception:
            pass
    if bitmap is not None:
        return bitmap
    maps = getattr(material, "maps", None)
    for slot in map_slots:
        bitmap = _seq_get(maps, slot)
        if bitmap:
            return bitmap
    return None


def _bitmap_filename(bitmap: Any) -> str:
    if bitmap is None:
        return ""
    for attr in ("filename", "fileName", "Filename"):
        try:
            value = getattr(bitmap, attr)
            if value:
                return os.path.basename(str(value))
        except Exception:
            pass
    return ""


def _bitmap_tiling(bitmap: Any) -> Tuple[float, float]:
    coords = getattr(bitmap, "coords", None)
    if coords is None:
        return (1.0, 1.0)
    return (
        _float_attr(coords, ("U_Tiling", "u_tiling"), 1.0),
        _float_attr(coords, ("V_Tiling", "v_tiling"), 1.0),
    )


def _float_attr(obj: Any, names: Sequence[str], default: float) -> float:
    for name in names:
        try:
            value = getattr(obj, name)
            return float(value)
        except Exception:
            pass
    return float(default)


def _bool_attr(obj: Any, names: Sequence[str], default: bool) -> bool:
    for name in names:
        try:
            return bool(getattr(obj, name))
        except Exception:
            pass
    return bool(default)


def _shader_from_material_name(name: str) -> str:
    match = re.match(r"^Material_\d+_(.+)$", name)
    return match.group(1) if match else ""


def _is_light(rt: Any, node: Any) -> bool:
    try:
        return bool(rt.isKindOf(node, rt.Light))
    except Exception:
        pass
    return "light" in _class_name(rt, node).lower()


def _class_name(rt: Any, obj: Any) -> str:
    try:
        return str(rt.classOf(obj))
    except Exception:
        return obj.__class__.__name__


def _node_transform(node: Any) -> Any:
    try:
        return node.transform
    except Exception:
        return _IdentityMatrix(_world_position(node))


def _world_position(node: Any) -> Vec3:
    try:
        return _point_tuple(node.transform.position)
    except Exception:
        pass
    try:
        return _point_tuple(node.position)
    except Exception:
        return (0.0, 0.0, 0.0)


def _matrix_position(matrix: Any) -> Vec3:
    for attr in ("position", "translation", "row4"):
        try:
            return _point_tuple(getattr(matrix, attr))
        except Exception:
            pass
    return (0.0, 0.0, 0.0)


def _matrix_row(matrix: Any, row_index: int) -> Vec3:
    for attr in ("row%d" % (row_index + 1),):
        try:
            return _point_tuple(getattr(matrix, attr))
        except Exception:
            pass
    try:
        row = matrix[row_index]
        return (float(row[0]), float(row[1]), float(row[2]))
    except Exception:
        pass
    return (1.0, 0.0, 0.0) if row_index == 0 else (0.0, 1.0, 0.0) if row_index == 1 else (0.0, 0.0, 1.0)


def _transpose_matrix3(matrix: Any) -> Any:
    rows = [_matrix_row(matrix, 0), _matrix_row(matrix, 1), _matrix_row(matrix, 2)]
    return _MatrixRows(
        (
            (rows[0][0], rows[1][0], rows[2][0]),
            (rows[0][1], rows[1][1], rows[2][1]),
            (rows[0][2], rows[1][2], rows[2][2]),
        ),
        _matrix_position(matrix),
    )


def _transform_point(matrix: Any, point: Vec3) -> Vec3:
    rows = [_matrix_row(matrix, 0), _matrix_row(matrix, 1), _matrix_row(matrix, 2)]
    pos = _matrix_position(matrix)
    return (
        point[0] * rows[0][0] + point[1] * rows[1][0] + point[2] * rows[2][0] + pos[0],
        point[0] * rows[0][1] + point[1] * rows[1][1] + point[2] * rows[2][1] + pos[1],
        point[0] * rows[0][2] + point[1] * rows[1][2] + point[2] * rows[2][2] + pos[2],
    )


def _point_tuple(value: Any) -> Vec3:
    try:
        return (float(value.x), float(value.y), float(value.z))
    except Exception:
        return (float(value[0]), float(value[1]), float(value[2]))


def _color_tuple(value: Any) -> Vec3:
    if value is None:
        return (1.0, 1.0, 1.0)
    point = _color_value_tuple(value)
    if max(point) > 1.0:
        return (
            max(0.0, min(point[0] / 255.0, 1.0)),
            max(0.0, min(point[1] / 255.0, 1.0)),
            max(0.0, min(point[2] / 255.0, 1.0)),
        )
    return point


def _color_value_tuple(value: Any) -> Vec3:
    for attrs in (("r", "g", "b"), ("red", "green", "blue"), ("x", "y", "z")):
        try:
            return (
                float(getattr(value, attrs[0])),
                float(getattr(value, attrs[1])),
                float(getattr(value, attrs[2])),
            )
        except Exception:
            pass
    try:
        return (float(value[0]), float(value[1]), float(value[2]))
    except Exception:
        return (1.0, 1.0, 1.0)


def _normalize(value: Vec3, tolerance: float = 1e-8) -> Optional[Vec3]:
    length = math.sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2])
    if length <= tolerance:
        return None
    return (value[0] / length, value[1] / length, value[2] / length)


def _face_render_normals(rt: Any, tri: Any, face_index: int) -> List[Vec3]:
    try:
        values = rt.meshop.getFaceRNormals(tri, face_index)
    except Exception:
        return []
    out: List[Vec3] = []
    for value in values:
        vec = _normalize(_point_tuple(value))
        if vec is not None:
            out.append(vec)
    return out


def _face_geometry_normal(rt: Any, tri: Any, face: Any) -> Optional[Vec3]:
    vertices = [
        _point_tuple(rt.meshop.getVert(tri, int(face.x))),
        _point_tuple(rt.meshop.getVert(tri, int(face.y))),
        _point_tuple(rt.meshop.getVert(tri, int(face.z))),
    ]
    edge_a = _vec_sub(vertices[1], vertices[0])
    edge_b = _vec_sub(vertices[2], vertices[0])
    return _normalize(_vec_cross(edge_a, edge_b), tolerance=1e-12)


def _face_export_geometry_normal(ase_obj: Any, indices: Sequence[int]) -> Optional[Vec3]:
    vertices = []
    for index in indices:
        base = int(index) * 3
        vertices.append(
            (
                float(ase_obj.verts[base + 0]),
                float(ase_obj.verts[base + 1]),
                float(ase_obj.verts[base + 2]),
            )
        )
    edge_a = _vec_sub(vertices[1], vertices[0])
    edge_b = _vec_sub(vertices[2], vertices[0])
    return _normalize(_vec_cross(edge_a, edge_b), tolerance=1e-12)


def _vec_sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _vec_cross(a: Vec3, b: Vec3) -> Vec3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _vec_close(a: Vec3, b: Vec3, tolerance: float = 1e-4) -> bool:
    return (
        abs(a[0] - b[0]) <= tolerance
        and abs(a[1] - b[1]) <= tolerance
        and abs(a[2] - b[2]) <= tolerance
    )


def _write_weight_slots(out_weight: Any, entries: Sequence[Tuple[int, float]]) -> None:
    for slot in range(4):
        out_weight.bone_index[slot] = -1
        out_weight.weight[slot] = 0.0
    for slot, (bone_idx, weight) in enumerate(entries[:4]):
        out_weight.bone_index[slot] = int(bone_idx)
        out_weight.weight[slot] = float(weight)


def _bone_marker_mesh(origin: Vec3) -> Tuple[List[Vec3], Tuple[Tuple[int, int, int], ...]]:
    return (
        [
            (
                float(origin[0]) + vert[0],
                float(origin[1]) + vert[1],
                float(origin[2]) + vert[2],
            )
            for vert in BONE_VERTS
        ],
        BONE_FACES,
    )


def _ase_y(value: float) -> float:
    value = float(value)
    return -0.0 if value == 0.0 else value


def _positive_zero(value: float) -> float:
    return 0.0 if abs(float(value)) < 1e-8 else float(value)


def _safe_int(fn: Any, default: int) -> int:
    try:
        return int(fn())
    except Exception:
        return int(default)


def _clean_duplicate_suffix(name: str) -> str:
    return re.sub(r"\.\d{3}$", "", name)


def _leading_number_key(name: str) -> int:
    match = re.search(r"\d+", name)
    return int(match.group(0)) if match else 999999


def _userpoint_sort_key(name: str) -> Tuple[Any, ...]:
    match = re.match(r"^(UP(.|$)|USR)(\d{2})(?:\s+(.*))?$", name)
    if not match:
        return (0, 0, name.casefold())
    prefix = match.group(1)
    type_code = 0
    if prefix.startswith("UP") and len(prefix) >= 3:
        type_code = ord(prefix[2])
    part_index = int(match.group(3)) - 1
    label = (match.group(4) or "").casefold()
    return (-part_index, type_code, label)


def _is_center_marker_name(name: str) -> bool:
    return re.match(r"^_\d{2} center$", name) is not None


def _marker_local_vertices(name: str) -> Tuple[Vec3, ...]:
    group = _mesh_group(name)
    if group in (2, 4):
        return _cube_vertices(0.015)
    if group == 3:
        return _cube_vertices(0.012)
    return ()


def _cube_vertices(size: float) -> Tuple[Vec3, ...]:
    half = float(size) * 0.5
    return (
        (-half, -half, -half),
        (half, -half, -half),
        (half, half, -half),
        (-half, half, -half),
        (-half, -half, half),
        (half, -half, half),
        (half, half, half),
        (-half, half, half),
    )


def _scene_order_key(node: Any, fallback: int) -> int:
    for attr in ("handle", "inodeHandle"):
        try:
            value = getattr(node, attr)
            if value is not None:
                return int(value)
        except Exception:
            pass
    return int(fallback)


def _mesh_group(name: str) -> int:
    if re.match(r"^\d{2} Mesh\d+$", name):
        return 0
    if re.match(r"^_\d{2} center$", name):
        return 2
    if name.startswith("~") and " attach" in name:
        return 3
    if name.startswith("UP") or name.startswith("USR"):
        return 4
    if "-colonly" in name:
        return 5
    if "-occonly" in name:
        return 6
    return 9


def _bone_number_from_name(name: str, fallback: int) -> int:
    match = re.match(r"^BN(\d+)", name.upper())
    if match:
        return int(match.group(1)) - 1
    return int(fallback)


class _IdentityMatrix:
    def __init__(self, position: Vec3) -> None:
        self.row1 = (1.0, 0.0, 0.0)
        self.row2 = (0.0, 1.0, 0.0)
        self.row3 = (0.0, 0.0, 1.0)
        self.position = position


class _MatrixRows:
    def __init__(self, rows: Sequence[Vec3], position: Vec3) -> None:
        self.row1 = rows[0]
        self.row2 = rows[1]
        self.row3 = rows[2]
        self.position = position
