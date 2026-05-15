"""3ds Max Editable_Mesh <-> pyopennova.flat_mesh_ffi.FlatMesh bridge.

Pure data marshaling. Symmetric to opennova_blender.flat_mesh_bridge but
uses pymxs (3ds Max's Python binding) for scene access.

Used by opennova_max/ase_export.py to feed Max-edited geometry back into
the C++ ASE writer chain.

Max mesh model (Editable_Mesh):
  - mesh.verts[i].pos: (x, y, z) per vertex (1-indexed in MAXScript)
  - mesh.faces[i].v: (v1, v2, v3) per face (1-indexed)
  - mesh.faces[i].matID: per-face material ID (1-indexed)
  - tvert[i]: texture vertex (UV) (1-indexed)
  - tvFace[i].v: texture-vertex indices per face corner
  - getNormal/setNormal for per-vertex normals
  - vertex_groups via Skin modifier (bones list + weights per vertex)

FlatMesh model (libs/object/flat_mesh.h):
  - vertices: FlatMeshVertex[] (.x, .y, .z) (0-indexed)
  - faces: FlatMeshFace[] (.v[3], .material_id, .smoothing_group_mask) (0-indexed)
  - corners: FlatMeshFaceCornerData[face_count*3] (per-corner UVs, normals)
  - material_id_set: ordered unique material ids referenced
  - vertex_influences + offsets: CSR-style skin weights

IMPORTANT: Max uses 1-indexed arrays (legacy MAXScript convention). When
ferrying indices to/from FlatMesh (which is 0-indexed), subtract/add 1
consistently.

Lifetime contract:
  export_max_to_flat_mesh() returns a FlatMesh whose ctypes pointers
  refer to Python-owned heap arrays stashed as _py_* attributes. Callers
  must keep the FlatMesh alive until any downstream FFI call returns.
"""
from __future__ import annotations

import ctypes
from typing import Any, Sequence

from pyopennova.flat_mesh_ffi import (
    FlatMesh,
    FlatMeshBoneInfluence,
    FlatMeshFace,
    FlatMeshFaceCornerData,
    FlatMeshVertex,
    set_flat_mesh_name,
)


def _rt():
    """Return ``pymxs.runtime``, raising a clear error outside Max."""
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover — exercised only inside Max
        raise RuntimeError(
            "pymxs is not available; opennova_max only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime


def import_flat_mesh_to_max(flat_mesh: FlatMesh, name: str = "") -> Any:
    """Create a new Editable_Mesh node populated from a FlatMesh.

    Returns the created Max node. Caller is responsible for parenting it
    or removing it from the scene (via rt.delete).

    The Editable_Mesh has:
      - vertices set from flat_mesh.vertices
      - faces set from flat_mesh.faces (1-indexed conversion)
      - matIDs set per face
      - tverts + tvFace set from corner UVs (channels 1 and 2)
      - per-vertex normals NOT set here (Max recomputes via faceted/smooth)
    """
    rt = _rt()

    vert_array = rt.Array(*[
        rt.Point3(flat_mesh.vertices[i].x,
                  flat_mesh.vertices[i].y,
                  flat_mesh.vertices[i].z)
        for i in range(flat_mesh.vertex_count)
    ])

    face_array = rt.Array(*[
        rt.Point3(flat_mesh.faces[i].v[0] + 1,
                  flat_mesh.faces[i].v[1] + 1,
                  flat_mesh.faces[i].v[2] + 1)
        for i in range(flat_mesh.face_count)
    ])

    obj = rt.mesh(vertices=vert_array, faces=face_array, name=name or "")

    for i in range(flat_mesh.face_count):
        rt.setFaceMatID(obj, i + 1, int(flat_mesh.faces[i].material_id) + 1)

    if flat_mesh.face_count > 0 and flat_mesh.corners:
        # UV channel 1.
        _ensure_map_channel(rt, obj, 1)
        rt.meshop.setNumMapVerts(obj, 1, flat_mesh.face_count * 3)
        rt.meshop.setNumMapFaces(obj, 1, flat_mesh.face_count)
        for fi in range(flat_mesh.face_count):
            for ci in range(3):
                c = flat_mesh.corners[fi * 3 + ci]
                tv_idx = fi * 3 + ci + 1
                rt.meshop.setMapVert(obj, 1, tv_idx, rt.Point3(c.u0, c.v0, 0.0))
            rt.meshop.setMapFace(obj, 1, fi + 1,
                                  rt.Point3(fi*3+1, fi*3+2, fi*3+3))

        # UV channel 2.
        _ensure_map_channel(rt, obj, 2)
        rt.meshop.setNumMapVerts(obj, 2, flat_mesh.face_count * 3)
        rt.meshop.setNumMapFaces(obj, 2, flat_mesh.face_count)
        for fi in range(flat_mesh.face_count):
            for ci in range(3):
                c = flat_mesh.corners[fi * 3 + ci]
                tv_idx = fi * 3 + ci + 1
                rt.meshop.setMapVert(obj, 2, tv_idx, rt.Point3(c.u1, c.v1, 0.0))
            rt.meshop.setMapFace(obj, 2, fi + 1,
                                  rt.Point3(fi*3+1, fi*3+2, fi*3+3))

    rt.update(obj)
    return obj


def _ensure_map_channel(rt, obj, channel: int) -> None:
    """Enable a Max mesh map channel before assigning map verts/faces."""
    if int(rt.meshop.getNumMaps(obj)) <= channel:
        rt.meshop.setNumMaps(obj, channel + 1, keep=True)
    rt.meshop.setMapSupport(obj, channel, True)


def attach_flat_mesh_weights_to_max_object(flat_mesh: FlatMesh, max_obj,
                                              bone_nodes: Sequence[Any]) -> Any:
    """Attach a Skin modifier to ``max_obj`` and populate it from a FlatMesh's
    CSR-style skin weights.

    Mirrors ``opennova_blender.flat_mesh_bridge.attach_flat_mesh_weights_to_bpy_object``
    (see that function for the CSR contract). The Blender side stores weights
    in vertex_groups; in Max the equivalent is a Skin modifier with one bone
    per distinct ``bone_index`` referenced by the FlatMesh.

    ``bone_nodes`` is a sequence of persistent Max scene nodes (typically
    ``rt.Dummy()`` or actual bones) the caller owns. We index into it using
    the FlatMesh's ``bone_index`` values. The list must be long enough to
    cover ``max(bone_index) + 1`` of any influence in the FlatMesh.

    CSR layout (libs/object/flat_mesh.h, see flat_mesh_ffi.py):
        vertex_influence_offsets[i]   = first influence index for vertex i
        vertex_influence_offsets[i+1] = first influence index for vertex i+1
        Influences in [offsets[i], offsets[i+1]) belong to vertex i.

    Returns the Skin modifier, or None if the FlatMesh has no skin data
    (NULL offsets pointer or zero total influences).

    pymxs quirks worked around here:
      - skinOps.addBone must come AFTER rt.addModifier(obj, skin); the
        modifier's bones array is empty until attached.
      - The object must be selected before skinOps calls in some Max
        versions; we do rt.select(obj) defensively.
      - bone_index is 0-indexed in the FlatMesh; Max's skinOps bone IDs are
        1-indexed (matching its position in the addBone insertion order).
      - skinOps.ReplaceVertexWeights expects ``Array`` (MAXScript) objects
        for both bone IDs and weights, not plain Python lists.
    """
    rt = _rt()

    # FlatMesh may have a NULL vertex_influences pointer (unskinned case).
    if not flat_mesh.vertex_influence_offsets:
        return None

    vert_count = int(flat_mesh.vertex_count)
    if vert_count <= 0:
        return None

    # CSR: offsets[vert_count] is the total influence count.
    total_influences = int(flat_mesh.vertex_influence_offsets[vert_count])
    if total_influences <= 0:
        return None

    # Discover distinct bone indices referenced, in first-seen order. This
    # mirrors the Blender side (line 116-126 of flat_mesh_bridge.py) which
    # keys vertex_groups by the same first-seen ordering.
    bone_indices_seen: list[int] = []
    bone_index_to_max_slot: dict[int, int] = {}
    for i in range(total_influences):
        b = int(flat_mesh.vertex_influences[i].bone_index)
        if b not in bone_index_to_max_slot:
            bone_index_to_max_slot[b] = len(bone_indices_seen)  # 0-indexed slot
            bone_indices_seen.append(b)

    # Verify bone_nodes is long enough for the sparse FlatMesh bone indices.
    required_bone_count = max(bone_indices_seen) + 1
    if len(bone_nodes) < required_bone_count:
        raise ValueError(
            f"bone_nodes has {len(bone_nodes)} entries, but FlatMesh references "
            f"{len(bone_indices_seen)} distinct bone indices (max needed: "
            f"{max(bone_indices_seen)}). Caller must provide one persistent "
            "scene node per referenced bone slot."
        )

    # Attach Skin modifier. Select the object first; some Max versions
    # require it for skinOps calls to land on the right modifier.
    rt.select(max_obj)
    skin = rt.Skin()
    rt.addModifier(max_obj, skin)

    # Register one bone per distinct bone_index, in first-seen order. The
    # `update` flag is 0 for all but the last call (batch behavior); the
    # final call passes 1 to trigger a single rebuild.
    for slot_idx, b in enumerate(bone_indices_seen):
        node = bone_nodes[b]
        update_flag = 1 if slot_idx == len(bone_indices_seen) - 1 else 0
        rt.skinOps.addBone(skin, node, update_flag)

    # Set weights per vertex. FlatMesh is 0-indexed; Max is 1-indexed for
    # both vertex slots and bone IDs.
    for vi in range(vert_count):
        start = int(flat_mesh.vertex_influence_offsets[vi])
        end = int(flat_mesh.vertex_influence_offsets[vi + 1])
        if start == end:
            continue
        bone_ids: list[int] = []
        weights: list[float] = []
        for k in range(start, end):
            inf = flat_mesh.vertex_influences[k]
            slot = bone_index_to_max_slot[int(inf.bone_index)]
            bone_ids.append(slot + 1)  # 1-indexed
            weights.append(float(inf.weight))
        bone_ids_arr = rt.Array(*bone_ids)
        weights_arr = rt.Array(*weights)
        rt.skinOps.ReplaceVertexWeights(skin, vi + 1, bone_ids_arr, weights_arr)

    # Some Max versions need skinOps.bake before GetVertexWeight returns
    # non-zero. Wrap in try/except since the function isn't on every
    # version of skinOps.
    try:
        rt.skinOps.bake(skin)
    except BaseException:
        pass

    # Stash the bone-index map via Max's user-property store so the
    # symmetric read function can recover the original FlatMesh bone
    # indices (otherwise they'd come back as 1-indexed Max slot IDs,
    # losing the original numbering). Encoded as a comma-separated list
    # since setUserProp only takes simple value types.
    rt.setUserProp(
        max_obj,
        "opennova_bone_slot_to_fm_index",
        ",".join(str(b) for b in bone_indices_seen),
    )

    return skin


def read_weights_from_max_object(max_obj, vertex_count: int):
    """Read CSR-style skin weights from a Max object's Skin modifier.

    Returns ``(offsets_arr, influences_arr)`` as ctypes arrays the caller
    can stash on a FlatMesh's vertex_influence_offsets / vertex_influences
    fields. The returned arrays are Python-owned; caller must keep them
    alive for the FlatMesh's lifetime.

    Mirrors ``opennova_blender.flat_mesh_bridge.read_weights_from_bpy_object``.
    Each vertex's influences are sorted by bone_index ascending to match
    the pre-Phase-D pyopennova.ase_from_3di3._pack_weights ordering pinned
    byte-exact in Phase C.

    Bone-index recovery: if the symmetric ``attach_flat_mesh_weights_to_max_object``
    was called earlier on ``max_obj``, it stashes
    ``_py_bone_slot_to_fm_index`` mapping the 1-indexed Max bone slot
    back to the original FlatMesh bone_index. If that attribute is
    absent (e.g., artist edited the Skin in Max from scratch), we fall
    back to the 0-indexed Max slot ID (1 -> 0, 2 -> 1, etc.).

    Returns empty influences (offsets all zero) if max_obj has no Skin
    modifier.
    """
    rt = _rt()

    # Find the Skin modifier on the object (there may be other modifiers
    # in the stack; we want the first Skin we find).
    skin = None
    modifiers = getattr(max_obj, "modifiers", None)
    if modifiers is None:
        modifiers = []
        count = int(rt.getModifierCount(max_obj))
        for mi in range(count):
            modifiers.append(rt.getModifier(max_obj, mi + 1))
    for mod in modifiers:
        if rt.classOf(mod) == rt.Skin:
            skin = mod
            break

    if skin is None:
        # No skinning -> empty CSR.
        offsets_arr = (ctypes.c_int32 * (vertex_count + 1))()
        influences_arr = (FlatMeshBoneInfluence * 0)()
        return offsets_arr, influences_arr

    # Most skinOps calls require the object to be selected and the Skin
    # modifier to be the current modifier in the stack.
    rt.select(max_obj)
    try:
        rt.modPanel.setCurrentObject(skin)
    except Exception:
        # modPanel doesn't exist when Max runs headless (3dsmaxbatch).
        # GetVertexWeight* still works without it on recent versions.
        pass

    # Recover slot -> FlatMesh bone_index mapping (stashed by the attach
    # function above via setUserProp). Fall back to 0-indexed Max slot ID
    # when absent (e.g., artist authored the Skin from scratch).
    slot_to_fm_index: list[int] | None = None
    try:
        encoded = rt.getUserProp(max_obj, "opennova_bone_slot_to_fm_index")
        if encoded:
            slot_to_fm_index = [int(s) for s in str(encoded).split(",") if s]
    except Exception:
        slot_to_fm_index = None

    def _slot_to_bone_index(max_slot_1based: int) -> int:
        slot_0based = max_slot_1based - 1
        if slot_to_fm_index is not None and 0 <= slot_0based < len(slot_to_fm_index):
            return int(slot_to_fm_index[slot_0based])
        return slot_0based

    # Walk each vertex; collect (bone_index, weight) pairs.
    per_vertex: list[list[tuple[int, float]]] = []
    for vi in range(vertex_count):
        n = int(rt.skinOps.GetVertexWeightCount(skin, vi + 1))
        pairs: list[tuple[int, float]] = []
        for wi in range(n):
            w = float(rt.skinOps.GetVertexWeight(skin, vi + 1, wi + 1))
            b_max = int(rt.skinOps.GetVertexWeightBoneID(skin, vi + 1, wi + 1))
            bone_idx = _slot_to_bone_index(b_max)
            pairs.append((bone_idx, w))
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


def export_max_to_flat_mesh(max_obj, name: str, part_index: int,
                              origin: tuple[float, float, float] = (0.0, 0.0, 0.0)) -> FlatMesh:
    """Convert a Max node's Editable_Mesh back to a FlatMesh.

    Returns a FlatMesh whose ctypes pointers refer to Python-owned heap
    arrays stashed as _py_* attributes to keep them alive.

    Assumes max_obj is a triangulated mesh. If max_obj is an Editable_Poly,
    snapshotAsMesh triangulates it.
    """
    rt = _rt()

    tri = rt.snapshotAsMesh(max_obj) if rt.classOf(max_obj) != rt.TriMesh else max_obj

    fm = FlatMesh()
    set_flat_mesh_name(fm, name)
    fm.part_index = part_index
    fm.origin = (ctypes.c_float * 3)(*origin)

    vert_count = int(rt.meshop.getNumVerts(tri))
    fm.vertex_count = vert_count
    verts_arr = (FlatMeshVertex * vert_count)()
    for i in range(vert_count):
        p = rt.meshop.getVert(tri, i + 1)
        verts_arr[i].x = float(p.x)
        verts_arr[i].y = float(p.y)
        verts_arr[i].z = float(p.z)
    fm.vertices = ctypes.cast(verts_arr, ctypes.POINTER(FlatMeshVertex))

    face_count = int(rt.meshop.getNumFaces(tri))
    fm.face_count = face_count
    faces_arr = (FlatMeshFace * face_count)()
    corners_arr = (FlatMeshFaceCornerData * (face_count * 3))()
    material_ids_seen: set[int] = set()

    has_uv0 = int(rt.meshop.getNumMaps(tri)) > 1
    has_uv1 = int(rt.meshop.getNumMaps(tri)) > 2

    for fi in range(face_count):
        face = rt.meshop.getFace(tri, fi + 1)
        faces_arr[fi].v[0] = int(face.x) - 1
        faces_arr[fi].v[1] = int(face.y) - 1
        faces_arr[fi].v[2] = int(face.z) - 1
        mat_id = int(rt.getFaceMatID(tri, fi + 1)) - 1
        faces_arr[fi].material_id = mat_id
        faces_arr[fi].smoothing_group_mask = int(rt.getFaceSmoothGroup(tri, fi + 1)) or 1
        material_ids_seen.add(mat_id)

        for ci in range(3):
            c = corners_arr[fi * 3 + ci]
            if has_uv0:
                tv_face = rt.meshop.getMapFace(tri, 1, fi + 1)
                tv_idx = int(getattr(tv_face, ['x', 'y', 'z'][ci]))
                if tv_idx > 0:
                    p = rt.meshop.getMapVert(tri, 1, tv_idx)
                    c.u0 = float(p.x); c.v0 = float(p.y)
            if has_uv1:
                tv_face = rt.meshop.getMapFace(tri, 2, fi + 1)
                tv_idx = int(getattr(tv_face, ['x', 'y', 'z'][ci]))
                if tv_idx > 0:
                    p = rt.meshop.getMapVert(tri, 2, tv_idx)
                    c.u1 = float(p.x); c.v1 = float(p.y)
            v_idx = [int(face.x), int(face.y), int(face.z)][ci]
            n = rt.getNormal(tri, v_idx)
            c.nx = float(n.x); c.ny = float(n.y); c.nz = float(n.z)

    fm.faces = ctypes.cast(faces_arr, ctypes.POINTER(FlatMeshFace))
    fm.corners = ctypes.cast(corners_arr, ctypes.POINTER(FlatMeshFaceCornerData))

    sorted_ids = sorted(material_ids_seen)
    fm.material_id_set_count = len(sorted_ids)
    id_set_arr = (ctypes.c_int32 * max(1, len(sorted_ids)))(*sorted_ids) if sorted_ids else (ctypes.c_int32 * 0)()
    fm.material_id_set = ctypes.cast(id_set_arr, ctypes.POINTER(ctypes.c_int32))

    offsets_arr = (ctypes.c_int32 * (vert_count + 1))()
    fm.vertex_influence_offsets = ctypes.cast(offsets_arr, ctypes.POINTER(ctypes.c_int32))
    fm.vertex_influences = ctypes.POINTER(FlatMeshBoneInfluence)()

    fm._py_verts = verts_arr
    fm._py_faces = faces_arr
    fm._py_corners = corners_arr
    fm._py_id_set = id_set_arr
    fm._py_offsets = offsets_arr
    fm._py_tri = tri

    return fm
