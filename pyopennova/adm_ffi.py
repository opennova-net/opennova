"""
ctypes bindings for the ADM animation definition C API (libopennova.so / opennova.dll).

Mirrors structs from libs/anim/include/adm/adm.h.
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Struct definitions matching adm.h
# ---------------------------------------------------------------------------

ADM_MAX_VARIANTS = 8


class AdmEntry(ctypes.Structure):
    _fields_ = [
        ("key",   ctypes.c_char * 64),
        ("value", ctypes.c_char * 256),  # first variant (== values[0])
        # Multi-clip rows: every quoted token on the row, rotated round-robin
        # by the engine (see adm.h ADM_MAX_VARIANTS).
        ("value_count", ctypes.c_size_t),
        ("values", (ctypes.c_char * 64) * ADM_MAX_VARIANTS),
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
    lib.adm_write.restype = ctypes.c_int
    lib.adm_write.argtypes = [ctypes.c_char_p, ctypes.POINTER(AdmEntry), ctypes.c_size_t]
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


def write_adm(path: str, entries) -> None:
    """Write a list of (key, value) pairs to an ADM file.

    `entries` is an iterable of (key, value) string pairs, ordered as they
    should appear (reset first, key ``anim_reset``).
    """
    _bind()
    lib = load_lib()
    pairs = list(entries)
    arr = (AdmEntry * len(pairs))()
    for i, (key, value) in enumerate(pairs):
        arr[i].key = str(key).encode("utf-8")[:63]
        arr[i].value = str(value).encode("utf-8")[:255]
        arr[i].value_count = 1
        arr[i].values[0].value = str(value).encode("utf-8")[:63]
    if isinstance(path, str):
        path = path.encode("utf-8")
    ptr = arr if pairs else None
    rc = lib.adm_write(path, ptr, len(pairs))
    if rc != 0:
        raise RuntimeError(f"adm_write failed for {path!r}")
