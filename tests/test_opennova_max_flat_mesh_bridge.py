"""Unit tests for opennova_max.flat_mesh_bridge.

Requires pymxs (3ds Max). Tests skip if pymxs isn't installed.
"""
from __future__ import annotations

import pytest

pymxs = pytest.importorskip("pymxs", reason="pymxs is not installed in this environment")


def test_round_trip_quad_preserves_verts_and_faces() -> None:
    """A simple quad goes through import_flat_mesh_to_max ->
    export_max_to_flat_mesh and emerges with the same vertices + faces.
    """
    import ctypes
    from pyopennova.flat_mesh_ffi import (
        FlatMesh,
        FlatMeshFace,
        FlatMeshFaceCornerData,
        FlatMeshVertex,
    )
    from opennova_max.flat_mesh_bridge import (
        export_max_to_flat_mesh,
        import_flat_mesh_to_max,
    )

    fm = FlatMesh()
    fm.vertex_count = 4
    verts = (FlatMeshVertex * 4)(
        FlatMeshVertex(0.0, 0.0, 0.0),
        FlatMeshVertex(1.0, 0.0, 0.0),
        FlatMeshVertex(1.0, 1.0, 0.0),
        FlatMeshVertex(0.0, 1.0, 0.0),
    )
    fm.vertices = ctypes.cast(verts, ctypes.POINTER(FlatMeshVertex))
    fm.face_count = 2
    faces = (FlatMeshFace * 2)(
        FlatMeshFace((0, 1, 2), 0, 1),
        FlatMeshFace((0, 2, 3), 0, 1),
    )
    fm.faces = ctypes.cast(faces, ctypes.POINTER(FlatMeshFace))
    corners = (FlatMeshFaceCornerData * 6)()
    fm.corners = ctypes.cast(corners, ctypes.POINTER(FlatMeshFaceCornerData))
    id_set = (ctypes.c_int32 * 1)(0)
    fm.material_id_set_count = 1
    fm.material_id_set = ctypes.cast(id_set, ctypes.POINTER(ctypes.c_int32))

    rt = pymxs.runtime
    max_obj = import_flat_mesh_to_max(fm, "test_quad")
    try:
        fm2 = export_max_to_flat_mesh(max_obj, "test_quad", part_index=0)
        assert bytes(fm2.name).rstrip(b"\x00") == b"test_quad"
        assert fm2.vertex_count == 4
        assert fm2.face_count == 2
        for i in range(4):
            assert fm2.vertices[i].x == pytest.approx(verts[i].x)
            assert fm2.vertices[i].y == pytest.approx(verts[i].y)
            assert fm2.vertices[i].z == pytest.approx(verts[i].z)
    finally:
        rt.delete(max_obj)


def test_round_trip_quad_with_skinning_preserves_weights() -> None:
    """A FlatMesh with skin weights goes through attach + read and emerges
    with the same (bone_index, weight) pairs per vertex, sorted ascending.

    Mirrors tests/test_opennova_blender_flat_mesh_bridge.py::
    test_weight_round_trip_preserves_bone_indices_and_weights, but uses
    Max's Skin modifier instead of bpy vertex_groups.
    """
    import ctypes
    from pyopennova.flat_mesh_ffi import (
        FlatMesh,
        FlatMeshBoneInfluence,
        FlatMeshFace,
        FlatMeshFaceCornerData,
        FlatMeshVertex,
    )
    from opennova_max.flat_mesh_bridge import (
        attach_flat_mesh_weights_to_max_object,
        import_flat_mesh_to_max,
        read_weights_from_max_object,
    )

    # 4 verts, 2 faces, each vertex has 2 bone influences (4 bones total).
    fm = FlatMesh()
    fm.vertex_count = 4
    verts = (FlatMeshVertex * 4)(
        FlatMeshVertex(0.0, 0.0, 0.0),
        FlatMeshVertex(1.0, 0.0, 0.0),
        FlatMeshVertex(1.0, 1.0, 0.0),
        FlatMeshVertex(0.0, 1.0, 0.0),
    )
    fm.vertices = ctypes.cast(verts, ctypes.POINTER(FlatMeshVertex))

    fm.face_count = 2
    faces = (FlatMeshFace * 2)(
        FlatMeshFace((0, 1, 2), 0, 1),
        FlatMeshFace((0, 2, 3), 0, 1),
    )
    fm.faces = ctypes.cast(faces, ctypes.POINTER(FlatMeshFace))

    corners = (FlatMeshFaceCornerData * 6)()
    fm.corners = ctypes.cast(corners, ctypes.POINTER(FlatMeshFaceCornerData))

    id_set = (ctypes.c_int32 * 1)(0)
    fm.material_id_set_count = 1
    fm.material_id_set = ctypes.cast(id_set, ctypes.POINTER(ctypes.c_int32))

    # CSR weights: 2 influences per vert = 8 total. Same payload as Blender's
    # weight round-trip test so we can compare results across DCCs.
    offsets = (ctypes.c_int32 * 5)(0, 2, 4, 6, 8)
    influences = (FlatMeshBoneInfluence * 8)(
        FlatMeshBoneInfluence(0, 0.6), FlatMeshBoneInfluence(3, 0.4),  # v0 -> bones 0,3
        FlatMeshBoneInfluence(1, 0.6), FlatMeshBoneInfluence(3, 0.4),  # v1 -> bones 1,3
        FlatMeshBoneInfluence(2, 0.6), FlatMeshBoneInfluence(3, 0.4),  # v2 -> bones 2,3
        FlatMeshBoneInfluence(0, 0.5), FlatMeshBoneInfluence(2, 0.5),  # v3 -> bones 0,2
    )
    fm.vertex_influence_offsets = ctypes.cast(offsets, ctypes.POINTER(ctypes.c_int32))
    fm.vertex_influences = ctypes.cast(influences, ctypes.POINTER(FlatMeshBoneInfluence))

    rt = pymxs.runtime

    # Create 4 persistent Dummy bone nodes; the Skin modifier holds refs
    # to these for the duration of the test.
    bone_nodes = []
    for bi in range(4):
        d = rt.Dummy()
        d.name = f"bone_{bi}"
        bone_nodes.append(d)

    max_obj = import_flat_mesh_to_max(fm, "test_skin_quad")
    try:
        attach_flat_mesh_weights_to_max_object(fm, max_obj, bone_nodes)

        offsets_arr, influences_arr = read_weights_from_max_object(max_obj, 4)

        assert list(offsets_arr) == [0, 2, 4, 6, 8]
        expected_pairs = [
            (0, 0.6), (3, 0.4),
            (1, 0.6), (3, 0.4),
            (2, 0.6), (3, 0.4),
            (0, 0.5), (2, 0.5),
        ]
        for i, (eb, ew) in enumerate(expected_pairs):
            assert influences_arr[i].bone_index == eb, (
                f"influence {i}: bone_index expected {eb}, got "
                f"{influences_arr[i].bone_index}"
            )
            assert influences_arr[i].weight == pytest.approx(ew, abs=1e-4), (
                f"influence {i}: weight expected {ew}, got "
                f"{influences_arr[i].weight}"
            )
    finally:
        rt.delete(max_obj)
        for d in bone_nodes:
            rt.delete(d)
