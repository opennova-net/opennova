"""
ctypes bindings for the TDP project file C API (libopennova.so / opennova.dll).

Mirrors structs from libs/tdp/include/tdp/tdp.h.
"""

import ctypes

from ._native import load_lib
from .threedi_ffi import ThreediModelIR

TDP_MAX_LODS = 8
TDP_MAX_ANIM_FRAMES = 8


# ---------------------------------------------------------------------------
# Struct definitions matching tdp.h
# ---------------------------------------------------------------------------

class TdpAxisFunc(ctypes.Structure):
    _fields_ = [
        ("func_id",  ctypes.c_int32),
        ("param0",   ctypes.c_float),
        ("param1",   ctypes.c_float),
        ("param2",   ctypes.c_float),
        ("param3",   ctypes.c_float),
        ("ctrl_reg", ctypes.c_char * 64),
    ]


class TdpPartAnim(ctypes.Structure):
    _fields_ = [
        ("rotate_type",    ctypes.c_int32),
        ("scale_type",     ctypes.c_int32),
        ("trans_type",     ctypes.c_int32),
        ("transform_as",   ctypes.c_int32),
        ("yaw_rate",       ctypes.c_float),
        ("pitch_rate",     ctypes.c_float),
        ("roll_rate",      ctypes.c_float),
        ("yaw",            TdpAxisFunc),
        ("pitch",          TdpAxisFunc),
        ("roll",           TdpAxisFunc),
        ("reverse_rotate", ctypes.c_int32),
        ("scale",          TdpAxisFunc),
        ("scale_x",        TdpAxisFunc),
        ("scale_y",        TdpAxisFunc),
        ("scale_z",        TdpAxisFunc),
        ("trans_x",        TdpAxisFunc),
        ("trans_y",        TdpAxisFunc),
        ("trans_z",        TdpAxisFunc),
    ]


class TdpAnimFrame(ctypes.Structure):
    _fields_ = [
        ("path",    ctypes.c_char * 32),
        ("enabled", ctypes.c_int32),
    ]


class TdpMaterial(ctypes.Structure):
    _fields_ = [
        ("name",              ctypes.c_char * 80),
        ("shader_tag",        ctypes.c_char * 32),
        ("rattrib",           ctypes.c_int32),
        ("pattrib",           ctypes.c_int32),
        ("ptype",             ctypes.c_int32),
        ("geofx",             ctypes.c_int32),
        ("geofx_value",       ctypes.c_float),
        ("alphatestvalue",    ctypes.c_int32),
        ("diffuse_tex",       (ctypes.c_char * 32) * 2),
        ("diffuse_flags",     ctypes.c_int32 * 2),
        ("normal_tex",        (ctypes.c_char * 32) * 2),
        ("normal_flags",      ctypes.c_int32 * 2),
        ("anim_frames",       ctypes.c_int32),
        ("anim_type",         ctypes.c_int32),
        ("anim_frametime",    ctypes.c_int32),
        ("anim_ctrlreg",      ctypes.c_char * 64),
        ("anim_diffuse",      (TdpAnimFrame * TDP_MAX_ANIM_FRAMES) * 2),
        ("anim_normal",       (TdpAnimFrame * TDP_MAX_ANIM_FRAMES) * 2),
        ("reflect_rgb",       ctypes.c_int32 * 3),
        ("rgbgen_style",      ctypes.c_int32),
        ("rgbgen_rate",       ctypes.c_float),
        ("rgbgen_phase",      ctypes.c_float),
        ("rgbgen_srgb",       ctypes.c_int32 * 3),
        ("rgbgen_ergb",       ctypes.c_int32 * 3),
        ("rgbgen_ctrlreg",    ctypes.c_char * 64),
        ("alphagen_style",    ctypes.c_int32),
        ("alphagen_rate",     ctypes.c_float),
        ("alphagen_phase",    ctypes.c_float),
        ("alphagen_start",    ctypes.c_float),
        ("alphagen_end",      ctypes.c_float),
        ("alphagen_ctrlreg",  ctypes.c_char * 64),
        ("mapfunc_u_style",   ctypes.c_int32),
        ("mapfunc_u_rate",    ctypes.c_float),
        ("mapfunc_u_phase",   ctypes.c_float),
        ("mapfunc_u_start",   ctypes.c_float),
        ("mapfunc_u_end",     ctypes.c_float),
        ("mapfunc_u_ctrlreg", ctypes.c_char * 64),
        ("mapfunc_v_style",   ctypes.c_int32),
        ("mapfunc_v_rate",    ctypes.c_float),
        ("mapfunc_v_phase",   ctypes.c_float),
        ("mapfunc_v_start",   ctypes.c_float),
        ("mapfunc_v_end",     ctypes.c_float),
        ("mapfunc_v_ctrlreg", ctypes.c_char * 64),
    ]


class TdpLight(ctypes.Structure):
    _fields_ = [
        ("name",                  ctypes.c_char * 32),
        ("colorgen_style",        ctypes.c_int32),
        ("colorgen_rate",         ctypes.c_float),
        ("colorgen_phase",        ctypes.c_float),
        ("colorgen_start",        ctypes.c_int32 * 3),
        ("colorgen_end",          ctypes.c_int32 * 3),
        ("colorgen_ctrlreg",      ctypes.c_char * 64),
        ("disable_corona",        ctypes.c_int32),
        ("disable_lightterrain",  ctypes.c_int32),
        ("disable_lightobjects",  ctypes.c_int32),
    ]


class TdpLod(ctypes.Structure):
    _fields_ = [
        ("scene_file",        ctypes.c_char * 64),
        ("attributes",        ctypes.c_int32),
        ("render_function",   ctypes.c_char * 32),
        ("threshold",         ctypes.c_float),
        ("part_anim_enabled", ctypes.c_int32),
        ("part_anims",        ctypes.POINTER(TdpPartAnim)),
        ("part_anim_count",   ctypes.c_size_t),
        ("lights",            ctypes.POINTER(TdpLight)),
        ("light_count",       ctypes.c_size_t),
    ]


class TdpProject(ctypes.Structure):
    _fields_ = [
        ("version",           ctypes.c_int32),
        ("poly_collision_lod", ctypes.c_int32),
        ("materials",         ctypes.POINTER(TdpMaterial)),
        ("material_count",    ctypes.c_size_t),
        ("lods",              TdpLod * TDP_MAX_LODS),
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
    lib.tdp_init.restype = None
    lib.tdp_init.argtypes = [ctypes.POINTER(TdpProject)]
    lib.tdp_free.restype = None
    lib.tdp_free.argtypes = [ctypes.POINTER(TdpProject)]
    lib.tdp_parse.restype = ctypes.c_int
    lib.tdp_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(TdpProject)]
    lib.tdp_write.restype = ctypes.c_int
    lib.tdp_write.argtypes = [ctypes.c_char_p, ctypes.POINTER(TdpProject)]
    lib.tdp_from_ir.restype = ctypes.c_int
    lib.tdp_from_ir.argtypes = [ctypes.POINTER(ThreediModelIR), ctypes.POINTER(TdpProject)]
    lib.tdp_alloc_materials.restype = None
    lib.tdp_alloc_materials.argtypes = [ctypes.POINTER(TdpProject), ctypes.c_size_t]
    lib.tdp_alloc_part_anims.restype = None
    lib.tdp_alloc_part_anims.argtypes = [ctypes.POINTER(TdpLod), ctypes.c_size_t]
    lib.tdp_alloc_lights.restype = None
    lib.tdp_alloc_lights.argtypes = [ctypes.POINTER(TdpLod), ctypes.c_size_t]
    _bound = True


def parse_tdp(path: str) -> TdpProject:
    """Parse a .3dp project file. Caller must call free_tdp() when done."""
    _bind()
    lib = load_lib()
    proj = TdpProject()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.tdp_parse(path, ctypes.byref(proj))
    if rc != 0:
        raise RuntimeError(f"tdp_parse failed for {path!r}")
    return proj


def write_tdp(path: str, proj) -> None:
    """Write a .3dp project file."""
    _bind()
    lib = load_lib()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.tdp_write(path, ctypes.byref(proj))
    if rc != 0:
        raise RuntimeError(f"tdp_write failed for {path!r}")


def tdp_from_ir(ir) -> TdpProject:
    """Populate a TdpProject from a ThreediModelIR. Caller must call free_tdp() when done."""
    _bind()
    lib = load_lib()
    proj = TdpProject()
    rc = lib.tdp_from_ir(ctypes.byref(ir), ctypes.byref(proj))
    if rc != 0:
        raise RuntimeError("tdp_from_ir failed")
    return proj


def init_tdp() -> TdpProject:
    """Create a zero-initialized TdpProject."""
    _bind()
    lib = load_lib()
    proj = TdpProject()
    lib.tdp_init(ctypes.byref(proj))
    return proj


def free_tdp(proj) -> None:
    """Free all C-side allocations inside a TdpProject."""
    _bind()
    lib = load_lib()
    lib.tdp_free(ctypes.byref(proj))


def alloc_materials(proj, count: int) -> None:
    """Allocate (or reallocate) the materials array in a TdpProject."""
    _bind()
    lib = load_lib()
    lib.tdp_alloc_materials(ctypes.byref(proj), count)


def alloc_part_anims(lod, count: int) -> None:
    """Allocate (or reallocate) the part_anims array in a TdpLod."""
    _bind()
    lib = load_lib()
    lib.tdp_alloc_part_anims(ctypes.byref(lod), count)


def alloc_lights(lod, count: int) -> None:
    """Allocate (or reallocate) the lights array in a TdpLod."""
    _bind()
    lib = load_lib()
    lib.tdp_alloc_lights(ctypes.byref(lod), count)
