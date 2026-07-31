"""DCC-neutral material interpretation for OpenNova 3DI3 model data.

This module intentionally contains no Blender or PyMXS imports.  DCC embedders use
the descriptors here to build their own native material objects while sharing
texture-slot parsing, path resolution, diagnostics, and export metadata.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple


THREEDI_TEX_SLOT_DIFFUSE = 1
THREEDI_TEX_SLOT_DETAIL = 2
THREEDI_TEX_SLOT_NORMAL = 3
THREEDI_TEX_SLOT_NORMAL_B = 4

THREEDI_MATERIAL_FLAG_ALPHA_TEST = 0x01
THREEDI_MATERIAL_FLAG_ALPHA_INVERT = 0x02
THREEDI_MATERIAL_FLAG_TWO_SIDED = 0x04
THREEDI_MATERIAL_FLAG_EMISSIVE = 0x08

# The 3DI3 MTRL flag byte uses the same low bits as the normalized material flags.
THREEDI_EMISSIVE_FULL = 2

MATERIAL_BLEND_OPAQUE = "opaque"
MATERIAL_BLEND_ALPHA = "alpha_blend"
MATERIAL_BLEND_ADDITIVE = "additive"
MATERIAL_BLEND_MULTIPLICATIVE = "multiplicative"

MATERIAL_SHADER_UNKNOWN = "unknown"
MATERIAL_SHADER_FIXED_FUNCTION = "fixed_function"
MATERIAL_SHADER_PHONG = "phong"
MATERIAL_SHADER_FLAG = "flag"
MATERIAL_SHADER_DOT3 = "dot3"
MATERIAL_SHADER_ENVIRONMENT = "environment"
MATERIAL_SHADER_GLASS = "glass"

MATERIAL_NORMAL_NONE = "none"
MATERIAL_NORMAL_TANGENT = "tangent"
MATERIAL_NORMAL_OBJECT = "object"

_MAT_FLAG_EMISSIVE = 0x0001
_MAT_FLAG_ALPHA = 0x0002
_MAT_FLAG_DIFFUSE = 0x0004
_MAT_FLAG_SECONDARY = 0x0008
_MAT_FLAG_NORMAL_A = 0x0010
_MAT_FLAG_NORMAL_B = 0x0020
_MAT_FLAG_SPECIAL = 0x1000
_MAT_FLAG_GLASS = 0x2000
_MAT_FLAG_FILTER = 0x4000
_MAT_FLAG_SMOOTH = 0x8000
_MAT_FLAG_UI_TOGGLE = 0x10000
_MAT_FLAG_LUMINANCE = 0x10000000

_MATERIAL_INFO_FLAGS = {
    "FF_ST_OP": _MAT_FLAG_DIFFUSE,
    "FF_ST_OP#UV": _MAT_FLAG_DIFFUSE | _MAT_FLAG_UI_TOGGLE,
    "FF_ST_AB": _MAT_FLAG_ALPHA | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL,
    "FF_ST_AB#UV": _MAT_FLAG_ALPHA | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_ST_AD": _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL,
    "FF_ST_AD#UV": _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_ST_OP_LUM": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE,
    "FF_ST_OP_LUM#UV": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_UI_TOGGLE,
    "FF_ST_AB_LUM": _MAT_FLAG_LUMINANCE | _MAT_FLAG_ALPHA | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL,
    "FF_ST_AB_LUM#UV": _MAT_FLAG_LUMINANCE | _MAT_FLAG_ALPHA | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_ST_AD_LUM": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL,
    "FF_ST_AD_LUM#UV": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_MT_OP": _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY,
    "FF_MT_OP#UV": _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_UI_TOGGLE,
    "FF_MT_AB": _MAT_FLAG_ALPHA | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL,
    "FF_MT_AB#UV": _MAT_FLAG_ALPHA | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_MT_AD": _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL,
    "FF_MT_AD#UV": _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_MT_OP_LUM": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY,
    "FF_MT_OP_LUM#UV": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_UI_TOGGLE,
    "FF_MT_AB_LUM": _MAT_FLAG_LUMINANCE | _MAT_FLAG_ALPHA | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL,
    "FF_MT_AB_LUM#UV": _MAT_FLAG_LUMINANCE | _MAT_FLAG_ALPHA | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FF_MT_AD_LUM": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL,
    "FF_MT_AD_LUM#UV": _MAT_FLAG_LUMINANCE | _MAT_FLAG_EMISSIVE | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY | _MAT_FLAG_SPECIAL | _MAT_FLAG_UI_TOGGLE,
    "FFP_GLASS": _MAT_FLAG_GLASS | _MAT_FLAG_SMOOTH | _MAT_FLAG_SPECIAL,
    "VS_DOT3DIFFOBJ": _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_PHONGO": _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_DOT3DIFF": _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_DOT3DIFF#UV": _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE | _MAT_FLAG_UI_TOGGLE,
    "VS_PHONGT": _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_PHONGT#UV": _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE | _MAT_FLAG_UI_TOGGLE,
    "VS_PHONGT_MDT": _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_DOT3DIFF2": _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY,
    "VS_BMTXMIRRT": _MAT_FLAG_GLASS | _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_BUMPMIRRT": _MAT_FLAG_GLASS | _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_ENVPHONGT": _MAT_FLAG_GLASS | _MAT_FLAG_SMOOTH | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_SKBASIC": _MAT_FLAG_FILTER | _MAT_FLAG_DIFFUSE,
    "VS_SKBASIC#UV": _MAT_FLAG_FILTER | _MAT_FLAG_DIFFUSE | _MAT_FLAG_UI_TOGGLE,
    "VS_SKGLASS": _MAT_FLAG_GLASS | _MAT_FLAG_FILTER | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SPECIAL,
    "VS_SKBUMPDIFFOBJ": _MAT_FLAG_FILTER | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_SKBUMPPHONGOBJ": _MAT_FLAG_FILTER | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_SKBUMPDIFFOBJ2": _MAT_FLAG_FILTER | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY,
    "VS_SKBUMPDIFFT": _MAT_FLAG_FILTER | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_SKBUMPPHONGT": _MAT_FLAG_FILTER | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE,
    "VS_SKBUMPDIFFT2": _MAT_FLAG_FILTER | _MAT_FLAG_NORMAL_A | _MAT_FLAG_DIFFUSE | _MAT_FLAG_SECONDARY,
    "VS_FLAG": _MAT_FLAG_DIFFUSE,
}

NORMAL_TYPE_MDT = 4
NORMAL_TYPE_TGA_ALPHA = 5

MAX_TEX_FILENAME = 15


@dataclass(frozen=True)
class MaterialShaderSemantics:
    """Static shader-tag classification shared by DCC material builders."""

    known_shader: bool = False
    shader: str = ""
    resolved_shader: str = ""
    family: str = MATERIAL_SHADER_UNKNOWN
    blend: str = MATERIAL_BLEND_OPAQUE
    is_emissive: bool = False
    is_luminance: bool = False
    is_two_sided: bool = False
    needs_normal_map: bool = False
    is_glass: bool = False
    uses_environment: bool = False
    alpha_test: bool = False
    alpha_test_invert: bool = False
    alpha_test_value: float = 0.0
    has_detail: bool = False
    is_skinned: bool = False
    uses_specular: bool = False
    normal_uses_uv2: bool = False
    normal_space: str = MATERIAL_NORMAL_NONE


@dataclass(frozen=True)
class TextureDescriptor:
    """One interpreted material texture slot."""

    role: str
    name: str = ""
    path: Optional[str] = None
    texture_index: int = -1
    slot: int = 0
    type: int = 0
    flags: int = 0
    frame: int = 0

    @property
    def missing(self) -> bool:
        return bool(self.name and not self.path)


@dataclass(frozen=True)
class GeneratorDescriptor:
    style: int = 0
    phase: float = 0.0
    reg: int = -1
    rate: float = 0.0
    start: Any = 0.0
    end: Any = 0.0
    ctrlreg: str = ""


@dataclass(frozen=True)
class RgbGeneratorDescriptor:
    style: int = 0
    phase: float = 0.0
    reg: int = -1
    rate: float = 0.0
    start_color: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    end_color: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    ctrlreg: str = ""


@dataclass(frozen=True)
class TexAnimDescriptor:
    num_frames: int = 0
    animation_type: int = 0
    cycle_frame_time: int = 0


def classify_material_shader(
    shader_name: str,
    material_flags: int = 0,
    emissive_type: int = 0,
    is_glass: int | bool = 0,
    alpha_test_value_byte: int = 0,
) -> MaterialShaderSemantics:
    """Classify a 3DI shader tag without parsing effect files."""

    shader = _normalize_shader(shader_name)
    flags = int(material_flags) & 0xFF
    alpha_byte = max(0, min(int(alpha_test_value_byte), 255))
    alpha_test = bool(flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST)
    alpha_invert = bool(flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT)

    resolved = shader
    if resolved == "VS_LEAVESWIND":
        resolved = "FF_ST_OP" if alpha_test else "FF_ST_AB"

    info_flags = _MATERIAL_INFO_FLAGS.get(resolved, 0)
    family = _shader_family_from_tag(resolved)
    blend = _blend_from_tag(resolved, info_flags)
    is_glass_semantic = (
        bool(info_flags & _MAT_FLAG_GLASS)
        or bool(is_glass)
        or family == MATERIAL_SHADER_GLASS
    )
    if is_glass_semantic and blend == MATERIAL_BLEND_OPAQUE:
        blend = MATERIAL_BLEND_ALPHA
    if family == MATERIAL_SHADER_UNKNOWN and "WIND" in resolved:
        family = MATERIAL_SHADER_FLAG

    needs_normal = bool(info_flags & (_MAT_FLAG_NORMAL_A | _MAT_FLAG_NORMAL_B))
    normal_space = _normal_space_for_tag(resolved, needs_normal)

    return MaterialShaderSemantics(
        known_shader=info_flags != 0,
        shader=shader,
        resolved_shader=resolved,
        family=family,
        blend=blend,
        is_emissive=(
            emissive_type == THREEDI_EMISSIVE_FULL
            or bool(info_flags & _MAT_FLAG_EMISSIVE)
        ),
        is_luminance=bool(info_flags & _MAT_FLAG_LUMINANCE),
        is_two_sided=bool(flags & THREEDI_MATERIAL_FLAG_TWO_SIDED),
        needs_normal_map=needs_normal,
        is_glass=is_glass_semantic,
        uses_environment=family in (MATERIAL_SHADER_ENVIRONMENT, MATERIAL_SHADER_GLASS),
        alpha_test=alpha_test,
        alpha_test_invert=alpha_invert,
        alpha_test_value=float(alpha_byte) / 255.0,
        has_detail=bool(info_flags & _MAT_FLAG_SECONDARY),
        is_skinned=resolved.startswith("VS_SK"),
        uses_specular=family in (
            MATERIAL_SHADER_PHONG,
            MATERIAL_SHADER_ENVIRONMENT,
            MATERIAL_SHADER_GLASS,
        ),
        normal_uses_uv2=bool(info_flags & _MAT_FLAG_NORMAL_B),
        normal_space=normal_space,
    )


@dataclass(frozen=True)
class MaterialDescriptor:
    index: int
    shader: str
    name: str
    diffuse: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("diffuse"))
    detail: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("detail"))
    normal: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("normal"))
    secondary_normal: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("secondary_normal"))
    textures: Tuple[TextureDescriptor, ...] = ()
    unknown_textures: Tuple[TextureDescriptor, ...] = ()
    flags: int = 0
    material_flags: int = 0
    source_material_flags: int = 0
    alpha_test_value_byte: int = 0
    alpha_threshold: float = 0.0
    blend_mode: int = 0
    renderer_blend: str = MATERIAL_BLEND_OPAQUE
    alpha_test: bool = False
    alpha_inverted: bool = False
    two_sided: bool = False
    emissive: bool = False
    emissive_type: int = 0
    emissive_type2: int = 0
    glass: bool = False
    glass_type2: int = 0
    reflect_color: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    reflect_color2: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    specular_intensity: int = 0
    specular_strength: float = 0.0
    viewport_specular: float = 0.0
    viewport_roughness: float = 1.0
    luminosity: int = 0
    luminosity_strength: float = 0.0
    u_tiling: float = 0.0
    v_tiling: float = 0.0
    effective_u_tiling: float = 1.0
    effective_v_tiling: float = 1.0
    # Channel-1 (uv1) tiling. The model's MTRL chunk doesn't store this directly,
    # but it's recoverable from per-vertex (uv0, uv1) ratios because OED's
    # WriteRDTA emits uv1 = (uv0 - 0.5) * tiling + 0.5 (see WriteRDTA @
    # 0x459a04 in ModSuperOed.exe). Set 1.0 when no transform should apply.
    effective_u1_tiling: float = 1.0
    effective_v1_tiling: float = 1.0
    has_custom_tiling: bool = False
    bump_mode: str = ""
    bump_uses_alpha: bool = False
    phong_shader: bool = False
    bump_shader: bool = False
    known_shader: bool = False
    shader_family: str = MATERIAL_SHADER_UNKNOWN
    resolved_shader: str = ""
    needs_normal_map: bool = False
    normal_space: str = MATERIAL_NORMAL_NONE
    normal_uses_uv2: bool = False
    has_detail_slot: bool = False
    uses_specular: bool = False
    uses_environment: bool = False
    is_luminance: bool = False
    is_skinned: bool = False
    u_params: GeneratorDescriptor = field(default_factory=GeneratorDescriptor)
    v_params: GeneratorDescriptor = field(default_factory=GeneratorDescriptor)
    alpha_gen: GeneratorDescriptor = field(default_factory=GeneratorDescriptor)
    rgb_gen: RgbGeneratorDescriptor = field(default_factory=RgbGeneratorDescriptor)
    rgb_gen2: RgbGeneratorDescriptor = field(default_factory=RgbGeneratorDescriptor)
    tex_anim: TexAnimDescriptor = field(default_factory=TexAnimDescriptor)


def derive_uv1_tilings(ir) -> Dict[int, Tuple[float, float]]:
    """Back-calculate per-material (uv1_u_tiling, uv1_v_tiling) from per-vertex
    (uv0, uv1) ratios across all LODs.

    The model's MTRL chunk stores no uv1 tiling, but the per-vertex uv1 carries
    the transform OED's WriteRDTA applied at bake time:
        uv1 = (uv0 - 0.5) * tiling + 0.5    (see WriteRDTA @ 0x459a04)
    so   tiling = (uv1 - 0.5) / (uv0 - 0.5).
    Robust to the (uv0 == 0.5) edge by skipping near-pivot samples and taking
    the median. Returns 1.0 (identity transform) when no usable samples exist.
    """
    EPS = 1e-4
    out: Dict[int, Tuple[float, float]] = {}
    mat_count = int(getattr(ir, "material_count", 0))
    for mat_idx in range(mat_count):
        u_ratios: list[float] = []
        v_ratios: list[float] = []
        for lod_i in range(int(getattr(ir, "lod_count", 0))):
            lod = ir.lods[lod_i]
            primitive_part_indices = _primitive_part_indices(lod)
            for prim_i in range(int(getattr(lod, "primitive_count", 0))):
                p = lod.primitives[prim_i]
                if int(p.material_index) != mat_idx:
                    continue
                vstart = int(p.vertex_offset)
                vcount = int(p.vertex_count)
                for vi in range(vstart, vstart + vcount):
                    v = lod.vertices[vi]
                    u0 = float(v.uv0[0]) - 0.5
                    u1 = float(v.uv1[0]) - 0.5
                    v0 = float(v.uv0[1]) - 0.5
                    v1 = float(v.uv1[1]) - 0.5
                    if abs(u0) > EPS:
                        u_ratios.append(u1 / u0)
                    if abs(v0) > EPS:
                        v_ratios.append(v1 / v0)
        u_tiling = _median(u_ratios) if u_ratios else 1.0
        v_tiling = _median(v_ratios) if v_ratios else 1.0
        out[mat_idx] = (u_tiling, v_tiling)
    return out


def _primitive_part_indices(lod) -> list[int]:
    count = int(getattr(lod, "primitive_count", 0))
    out = [-1] * count
    parts = getattr(lod, "parts", None)
    part_count = int(getattr(lod, "part_count", 0))
    cursor = 0
    for part_idx in range(part_count):
        part = parts[part_idx]
        prim_count = int(getattr(part, "primitive_count", 0))
        if prim_count <= 0:
            prim_count = int(getattr(part, "num_strips", 0)) + int(getattr(part, "num_alpha_strips", 0))
        for _ in range(max(0, prim_count)):
            if cursor >= count:
                return out
            out[cursor] = part_idx
            cursor += 1
    return out


def _median(values: list[float]) -> float:
    s = sorted(values)
    n = len(s)
    if n == 0:
        return 1.0
    if n % 2 == 1:
        return s[n // 2]
    return 0.5 * (s[n // 2 - 1] + s[n // 2])


def describe_material(
    ir_mat,
    resolver=None,
    ctrl_resolver=None,
    *,
    source_format: int | None = None,
    texture_strategy: str | None = None,
    uv1_tiling_override: Tuple[float, float] | None = None,
) -> MaterialDescriptor:
    """Interpret one ``TdpMaterial`` into DCC-neutral material data.

    ``uv1_tiling_override`` (when provided) sets the channel-1 tiling that the
    model's MTRL chunk doesn't natively carry. Compute it once per 3DI3 model via
    ``derive_uv1_tilings(ir)`` and pass the per-material entry in.
    """

    index = _int_attr(ir_mat, "index", 0)
    shader = _decode(_attr(ir_mat, "shader_name", b"")).strip() or "FF_ST_OP"
    name = "Material_%d_%s" % (index, shader)
    flags = _int_attr(ir_mat, "flags", 0)
    diffuse, detail, normal, secondary_normal, all_textures = _texture_descriptors(
        shader,
        ir_mat,
        resolver,
        source_format=source_format,
        texture_strategy=texture_strategy,
    )
    unknown_textures = tuple(tex for tex in all_textures if tex.role == "unknown")

    emissive_type = _int_attr(ir_mat, "emissive_type", 0)
    emissive_type2 = _int_attr(ir_mat, "emissive_type2", 0)
    is_glass_flag = _int_attr(ir_mat, "is_glass", 0)
    source_material_flags = _int_attr(ir_mat, "material_flags", 0) & 0xFF
    material_flags = source_material_flags | _material_flags_from_ir_flags(flags)
    alpha_threshold = _float_attr(ir_mat, "alpha_threshold", 0.0)
    alpha_test_value_byte = _effective_alpha_test_byte(ir_mat, alpha_threshold)
    semantics = classify_material_shader(
        shader,
        material_flags=material_flags,
        emissive_type=emissive_type,
        is_glass=is_glass_flag,
        alpha_test_value_byte=alpha_test_value_byte,
    )

    specular_intensity = _int_attr(ir_mat, "specular_intensity", 0)
    specular_strength = min(float(specular_intensity) / 255.0, 1.0) if specular_intensity > 0 else 0.0
    phong_shader = semantics.family in (
        MATERIAL_SHADER_PHONG,
        MATERIAL_SHADER_ENVIRONMENT,
        MATERIAL_SHADER_GLASS,
    )
    bump_shader = semantics.needs_normal_map
    descriptor_needs_normal = semantics.needs_normal_map or bool(normal.name or secondary_normal.name)
    viewport_specular = specular_strength
    viewport_roughness = 1.0 - specular_strength * 0.7 if specular_strength > 0.0 else 1.0
    if semantics.uses_specular and viewport_specular <= 0.0:
        viewport_specular = 0.3
        viewport_roughness = 0.5

    luminosity = _int_attr(ir_mat, "luminosity", 0)
    luminosity_strength = min(float(luminosity) / 255.0, 1.0) if luminosity > 0 else 0.0
    u_tiling = _float_attr(ir_mat, "u_tiling", 0.0)
    v_tiling = _float_attr(ir_mat, "v_tiling", 0.0)
    effective_u_tiling = 1.0 if u_tiling == 0.0 else u_tiling
    effective_v_tiling = 1.0 if v_tiling == 0.0 else v_tiling
    has_custom_tiling = (
        (u_tiling != 0.0 and u_tiling != 1.0)
        or (v_tiling != 0.0 and v_tiling != 1.0)
    )
    if uv1_tiling_override is not None:
        effective_u1_tiling = float(uv1_tiling_override[0])
        effective_v1_tiling = float(uv1_tiling_override[1])
    else:
        # MTRL doesn't carry uv1 tiling natively; default to identity (1.0)
        # which matches OED's "skip transform" path when uv1_u_tiling == 0.0.
        effective_u1_tiling = 1.0
        effective_v1_tiling = 1.0

    bump_mode = ""
    bump_uses_alpha = False
    bump_texture = normal if normal.name else secondary_normal
    if bump_texture.name and bump_texture.path and bump_texture.type != NORMAL_TYPE_MDT:
        bump_mode = "normal_texture"
        bump_uses_alpha = bump_texture.type == NORMAL_TYPE_TGA_ALPHA
    # Phong/DOT3 shaders without a slot-3 texture stay flat-shaded. The previous
    # diffuse-alpha fallback (height from diffuse alpha, mimicking gsys_phong)
    # produced visible shading distortion on most assets because most VS_PHONGT
    # textures don't actually encode height in alpha — they use it for opacity
    # or leave it unused. Without a per-material flag in the 3DI3 model to gate this,
    # the safe default is no bump unless the model carries that signal.

    return MaterialDescriptor(
        index=index,
        shader=shader,
        name=name,
        diffuse=diffuse,
        detail=detail,
        normal=normal,
        secondary_normal=secondary_normal,
        textures=all_textures,
        unknown_textures=unknown_textures,
        flags=flags,
        material_flags=material_flags,
        source_material_flags=source_material_flags,
        alpha_test_value_byte=alpha_test_value_byte,
        alpha_threshold=alpha_threshold,
        blend_mode=_int_attr(ir_mat, "blend_mode", 0),
        renderer_blend=semantics.blend,
        alpha_test=semantics.alpha_test,
        alpha_inverted=semantics.alpha_test_invert,
        two_sided=semantics.is_two_sided,
        emissive=(
            bool(flags & THREEDI_MATERIAL_FLAG_EMISSIVE)
            or emissive_type != 0
            or emissive_type2 != 0
            or semantics.is_emissive
            or semantics.is_luminance
        ),
        emissive_type=emissive_type,
        emissive_type2=emissive_type2,
        glass=semantics.is_glass,
        glass_type2=_int_attr(ir_mat, "glass_type2", 0),
        reflect_color=_float4_attr(ir_mat, "reflect_color"),
        reflect_color2=_float4_attr(ir_mat, "reflect_color2"),
        specular_intensity=specular_intensity,
        specular_strength=specular_strength,
        viewport_specular=viewport_specular,
        viewport_roughness=viewport_roughness,
        luminosity=luminosity,
        luminosity_strength=luminosity_strength,
        u_tiling=u_tiling,
        v_tiling=v_tiling,
        effective_u_tiling=effective_u_tiling,
        effective_v_tiling=effective_v_tiling,
        effective_u1_tiling=effective_u1_tiling,
        effective_v1_tiling=effective_v1_tiling,
        has_custom_tiling=has_custom_tiling,
        bump_mode=bump_mode,
        bump_uses_alpha=bump_uses_alpha,
        phong_shader=phong_shader,
        bump_shader=bump_shader,
        known_shader=semantics.known_shader,
        shader_family=semantics.family,
        resolved_shader=semantics.resolved_shader,
        needs_normal_map=descriptor_needs_normal,
        normal_space=semantics.normal_space if descriptor_needs_normal else MATERIAL_NORMAL_NONE,
        normal_uses_uv2=semantics.normal_uses_uv2,
        has_detail_slot=semantics.has_detail or bool(detail.name),
        uses_specular=semantics.uses_specular,
        uses_environment=semantics.uses_environment,
        is_luminance=semantics.is_luminance,
        is_skinned=semantics.is_skinned,
        u_params=_uv_params(_attr(ir_mat, "u_params", None), ctrl_resolver),
        v_params=_uv_params(_attr(ir_mat, "v_params", None), ctrl_resolver),
        alpha_gen=_alpha_gen(_attr(ir_mat, "alpha_gen", None), ctrl_resolver),
        rgb_gen=_rgb_gen(_attr(ir_mat, "rgb_gen", None), ctrl_resolver),
        rgb_gen2=_rgb_gen(_attr(ir_mat, "rgb_gen2", None), ctrl_resolver),
        tex_anim=_tex_anim(_attr(ir_mat, "animation", None)),
    )


def describe_materials(ir, resolver=None, ctrl_resolver=None) -> List[MaterialDescriptor]:
    """Interpret all materials in a model data."""

    out = []
    source_format = _int_attr(ir, "source_format", 0)
    for i in range(_int_attr(ir, "material_count", 0)):
        out.append(
            describe_material(
                ir.materials[i],
                resolver=resolver,
                ctrl_resolver=ctrl_resolver,
                source_format=source_format,
            )
        )
    return out


def descriptor_from_user_props(
    props: Dict[str, Any],
    *,
    resolver=None,
) -> MaterialDescriptor:
    """Rebuild a MaterialDescriptor from a DCC-stored user-props dict.

    Used by DCC embedders that round-trip materials through a DCC scene (e.g. the
    Blender ASE exporter, which reads the props ``BlenderSceneBuilder`` stored
    via ``material_user_props`` during import). Fields the DCC did not store
    fall back to defaults; ``classify_material_shader`` derives the semantic
    signals from the shader tag.
    """

    def g(key: str, default: Any = None) -> Any:
        return props.get(key, default)

    def gi(key: str, default: int = 0) -> int:
        try:
            return int(g(key, default))
        except (TypeError, ValueError):
            return int(default)

    def gf(key: str, default: float = 0.0) -> float:
        try:
            return float(g(key, default))
        except (TypeError, ValueError):
            return float(default)

    def gs(key: str, default: str = "") -> str:
        value = g(key, default)
        return str(value) if value is not None else default

    def gb(key: str, default: bool = False) -> bool:
        return bool(g(key, default))

    def texture(role: str) -> TextureDescriptor:
        name = gs("opennova_%s_texture_name" % role)
        path = gs("opennova_%s_texture_path" % role) or None
        if path is not None and resolver is not None and not os.path.isfile(path):
            try:
                resolved = resolver.resolve_texture(name) if name else None
            except Exception:
                resolved = None
            if resolved and os.path.isfile(str(resolved)):
                path = str(resolved)
        type_key = "opennova_%s_type" % role if role != "diffuse" else None
        tex_type = gi(type_key, 0) if type_key else 0
        return TextureDescriptor(
            role=role,
            name=name,
            path=path,
            type=tex_type,
        )

    diffuse = texture("diffuse")
    detail = texture("detail")
    normal = texture("normal")
    secondary_normal = texture("secondary_normal")

    # Indexed texture array (opennova_texture_NN_*).
    indexed: Dict[int, Dict[str, Any]] = {}
    prefix = "opennova_texture_"
    for key in props:
        if not key.startswith(prefix):
            continue
        rest = key[len(prefix):]
        idx_str, _, field = rest.partition("_")
        if not idx_str.isdigit() or not field:
            continue
        indexed.setdefault(int(idx_str), {})[field] = props[key]
    textures: List[TextureDescriptor] = []
    for idx in sorted(indexed):
        entry = indexed[idx]
        textures.append(
            TextureDescriptor(
                role=str(entry.get("role", "unknown")),
                name=str(entry.get("name", "")),
                path=str(entry.get("path", "")) or None,
                texture_index=idx,
                slot=int(entry.get("slot", 0) or 0),
                type=int(entry.get("type", 0) or 0),
                flags=int(entry.get("flags", 0) or 0),
                frame=int(entry.get("frame", 0) or 0),
            )
        )

    shader = gs("opennova_shader") or "FF_ST_OP"
    index = gi("opennova_material_index", 0)
    name = gs("ase_material_name") or "Material_%d_%s" % (index, shader)

    material_flags = gi("opennova_material_flags", 0)
    source_material_flags = gi("opennova_material_flags_raw", material_flags & 0xFF)
    alpha_test_byte = gi("opennova_alpha_test_byte", 0)
    alpha_threshold = gf("alpha_threshold", float(alpha_test_byte) / 255.0)

    semantics = classify_material_shader(
        shader,
        material_flags=material_flags,
        emissive_type=gi("emissive_type", 0),
        is_glass=int(bool(g("reflect_color"))),
        alpha_test_value_byte=alpha_test_byte,
    )

    renderer_blend = gs("opennova_renderer_blend") or semantics.blend
    blend_mode = gi("blend_mode", 0)
    bump_mode = gs("opennova_bump_mode")
    bump_uses_alpha = gb("opennova_bump_uses_alpha", False)
    has_custom_tiling = "uv_u_tiling" in props or "uv_v_tiling" in props
    u_tiling = gf("uv_u_tiling", 0.0)
    v_tiling = gf("uv_v_tiling", 0.0)
    effective_u_tiling = u_tiling if u_tiling > 0.0 else 1.0
    effective_v_tiling = v_tiling if v_tiling > 0.0 else 1.0

    reflect = g("reflect_color")
    reflect_color = (
        tuple(float(v) for v in reflect)
        if reflect and hasattr(reflect, "__iter__")
        else (0.0, 0.0, 0.0, 0.0)
    )
    if len(reflect_color) < 4:
        reflect_color = tuple(list(reflect_color) + [0.0] * (4 - len(reflect_color)))[:4]

    reflect2 = g("reflect_color2")
    reflect_color2 = (
        tuple(float(v) for v in reflect2)
        if reflect2 and hasattr(reflect2, "__iter__")
        else (0.0, 0.0, 0.0, 0.0)
    )
    if len(reflect_color2) < 4:
        reflect_color2 = tuple(list(reflect_color2) + [0.0] * (4 - len(reflect_color2)))[:4]

    return MaterialDescriptor(
        index=index,
        shader=shader,
        name=name,
        diffuse=diffuse,
        detail=detail,
        normal=normal,
        secondary_normal=secondary_normal,
        textures=tuple(textures),
        unknown_textures=tuple(t for t in textures if t.role == "unknown"),
        flags=gi("opennova_material_flags", 0),
        material_flags=material_flags,
        source_material_flags=source_material_flags,
        alpha_test_value_byte=alpha_test_byte,
        alpha_threshold=alpha_threshold,
        blend_mode=blend_mode,
        renderer_blend=renderer_blend,
        alpha_test=gb("opennova_alpha_test", semantics.alpha_test),
        alpha_inverted=gb("opennova_alpha_inverted", semantics.alpha_test_invert),
        two_sided=semantics.is_two_sided,
        emissive=gi("emissive_type", 0) != 0
        or gi("emissive_type2", 0) != 0
        or semantics.is_emissive
        or semantics.is_luminance,
        emissive_type=gi("emissive_type", 0),
        emissive_type2=gi("emissive_type2", 0),
        glass=semantics.is_glass or bool(reflect),
        glass_type2=gi("glass_type2", 0),
        reflect_color=reflect_color,
        reflect_color2=reflect_color2,
        luminosity_strength=gf("luminosity_strength", 0.0),
        u_tiling=u_tiling,
        v_tiling=v_tiling,
        effective_u_tiling=effective_u_tiling,
        effective_v_tiling=effective_v_tiling,
        effective_u1_tiling=gf("opennova_uv1_u_tiling", 1.0),
        effective_v1_tiling=gf("opennova_uv1_v_tiling", 1.0),
        has_custom_tiling=has_custom_tiling,
        bump_mode=bump_mode,
        bump_uses_alpha=bump_uses_alpha,
        phong_shader=semantics.family in (
            MATERIAL_SHADER_PHONG,
            MATERIAL_SHADER_ENVIRONMENT,
            MATERIAL_SHADER_GLASS,
        ),
        bump_shader=semantics.needs_normal_map,
        known_shader=semantics.known_shader,
        shader_family=semantics.family,
        resolved_shader=semantics.resolved_shader,
        needs_normal_map=semantics.needs_normal_map,
        normal_space=semantics.normal_space,
        normal_uses_uv2=semantics.normal_uses_uv2,
        has_detail_slot=semantics.has_detail or bool(detail.name),
        uses_specular=semantics.uses_specular,
        uses_environment=semantics.uses_environment,
        is_luminance=semantics.is_luminance,
        is_skinned=semantics.is_skinned,
    )


def material_user_props(desc: MaterialDescriptor) -> Dict[str, Any]:
    """Return common custom/export properties for DCC material objects."""

    props: Dict[str, Any] = {
        "ase_material_name": desc.name,
        "opennova_material_index": desc.index,
        "opennova_shader": desc.shader,
        "opennova_resolved_shader": desc.resolved_shader,
        "opennova_known_shader": int(desc.known_shader),
        "opennova_shader_family": desc.shader_family,
        "opennova_renderer_blend": desc.renderer_blend,
        "blend_mode": desc.blend_mode,
        "opennova_texture_count": len(desc.textures),
        "opennova_material_flags_raw": desc.source_material_flags,
        "opennova_material_flags": desc.material_flags,
        "opennova_alpha_test_byte": desc.alpha_test_value_byte,
        "opennova_needs_normal_map": int(desc.needs_normal_map),
        "opennova_normal_space": desc.normal_space,
        "opennova_normal_uses_uv2": int(desc.normal_uses_uv2),
        "opennova_has_detail_slot": int(desc.has_detail_slot),
        "opennova_uses_specular": int(desc.uses_specular),
        "opennova_uses_environment": int(desc.uses_environment),
        "opennova_is_luminance": int(desc.is_luminance),
        "opennova_is_skinned": int(desc.is_skinned),
    }
    _texture_props(props, "diffuse", desc.diffuse, "ase_diffuse_bitmap")
    _texture_props(props, "detail", desc.detail, "ase_detail_bitmap")
    _texture_props(props, "normal", desc.normal, "ase_normal_bitmap")
    _texture_props(props, "secondary_normal", desc.secondary_normal, "ase_secondary_normal_bitmap")
    for tex in desc.textures:
        _indexed_texture_props(props, tex)
    if desc.normal.name:
        props["ase_normal_type"] = desc.normal.type
        props["opennova_normal_type"] = desc.normal.type
    if desc.secondary_normal.name:
        props["opennova_secondary_normal_type"] = desc.secondary_normal.type
    if desc.alpha_test:
        props["opennova_alpha_test"] = 1
        props["opennova_alpha_threshold"] = desc.alpha_threshold
    if desc.alpha_threshold > 0.0:
        props["alpha_threshold"] = desc.alpha_threshold
    if desc.emissive_type != 0:
        props["emissive_type"] = desc.emissive_type
    if desc.emissive_type2 != 0:
        props["emissive_type2"] = desc.emissive_type2
    if desc.glass:
        props["reflect_color"] = desc.reflect_color
    if desc.glass_type2 != 0:
        props["glass_type2"] = desc.glass_type2
    if any(desc.reflect_color2):
        props["reflect_color2"] = desc.reflect_color2
    if desc.u_params.style != 0:
        props.update(_generator_props("uv_u", desc.u_params))
    if desc.v_params.style != 0:
        props.update(_generator_props("uv_v", desc.v_params))
    if desc.alpha_gen.style != 0:
        props.update(_generator_props("alpha_gen", desc.alpha_gen))
    if desc.rgb_gen.style != 0:
        props.update({
            "rgb_gen_style": desc.rgb_gen.style,
            "rgb_gen_rate": desc.rgb_gen.rate,
            "rgb_gen_phase": desc.rgb_gen.phase,
            "rgb_gen_start_color": desc.rgb_gen.start_color,
            "rgb_gen_end_color": desc.rgb_gen.end_color,
        })
        if desc.rgb_gen.ctrlreg:
            props["rgb_gen_ctrlreg"] = desc.rgb_gen.ctrlreg
    if desc.rgb_gen2.style != 0:
        props.update({
            "rgb_gen2_style": desc.rgb_gen2.style,
            "rgb_gen2_rate": desc.rgb_gen2.rate,
            "rgb_gen2_phase": desc.rgb_gen2.phase,
            "rgb_gen2_start_color": desc.rgb_gen2.start_color,
            "rgb_gen2_end_color": desc.rgb_gen2.end_color,
        })
        if desc.rgb_gen2.ctrlreg:
            props["rgb_gen2_ctrlreg"] = desc.rgb_gen2.ctrlreg
    if desc.tex_anim.num_frames > 0:
        props["tex_anim_frames"] = desc.tex_anim.num_frames
        props["tex_anim_type"] = desc.tex_anim.animation_type
        props["tex_anim_time"] = desc.tex_anim.cycle_frame_time
    if desc.has_custom_tiling:
        props["uv_u_tiling"] = desc.effective_u_tiling
        props["uv_v_tiling"] = desc.effective_v_tiling
    if desc.bump_mode:
        props["opennova_bump_mode"] = desc.bump_mode
        props["opennova_bump_uses_alpha"] = int(desc.bump_uses_alpha)
    return props


def material_diagnostics(descriptors: Iterable[MaterialDescriptor]) -> Dict[str, Any]:
    """Collect root-level material texture diagnostics."""

    diffuse_paths: List[str] = []
    missing_diffuse: List[str] = []
    detail_paths: List[str] = []
    missing_detail: List[str] = []
    normal_paths: List[str] = []
    missing_normal: List[str] = []
    secondary_normal_paths: List[str] = []
    missing_secondary_normal: List[str] = []
    unknown_textures: List[str] = []
    opacity_maps = 0

    for desc in descriptors:
        _collect_texture_diag(desc.index, desc.diffuse, diffuse_paths, missing_diffuse)
        _collect_texture_diag(desc.index, desc.detail, detail_paths, missing_detail)
        _collect_texture_diag(desc.index, desc.normal, normal_paths, missing_normal)
        _collect_texture_diag(
            desc.index,
            desc.secondary_normal,
            secondary_normal_paths,
            missing_secondary_normal,
        )
        for tex in desc.unknown_textures:
            unknown_textures.append(
                "%d:%d:%d:%d:%s" % (
                    desc.index,
                    tex.texture_index,
                    tex.slot,
                    tex.type,
                    tex.name,
                )
            )
        if desc.alpha_test and desc.diffuse.path:
            opacity_maps += 1

    return {
        "diffuse_paths": diffuse_paths,
        "missing_diffuse": missing_diffuse,
        "detail_paths": detail_paths,
        "missing_detail": missing_detail,
        "normal_paths": normal_paths,
        "missing_normal": missing_normal,
        "secondary_normal_paths": secondary_normal_paths,
        "missing_secondary_normal": missing_secondary_normal,
        "unknown_textures": unknown_textures,
        "opacity_maps": opacity_maps,
    }


def material_texture_inventory(descriptors: Iterable[MaterialDescriptor]) -> List[Dict[str, Any]]:
    """Return counted shader/slot/type/role combinations for material audits."""

    counts: Dict[Tuple[str, str, int, int, int], int] = {}
    for desc in descriptors:
        for tex in desc.textures:
            key = (desc.shader, tex.role, tex.slot, tex.type, tex.flags)
            counts[key] = counts.get(key, 0) + 1
    return [
        {
            "shader": shader,
            "role": role,
            "slot": slot,
            "type": tex_type,
            "flags": flags,
            "count": count,
        }
        for (shader, role, slot, tex_type, flags), count in sorted(counts.items())
    ]


def ase_texture_names(desc: MaterialDescriptor, used_names: Dict[str, str]) -> Tuple[str, str]:
    """Return ASE MAP_DIFFUSE/MAP_OPACITY names using legacy truncation rules."""

    return (
        _fit_texture_name(used_names, _fix_tex_ext(desc.diffuse.name)),
        _fit_texture_name(used_names, _fix_tex_ext(desc.detail.name)),
    )


def _effective_alpha_test_byte(ir_mat, alpha_threshold: float) -> int:
    threshold_byte = int(round(max(0.0, min(alpha_threshold, 1.0)) * 255.0))
    raw_byte = _int_attr(ir_mat, "alpha_test_value_byte", -1)
    if raw_byte <= 0 and threshold_byte > 0:
        return threshold_byte
    return max(0, min(raw_byte, 255))


def _material_flags_from_ir_flags(flags: int) -> int:
    out = 0
    if flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST:
        out |= THREEDI_MATERIAL_FLAG_ALPHA_TEST
    if flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT:
        out |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT
    if flags & THREEDI_MATERIAL_FLAG_TWO_SIDED:
        out |= THREEDI_MATERIAL_FLAG_TWO_SIDED
    return out


def _normalize_shader(shader_name: str) -> str:
    return (_decode(shader_name).strip() or "FF_ST_OP").upper()


def _shader_family_from_tag(tag: str) -> str:
    if not tag:
        return MATERIAL_SHADER_UNKNOWN
    if tag == "FFP_GLASS":
        return MATERIAL_SHADER_GLASS
    if tag == "VS_FLAG" or "FLAG" in tag:
        return MATERIAL_SHADER_FLAG
    if tag in ("VS_BMTXMIRRT", "VS_BUMPMIRRT"):
        return MATERIAL_SHADER_GLASS
    if tag == "VS_ENVPHONGT":
        return MATERIAL_SHADER_ENVIRONMENT
    if "PHONG" in tag:
        return MATERIAL_SHADER_PHONG
    if "DOT3" in tag or "BUMPDIFF" in tag:
        return MATERIAL_SHADER_DOT3
    if tag == "VS_SKGLASS":
        return MATERIAL_SHADER_GLASS
    if tag in ("VS_SKBASIC", "VS_SKBASIC#UV"):
        return MATERIAL_SHADER_FIXED_FUNCTION
    if tag.startswith("FF_"):
        return MATERIAL_SHADER_FIXED_FUNCTION
    return MATERIAL_SHADER_UNKNOWN


def _blend_from_tag(tag: str, info_flags: int) -> str:
    if "_AD" in tag:
        return MATERIAL_BLEND_ADDITIVE
    if "_AB" in tag:
        return MATERIAL_BLEND_ALPHA
    if info_flags & _MAT_FLAG_ALPHA:
        return MATERIAL_BLEND_ALPHA
    return MATERIAL_BLEND_OPAQUE


def _normal_space_for_tag(tag: str, has_normal_map: bool) -> str:
    if not has_normal_map:
        return MATERIAL_NORMAL_NONE
    return MATERIAL_NORMAL_OBJECT if "OBJ" in tag else MATERIAL_NORMAL_TANGENT


def _texture_descriptors(
    shader: str,
    ir_mat,
    resolver,
    *,
    source_format: int | None = None,
    texture_strategy: str | None = None,
) -> Tuple[
    TextureDescriptor,
    TextureDescriptor,
    TextureDescriptor,
    TextureDescriptor,
    Tuple[TextureDescriptor, ...],
]:
    diffuse = TextureDescriptor("diffuse")
    detail = TextureDescriptor("detail")
    normal = TextureDescriptor("normal")
    secondary_normal = TextureDescriptor("secondary_normal")
    all_textures: List[TextureDescriptor] = []

    texture_count = _int_attr(ir_mat, "texture_count", 0)
    textures = _attr(ir_mat, "textures", [])
    for t_idx in range(texture_count):
        try:
            tex = textures[t_idx]
        except Exception:
            break
        tex_name = _decode(_attr(tex, "name", b"")).strip()
        if not tex_name:
            continue
        slot = _int_attr(tex, "slot", 0)
        tex_type = _int_attr(tex, "type", 0)
        flags = _int_attr(tex, "flags", 0)
        role = _texture_role(shader, slot, tex_type, t_idx, bool(diffuse.name), source_format)
        desc = TextureDescriptor(
            role=role,
            name=tex_name,
            path=_resolve_texture(
                tex_name,
                resolver,
                source_format=source_format,
                texture_strategy=texture_strategy,
                slot=slot,
                tex_type=tex_type,
                flags=flags,
                role=role,
            ),
            texture_index=t_idx,
            slot=slot,
            type=tex_type,
            flags=flags,
            frame=_int_attr(tex, "frame", 0),
        )
        all_textures.append(desc)
        if desc.role == "diffuse" and (slot == THREEDI_TEX_SLOT_DIFFUSE or not diffuse.name):
            diffuse = desc
        elif desc.role == "detail":
            detail = desc
        elif desc.role == "normal":
            normal = desc
        elif desc.role == "secondary_normal":
            secondary_normal = desc

    return diffuse, detail, normal, secondary_normal, tuple(all_textures)


def _texture_role(
    shader: str,
    slot: int,
    tex_type: int,
    texture_index: int,
    has_diffuse: bool,
    source_format: int | None = None,
) -> str:
    shader_key = (shader or "").upper()
    if slot == THREEDI_TEX_SLOT_DIFFUSE:
        return "diffuse"
    if slot == THREEDI_TEX_SLOT_DETAIL:
        return "detail" if _shader_supports_detail(shader_key) else "unknown"
    if slot == THREEDI_TEX_SLOT_NORMAL:
        return "normal" if _shader_supports_bump(shader_key) or tex_type in (NORMAL_TYPE_MDT, NORMAL_TYPE_TGA_ALPHA) else "unknown"
    if slot == THREEDI_TEX_SLOT_NORMAL_B:
        return (
            "secondary_normal"
            if _shader_supports_bump(shader_key) or tex_type in (NORMAL_TYPE_MDT, NORMAL_TYPE_TGA_ALPHA)
            else "unknown"
        )
    if texture_index == 0 and not has_diffuse:
        return "diffuse"
    return "unknown"


def _shader_supports_detail(shader: str) -> bool:
    semantics = classify_material_shader(shader)
    return (
        semantics.has_detail
        or shader.startswith("FF_MT")
        or shader.startswith("FF_DT")
        or shader.endswith("2")
        or "DIFF2" in shader
        or "T2" in shader
    )


def _shader_supports_bump(shader: str) -> bool:
    semantics = classify_material_shader(shader)
    return semantics.needs_normal_map or any(
        token in shader for token in ("DOT3", "PHONGT_MDT", "BUMP")
    )


def _texture_props(props: Dict[str, Any], role: str, tex: TextureDescriptor, ase_prop: str) -> None:
    props["opennova_%s_texture_name" % role] = tex.name
    props["opennova_%s_texture_path" % role] = tex.path or ""
    props["opennova_%s_texture_missing" % role] = 1 if tex.missing else 0
    if tex.name:
        props[ase_prop] = tex.name
        props["opennova_has_%s_map" % role] = 1 if tex.path else 0


def _indexed_texture_props(props: Dict[str, Any], tex: TextureDescriptor) -> None:
    if tex.texture_index < 0:
        return
    prefix = "opennova_texture_%02d" % tex.texture_index
    props["%s_role" % prefix] = tex.role
    props["%s_name" % prefix] = tex.name
    props["%s_path" % prefix] = tex.path or ""
    props["%s_slot" % prefix] = tex.slot
    props["%s_type" % prefix] = tex.type
    props["%s_flags" % prefix] = tex.flags
    props["%s_frame" % prefix] = tex.frame
    props["%s_missing" % prefix] = 1 if tex.missing else 0


def _generator_props(prefix: str, gen: GeneratorDescriptor) -> Dict[str, Any]:
    props = {
        "%s_style" % prefix: gen.style,
        "%s_rate" % prefix: gen.rate,
        "%s_phase" % prefix: gen.phase,
        "%s_start" % prefix: gen.start,
        "%s_end" % prefix: gen.end,
    }
    if gen.ctrlreg:
        props["%s_ctrlreg" % prefix] = gen.ctrlreg
    return props


def _collect_texture_diag(
    material_index: int,
    texture: TextureDescriptor,
    resolved: List[str],
    missing: List[str],
) -> None:
    if texture.path:
        resolved.append(str(texture.path))
    elif texture.name:
        missing.append("%d:%s" % (material_index, texture.name))


def _uv_params(params, ctrl_resolver) -> GeneratorDescriptor:
    style = _int_attr(params, "style", 0)
    reg = _int_attr(params, "reg", -1)
    return GeneratorDescriptor(
        style=style,
        phase=_float_attr(params, "phase", 0.0),
        reg=reg,
        rate=_float_attr(params, "gen_rate", 0.0),
        start=_float_attr(params, "start", 0.0),
        end=_float_attr(params, "end", 0.0),
        ctrlreg=_resolve_ctrl(style, reg, ctrl_resolver),
    )


def _alpha_gen(params, ctrl_resolver) -> GeneratorDescriptor:
    style = _int_attr(params, "style", 0)
    reg = _int_attr(params, "reg", -1)
    return GeneratorDescriptor(
        style=style,
        phase=_float_attr(params, "phase", 0.0),
        reg=reg,
        rate=_float_attr(params, "rate", 0.0),
        start=_int_attr(params, "start", 0),
        end=_int_attr(params, "end", 0),
        ctrlreg=_resolve_ctrl(style, reg, ctrl_resolver),
    )


def _rgb_gen(params, ctrl_resolver) -> RgbGeneratorDescriptor:
    style = _int_attr(params, "style", 0)
    reg = _int_attr(params, "reg", -1)
    return RgbGeneratorDescriptor(
        style=style,
        phase=_float_attr(params, "phase", 0.0),
        reg=reg,
        rate=_float_attr(params, "rate", 0.0),
        start_color=_float4_attr(params, "start_color"),
        end_color=_float4_attr(params, "end_color"),
        ctrlreg=_resolve_ctrl(style, reg, ctrl_resolver),
    )


def _tex_anim(anim) -> TexAnimDescriptor:
    return TexAnimDescriptor(
        num_frames=_int_attr(anim, "num_frames", 0),
        animation_type=_int_attr(anim, "animation_type", 0),
        cycle_frame_time=_int_attr(anim, "cycle_frame_time", 0),
    )


def _resolve_ctrl(style: int, reg: int, ctrl_resolver) -> str:
    if ctrl_resolver is None or style <= 0x70 or reg < 0:
        return ""
    try:
        value = ctrl_resolver(reg)
    except Exception:
        return ""
    return str(value) if value else ""


def _resolve_texture(
    texture_name: str,
    resolver,
    *,
    source_format: int | None = None,
    texture_strategy: str | None = None,
    slot: int = 0,
    tex_type: int = 0,
    flags: int = 0,
    role: str | None = None,
) -> Optional[str]:
    if not texture_name:
        return None
    if resolver is not None:
        try:
            resolved = resolver.resolve_texture(
                texture_name,
                strategy=texture_strategy,
                source_format=source_format,
                slot=slot,
                tex_type=tex_type,
                flags=flags,
                role=role,
            )
        except TypeError:
            resolved = resolver.resolve_texture(texture_name)
        except Exception:
            resolved = None
        if resolved is not None and os.path.isfile(str(resolved)):
            return str(resolved)
    if os.path.isfile(texture_name):
        return os.path.abspath(texture_name)
    return None


def _decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def _attr(obj, name: str, default):
    if obj is None:
        return default
    return getattr(obj, name, default)


def _int_attr(obj, name: str, default: int) -> int:
    try:
        return int(_attr(obj, name, default))
    except Exception:
        return int(default)


def _float_attr(obj, name: str, default: float) -> float:
    try:
        return float(_attr(obj, name, default))
    except Exception:
        return float(default)


def _float4_attr(obj, name: str) -> Tuple[float, float, float, float]:
    value = _attr(obj, name, None)
    if value is None:
        return (0.0, 0.0, 0.0, 0.0)
    out = []
    for i in range(4):
        try:
            out.append(float(value[i]))
        except Exception:
            out.append(0.0)
    return (out[0], out[1], out[2], out[3])


def _fix_tex_ext(name: str) -> str:
    if not name:
        return name
    name = os.path.basename(name)
    root, ext = os.path.splitext(name)
    if ext.upper() in (".TGA", ".PCX", ".MDT"):
        return root + ext
    return root + ".TGA"


def _fit_texture_name(used: Dict[str, str], name: str) -> str:
    if not name or len(name) <= MAX_TEX_FILENAME:
        if name:
            used[name.lower()] = name.lower()
        return name
    root, ext = os.path.splitext(name)
    max_stem = MAX_TEX_FILENAME - len(ext)
    candidate = root[:max_stem] + ext
    key = candidate.lower()
    if key in used and used[key] != name.lower():
        for i in range(2, 100):
            suffix = str(i)
            candidate = root[:max_stem - len(suffix)] + suffix + ext
            key = candidate.lower()
            if key not in used:
                break
    used[key] = name.lower()
    return candidate
