"""ctypes bindings for the C++ ASE writer module (Phase C writer body)."""
from __future__ import annotations

import ctypes
from pathlib import Path

from pyopennova._native import load_lib


class NlascexpOptions(ctypes.Structure):
    _fields_ = [
        ("include_collisions", ctypes.c_int),
        ("include_occlusion", ctypes.c_int),
        ("include_lights", ctypes.c_int),
        ("scaleby", ctypes.c_float),
        ("float_precision", ctypes.c_int),
    ]


_bound = False


def _bind() -> None:
    global _bound
    if _bound:
        return
    from pyopennova.threedi_ffi import Threedi3di3
    lib = load_lib()
    lib.object_nlascexp_options_init_defaults.argtypes = [ctypes.POINTER(NlascexpOptions)]
    lib.object_nlascexp_options_init_defaults.restype = None
    lib.object_write_ase_files_from_3di3.argtypes = [
        ctypes.POINTER(Threedi3di3),  # ir (const Threedi3di3*)
        ctypes.c_char_p,              # primary_path
        ctypes.POINTER(NlascexpOptions),  # opts
        ctypes.POINTER(ctypes.c_char_p),  # out_paths_array (char**)
        ctypes.c_int,                 # out_paths_capacity
        ctypes.POINTER(ctypes.c_int), # out_paths_count
    ]
    lib.object_write_ase_files_from_3di3.restype = ctypes.c_int
    from pyopennova.bad_ffi import BadFile
    lib.object_write_ase_files_from_3di3_with_bad.argtypes = [
        ctypes.POINTER(Threedi3di3),    # ir
        ctypes.POINTER(BadFile),         # bad_file (nullable)
        ctypes.c_char_p,                 # primary_path
        ctypes.POINTER(NlascexpOptions), # opts
        ctypes.POINTER(ctypes.c_char_p), # out_paths_array
        ctypes.c_int,                    # capacity
        ctypes.POINTER(ctypes.c_int),    # out_paths_count
    ]
    lib.object_write_ase_files_from_3di3_with_bad.restype = ctypes.c_int
    _bound = True


def write_ase_files_from_3di3(model, primary_path,
                              *,
                              bad_file=None,
                              include_collisions: bool = True,
                              include_occlusion: bool = True,
                              include_lights: bool = True) -> list[str]:
    """C++-backed equivalent of pyopennova.ase_from_3di3.write_ase_from_3di3."""
    _bind()
    lib = load_lib()
    opts = NlascexpOptions()
    lib.object_nlascexp_options_init_defaults(ctypes.byref(opts))
    opts.include_collisions = int(include_collisions)
    opts.include_occlusion = int(include_occlusion)
    opts.include_lights = int(include_lights)

    # We need 16 writable 260-byte buffers and an array of pointers to them.
    # ctypes c_char_p is read-only; use create_string_buffer for writable buffers.
    capacity = 16
    buffers = [ctypes.create_string_buffer(260) for _ in range(capacity)]
    ptr_array = (ctypes.c_char_p * capacity)()
    for i, b in enumerate(buffers):
        ptr_array[i] = ctypes.cast(b, ctypes.c_char_p)

    out_count = ctypes.c_int(0)
    primary_bytes = str(primary_path).encode("utf-8")

    if bad_file is not None:
        rc = lib.object_write_ase_files_from_3di3_with_bad(
            ctypes.byref(model),
            ctypes.byref(bad_file),
            primary_bytes,
            ctypes.byref(opts),
            ptr_array,
            capacity,
            ctypes.byref(out_count),
        )
        if rc != 0:
            raise RuntimeError(f"object_write_ase_files_from_3di3_with_bad failed (rc={rc})")
    else:
        rc = lib.object_write_ase_files_from_3di3(
            ctypes.byref(model),
            primary_bytes,
            ctypes.byref(opts),
            ptr_array,
            capacity,
            ctypes.byref(out_count),
        )
        if rc != 0:
            raise RuntimeError(f"object_write_ase_files_from_3di3 failed (rc={rc})")
    return [buffers[i].value.decode("utf-8") for i in range(out_count.value)]


def write_ase_files_from_flat_meshes(meshes_per_lod, ir, primary_path,
                                      *,
                                      bad_file=None,
                                      include_collisions: bool = True,
                                      include_occlusion: bool = True,
                                      include_lights: bool = True) -> list[str]:
    """Write .ase files from caller-provided FlatMeshArrays + IR side-data.

    Used by the Blender DCC bridge: Blender constructs edited FlatMeshes via
    flat_mesh_bridge.export_bpy_to_flat_mesh(); we pass them here together with
    the original IR (for collision, occlusion, lights, markers, bones, materials)
    and write the full .ase document via the C++ chain.

    Parameters
    ----------
    meshes_per_lod : list[FlatMeshArray]
        One FlatMeshArray per LOD, length must equal ir.lod_count.
    ir : Threedi3di3
        Original IR; used for side-data only (collision, occlusion, etc.).
    primary_path : str | Path
        Path for LOD 0 .ase; per-LOD files are named "<stem>_lod<N>.ase".

    Returns
    -------
    list[str] : paths actually written.
    """
    from pyopennova.flat_mesh_ffi import FlatMeshArray

    _bind_from_flat_meshes()
    lib = load_lib()
    opts = NlascexpOptions()
    lib.object_nlascexp_options_init_defaults(ctypes.byref(opts))
    opts.include_collisions = int(include_collisions)
    opts.include_occlusion = int(include_occlusion)
    opts.include_lights = int(include_lights)

    if len(meshes_per_lod) != int(ir.lod_count):
        raise ValueError(
            f"meshes_per_lod length ({len(meshes_per_lod)}) must equal "
            f"ir.lod_count ({int(ir.lod_count)})"
        )

    # Pack the FlatMeshArrays into a contiguous C array.
    arr_type = FlatMeshArray * len(meshes_per_lod)
    per_lod = arr_type(*meshes_per_lod)

    capacity = 16
    buffers = [ctypes.create_string_buffer(260) for _ in range(capacity)]
    ptr_array = (ctypes.c_char_p * capacity)()
    for i, b in enumerate(buffers):
        ptr_array[i] = ctypes.cast(b, ctypes.c_char_p)

    out_count = ctypes.c_int(0)
    primary_bytes = str(primary_path).encode("utf-8")

    if bad_file is not None:
        rc = lib.object_write_ase_files_from_flat_meshes_with_bad(
            per_lod,
            len(meshes_per_lod),
            ctypes.byref(ir),
            ctypes.byref(bad_file),
            primary_bytes,
            ctypes.byref(opts),
            ptr_array,
            capacity,
            ctypes.byref(out_count),
        )
        if rc != 0:
            raise RuntimeError(f"object_write_ase_files_from_flat_meshes_with_bad failed (rc={rc})")
    else:
        rc = lib.object_write_ase_files_from_flat_meshes(
            per_lod,
            len(meshes_per_lod),
            ctypes.byref(ir),
            primary_bytes,
            ctypes.byref(opts),
            ptr_array,
            capacity,
            ctypes.byref(out_count),
        )
        if rc != 0:
            raise RuntimeError(f"object_write_ase_files_from_flat_meshes failed (rc={rc})")
    return [buffers[i].value.decode("utf-8") for i in range(out_count.value)]


_bound_from_flat_meshes = False


def _bind_from_flat_meshes() -> None:
    global _bound_from_flat_meshes
    if _bound_from_flat_meshes:
        return
    from pyopennova.threedi_ffi import Threedi3di3
    from pyopennova.flat_mesh_ffi import FlatMeshArray
    _bind()  # ensure base bindings (NlascexpOptions, options_init_defaults) are loaded
    lib = load_lib()
    lib.object_write_ase_files_from_flat_meshes.argtypes = [
        ctypes.POINTER(FlatMeshArray),    # meshes_per_lod
        ctypes.c_int,                      # lod_count
        ctypes.POINTER(Threedi3di3),       # ir
        ctypes.c_char_p,                   # primary_path
        ctypes.POINTER(NlascexpOptions),   # opts
        ctypes.POINTER(ctypes.c_char_p),   # out_paths_array
        ctypes.c_int,                      # out_paths_capacity
        ctypes.POINTER(ctypes.c_int),      # out_paths_count
    ]
    lib.object_write_ase_files_from_flat_meshes.restype = ctypes.c_int
    from pyopennova.bad_ffi import BadFile
    lib.object_write_ase_files_from_flat_meshes_with_bad.argtypes = [
        ctypes.POINTER(FlatMeshArray),    # meshes_per_lod
        ctypes.c_int,                      # lod_count
        ctypes.POINTER(Threedi3di3),       # ir
        ctypes.POINTER(BadFile),           # bad_file (nullable)
        ctypes.c_char_p,                   # primary_path
        ctypes.POINTER(NlascexpOptions),   # opts
        ctypes.POINTER(ctypes.c_char_p),   # out_paths_array
        ctypes.c_int,                      # out_paths_capacity
        ctypes.POINTER(ctypes.c_int),      # out_paths_count
    ]
    lib.object_write_ase_files_from_flat_meshes_with_bad.restype = ctypes.c_int
    _bound_from_flat_meshes = True
