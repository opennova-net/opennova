"""
ctypes bindings for the ADM animation definition C API (libopennova.so / opennova.dll).

Mirrors structs from libs/adm/include/adm/adm.h.
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Struct definitions matching adm.h
# ---------------------------------------------------------------------------

class AdmEntry(ctypes.Structure):
    _fields_ = [
        ("key",   ctypes.c_char * 64),
        ("value", ctypes.c_char * 256),
    ]


class AdmFile(ctypes.Structure):
    _fields_ = [
        ("entries", ctypes.POINTER(AdmEntry)),
        ("count",   ctypes.c_size_t),
    ]


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

_bound = False

def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.adm_parse.restype = ctypes.c_int
    lib.adm_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(AdmFile)]
    lib.adm_free.restype = None
    lib.adm_free.argtypes = [ctypes.POINTER(AdmFile)]
    _bound = True


def parse_adm(path: str) -> AdmFile:
    """Parse an ADM animation definition file. Caller must call free_adm() when done."""
    _bind()
    lib = load_lib()
    af = AdmFile()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.adm_parse(path, ctypes.byref(af))
    if rc != 0:
        raise RuntimeError(f"adm_parse failed for {path!r}")
    return af


def free_adm(af: AdmFile):
    """Free all C-side allocations inside an AdmFile."""
    _bind()
    lib = load_lib()
    lib.adm_free(ctypes.byref(af))
