"""
ctypes bindings for the SCR decryption C API (libopennova.so / opennova.dll).

Mirrors defines from libs/scr/include/scr/scr.h.
Wraps the decrypt-to-buffer path that Python callers need.
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Constants (matching scr.h #defines)
# ---------------------------------------------------------------------------

SCR_KEY_DEFAULT = 0xABEEFACE
SCR_KEY_JO_DFX2 = 0x2A5A8EAD
SCR_KEY_SHADERS = 0xA55B1EED


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

_bound = False


def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.scr_is_scr.restype = ctypes.c_int
    lib.scr_is_scr.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.scr_get_version.restype = ctypes.c_uint8
    lib.scr_get_version.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.scr_decrypt.restype = None
    lib.scr_decrypt.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_uint32]
    lib.scr_decrypt_buf.restype = ctypes.c_int
    lib.scr_decrypt_buf.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_uint32,
    ]
    _bound = True


def is_scr(data: bytes) -> bool:
    """Return True if *data* starts with the SCR magic header."""
    _bind()
    lib = load_lib()
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return lib.scr_is_scr(buf, len(data)) != 0


def get_version(data: bytes) -> int:
    """Return the SCR version byte from the header (0 if too small)."""
    _bind()
    lib = load_lib()
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return lib.scr_get_version(buf, len(data))


def decrypt(data: bytes, key: int = SCR_KEY_DEFAULT) -> bytes:
    """Decrypt an SCR blob, returning the plaintext bytes.

    Raises ValueError if *data* is not a valid SCR stream, or RuntimeError
    if the output buffer is too small (should not happen with correct sizing).
    """
    _bind()
    lib = load_lib()
    in_buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    out_size = ctypes.c_size_t(len(data))
    out_buf = (ctypes.c_uint8 * len(data))()
    rc = lib.scr_decrypt_buf(in_buf, len(data), out_buf, ctypes.byref(out_size), key)
    if rc == -1:
        raise ValueError("data is not a valid SCR stream")
    if rc != 0:
        raise RuntimeError(f"scr_decrypt_buf failed (rc={rc})")
    return bytes(out_buf[: out_size.value])
