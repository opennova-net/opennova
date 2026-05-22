"""ctypes bindings for raw 3DI3 chunk comparison."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path

from ._native import load_lib


_lib = None


def _get_lib():
    global _lib
    if _lib is None:
        lib = load_lib()
        lib.threedi_3di3_compare_file_chunks.restype = ctypes.c_int
        lib.threedi_3di3_compare_file_chunks.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        _lib = lib
    return _lib


def _path_bytes(path: str | Path) -> bytes:
    return os.fsencode(Path(path))


def compare_3di3_chunks(expected_path: str | Path, actual_path: str | Path, chunk_ids: str) -> str:
    """Compare explicitly selected raw chunks in two 3DI3 files.

    Returns the native report string on equality. Raises RuntimeError on mismatch
    or native read errors.
    """
    if not chunk_ids or not chunk_ids.strip():
        raise ValueError("chunk_ids must contain one or more explicit FourCC values")

    report = ctypes.create_string_buffer(4096)
    lib = _get_lib()
    rc = lib.threedi_3di3_compare_file_chunks(
        _path_bytes(expected_path),
        _path_bytes(actual_path),
        chunk_ids.encode("ascii"),
        report,
        ctypes.sizeof(report),
    )
    text = report.value.decode("utf-8", errors="replace")
    if rc == 0:
        return text
    if rc == 1:
        raise RuntimeError(f"3DI3 chunk comparison mismatch: {text}")
    raise RuntimeError(f"3DI3 chunk comparison failed (rc={rc}): {text}")
