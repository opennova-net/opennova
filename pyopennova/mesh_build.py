"""DCC-agnostic mesh flattener.

Walks a ``Threedi3di3`` LOD and emits one ``FlatMesh`` per part with
plain Python data: vertex positions, triangle indices, per-face material
ids, per-corner UVs and normals, and smoothing-group bitmasks. The
result feeds either Blender (``mesh_data.from_pydata``) or 3ds Max
(``Editable_Mesh`` via PyMXS) without touching either DCC.

Mirrors the per-part grouping, index winding, skin-weight preservation,
and empty-subobject behaviour used by
``apps.importer.scene_builder.meshes`` (`_create_meshes_for_lod`) so DCC
importers can share one topology builder.
"""
from __future__ import annotations

from collections import OrderedDict, defaultdict
from dataclasses import dataclass, field
from typing import Sequence, Tuple

from pyopennova import coords
from pyopennova.mesh_utils import compute_smoothing_groups
from pyopennova.threedi_ffi import THREEDI_MESH_SKINNED

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
    face_material_ids: list[int] = field(default_factory=list)  # global TDP material index
    smoothing_groups: list[int] = field(default_factory=list)   # 1 entry per face
    material_id_set: list[int] = field(default_factory=list)    # ordered unique material ids
    vertex_bone_data: list[list[tuple[int, float]]] = field(default_factory=list)


def flatten_lod(
    model,
    lod_index: int,
    *,
    include_empty_parts: bool = False,
    track_bone_data: bool = False,
    preserve_source_indexing: bool = False,
) -> list[FlatMesh]:
    """Return one ``FlatMesh`` per part with geometry.

    The model is the ``Threedi3di3`` ctypes structure produced by
    ``pyopennova.threedi_ffi.read_model_3di3``.

    ``include_empty_parts`` adds empty meshes for parts with no usable
    geometry, preserving the subobject count expected by the original tools.
    ``track_bone_data`` populates ``FlatMesh.vertex_bone_data`` for Max/Blender
    skin binding.
    ``preserve_source_indexing`` keys vertex dedup on the source vertex index
    rather than position. Required for the collision LOD: OED's
    ``WriteCDTA @ 0x456050`` accumulates ``lod->subobjects[i].vertCount``
    verbatim, and stock 3DI files often store face-split collision verts
    (e.g. 24 = 12 unique * 2) that position-only dedup would collapse to 12,
    breaking ``CDTA/CMDL.num_vertices`` parity.
    """
    lod = model.lods[lod_index]
    num_parts = int(lod.render_object_count)
    num_strips = int(lod.strip_count)
    vertex_count = int(lod.vertices.count)
    is_skinned = int(model.header.mesh_type) == THREEDI_MESH_SKINNED

    part_strips: dict[int, list[int]] = defaultdict(list)
    strip_part_indices = _primitive_part_indices(lod)
    for strip_idx in range(num_strips):
        part_idx = strip_part_indices[strip_idx] if strip_idx < len(strip_part_indices) else -1
        if 0 <= part_idx < num_parts:
            part_strips[part_idx].append(strip_idx)

    out: list[FlatMesh] = []
    part_indices = range(num_parts) if include_empty_parts else sorted(part_strips.keys())
    for part_idx in part_indices:
        strip_indices = part_strips.get(part_idx, [])
        if not strip_indices and include_empty_parts:
            out.append(FlatMesh(name=f"{part_idx + 1:02d} Mesh0", part_index=part_idx))
            continue

        part = lod.render_objects[part_idx]
        part_abs = (
            float(part.abs[0]),
            float(part.abs[1]),
            float(part.abs[2]),
        )

        fm = _flatten_part(
            lod=lod,
            part_idx=part_idx,
            part_abs=part_abs,
            strip_indices=strip_indices,
            vertex_count=vertex_count,
            is_skinned=is_skinned,
            track_bone_data=track_bone_data,
            preserve_source_indexing=preserve_source_indexing,
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
    strip_indices: Sequence[int],
    vertex_count: int,
    is_skinned: bool,
    track_bone_data: bool,
    preserve_source_indexing: bool = False,
) -> FlatMesh | None:
    """Build a FlatMesh for a single part (all strips merged)."""
    fm = FlatMesh(name=f"{part_idx + 1:02d} Mesh0", part_index=part_idx)

    vert_map: dict[tuple, int] = {}
    mat_id_set: "OrderedDict[int, None]" = OrderedDict()

    def _add_vert(pos: Vec3, bone_entries: list[tuple[int, float]], source_index: int) -> int:
        if is_skinned or preserve_source_indexing:
            # Preserve original source vertex indexing for skinned meshes
            # (US01-style CDTA/COBJ parity) and for the collision LOD
            # (face-split verts that position-only dedup would collapse,
            # breaking CDTA/CMDL.num_vertices vs the stock 3DI).
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

    def _bone_entries(v, strip) -> list[tuple[int, float]]:
        if not is_skinned:
            return [(part_idx, 1.0)]

        # The authored 3 weights are unnormalized; normalize to sum 1.0 (a
        # zero sum pins the vertex fully to its first influence) so the DCC
        # skin weights and the dedup key see the same values as before.
        entries: list[tuple[int, float]] = []
        table_len = int(strip.bone_table_length)
        weight_sum = sum(float(w) for w in v.bone_weights)
        for bi in range(len(v.bone_weights)):
            weight = float(v.bone_weights[bi])
            if weight_sum > 0.0:
                weight /= weight_sum
            elif bi == 0:
                weight = 1.0
            else:
                weight = 0.0
            if weight <= 0.0:
                continue
            local_idx = int(v.bone_indices[bi])
            if table_len > 0 and local_idx < table_len:
                skel_idx = int(strip.bone_table[local_idx])
            else:
                skel_idx = 0
            entries.append((skel_idx, weight))
        if not entries:
            entries.append((part_idx, 1.0))
        merged: dict[int, float] = {}
        for bone_idx, weight in entries:
            merged[bone_idx] = merged.get(bone_idx, 0.0) + weight
        return sorted(merged.items(), key=lambda item: item[0])[:4]

    for strip_idx in strip_indices:
        strip = lod.strips[strip_idx]
        idx_offset = int(strip.index_offset)
        idx_count = int(strip.num_indices)
        vert_offset = int(strip.start_vertex)
        strip_mat_idx = int(strip.material_index)

        if idx_count < 3:
            continue

        if strip_mat_idx not in mat_id_set:
            mat_id_set[strip_mat_idx] = None

        for j in range(0, idx_count, 3):
            # Index winding swap (0, 2, 1) matches the Blender importer.
            i0 = int(lod.indices.indices[idx_offset + j + 0]) + vert_offset
            i1 = int(lod.indices.indices[idx_offset + j + 2]) + vert_offset
            i2 = int(lod.indices.indices[idx_offset + j + 1]) + vert_offset

            if i0 >= vertex_count or i1 >= vertex_count or i2 >= vertex_count:
                continue

            v0 = lod.vertices.items[i0]
            v1 = lod.vertices.items[i1]
            v2 = lod.vertices.items[i2]

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

            bd0 = _bone_entries(v0, strip)
            bd1 = _bone_entries(v1, strip)
            bd2 = _bone_entries(v2, strip)

            vi0 = _add_vert(pos0, bd0, i0)
            vi1 = _add_vert(pos1, bd1, i1)
            vi2 = _add_vert(pos2, bd2, i2)

            # Skip degenerate triangles produced by vertex merging.
            if vi0 == vi1 or vi1 == vi2 or vi0 == vi2:
                continue

            fm.faces.append((vi0, vi1, vi2))
            fm.face_material_ids.append(strip_mat_idx)

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


def flatten_collision(model) -> list[FlatMesh]:
    """Build per-subobject FlatMesh from ``model.collision[0]`` for the collision LOD.

    OED's WriteCDTA / WriteCVRT / WriteCFAC / WriteCOBJ pipeline is pure
    pass-through from ``lod->subobjects[i].verts/faces``, so the .ase mesh
    layout we hand it becomes CDTA verbatim. The stock 3DI's collision chunk already encodes
    the artist's exact subobject layout, so emitting one ``.ase`` GEOMOBJECT
    per ``coll.objects[k]`` (= one OED subobject per collision object)
    reproduces stock CMDL.subObjCount, COBJ.vertCount/faceCount/normalCount,
    and CFAC vert_index byte-exactly.

    Mapping (1:1): ``model.collision[0].objects[k]`` -> output subobject ``k``.
      - vertCount = ``objects[k].num_vertices``, drawn from
        ``coll.vertices[v_cum .. v_cum + num_vertices)`` where ``v_cum`` is
        the cumulative sum of prior objects' ``num_vertices``.
      - face triplets = ``coll.faces[f_cum .. f_cum + num_faces)``;
        ``vert_index`` values are already subobject-local (per
        ConvertToInternal's ``srcVertCount + srcObj->faces[k].v[0]`` reset
        per subobject), so use them as-is.

    The ``parent_subobject_index`` field on ``coll.objects[k]`` describes a
    hierarchy linkage between subobjects, NOT a grouping that affects
    CDTA layout. All observed stock fixtures have it pointing at parents
    in [0, k); irrelevant for WriteCDTA's pass-through walk.

    Vertices are returned with the OED inverse-coord-swizzle pre-applied so
    that when OED's ``Ase_ParseNode @ 0x42d180:425`` reads ``.ase``
    MESH_VERTEX values and applies its hardcoded swizzle
    ``ASE [x,y,z] -> OED [-y, x, z]``, the resulting ``pos.{x,y,z}`` matches
    ``coll.vertices[i].position`` exactly. Inverse swizzle: emit
    ``(pos.y, -pos.x, pos.z)`` per vertex.

    The caller's ``_lod_mesh_objects`` adds the part's
    ``coords.render_space(part.abs)`` to each vert; that's identity
    when ``part.abs == (0,0,0)`` (all observed stock fixtures so far).
    """
    coll_ptr = model.collision
    if not coll_ptr:
        return []
    coll = coll_ptr[0]
    obj_count = int(coll.object_count)
    if obj_count == 0:
        return []

    out: list[FlatMesh] = []
    v_cum = 0
    f_cum = 0
    for k in range(obj_count):
        obj = coll.objects[k]
        num_v = int(obj.num_vertices)
        num_f = int(obj.num_faces)

        fm = FlatMesh(name=f"{k + 1:02d} Mesh0", part_index=k)
        for vi in range(v_cum, v_cum + num_v):
            cp = coll.vertices[vi].position
            # Inverse of OED's ASE-parse swizzle [x,y,z] -> [-y,x,z]:
            # emit (cp.y, -cp.x, cp.z) so OED stores pos = (cp.x, cp.y, cp.z).
            fm.vertices.append((float(cp[1]), -float(cp[0]), float(cp[2])))
        for fi in range(f_cum, f_cum + num_f):
            f = coll.faces[fi]
            fm.faces.append((int(f.vert_index[0]),
                             int(f.vert_index[1]),
                             int(f.vert_index[2])))
            fm.face_material_ids.append(0)
        if fm.faces:
            fm.material_id_set = [0]
            fm.smoothing_groups = [1] * len(fm.faces)
        out.append(fm)

        v_cum += num_v
        f_cum += num_f
    return out


def primitive_part_indices(lod) -> list[int]:
    """Map each 3DI3 strip index to its owning render object."""
    return _primitive_part_indices(lod)


def _primitive_part_indices(lod) -> list[int]:
    """Map each 3DI3 strip index to its owning render object.

    Strips are stored sequentially per render object: ``num_strips`` opaque
    strips followed by ``num_alpha_strips`` alpha strips, so a running cursor
    over the render objects assigns each strip its owner.
    """
    count = int(lod.strip_count)
    out = [-1] * count
    cursor = 0
    parts = lod.render_objects
    for part_idx in range(int(lod.render_object_count)):
        part = parts[part_idx]
        strip_count = int(part.num_strips) + int(part.num_alpha_strips)
        for _ in range(max(0, strip_count)):
            if cursor >= count:
                return out
            out[cursor] = part_idx
            cursor += 1
    return out
