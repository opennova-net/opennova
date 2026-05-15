"""ctypes bindings for native 3DI comparison and geometry validation."""

from __future__ import annotations

import ctypes
import json
from pathlib import Path
from typing import Any

from ._native import load_lib


OBJECT_3DI_POLICY_OK = 0
OBJECT_3DI_POLICY_COMPARE_FAILED = 1
OBJECT_3DI_POLICY_VALIDATION_FAILED = 2
OBJECT_3DI_COMPARE_RELAX_GEOMETRY = 1 << 0


_bound = False


def _bind() -> None:
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.object_3di_compare_files.restype = ctypes.c_int
    lib.object_3di_compare_files.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint32,
        ctypes.c_char_p,
    ]
    lib.object_3di_validate_geometry_chunks.restype = ctypes.c_int
    lib.object_3di_validate_geometry_chunks.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint32,
        ctypes.c_char_p,
    ]
    _bound = True


def _read_report(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def compare_files(
    expected_path: str | Path,
    actual_path: str | Path,
    *,
    policy: int = 0,
    report_path: str | Path,
) -> dict[str, Any]:
    _bind()
    report = Path(report_path)
    rc = load_lib().object_3di_compare_files(
        str(Path(expected_path)).encode("utf-8"),
        str(Path(actual_path)).encode("utf-8"),
        ctypes.c_uint32(policy),
        str(report).encode("utf-8"),
    )
    if rc < 0:
        raise RuntimeError(f"object_3di_compare_files failed with status {rc}")
    payload = _read_report(report)
    payload["status"] = int(rc)
    return payload


def validate_geometry_chunks(
    path: str | Path,
    *,
    flags: int = 0,
    report_path: str | Path,
) -> dict[str, Any]:
    _bind()
    report = Path(report_path)
    rc = load_lib().object_3di_validate_geometry_chunks(
        str(Path(path)).encode("utf-8"),
        ctypes.c_uint32(flags),
        str(report).encode("utf-8"),
    )
    if rc < 0:
        raise RuntimeError(f"object_3di_validate_geometry_chunks failed with status {rc}")
    payload = _read_report(report)
    payload["status"] = int(rc)
    return payload
