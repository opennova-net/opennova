"""ctypes bindings for the C++ flat_mesh module.

Mirrors libs/object/include/object/flat_mesh.h field-for-field. Keep in sync.
"""

from __future__ import annotations

import ctypes

from ._native import load_lib


class FlatMeshVertex(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("z", ctypes.c_float),
    ]


class FlatMeshFace(ctypes.Structure):
    _fields_ = [
        ("v", ctypes.c_int32 * 3),
        ("material_id", ctypes.c_int32),
        ("smoothing_group_mask", ctypes.c_uint32),
    ]


class FlatMeshFaceCornerData(ctypes.Structure):
    _fields_ = [
        ("u0", ctypes.c_float),
        ("v0", ctypes.c_float),
        ("u1", ctypes.c_float),
        ("v1", ctypes.c_float),
        ("nx", ctypes.c_float),
        ("ny", ctypes.c_float),
        ("nz", ctypes.c_float),
    ]


class FlatMeshBoneInfluence(ctypes.Structure):
    _fields_ = [
        ("bone_index", ctypes.c_int32),
        ("weight", ctypes.c_float),
    ]


class FlatMesh(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("part_index", ctypes.c_int32),
        ("origin", ctypes.c_float * 3),
        ("vertex_count", ctypes.c_int32),
        ("vertices", ctypes.POINTER(FlatMeshVertex)),
        ("face_count", ctypes.c_int32),
        ("faces", ctypes.POINTER(FlatMeshFace)),
        ("corners", ctypes.POINTER(FlatMeshFaceCornerData)),
        ("material_id_set_count", ctypes.c_int32),
        ("material_id_set", ctypes.POINTER(ctypes.c_int32)),
        ("vertex_influence_offsets", ctypes.POINTER(ctypes.c_int32)),
        ("vertex_influences", ctypes.POINTER(FlatMeshBoneInfluence)),
    ]


class FlatMeshArray(ctypes.Structure):
    _fields_ = [
        ("count", ctypes.c_int32),
        ("meshes", ctypes.POINTER(FlatMesh)),
    ]


def set_flat_mesh_name(mesh: FlatMesh, name: str) -> None:
    """Write a Python string into ``FlatMesh.name``.

    ``ctypes`` exposes ``c_char * N`` structure fields as ``bytes`` copies,
    so pointer writes such as ``memmove(mesh.name, ...)`` do not mutate the
    struct field. Direct assignment writes into the backing structure and
    null-pads the remaining bytes.
    """
    mesh.name = name.encode("utf-8")[:63]


_bound = False


def _bind() -> None:
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.object_flat_mesh_array_alloc.argtypes = [ctypes.c_int, ctypes.POINTER(FlatMeshArray)]
    lib.object_flat_mesh_array_alloc.restype = ctypes.c_int
    lib.object_flat_mesh_array_free.argtypes = [ctypes.POINTER(FlatMeshArray)]
    lib.object_flat_mesh_array_free.restype = None
    # object_ir_to_flat_meshes takes a Threedi3di3*; we receive that as a
    # plain ctypes byref into the same struct that threedi_ffi populates.
    from pyopennova.threedi_ffi import Threedi3di3
    lib.object_ir_to_flat_meshes.argtypes = [
        ctypes.POINTER(Threedi3di3),
        ctypes.c_int,  # lod_index
        ctypes.c_int,  # include_empty_parts
        ctypes.c_int,  # track_bone_data
        ctypes.POINTER(FlatMeshArray),
    ]
    lib.object_ir_to_flat_meshes.restype = ctypes.c_int
    lib.object_ir_to_flat_meshes_v2.argtypes = [
        ctypes.POINTER(Threedi3di3),
        ctypes.c_int,  # lod_index
        ctypes.c_int,  # include_empty_parts
        ctypes.c_int,  # track_bone_data
        ctypes.c_int,  # preserve_source_indexing
        ctypes.POINTER(FlatMeshArray),
    ]
    lib.object_ir_to_flat_meshes_v2.restype = ctypes.c_int
    _bound = True


def alloc_array(count: int) -> FlatMeshArray:
    _bind()
    lib = load_lib()
    arr = FlatMeshArray()
    rc = lib.object_flat_mesh_array_alloc(count, ctypes.byref(arr))
    if rc != 0:
        raise RuntimeError(f"object_flat_mesh_array_alloc failed (rc={rc})")
    return arr


def free_array(arr: FlatMeshArray) -> None:
    _bind()
    lib = load_lib()
    lib.object_flat_mesh_array_free(ctypes.byref(arr))


def flat_meshes_from_3di3(
    ir,
    lod_index: int,
    *,
    include_empty_parts: bool = False,
    track_bone_data: bool = False,
    preserve_source_indexing: bool = False,
) -> FlatMeshArray:
    """Convert one render LOD of a 3DI3 IR into a FlatMeshArray.

    `ir` is whatever `pyopennova.threedi_ffi.read_model_3di3()` returns
    (a `Threedi3di3` ctypes Structure).

    When preserve_source_indexing=True, vertices are deduped by source IR index
    (first-seen wins), matching pyopennova.mesh_build.flatten_lod() behaviour.
    """
    _bind()
    lib = load_lib()
    arr = FlatMeshArray()
    rc = lib.object_ir_to_flat_meshes_v2(
        ctypes.byref(ir),
        int(lod_index),
        int(bool(include_empty_parts)),
        int(bool(track_bone_data)),
        int(bool(preserve_source_indexing)),
        ctypes.byref(arr),
    )
    if rc != 0:
        raise RuntimeError(f"object_ir_to_flat_meshes_v2 failed (rc={rc}) for LOD {lod_index}")
    return arr
