"""ctypes bindings for the native ASE source writer."""
from __future__ import annotations

import ctypes
from pathlib import Path

from ._native import load_lib


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
    from .bad_ffi import BadFile
    from .threedi_3di3_ffi import Threedi3di3

    lib = load_lib()
    lib.object_nlascexp_options_init_defaults.argtypes = [ctypes.POINTER(NlascexpOptions)]
    lib.object_nlascexp_options_init_defaults.restype = None
    lib.object_write_ase_files_from_3di3.argtypes = [
        ctypes.POINTER(Threedi3di3),
        ctypes.c_char_p,
        ctypes.POINTER(NlascexpOptions),
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.object_write_ase_files_from_3di3.restype = ctypes.c_int
    lib.object_write_ase_files_from_3di3_with_bad.argtypes = [
        ctypes.POINTER(Threedi3di3),
        ctypes.POINTER(BadFile),
        ctypes.c_char_p,
        ctypes.POINTER(NlascexpOptions),
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.object_write_ase_files_from_3di3_with_bad.restype = ctypes.c_int
    _bound = True


def write_ase_files_from_3di3(
    model,
    primary_path: str | Path,
    *,
    bad_file=None,
    include_collisions: bool = True,
    include_occlusion: bool = True,
    include_lights: bool = True,
) -> tuple[str, ...]:
    """Write primary and per-LOD ASE files for a canonical 3DI3 model."""
    _bind()
    lib = load_lib()
    opts = NlascexpOptions()
    lib.object_nlascexp_options_init_defaults(ctypes.byref(opts))
    opts.include_collisions = int(include_collisions)
    opts.include_occlusion = int(include_occlusion)
    opts.include_lights = int(include_lights)

    capacity = 16
    buffers = [ctypes.create_string_buffer(260) for _ in range(capacity)]
    out_paths = (ctypes.c_char_p * capacity)()
    for idx, buffer in enumerate(buffers):
        out_paths[idx] = ctypes.cast(buffer, ctypes.c_char_p)

    out_count = ctypes.c_int(0)
    primary_bytes = str(primary_path).encode("utf-8")
    if bad_file is not None:
        rc = lib.object_write_ase_files_from_3di3_with_bad(
            ctypes.byref(model),
            ctypes.byref(bad_file),
            primary_bytes,
            ctypes.byref(opts),
            out_paths,
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
            out_paths,
            capacity,
            ctypes.byref(out_count),
        )
        if rc != 0:
            raise RuntimeError(f"object_write_ase_files_from_3di3 failed (rc={rc})")
    return tuple(buffers[idx].value.decode("utf-8") for idx in range(out_count.value))
