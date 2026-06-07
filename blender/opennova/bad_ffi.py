"""
ctypes bindings for the BAD animation C API (libopennova.so / opennova.dll).

Mirrors structs from libs/bad/include/bad/bad.h.
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Struct definitions matching bad.h
# ---------------------------------------------------------------------------

class BadBone(ctypes.Structure):
    _fields_ = [
        ("name",          ctypes.c_char * 33),
        ("num_children",  ctypes.c_int32),
        ("child_offset",  ctypes.c_int32),
        ("parent_offset", ctypes.c_int32),
        ("parent_index",  ctypes.c_int32),
        ("length",        ctypes.c_float),
        ("position",      ctypes.c_float * 3),
        ("rotation",      ctypes.c_float * 9),
    ]


class BadQuaternion(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("z", ctypes.c_float),
        ("w", ctypes.c_float),
    ]


class BadChannel(ctypes.Structure):
    _fields_ = [
        ("frame_count",          ctypes.c_uint32),
        ("frame_lengths_offset", ctypes.c_uint32),
        ("rotations_offset",     ctypes.c_uint32),
        ("frame_lengths",        ctypes.POINTER(ctypes.c_uint16)),
        ("rotations",            ctypes.POINTER(BadQuaternion)),
    ]


class BadEvent(ctypes.Structure):
    _fields_ = [
        ("velocity", ctypes.c_float * 3),
        ("bottom",   ctypes.c_float),
        ("top",      ctypes.c_float),
        ("trigger",  ctypes.c_int32),
    ]


class BadFile(ctypes.Structure):
    _fields_ = [
        ("version",     ctypes.c_uint32),
        ("header_size", ctypes.c_uint32),
        ("fps",         ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("flags",       ctypes.c_uint32),
        ("bone_count",  ctypes.c_uint32),

        ("bones",     ctypes.POINTER(BadBone)),
        ("num_bones", ctypes.c_size_t),

        ("channels",     ctypes.POINTER(BadChannel)),
        ("num_channels", ctypes.c_size_t),

        ("events",     ctypes.POINTER(BadEvent)),
        ("num_events", ctypes.c_size_t),

        ("translations",     ctypes.POINTER(ctypes.c_float * 3)),
        ("num_translations", ctypes.c_size_t),
    ]


class BadWriteOptions(ctypes.Structure):
    """Header/policy fields the parser ignores; see bad.h BadWriteOptions."""
    _fields_ = [
        ("header_size",           ctypes.c_uint32),
        ("unknown1",              ctypes.c_uint32),
        ("num_event_blocks",      ctypes.c_uint32),
        ("unknown3",              ctypes.c_uint32),
        ("bone_stride",           ctypes.c_uint32),
        ("frame_rec_stride",      ctypes.c_uint32),
        ("rot_stride",            ctypes.c_uint32),
        ("pos_offsets_frame_len", ctypes.c_uint32),
        ("unknown6",              ctypes.c_uint32),
        ("unknown7",              ctypes.c_uint32),
        ("unknown8",              ctypes.c_uint32),
        ("zero_bn01_position",    ctypes.c_int),
        ("write_bone_index_byte", ctypes.c_int),
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
    lib.bad_parse.restype = ctypes.c_int
    lib.bad_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(BadFile)]
    lib.bad_free.restype = None
    lib.bad_free.argtypes = [ctypes.POINTER(BadFile)]
    lib.bad_write_options_default.restype = None
    lib.bad_write_options_default.argtypes = [ctypes.POINTER(BadWriteOptions)]
    lib.bad_write.restype = ctypes.c_int
    lib.bad_write.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(BadFile), ctypes.POINTER(BadWriteOptions)
    ]
    _bound = True


def parse_bad(path: str) -> BadFile:
    """Parse a BAD animation file. Caller must call free_bad() when done."""
    _bind()
    lib = load_lib()
    bf = BadFile()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.bad_parse(path, ctypes.byref(bf))
    if rc != 0:
        raise RuntimeError(f"bad_parse failed for {path!r}")
    return bf


def free_bad(bf: BadFile):
    """Free all C-side allocations inside a BadFile."""
    _bind()
    lib = load_lib()
    lib.bad_free(ctypes.byref(bf))


def default_write_options() -> BadWriteOptions:
    """Return a BadWriteOptions filled with the C stock defaults."""
    _bind()
    lib = load_lib()
    opts = BadWriteOptions()
    lib.bad_write_options_default(ctypes.byref(opts))
    return opts


def write_bad(path: str, bf: BadFile, opts: "BadWriteOptions | None" = None) -> None:
    """Serialize a BadFile to a .bad file. opts=None uses stock defaults."""
    _bind()
    lib = load_lib()
    if isinstance(path, str):
        path = path.encode("utf-8")
    opts_ptr = ctypes.byref(opts) if opts is not None else None
    rc = lib.bad_write(path, ctypes.byref(bf), opts_ptr)
    if rc != 0:
        raise RuntimeError(f"bad_write failed for {path!r}")
