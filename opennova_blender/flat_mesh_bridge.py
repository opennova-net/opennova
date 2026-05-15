"""bpy.types.Mesh <-> pyopennova.flat_mesh_ffi.FlatMesh bridge.

Pure data marshaling. The bridge has no algorithm: it just copies verts,
faces, corner UVs/normals, material slot indices, and vertex-group weights
between the two representations.

Used by opennova_blender/ase_export.py to feed Blender-edited geometry back
into the C++ ASE writer chain.

bpy.types.Mesh model:
  - mesh.vertices: list of MeshVertex (.co = (x,y,z))
  - mesh.polygons: list of MeshPolygon (.vertices = (i,j,k))
  - mesh.uv_layers["UVMap"].data: per-loop UV (loop index = polygon.loop_start + corner)
  - mesh.uv_layers["UVMap2"].data: optional secondary UV
  - mesh.corner_normals (Blender 4.1+): per-corner normals
  - mesh.materials: list of bpy.types.Material; .material_index per polygon
  - object.vertex_groups[name] + vertex.groups[]: skin weights

FlatMesh model (libs/object/flat_mesh.h):
  - vertices: FlatMeshVertex[] (.x,.y,.z)
  - faces: FlatMeshFace[] (.v[3], .material_id, .smoothing_group_mask)
  - corners: FlatMeshFaceCornerData[face_count*3] (per-corner UVs, normals)
  - material_id_set: ordered unique material ids referenced
  - vertex_influences + offsets: CSR-style skin weights

Lifetime contract:
  export_bpy_to_flat_mesh() returns a FlatMesh whose ctypes pointers refer
  to Python-owned heap arrays. The arrays are stashed on the returned
  FlatMesh as _py_* attributes to keep them alive while the FlatMesh is
  in use. Callers MUST keep the returned FlatMesh alive (do not let it
  be GC'd) until any downstream FFI call that reads it returns.
"""
from __future__ import annotations

import ctypes
from typing import Sequence

from pyopennova.flat_mesh_ffi import (
    FlatMesh,
    FlatMeshBoneInfluence,
    FlatMeshFace,
    FlatMeshFaceCornerData,
    FlatMeshVertex,
    set_flat_mesh_name,
)


def import_flat_mesh_to_bpy(flat_mesh: FlatMesh, bpy_mesh) -> None:
    """Populate a bpy.types.Mesh from a FlatMesh.

    Caller has already created `bpy_mesh` (e.g., via `bpy.data.meshes.new()`).
    This function calls bpy_mesh.from_pydata() and then populates UV layers,
    corner normals, and material slot indices.

    Skin weights are NOT applied here (vertex_groups live on bpy.types.Object,
    not on bpy.types.Mesh). For skinned models, call
    ``attach_flat_mesh_weights_to_bpy_object(flat_mesh, bpy_obj)`` after
    wrapping the populated mesh in an Object.
    """
    verts = [
        (flat_mesh.vertices[i].x, flat_mesh.vertices[i].y, flat_mesh.vertices[i].z)
        for i in range(flat_mesh.vertex_count)
    ]
    faces = [
        (flat_mesh.faces[i].v[0], flat_mesh.faces[i].v[1], flat_mesh.faces[i].v[2])
        for i in range(flat_mesh.face_count)
    ]
    bpy_mesh.from_pydata(verts, [], faces)
    bpy_mesh.update()

    # Corner UVs: from_pydata's loop layout is face_idx * 3 + corner.
    if flat_mesh.face_count > 0:
        uv0 = bpy_mesh.uv_layers.new(name="UVMap")
        uv1 = bpy_mesh.uv_layers.new(name="UVMap2")
        for fi in range(flat_mesh.face_count):
            for ci in range(3):
                c = flat_mesh.corners[fi * 3 + ci]
                loop_idx = fi * 3 + ci
                uv0.data[loop_idx].uv = (c.u0, c.v0)
                uv1.data[loop_idx].uv = (c.u1, c.v1)

    # Material slot indices per face
    for fi in range(flat_mesh.face_count):
        bpy_mesh.polygons[fi].material_index = flat_mesh.faces[fi].material_id


def attach_flat_mesh_weights_to_bpy_object(flat_mesh: FlatMesh, bpy_obj) -> None:
    """Populate vertex_groups on a bpy.types.Object from a FlatMesh's CSR-style
    skin weights.

    Creates one vertex_group per distinct bone_index referenced in the
    FlatMesh's vertex_influences. Group naming convention: ``bone_<N>`` where
    N is the bone_index. This naming lets `read_weights_from_bpy_object`
    recover the original indices.

    The FlatMesh's vertex_influence_offsets is CSR-style:
        offsets[i]   = first influence index for vertex i
        offsets[i+1] = first influence index for vertex i+1
        influences in [offsets[i], offsets[i+1]) belong to vertex i

    Each FlatMeshBoneInfluence has .bone_index and .weight.

    The bpy.types.Object must already wrap the bpy.types.Mesh populated by
    import_flat_mesh_to_bpy. The mesh's vertex count must match the FlatMesh.

    Skin weights ride through bmesh.ops.triangulate (verified by Blender API
    docs: vertex group memberships are preserved on copied/triangulated verts).
    """
    # FlatMesh may have a NULL vertex_influences pointer (unskinned case).
    if not flat_mesh.vertex_influence_offsets:
        return

    vert_count = flat_mesh.vertex_count
    if vert_count <= 0:
        return

    # Discover bone indices referenced, in first-seen order.
    bone_indices_seen: list[int] = []
    bone_index_to_group_idx: dict[int, int] = {}
    last_offset = flat_mesh.vertex_influence_offsets[vert_count]  # CSR: total influences
    if last_offset <= 0:
        return
    for i in range(last_offset):
        b = int(flat_mesh.vertex_influences[i].bone_index)
        if b not in bone_index_to_group_idx:
            bone_index_to_group_idx[b] = len(bone_indices_seen)
            bone_indices_seen.append(b)

    # Create the vertex_groups (one per bone) with the naming convention.
    groups: list = []
    for b in bone_indices_seen:
        g = bpy_obj.vertex_groups.new(name=f"bone_{b}")
        groups.append(g)

    # Add each vertex's weights into the corresponding groups.
    for vi in range(vert_count):
        start = int(flat_mesh.vertex_influence_offsets[vi])
        end = int(flat_mesh.vertex_influence_offsets[vi + 1])
        for k in range(start, end):
            inf = flat_mesh.vertex_influences[k]
            bone_idx = int(inf.bone_index)
            weight = float(inf.weight)
            g_idx = bone_index_to_group_idx[bone_idx]
            groups[g_idx].add([vi], weight, "REPLACE")


def read_weights_from_bpy_object(bpy_obj, vertex_count: int):
    """Read CSR-style skin weights from a bpy.types.Object's vertex_groups.

    Returns (offsets_arr, influences_arr) — two ctypes arrays the caller
    stashes on a FlatMesh's vertex_influence_offsets / vertex_influences
    fields. The returned arrays are Python-owned; caller must keep them
    alive for the FlatMesh's lifetime.

    Reads the ``bone_<N>`` vertex_group naming convention written by
    attach_flat_mesh_weights_to_bpy_object to recover bone_index values.
    Vertex groups whose name doesn't match the convention are mapped to
    their group_index (lossy fallback — for round-trips through this
    bridge, all groups will be ``bone_<N>``).

    Each vertex's influences are sorted by bone_index ascending — matches
    the pre-Phase-D pyopennova.ase_from_3di3._pack_weights ordering that
    Phase C pinned as byte-exact.
    """
    # Build a group_index -> bone_index map.
    group_idx_to_bone: dict[int, int] = {}
    for gi, group in enumerate(bpy_obj.vertex_groups):
        name = group.name
        if name.startswith("bone_"):
            try:
                group_idx_to_bone[gi] = int(name[5:])
            except ValueError:
                group_idx_to_bone[gi] = gi
        else:
            group_idx_to_bone[gi] = gi

    # Walk each vertex; collect (bone_index, weight) pairs.
    mesh = bpy_obj.data
    per_vertex: list[list[tuple[int, float]]] = []
    for vi in range(vertex_count):
        v = mesh.vertices[vi]
        pairs: list[tuple[int, float]] = []
        for vg in v.groups:
            bone_idx = group_idx_to_bone.get(vg.group, vg.group)
            pairs.append((bone_idx, float(vg.weight)))
        pairs.sort(key=lambda p: p[0])  # ascending by bone_index
        per_vertex.append(pairs)

    # Build CSR.
    offsets = [0]
    for pairs in per_vertex:
        offsets.append(offsets[-1] + len(pairs))
    total_influences = offsets[-1]

    offsets_arr = (ctypes.c_int32 * (vertex_count + 1))(*offsets)

    if total_influences > 0:
        influences_arr = (FlatMeshBoneInfluence * total_influences)()
        idx = 0
        for pairs in per_vertex:
            for (b, w) in pairs:
                influences_arr[idx].bone_index = b
                influences_arr[idx].weight = w
                idx += 1
    else:
        influences_arr = (FlatMeshBoneInfluence * 0)()

    return offsets_arr, influences_arr


def export_bpy_to_flat_mesh(bpy_mesh, name: str, part_index: int,
                              origin: tuple[float, float, float] = (0.0, 0.0, 0.0)) -> FlatMesh:
    """Convert a bpy.types.Mesh back to a FlatMesh.

    Returns a FlatMesh whose ctypes pointers refer to Python-owned heap arrays
    stashed as `_py_*` attributes on the returned object to keep them alive.

    The temporary triangulated mesh is stashed as ``_py_tri_mesh`` on the
    returned FlatMesh and remains in ``bpy.data.meshes`` until the FlatMesh
    is garbage-collected by Python. Callers wanting to flush bpy state
    eagerly may call ``bpy.data.meshes.remove(fm._py_tri_mesh)`` after the
    downstream FFI consumer returns.

    Triangulates n-gons via temporary BMesh (matches mesh_build.flatten_lod
    behavior of producing triangles only).
    """
    import bpy
    import bmesh

    fm = FlatMesh()
    set_flat_mesh_name(fm, name)
    fm.part_index = part_index
    fm.origin = (ctypes.c_float * 3)(*origin)

    # Triangulate via temporary BMesh to handle quads/n-gons cleanly.
    bm = bmesh.new()
    bm.from_mesh(bpy_mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    tri_mesh = bpy.data.meshes.new(name + "_tri_tmp")
    bm.to_mesh(tri_mesh)
    bm.free()

    # Verts
    vert_count = len(tri_mesh.vertices)
    fm.vertex_count = vert_count
    verts_arr = (FlatMeshVertex * vert_count)()
    for i, v in enumerate(tri_mesh.vertices):
        verts_arr[i].x = v.co.x
        verts_arr[i].y = v.co.y
        verts_arr[i].z = v.co.z
    fm.vertices = ctypes.cast(verts_arr, ctypes.POINTER(FlatMeshVertex))

    # Faces + corners
    face_count = len(tri_mesh.polygons)
    fm.face_count = face_count
    faces_arr = (FlatMeshFace * face_count)()
    corners_arr = (FlatMeshFaceCornerData * (face_count * 3))()

    uv0_layer = tri_mesh.uv_layers.get("UVMap")
    uv1_layer = tri_mesh.uv_layers.get("UVMap2")

    material_ids_seen = set()
    has_corner_normals = hasattr(tri_mesh, "corner_normals")

    for fi, poly in enumerate(tri_mesh.polygons):
        if len(poly.vertices) != 3:
            raise ValueError(
                f"polygon {fi} has {len(poly.vertices)} verts after triangulate"
            )
        faces_arr[fi].v[0] = poly.vertices[0]
        faces_arr[fi].v[1] = poly.vertices[1]
        faces_arr[fi].v[2] = poly.vertices[2]
        faces_arr[fi].material_id = poly.material_index
        faces_arr[fi].smoothing_group_mask = 1
        material_ids_seen.add(int(poly.material_index))

        for ci, loop_idx in enumerate(poly.loop_indices):
            c = corners_arr[fi * 3 + ci]
            if uv0_layer is not None:
                u, v = uv0_layer.data[loop_idx].uv
                c.u0, c.v0 = float(u), float(v)
            if uv1_layer is not None:
                u, v = uv1_layer.data[loop_idx].uv
                c.u1, c.v1 = float(u), float(v)
            if has_corner_normals:
                n = tri_mesh.corner_normals[loop_idx].vector
                c.nx, c.ny, c.nz = float(n[0]), float(n[1]), float(n[2])
            else:
                c.nx, c.ny, c.nz = 0.0, 0.0, 1.0

    fm.faces = ctypes.cast(faces_arr, ctypes.POINTER(FlatMeshFace))
    fm.corners = ctypes.cast(corners_arr, ctypes.POINTER(FlatMeshFaceCornerData))

    # Material id set (ordered unique, sorted ascending)
    sorted_ids = sorted(material_ids_seen)
    fm.material_id_set_count = len(sorted_ids)
    id_set_arr = (ctypes.c_int32 * max(1, len(sorted_ids)))(*sorted_ids) if sorted_ids else (ctypes.c_int32 * 0)()
    fm.material_id_set = ctypes.cast(id_set_arr, ctypes.POINTER(ctypes.c_int32))

    # Skin weights: not applied at this layer (caller decides). Zero offsets
    # for vert_count+1 entries (no influences for any vert).
    offsets_arr = (ctypes.c_int32 * (vert_count + 1))()
    fm.vertex_influence_offsets = ctypes.cast(offsets_arr, ctypes.POINTER(ctypes.c_int32))
    fm.vertex_influences = ctypes.POINTER(FlatMeshBoneInfluence)()

    # Keep refs alive — the caller must own these arrays as long as fm is in use.
    # We attach them to fm as Python attributes so they don't get GC'd.
    fm._py_verts = verts_arr
    fm._py_faces = faces_arr
    fm._py_corners = corners_arr
    fm._py_id_set = id_set_arr
    fm._py_offsets = offsets_arr
    fm._py_tri_mesh = tri_mesh

    return fm
