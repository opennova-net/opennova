"""
ctypes bindings for the TDP project file C API (libopennova.so / opennova.dll).

Mirrors structs from libs/tdp/include/tdp/tdp.h.
"""

import ctypes

from ._native import load_lib
from .model_access import material_collision_attributes
from .threedi_ffi import (
    THREEDI_MESH_BASIC,
    THREEDI_MESH_SKINNED,
    THREEDI_MATERIAL_FLAG_ALPHA_INVERT,
    THREEDI_MATERIAL_FLAG_ALPHA_TEST,
    THREEDI_MATERIAL_FLAG_TWO_SIDED,
    THREEDI_TEX_SLOT_DETAIL,
    THREEDI_TEX_SLOT_DIFFUSE,
    THREEDI_TEX_SLOT_NORMAL,
    THREEDI_TEX_SLOT_NORMAL_B,
    ctrl_reg_name_3di3,
    decode_transform_3di3,
)

TDP_MAX_LODS = 8
TDP_MAX_ANIM_FRAMES = 8
TDP_MAX_MATERIAL_TEXTURES = 24

TDP_MATERIAL_FLAG_ALPHA_TEST = 0x01
TDP_MATERIAL_FLAG_ALPHA_INVERT = 0x02
TDP_MATERIAL_FLAG_TWO_SIDED = 0x04
TDP_MATERIAL_FLAG_EMISSIVE = 0x08

TDP_TEX_SLOT_DIFFUSE = 1
TDP_TEX_SLOT_DETAIL = 2
TDP_TEX_SLOT_NORMAL = 3
TDP_TEX_SLOT_NORMAL_B = 4


class TdpMaterialTexture(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 17),
        ("slot", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("frame", ctypes.c_uint8),
    ]


class TdpUvParams(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg", ctypes.c_int32),
        ("gen_rate", ctypes.c_float),
        ("start", ctypes.c_float),
        ("end", ctypes.c_float),
    ]


class TdpAlphaGen(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg", ctypes.c_int32),
        ("rate", ctypes.c_float),
        ("start", ctypes.c_int16),
        ("end", ctypes.c_int16),
    ]


class TdpRgbGen(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg", ctypes.c_int32),
        ("rate", ctypes.c_float),
        ("start_color", ctypes.c_float * 4),
        ("end_color", ctypes.c_float * 4),
    ]


class TdpTexAnim(ctypes.Structure):
    _fields_ = [
        ("num_frames", ctypes.c_uint8),
        ("animation_type", ctypes.c_uint8),
        ("cycle_frame_time", ctypes.c_int16),
    ]


class TdpMaterialClass(ctypes.Structure):
    _fields_ = [
        ("family", ctypes.c_uint8),
        ("bump_mode", ctypes.c_uint8),
        ("specular_mode", ctypes.c_uint8),
        ("has_detail", ctypes.c_uint8),
        ("has_overlay", ctypes.c_uint8),
        ("is_skinned", ctypes.c_uint8),
        ("is_glass", ctypes.c_uint8),
        ("is_flag", ctypes.c_uint8),
        ("luminance", ctypes.c_uint8),
        ("uv_animated", ctypes.c_uint8),
        ("blend_mode", ctypes.c_uint8),
        ("two_sided", ctypes.c_uint8),
        ("alpha_test", ctypes.c_uint8),
        ("alpha_test_invert", ctypes.c_uint8),
    ]


class TdpAnimTexture(ctypes.Structure):
    _fields_ = [
        ("path", ctypes.c_char * 32),
        ("enabled", ctypes.c_int32),
    ]


class TdpMaterial(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_int32),
        ("name", ctypes.c_char * 80),
        ("shader_name", ctypes.c_char * 33),
        ("texture_count", ctypes.c_uint32),
        ("textures", TdpMaterialTexture * TDP_MAX_MATERIAL_TEXTURES),
        ("flags", ctypes.c_uint32),
        ("material_flags", ctypes.c_uint8),
        ("alpha_test_value_byte", ctypes.c_uint8),
        ("material_pad", ctypes.c_uint8 * 2),
        ("alpha_threshold", ctypes.c_float),
        ("blend_mode", ctypes.c_int),
        ("classification", TdpMaterialClass),
        ("u_params", TdpUvParams),
        ("v_params", TdpUvParams),
        ("alpha_gen", TdpAlphaGen),
        ("rgb_gen", TdpRgbGen),
        ("rgb_gen2", TdpRgbGen),
        ("animation", TdpTexAnim),
        ("reflect_color", ctypes.c_float * 4),
        ("reflect_color2", ctypes.c_float * 4),
        ("is_glass", ctypes.c_int),
        ("glass_reflect_hi", ctypes.c_int32),
        ("glass_reflect_mid", ctypes.c_int32),
        ("glass_reflect_lo", ctypes.c_int32),
        ("reflect_type", ctypes.c_uint32),
        ("reflect_alpha", ctypes.c_uint32),
        ("specular_intensity", ctypes.c_uint32),
        ("specular_sharpness", ctypes.c_uint32),
        ("luminosity", ctypes.c_uint32),
        ("emissive_color", ctypes.c_uint32),
        ("transparency", ctypes.c_uint32),
        ("color_green", ctypes.c_uint8 * 3),
        ("color_alpha", ctypes.c_uint8 * 3),
        ("emissive_type", ctypes.c_uint8),
        ("emissive_type2", ctypes.c_uint8),
        ("glass_type2", ctypes.c_uint8),
        ("material_pad2", ctypes.c_uint8),
        ("u_offset", ctypes.c_float),
        ("v_offset", ctypes.c_float),
        ("u_tiling", ctypes.c_float),
        ("v_tiling", ctypes.c_float),
        ("uv1_u_offset", ctypes.c_float),
        ("uv1_v_offset", ctypes.c_float),
        ("uv1_u_tiling", ctypes.c_float),
        ("uv1_v_tiling", ctypes.c_float),
        ("anim_ctrlreg", ctypes.c_char * 64),
        ("anim_diffuse", (TdpAnimTexture * TDP_MAX_ANIM_FRAMES) * 2),
        ("anim_normal", (TdpAnimTexture * TDP_MAX_ANIM_FRAMES) * 2),
        ("anim_sequence", ctypes.c_uint32),
        ("surface_type", ctypes.c_uint8),
        ("surface_extra_bits", ctypes.c_uint32),
        ("pattrib", ctypes.c_uint32),
        ("geofx", ctypes.c_int32),
        ("geofx_value", ctypes.c_float),
        ("color_type", ctypes.c_uint32),
        ("actionplane_type", ctypes.c_uint32),
        ("projector_type", ctypes.c_uint32),
        ("projector_no_receive", ctypes.c_uint32),
        ("projector_yaw", ctypes.c_uint32),
        ("projector_pitch", ctypes.c_uint32),
    ]


class TdpControlRegister(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 25)]


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
        ("version",            ctypes.c_int32),
        ("poly_collision_lod", ctypes.c_int32),
        ("materials",          ctypes.POINTER(TdpMaterial)),
        ("material_count",     ctypes.c_size_t),
        ("lods",               TdpLod * TDP_MAX_LODS),
        ("ctrl_regs",          ctypes.POINTER(TdpControlRegister)),
        ("ctrl_reg_count",     ctypes.c_size_t),
        ("username",           ctypes.c_char * 64),
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
    lib.tdp_write_3da.restype = ctypes.c_int
    lib.tdp_write_3da.argtypes = [ctypes.c_char_p, ctypes.POINTER(TdpProject)]
    lib.tdp_read_3da.restype = ctypes.c_int
    lib.tdp_read_3da.argtypes = [ctypes.c_char_p, ctypes.POINTER(TdpProject)]
    lib.tdp_alloc_materials.restype = None
    lib.tdp_alloc_materials.argtypes = [ctypes.POINTER(TdpProject), ctypes.c_size_t]
    lib.tdp_alloc_ctrl_regs.restype = None
    lib.tdp_alloc_ctrl_regs.argtypes = [ctypes.POINTER(TdpProject), ctypes.c_size_t]
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


def write_3da(path: str, proj) -> None:
    """Write a .3da project file (legacy ModSuperOED format)."""
    _bind()
    lib = load_lib()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.tdp_write_3da(path, ctypes.byref(proj))
    if rc != 0:
        raise RuntimeError(f"tdp_write_3da failed for {path!r}")


def read_3da(path: str) -> TdpProject:
    """Read a .3da project file. Caller must call free_tdp() when done."""
    _bind()
    lib = load_lib()
    proj = TdpProject()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.tdp_read_3da(path, ctypes.byref(proj))
    if rc != 0:
        raise RuntimeError(f"tdp_read_3da failed for {path!r}")
    return proj


def tdp_from_3di3(model) -> TdpProject:
    """Populate a TdpProject from a direct Threedi3di3 model."""
    proj = init_tdp()
    try:
        _populate_project_ctrl_regs(proj, model)
        _populate_project_materials(proj, model)
        _populate_project_lods(proj, model)
        _populate_project_lights(proj, model)
    except Exception:
        free_tdp(proj)
        raise
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


def alloc_ctrl_regs(proj, count: int) -> None:
    """Allocate (or reallocate) the control-register array in a TdpProject."""
    _bind()
    lib = load_lib()
    lib.tdp_alloc_ctrl_regs(ctypes.byref(proj), count)


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


def _populate_project_ctrl_regs(proj: TdpProject, model) -> None:
    count = int(getattr(model, "control_register_count", 0))
    if count <= 0:
        return
    alloc_ctrl_regs(proj, count)
    for i in range(count):
        proj.ctrl_regs[i].name = _ctrl_reg_name(model, i).encode("utf-8")[:24]


def _populate_project_materials(proj: TdpProject, model) -> None:
    material_count = int(getattr(model, "material_count", 0))
    alloc_materials(proj, material_count)
    collision_attrs = material_collision_attributes(model)
    for i in range(material_count):
        src = model.materials[i]
        dst = proj.materials[i]
        dst.index = int(src.index)
        shader = _decode(src.shader_name) or "FF_ST_OP"
        dst.shader_name = shader.encode("utf-8")[:32]
        dst.name = f"Material_{i}_{shader}".encode("utf-8")[:79]
        dst.texture_count = min(int(src.texture_count), 24)
        for ti in range(dst.texture_count):
            st = src.textures[ti]
            dt = dst.textures[ti]
            dt.name = bytes(st.name)[:16]
            dt.slot = int(st.slot)
            dt.type = int(st.type)
            dt.flags = int(st.flags)
            dt.frame = int(st.frame)
        dst.material_flags = int(src.material_flags)
        dst.alpha_test_value_byte = int(src.alpha_test_value_byte)
        dst.alpha_threshold = float(src.alpha_test_value_byte) / 255.0
        dst.blend_mode = _blend_mode_from_shader(shader)
        flags = 0
        if int(src.material_flags) & THREEDI_MATERIAL_FLAG_ALPHA_TEST:
            flags |= 0x01
        if int(src.material_flags) & THREEDI_MATERIAL_FLAG_ALPHA_INVERT:
            flags |= 0x02
        if int(src.material_flags) & THREEDI_MATERIAL_FLAG_TWO_SIDED:
            flags |= 0x04
        if int(src.emissive_type) == 2 or int(src.emissive_type2) == 2:
            flags |= 0x08
        dst.flags = flags
        _populate_material_classification(
            dst.classification,
            shader,
            int(src.material_flags),
            int(src.emissive_type),
            int(src.emissive_type2),
            int(src.is_glass),
        )
        _copy_uv_params(dst.u_params, src.u_params)
        _copy_uv_params(dst.v_params, src.v_params)
        _copy_alpha_gen(dst.alpha_gen, src.alpha_gen)
        _copy_rgb_gen(dst.rgb_gen, src.rgb_gen)
        _copy_rgb_gen(dst.rgb_gen2, src.rgb_gen2)
        dst.animation.num_frames = int(src.animation.num_frames)
        dst.animation.animation_type = int(src.animation.animation_type)
        dst.animation.cycle_frame_time = int(src.animation.cycle_frame_time)
        if dst.animation.animation_type == 1:
            dst.anim_ctrlreg = _ctrl_reg_name(model, dst.animation.cycle_frame_time).encode("utf-8")[:63]
        for ci in range(4):
            dst.reflect_color[ci] = float(src.reflect_color[ci])
            dst.reflect_color2[ci] = float(src.reflect_color2[ci])
        dst.is_glass = int(src.is_glass)
        dst.emissive_type = int(src.emissive_type)
        dst.emissive_type2 = int(src.emissive_type2)
        dst.glass_type2 = int(src.glass_type2)
        surface_type, pattrib = collision_attrs.get(i, (0x01, 0))
        dst.surface_type = int(surface_type)
        dst.pattrib = int(pattrib)


def _populate_project_lods(proj: TdpProject, model) -> None:
    lod_count = min(int(getattr(model, "lod_count", 0)), TDP_MAX_LODS)
    model_name = _model_stem(model)
    mesh_type = int(getattr(model.header, "mesh_type", 0)) if hasattr(model, "header") else 0
    attributes = 1 if mesh_type == THREEDI_MESH_BASIC else 5
    for li in range(lod_count):
        src = model.lods[li]
        dst = proj.lods[li]
        scene = f"{model_name}.ase" if li == 0 else f"{model_name}_lod{li}.ase"
        dst.scene_file = scene.encode("utf-8")[:63]
        dst.attributes = attributes
        render_function = _decode(src.model_type) or "gnrc"
        dst.render_function = render_function.encode("utf-8")[:31]
        dst.threshold = float(src.lod_threshold)
        _populate_part_anims(dst, src, model)


def _populate_part_anims(dst_lod: TdpLod, src_lod, model) -> None:
    part_count = int(getattr(src_lod, "part_count", 0))
    raw_count = int(getattr(src_lod, "part_animation_count", 0))
    has_panm = raw_count > 0 and any(int(src_lod.part_animations[i].flags) != 0 for i in range(raw_count))
    count = max(part_count, raw_count if has_panm else 0)
    if count <= 0:
        return
    alloc_part_anims(dst_lod, count)
    dst_lod.part_anim_enabled = 1 if has_panm else 0
    for i in range(count):
        dst = dst_lod.part_anims[i]
        dst.transform_as = i
        if not has_panm or i >= raw_count:
            continue
        src = src_lod.part_animations[i]
        flags = int(src.flags)
        dst.transform_as = int(src.subobject_index)
        dst.scale_type = flags & 0xFF
        dst.rotate_type = (flags >> 8) & 0xFF
        dst.reverse_rotate = 1 if ((flags >> 16) & 0xFF) != 0 else 0
        dst.trans_type = (flags >> 24) & 0xFF
        if dst.rotate_type == 2:
            _transform_to_axis(src.rotation_x, dst.yaw, is_rotation=True, model=model)
            _transform_to_axis(src.rotation_y, dst.pitch, is_rotation=True, model=model)
            _transform_to_axis(src.rotation_z, dst.roll, is_rotation=True, model=model)
        if dst.scale_type == 1:
            _transform_to_axis(src.scale_x, dst.scale, is_rotation=False, model=model)
        elif dst.scale_type == 2:
            _transform_to_axis(src.scale_x, dst.scale_x, is_rotation=False, model=model)
            _transform_to_axis(src.scale_y, dst.scale_y, is_rotation=False, model=model)
            _transform_to_axis(src.scale_z, dst.scale_z, is_rotation=False, model=model)
        if dst.trans_type == 1:
            _transform_to_axis(src.translation, dst.trans_x, is_rotation=False, model=model)
        elif dst.trans_type == 2:
            _transform_to_axis(src.translation, dst.trans_y, is_rotation=False, model=model)
        elif dst.trans_type == 3:
            _transform_to_axis(src.translation, dst.trans_z, is_rotation=False, model=model)


def _populate_project_lights(proj: TdpProject, model) -> None:
    count = int(getattr(model, "light_count", 0))
    if count <= 0:
        return
    lod0 = proj.lods[0]
    alloc_lights(lod0, count)
    for i in range(count):
        src = model.lights[i]
        dst = lod0.lights[i]
        part_idx = int(src.subobj_index)
        dst.name = f"LP{part_idx + 1:02d}".encode("utf-8")[:31]
        dst.colorgen_style = int(src.style)
        dst.colorgen_rate = float(src.rate) / 256.0
        dst.colorgen_phase = float(src.phase) / 256.0
        dst.colorgen_start[0] = int(src.color_start[2])
        dst.colorgen_start[1] = int(src.color_start[1])
        dst.colorgen_start[2] = int(src.color_start[0])
        dst.colorgen_end[0] = int(src.color_end[2])
        dst.colorgen_end[1] = int(src.color_end[1])
        dst.colorgen_end[2] = int(src.color_end[0])
        dst.disable_corona = 1 if (int(src.flags) & 0x01) else 0
        dst.disable_lightterrain = 1 if (int(src.flags) & 0x02) else 0
        dst.disable_lightobjects = 1 if (int(src.flags) & 0x04) else 0
        if dst.colorgen_style > 112:
            dst.colorgen_ctrlreg = _ctrl_reg_name(model, int(src.phase)).encode("utf-8")[:63]


def _copy_uv_params(dst, src) -> None:
    dst.style = int(src.style)
    dst.phase = float(src.phase)
    dst.reg = int(src.reg)
    dst.gen_rate = float(src.gen_rate)
    dst.start = float(src.start)
    dst.end = float(src.end)


def _copy_alpha_gen(dst, src) -> None:
    dst.style = int(src.style)
    dst.phase = float(src.phase)
    dst.reg = int(src.reg)
    dst.rate = float(src.rate)
    dst.start = int(src.start)
    dst.end = int(src.end)


def _copy_rgb_gen(dst, src) -> None:
    dst.style = int(src.style)
    dst.phase = float(src.phase)
    dst.reg = int(src.reg)
    dst.rate = float(src.rate)
    for i in range(4):
        dst.start_color[i] = float(src.start_color[i])
        dst.end_color[i] = float(src.end_color[i])


def _transform_to_axis(src, dst, *, is_rotation: bool, model) -> None:
    decoded = decode_transform_3di3(src, is_rotation=is_rotation, ctrl=model.ctrl)
    dst.func_id = int(decoded.control)
    dst.param0 = float(decoded.rate)
    dst.param1 = float(decoded.phase)
    dst.param2 = float(decoded.start)
    dst.param3 = float(decoded.end)
    if decoded.ctrl_reg_name:
        dst.ctrl_reg = decoded.ctrl_reg_name[:63]


def _blend_mode_from_shader(shader: str) -> int:
    shader = shader.upper()
    if "_AD" in shader:
        return 2
    if "_AB" in shader:
        return 1
    return 0


def _populate_material_classification(
    out: TdpMaterialClass,
    shader: str,
    material_flags: int,
    emissive_type: int,
    emissive_type2: int,
    is_glass: int,
) -> None:
    tag = shader.upper()
    base = tag
    if base.endswith("#UV"):
        out.uv_animated = 1
        base = base[:-3]
    if base.endswith("_LUM"):
        out.luminance = 1
        base = base[:-4]
    if base.endswith("_AB"):
        out.blend_mode = 1
        base = base[:-3]
    elif base.endswith("_AD"):
        out.blend_mode = 2
        base = base[:-3]
    else:
        out.blend_mode = 0

    out.two_sided = 1 if material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED else 0
    out.alpha_test = 1 if material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST else 0
    out.alpha_test_invert = 1 if material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT else 0
    if emissive_type == 2 or emissive_type2 == 2:
        out.luminance = 1
    if is_glass or "GLASS" in base or base in {"FFP_GLASS", "VS_BMTXMIRRT", "VS_BUMPMIRRT"}:
        out.family = 4
        out.is_glass = 1
        if out.blend_mode == 0:
            out.blend_mode = 1
    elif "FLAG" in base:
        out.family = 5
        out.is_flag = 1
    elif "ENV" in base:
        out.family = 3
        out.bump_mode = 1
        out.specular_mode = 6
    elif "PHONG" in base:
        out.family = 1
        out.bump_mode = 0 if base == "VS_PHONGO" else 1
        out.specular_mode = 1
    elif "DOT3" in base or "BUMPDIFF" in base:
        out.family = 2
        out.bump_mode = 1
    else:
        out.family = 0
    out.is_skinned = 1 if base.startswith("VS_SK") else 0
    out.has_detail = 1 if base.startswith("FF_MT") or base.endswith("2") else 0


def _model_stem(model) -> str:
    name = _decode(getattr(model.header, "name", b"")) if hasattr(model, "header") else ""
    if name.lower().endswith(".3di"):
        name = name[:-4]
    return name or "Untitled"


def _decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def _ctrl_reg_name(model, index: int) -> str:
    if not hasattr(model, "ctrl"):
        return ""
    return ctrl_reg_name_3di3(model.ctrl, index)
