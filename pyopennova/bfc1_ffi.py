"""
ctypes bindings for the BFC1 decompression C API (libopennova.so / opennova.dll).

Mirrors libs/bfc1/include/bfc1/bfc1.h.
"""

import ctypes

from ._native import load_lib

_bound = False


def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.bfc1_is_bfc1.restype = ctypes.c_int
    lib.bfc1_is_bfc1.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.bfc1_uncompressed_size.restype = ctypes.c_int
    lib.bfc1_uncompressed_size.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.bfc1_decompress.restype = ctypes.c_int
    lib.bfc1_decompress.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    _bound = True


def is_bfc1(data: bytes) -> bool:
    """Return True if *data* starts with the BFC1 magic header."""
    _bind()
    lib = load_lib()
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return lib.bfc1_is_bfc1(buf, len(data)) != 0


def decompress(data: bytes) -> bytes:
    """Decompress a BFC1 blob, returning the raw bytes.

    Raises ValueError if *data* is not valid BFC1, or RuntimeError on
    decompression failure.
    """
    _bind()
    lib = load_lib()
    in_buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)

    # Get uncompressed size
    uncomp_size = ctypes.c_uint32()
    rc = lib.bfc1_uncompressed_size(in_buf, len(data), ctypes.byref(uncomp_size))
    if rc != 0:
        raise ValueError("data is not a valid BFC1 stream")

    out_size = ctypes.c_size_t(uncomp_size.value)
    out_buf = (ctypes.c_uint8 * uncomp_size.value)()
    rc = lib.bfc1_decompress(in_buf, len(data), out_buf, ctypes.byref(out_size))
    if rc != 0:
        raise RuntimeError(f"bfc1_decompress failed (rc={rc})")
    return bytes(out_buf[: out_size.value])
