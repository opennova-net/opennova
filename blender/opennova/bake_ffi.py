"""ctypes bindings for native OpenNova object bake helpers."""
from __future__ import annotations

import ctypes
from pathlib import Path

from ._native import load_lib


BAKE_STATUS_OK = 0
BAKE_STATUS_INVALID_ARGUMENT = -1
BAKE_STATUS_ALLOCATION_FAILED = -2
BAKE_STATUS_ASE_PARSE_FAILED = -3
BAKE_STATUS_CONVERT_FAILED = -4
BAKE_STATUS_EXPORT_FAILED = -5
BAKE_STATUS_PROJECT_PARSE_FAILED = -6
BAKE_STATUS_MISSING_ASE = -7

BAKE_UPDATE_MTRL = 1 << 0
BAKE_UPDATE_LGHT = 1 << 1
BAKE_UPDATE_PANM = 1 << 2
BAKE_UPDATE_ALL = BAKE_UPDATE_MTRL | BAKE_UPDATE_LGHT | BAKE_UPDATE_PANM


_bound = False


def _bind() -> None:
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.bake_project_export.restype = ctypes.c_int
    lib.bake_project_export.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint8,
    ]
    _bound = True


def _encode_path(path: str | Path) -> bytes:
    return str(Path(path)).encode("utf-8")


def _encode_optional(value: str | None) -> bytes | None:
    return value.encode("utf-8") if value else None


def bake_project_export(
    project_path: str | Path,
    output_path: str | Path,
    *,
    model_name: str | None = None,
    update_mask: int = BAKE_UPDATE_ALL,
) -> int:
    _bind()
    rc = load_lib().bake_project_export(
        _encode_path(project_path),
        _encode_path(output_path),
        _encode_optional(model_name),
        ctypes.c_uint8(update_mask),
    )
    if rc != BAKE_STATUS_OK:
        raise RuntimeError(f"bake_project_export failed with status {rc}")
    return rc
