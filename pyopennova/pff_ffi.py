"""
ctypes bindings for the PFF archive C API (libopennova.so / opennova.dll).

Mirrors structs from libs/pff/include/pff/pff.h.
Wraps open/close/find/extract for reading PFF3/PFF4 archives.
"""
from __future__ import annotations

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Struct definitions matching pff.h
# ---------------------------------------------------------------------------

class PffHeader(ctypes.Structure):
    _fields_ = [
        ("header_size", ctypes.c_uint32),
        ("magic", ctypes.c_uint32),
        ("num_entries", ctypes.c_uint32),
        ("entry_size", ctypes.c_uint32),
        ("file_table_offset", ctypes.c_uint32),
    ]


class PffEntry(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint32),
        ("offset", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("timestamp", ctypes.c_uint32),
        ("filename", ctypes.c_char * 16),
        ("checksum", ctypes.c_uint32),
    ]


class PffArchiveC(ctypes.Structure):
    _fields_ = [
        ("header", PffHeader),
        ("entries", ctypes.POINTER(PffEntry)),
        ("entry_count", ctypes.c_uint32),
        ("_file", ctypes.c_void_p),
        ("_path", ctypes.c_char * 260),
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
    lib.pff_open.restype = ctypes.c_int
    lib.pff_open.argtypes = [ctypes.POINTER(PffArchiveC), ctypes.c_char_p]
    lib.pff_close.restype = None
    lib.pff_close.argtypes = [ctypes.POINTER(PffArchiveC)]
    lib.pff_find.restype = ctypes.POINTER(PffEntry)
    lib.pff_find.argtypes = [ctypes.POINTER(PffArchiveC), ctypes.c_char_p]
    lib.pff_extract.restype = ctypes.c_int
    lib.pff_extract.argtypes = [
        ctypes.POINTER(PffArchiveC),
        ctypes.POINTER(PffEntry),
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    _bound = True


class PffArchive:
    """Context manager for reading PFF archives."""

    def __init__(self, path: str):
        _bind()
        self._lib = load_lib()
        self._arc = PffArchiveC()
        self._closed = False
        if isinstance(path, str):
            path = path.encode("utf-8")
        rc = self._lib.pff_open(ctypes.byref(self._arc), path)
        if rc != 0:
            raise RuntimeError(f"pff_open failed for {path!r}")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        """Close the archive. Safe to call multiple times."""
        if not self._closed:
            self._lib.pff_close(ctypes.byref(self._arc))
            self._closed = True

    def find(self, name: str) -> PffEntry | None:
        """Find an entry by name (case-insensitive). Returns None if not found."""
        if isinstance(name, str):
            name = name.encode("utf-8")
        ptr = self._lib.pff_find(ctypes.byref(self._arc), name)
        if not ptr:
            return None
        return ptr.contents

    def extract(self, entry: PffEntry) -> bytes:
        """Extract file data for *entry*, returning the raw bytes."""
        buf = (ctypes.c_uint8 * entry.size)()
        rc = self._lib.pff_extract(
            ctypes.byref(self._arc), ctypes.byref(entry), buf, entry.size
        )
        if rc != 0:
            raise RuntimeError(f"pff_extract failed (rc={rc})")
        return bytes(buf)
