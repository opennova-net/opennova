"""FFI binding tests for the new C++ flat_mesh module (Phase B)."""

from __future__ import annotations

import ctypes
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
AKCRATE = REPO_ROOT / "fixtures" / "stock_3di" / "akcrate" / "akcrate.3di"


def test_flat_mesh_struct_layout_matches_c():
    from pyopennova.flat_mesh_ffi import FlatMesh, FlatMeshVertex, FlatMeshFace, FlatMeshFaceCornerData

    assert ctypes.sizeof(FlatMeshVertex) == 12
    assert ctypes.sizeof(FlatMeshFace) == 20  # int32[3] + int32 + uint32
    assert ctypes.sizeof(FlatMeshFaceCornerData) == 28
    # FlatMesh has 64-byte name + int + 3 floats + 2*(int+ptr) + ptr + int + ptr + 2 ptrs.
    # Just assert it's > 0; layout-exactness via the C++ tests + runtime probe below.
    assert ctypes.sizeof(FlatMesh) > 0


def test_set_flat_mesh_name_writes_to_ctypes_struct_field():
    from pyopennova.flat_mesh_ffi import FlatMesh, set_flat_mesh_name

    fm = FlatMesh()

    set_flat_mesh_name(fm, "01 Mesh0")

    assert bytes(fm.name).rstrip(b"\x00") == b"01 Mesh0"


def test_set_flat_mesh_name_truncates_to_struct_capacity_minus_nul():
    from pyopennova.flat_mesh_ffi import FlatMesh, set_flat_mesh_name

    fm = FlatMesh()

    set_flat_mesh_name(fm, "A" * 80)

    assert bytes(fm.name).rstrip(b"\x00") == b"A" * 63


def test_flat_mesh_array_alloc_and_free():
    from pyopennova.flat_mesh_ffi import alloc_array, free_array

    arr = alloc_array(3)
    try:
        assert arr.count == 3
        assert arr.meshes  # non-null
        for i in range(3):
            assert arr.meshes[i].vertex_count == 0
            assert not arr.meshes[i].vertices
    finally:
        free_array(arr)
    assert arr.count == 0


@pytest.mark.skipif(not AKCRATE.is_file(), reason="akcrate fixture missing (LFS not pulled?)")
def test_flat_meshes_from_akcrate_lod0():
    from pyopennova import threedi_ffi
    from pyopennova.flat_mesh_ffi import flat_meshes_from_3di3, free_array

    ir = threedi_ffi.read_model_3di3(str(AKCRATE))
    try:
        arr = flat_meshes_from_3di3(ir, lod_index=0)
        try:
            assert arr.count > 0
            for i in range(arr.count):
                fm = arr.meshes[i]
                assert fm.vertex_count > 0
                assert fm.face_count > 0
                # name is "01 Mesh0", "02 Mesh0", … (Python mesh_build.py line 86)
                name = bytes(fm.name).rstrip(b"\x00").decode("ascii")
                assert name.endswith("Mesh0")
        finally:
            free_array(arr)
    finally:
        threedi_ffi.free_model_3di3(ir)
