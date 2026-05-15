"""Unit tests for opennova_blender.flat_mesh_bridge.

Requires bpy. Tests skip if bpy isn't installed.
"""
from __future__ import annotations

import pytest

bpy = pytest.importorskip("bpy", reason="bpy is not installed in this environment")


def test_round_trip_quad_preserves_verts_and_faces() -> None:
    """A simple quad goes through import_flat_mesh_to_bpy ->
    export_bpy_to_flat_mesh and emerges with the same vertices + faces
    (the input is already a triangle pair, so triangulation is a no-op).
    """
    import ctypes
    from pyopennova.flat_mesh_ffi import (
        FlatMesh,
        FlatMeshFace,
        FlatMeshFaceCornerData,
        FlatMeshVertex,
    )
    from opennova_blender.flat_mesh_bridge import (
        export_bpy_to_flat_mesh,
        import_flat_mesh_to_bpy,
    )

    # Build a synthetic FlatMesh: a single quad as 2 triangles.
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
    for ci in range(6):
        corners[ci].u0, corners[ci].v0 = 0.5, 0.5
        corners[ci].nx, corners[ci].ny, corners[ci].nz = 0.0, 0.0, 1.0
    fm.corners = ctypes.cast(corners, ctypes.POINTER(FlatMeshFaceCornerData))

    id_set = (ctypes.c_int32 * 1)(0)
    fm.material_id_set_count = 1
    fm.material_id_set = ctypes.cast(id_set, ctypes.POINTER(ctypes.c_int32))

    bpy_mesh = bpy.data.meshes.new("test_quad")
    try:
        import_flat_mesh_to_bpy(fm, bpy_mesh)

        assert len(bpy_mesh.vertices) == 4
        assert len(bpy_mesh.polygons) == 2

        fm2 = export_bpy_to_flat_mesh(bpy_mesh, "test_quad", part_index=0)

        assert bytes(fm2.name).rstrip(b"\x00") == b"test_quad"
        assert fm2.vertex_count == 4
        assert fm2.face_count == 2
        for i in range(4):
            assert fm2.vertices[i].x == pytest.approx(verts[i].x)
            assert fm2.vertices[i].y == pytest.approx(verts[i].y)
            assert fm2.vertices[i].z == pytest.approx(verts[i].z)
    finally:
        bpy.data.meshes.remove(bpy_mesh)


def test_export_bpy_to_flat_mesh_handles_no_uv_layers() -> None:
    """A bpy.types.Mesh without UV layers should still produce a valid
    FlatMesh; corner UVs default to (0.0, 0.0).
    """
    from opennova_blender.flat_mesh_bridge import export_bpy_to_flat_mesh

    bpy_mesh = bpy.data.meshes.new("test_no_uv")
    bpy_mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        [],
        [(0, 1, 2)],
    )
    bpy_mesh.update()

    try:
        fm = export_bpy_to_flat_mesh(bpy_mesh, "tri", part_index=0)
        assert bytes(fm.name).rstrip(b"\x00") == b"tri"
        assert fm.vertex_count == 3
        assert fm.face_count == 1
        # No UV layers — corners[0].u0 should be the default 0.0.
        assert fm.corners[0].u0 == pytest.approx(0.0)
        assert fm.corners[0].v0 == pytest.approx(0.0)
    finally:
        bpy.data.meshes.remove(bpy_mesh)


def test_weight_round_trip_preserves_bone_indices_and_weights() -> None:
    """A FlatMesh with skin weights goes through attach/read and emerges
    with the same (bone_index, weight) pairs per vertex, sorted ascending."""
    import ctypes
    from pyopennova.flat_mesh_ffi import (
        FlatMesh,
        FlatMeshBoneInfluence,
        FlatMeshFace,
        FlatMeshFaceCornerData,
        FlatMeshVertex,
    )
    from opennova_blender.flat_mesh_bridge import (
        attach_flat_mesh_weights_to_bpy_object,
        import_flat_mesh_to_bpy,
        read_weights_from_bpy_object,
    )

    # 4 verts, 2 faces, each vertex has 2 bone influences.
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

    # CSR weights: 2 influences per vert = 8 total.
    offsets = (ctypes.c_int32 * 5)(0, 2, 4, 6, 8)
    influences = (FlatMeshBoneInfluence * 8)(
        FlatMeshBoneInfluence(0, 0.6), FlatMeshBoneInfluence(3, 0.4),  # v0 -> bones 0,3
        FlatMeshBoneInfluence(1, 0.6), FlatMeshBoneInfluence(3, 0.4),  # v1 -> bones 1,3
        FlatMeshBoneInfluence(2, 0.6), FlatMeshBoneInfluence(3, 0.4),  # v2 -> bones 2,3
        FlatMeshBoneInfluence(0, 0.5), FlatMeshBoneInfluence(2, 0.5),  # v3 -> bones 0,2
    )
    fm.vertex_influence_offsets = ctypes.cast(offsets, ctypes.POINTER(ctypes.c_int32))
    fm.vertex_influences = ctypes.cast(influences, ctypes.POINTER(FlatMeshBoneInfluence))

    bpy_mesh = bpy.data.meshes.new("test_skin_quad")
    bpy_obj = bpy.data.objects.new("test_skin_quad_obj", bpy_mesh)
    try:
        import_flat_mesh_to_bpy(fm, bpy_mesh)
        attach_flat_mesh_weights_to_bpy_object(fm, bpy_obj)

        group_names = sorted(g.name for g in bpy_obj.vertex_groups)
        assert group_names == ["bone_0", "bone_1", "bone_2", "bone_3"]

        offsets_arr, influences_arr = read_weights_from_bpy_object(bpy_obj, 4)

        assert list(offsets_arr) == [0, 2, 4, 6, 8]
        expected_pairs = [
            (0, 0.6), (3, 0.4),
            (1, 0.6), (3, 0.4),
            (2, 0.6), (3, 0.4),
            (0, 0.5), (2, 0.5),
        ]
        for i, (eb, ew) in enumerate(expected_pairs):
            assert influences_arr[i].bone_index == eb, (
                f"influence {i}: bone_index expected {eb}, got {influences_arr[i].bone_index}"
            )
            assert influences_arr[i].weight == pytest.approx(ew), (
                f"influence {i}: weight expected {ew}, got {influences_arr[i].weight}"
            )
    finally:
        bpy.data.objects.remove(bpy_obj)
        bpy.data.meshes.remove(bpy_mesh)
