"""Host-agnostic mesh flattener.

Walks a ``ThreediModelIR`` LOD and emits one ``FlatMesh`` per part with
plain Python data: vertex positions, triangle indices, per-face material
ids, per-corner UVs and normals, and smoothing-group bitmasks. The
result feeds either Blender (``mesh_data.from_pydata``) or 3ds Max
(``Editable_Mesh`` via PyMXS) without touching either DCC.

Mirrors the per-part grouping, index winding, skin-weight preservation,
and empty-subobject behaviour used by
``apps.importer.scene_builder._create_meshes_for_lod`` so host importers
can share one topology builder.
"""
from __future__ import annotations

from collections import OrderedDict, defaultdict
from dataclasses import dataclass, field
from typing import Sequence, Tuple

from pyopennova import coords
from pyopennova.mesh_utils import compute_smoothing_groups

Vec3 = Tuple[float, float, float]
Vec2 = Tuple[float, float]


@dataclass
class FlatMesh:
    """Per-part flattened mesh data ready for any DCC's mesh API."""

    name: str
    part_index: int
    vertices: list[Vec3] = field(default_factory=list)
    faces: list[tuple[int, int, int]] = field(default_factory=list)
    face_uvs0: list[Vec2] = field(default_factory=list)         # 3 entries per face
    face_uvs1: list[Vec2] = field(default_factory=list)         # 3 entries per face
    face_normals: list[Vec3] = field(default_factory=list)      # 3 entries per face
    face_material_ids: list[int] = field(default_factory=list)  # global IR material index
    smoothing_groups: list[int] = field(default_factory=list)   # 1 entry per face
    material_id_set: list[int] = field(default_factory=list)    # ordered unique material ids
    vertex_bone_data: list[list[tuple[int, float]]] = field(default_factory=list)


def flatten_lod(
    ir,
    lod_index: int,
    *,
    include_empty_parts: bool = False,
    track_bone_data: bool = False,
) -> list[FlatMesh]:
    """Return one ``FlatMesh`` per part with geometry.

    The IR is the ``ThreediModelIR`` ctypes structure produced by
    ``pyopennova.threedi_ffi.read_model_ir``.

    ``include_empty_parts`` adds empty meshes for parts with no usable
    geometry, preserving the subobject count expected by the legacy tools.
    ``track_bone_data`` populates ``FlatMesh.vertex_bone_data`` for Max/Blender
    skin binding.
    """
    lod = ir.lods[lod_index]
    num_parts = int(lod.part_count)
    num_primitives = int(lod.primitive_count)
    vertex_count = int(lod.vertex_count)
    is_skinned = int(getattr(ir, "mesh_type", 0)) == 3

    part_prims: dict[int, list[int]] = defaultdict(list)
    for prim_idx in range(num_primitives):
        prim = lod.primitives[prim_idx]
        part_idx = int(prim.part_index)
        if 0 <= part_idx < num_parts:
            part_prims[part_idx].append(prim_idx)

    out: list[FlatMesh] = []
    part_indices = range(num_parts) if include_empty_parts else sorted(part_prims.keys())
    for part_idx in part_indices:
        prim_indices = part_prims.get(part_idx, [])
        if not prim_indices and include_empty_parts:
            out.append(FlatMesh(name=f"{part_idx + 1:02d} Mesh0", part_index=part_idx))
            continue

        part = lod.parts[part_idx]
        part_abs = (
            float(part.abs_position[0]),
            float(part.abs_position[1]),
            float(part.abs_position[2]),
        )

        fm = _flatten_part(
            lod=lod,
            part_idx=part_idx,
            part_abs=part_abs,
            prim_indices=prim_indices,
            vertex_count=vertex_count,
            is_skinned=is_skinned,
            track_bone_data=track_bone_data,
        )
        if fm is not None:
            out.append(fm)
        elif include_empty_parts:
            out.append(FlatMesh(name=f"{part_idx + 1:02d} Mesh0", part_index=part_idx))

    return out


def _flatten_part(
    *,
    lod,
    part_idx: int,
    part_abs: Vec3,
    prim_indices: Sequence[int],
    vertex_count: int,
    is_skinned: bool,
    track_bone_data: bool,
) -> FlatMesh | None:
    """Build a FlatMesh for a single part (all primitives merged)."""
    fm = FlatMesh(name=f"{part_idx + 1:02d} Mesh0", part_index=part_idx)

    vert_map: dict[tuple[float, float, float], int] = {}
    mat_id_set: "OrderedDict[int, None]" = OrderedDict()

    def _add_vert(pos: Vec3, bone_entries: list[tuple[int, float]], source_index: int) -> int:
        if is_skinned:
            # Preserve original source vertex indexing for skinned meshes.
            # Position-only dedup collapses distinct vertices with different
            # weights and breaks CDTA/COBJ parity on assets like US01.
            key = ("src", int(source_index))
        else:
            key = (
                round(pos[0], 6),
                round(pos[1], 6),
                round(pos[2], 6),
                _bone_key(bone_entries),
            )
        existing = vert_map.get(key)
        if existing is not None:
            return existing
        idx = len(fm.vertices)
        fm.vertices.append(pos)
        if track_bone_data:
            fm.vertex_bone_data.append(bone_entries)
        vert_map[key] = idx
        return idx

    def _bone_key(entries: list[tuple[int, float]]) -> tuple[tuple[int, float], ...]:
        if not is_skinned:
            return ()
        return tuple(
            (int(bone_idx), round(float(weight), 6))
            for bone_idx, weight in entries
        )

    def _bone_entries(v, prim) -> list[tuple[int, float]]:
        if not is_skinned:
            return [(part_idx, 1.0)]

        entries: list[tuple[int, float]] = []
        table_len = int(getattr(prim, "bone_table_length", 0))
        for bi in range(4):
            weight = float(v.bone_weights[bi])
            if weight <= 0.0:
                continue
            local_idx = int(v.bone_indices[bi])
            if table_len > 0 and local_idx < table_len:
                skel_idx = int(prim.bone_table[local_idx])
            else:
                skel_idx = 0
            entries.append((skel_idx, weight))
        if not entries:
            entries.append((part_idx, 1.0))
        return entries

    for prim_idx in prim_indices:
        prim = lod.primitives[prim_idx]
        idx_offset = int(prim.index_offset)
        idx_count = int(prim.index_count)
        vert_offset = int(prim.vertex_offset)
        prim_mat_idx = int(prim.material_index)

        if idx_count < 3:
            continue

        if prim_mat_idx not in mat_id_set:
            mat_id_set[prim_mat_idx] = None

        for j in range(0, idx_count, 3):
            # Index winding swap (0, 2, 1) matches the Blender importer.
            i0 = int(lod.indices[idx_offset + j + 0]) + vert_offset
            i1 = int(lod.indices[idx_offset + j + 2]) + vert_offset
            i2 = int(lod.indices[idx_offset + j + 1]) + vert_offset

            if i0 >= vertex_count or i1 >= vertex_count or i2 >= vertex_count:
                continue

            v0 = lod.vertices[i0]
            v1 = lod.vertices[i1]
            v2 = lod.vertices[i2]

            pos0 = coords.render_space((
                v0.position[0] - part_abs[0],
                v0.position[1] - part_abs[1],
                v0.position[2] - part_abs[2],
            ))
            pos1 = coords.render_space((
                v1.position[0] - part_abs[0],
                v1.position[1] - part_abs[1],
                v1.position[2] - part_abs[2],
            ))
            pos2 = coords.render_space((
                v2.position[0] - part_abs[0],
                v2.position[1] - part_abs[1],
                v2.position[2] - part_abs[2],
            ))

            bd0 = _bone_entries(v0, prim)
            bd1 = _bone_entries(v1, prim)
            bd2 = _bone_entries(v2, prim)

            vi0 = _add_vert(pos0, bd0, i0)
            vi1 = _add_vert(pos1, bd1, i1)
            vi2 = _add_vert(pos2, bd2, i2)

            # Skip degenerate triangles produced by vertex merging.
            if vi0 == vi1 or vi1 == vi2 or vi0 == vi2:
                continue

            fm.faces.append((vi0, vi1, vi2))
            fm.face_material_ids.append(prim_mat_idx)

            # UV V-flip: source has V=0 at top, target DCCs use V=0 at bottom.
            fm.face_uvs0.extend([
                (float(v0.uv0[0]), 1.0 - float(v0.uv0[1])),
                (float(v1.uv0[0]), 1.0 - float(v1.uv0[1])),
                (float(v2.uv0[0]), 1.0 - float(v2.uv0[1])),
            ])
            fm.face_uvs1.extend([
                (float(v0.uv1[0]), 1.0 - float(v0.uv1[1])),
                (float(v1.uv1[0]), 1.0 - float(v1.uv1[1])),
                (float(v2.uv1[0]), 1.0 - float(v2.uv1[1])),
            ])
            fm.face_normals.extend([
                coords.render_space(_normal_tuple(v0.normal)),
                coords.render_space(_normal_tuple(v1.normal)),
                coords.render_space(_normal_tuple(v2.normal)),
            ])

    if not fm.vertices or not fm.faces:
        return None

    if fm.face_normals and len(fm.face_normals) == len(fm.faces) * 3:
        fm.smoothing_groups = compute_smoothing_groups(fm.faces, fm.face_normals)
    else:
        fm.smoothing_groups = [1] * len(fm.faces)

    fm.material_id_set = list(mat_id_set.keys())
    return fm


def _normal_tuple(n) -> Vec3:
    return (float(n[0]), float(n[1]), float(n[2]))
