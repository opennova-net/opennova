"""ctypes bindings for the OpenNova object bake session API."""

from __future__ import annotations

import ctypes
from pathlib import Path

from ._native import load_lib
from .tdp_ffi import TdpProject


BAKE_STATUS_OK = 0
BAKE_UPDATE_MTRL = 1 << 0
BAKE_UPDATE_LGHT = 1 << 1
BAKE_UPDATE_PANM = 1 << 2
BAKE_UPDATE_ALL = BAKE_UPDATE_MTRL | BAKE_UPDATE_LGHT | BAKE_UPDATE_PANM


class BakeExportRequest(ctypes.Structure):
    _fields_ = [
        ("project", ctypes.POINTER(TdpProject)),
        ("output_path", ctypes.c_char_p),
        ("update_mask", ctypes.c_uint8),
        ("model_name", ctypes.c_char_p),
    ]


class BakeSession(ctypes.Structure):
    pass


_bound = False


def _bind() -> None:
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.bake_session_create.restype = ctypes.c_int
    lib.bake_session_create.argtypes = [
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_int,
        ctypes.POINTER(TdpProject),
        ctypes.POINTER(ctypes.POINTER(BakeSession)),
    ]
    lib.bake_session_export.restype = ctypes.c_int
    lib.bake_session_export.argtypes = [
        ctypes.POINTER(BakeSession),
        ctypes.POINTER(BakeExportRequest),
    ]
    lib.bake_session_destroy.restype = None
    lib.bake_session_destroy.argtypes = [ctypes.POINTER(BakeSession)]
    lib.bake_project_export.restype = ctypes.c_int
    lib.bake_project_export.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint8,
    ]
    _bound = True


def create_session(ase_paths: list[str | Path], project: TdpProject):
    _bind()
    lib = load_lib()
    encoded = [
        str(Path(path)).encode("utf-8")
        for path in ase_paths
    ]
    array_type = ctypes.c_char_p * len(encoded)
    array = array_type(*encoded)
    session = ctypes.POINTER(BakeSession)()
    rc = lib.bake_session_create(
        array,
        len(encoded),
        ctypes.byref(project),
        ctypes.byref(session),
    )
    if rc != BAKE_STATUS_OK:
        raise RuntimeError(f"bake_session_create failed with status {rc}")
    return session


def export_session(
    session,
    project: TdpProject,
    output_path: str | Path,
    *,
    model_name: str | None = None,
    update_mask: int = BAKE_UPDATE_ALL,
) -> None:
    _bind()
    lib = load_lib()
    request = BakeExportRequest(
        project=ctypes.cast(ctypes.byref(project), ctypes.POINTER(TdpProject)),
        output_path=str(Path(output_path)).encode("utf-8"),
        update_mask=ctypes.c_uint8(update_mask),
        model_name=model_name.encode("utf-8") if model_name else None,
    )
    rc = lib.bake_session_export(session, ctypes.byref(request))
    if rc != BAKE_STATUS_OK:
        raise RuntimeError(f"bake_session_export failed with status {rc}")


def destroy_session(session) -> None:
    if not session:
        return
    _bind()
    load_lib().bake_session_destroy(session)


def export_project(
    project_path: str | Path,
    output_path: str | Path,
    *,
    model_name: str | None = None,
    update_mask: int = BAKE_UPDATE_ALL,
) -> None:
    _bind()
    lib = load_lib()
    rc = lib.bake_project_export(
        str(Path(project_path)).encode("utf-8"),
        str(Path(output_path)).encode("utf-8"),
        model_name.encode("utf-8") if model_name else None,
        ctypes.c_uint8(update_mask),
    )
    if rc != BAKE_STATUS_OK:
        raise RuntimeError(f"bake_project_export failed with status {rc}")
