"""Host-neutral material interpretation for OpenNova model IR data.

This module intentionally contains no Blender or PyMXS imports.  DCC hosts use
the descriptors here to build their own native material objects while sharing
texture-slot parsing, path resolution, diagnostics, and export metadata.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple


THREEDI_IR_TEX_SLOT_DIFFUSE = 1
THREEDI_IR_TEX_SLOT_DETAIL = 2
THREEDI_IR_TEX_SLOT_NORMAL = 3

THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST = 0x01
THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT = 0x02
THREEDI_IR_MATERIAL_FLAG_TWO_SIDED = 0x04
THREEDI_IR_MATERIAL_FLAG_EMISSIVE = 0x08

NORMAL_TYPE_MDT = 4
NORMAL_TYPE_TGA_ALPHA = 5

MAX_TEX_FILENAME = 15


@dataclass(frozen=True)
class TextureDescriptor:
    """One interpreted material texture slot."""

    role: str
    name: str = ""
    path: Optional[str] = None
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


@dataclass(frozen=True)
class MaterialDescriptor:
    index: int
    shader: str
    name: str
    diffuse: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("diffuse"))
    detail: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("detail"))
    normal: TextureDescriptor = field(default_factory=lambda: TextureDescriptor("normal"))
    flags: int = 0
    alpha_threshold: float = 0.0
    blend_mode: int = 0
    alpha_test: bool = False
    alpha_inverted: bool = False
    two_sided: bool = False
    emissive: bool = False
    emissive_type: int = 0
    glass: bool = False
    reflect_color: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
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
    has_custom_tiling: bool = False
    bump_mode: str = ""
    bump_uses_alpha: bool = False
    phong_shader: bool = False
    bump_shader: bool = False
    u_params: GeneratorDescriptor = field(default_factory=GeneratorDescriptor)
    v_params: GeneratorDescriptor = field(default_factory=GeneratorDescriptor)
    alpha_gen: GeneratorDescriptor = field(default_factory=GeneratorDescriptor)
    rgb_gen: RgbGeneratorDescriptor = field(default_factory=RgbGeneratorDescriptor)
    tex_anim: TexAnimDescriptor = field(default_factory=TexAnimDescriptor)


def describe_material(
    ir_mat,
    resolver=None,
    ctrl_resolver=None,
    *,
    source_format: int | None = None,
    texture_strategy: str | None = None,
) -> MaterialDescriptor:
    """Interpret one ``ThreediIRMaterial`` into host-neutral material data."""

    index = _int_attr(ir_mat, "index", 0)
    shader = _decode(_attr(ir_mat, "shader_name", b"")).strip() or "FF_ST_OP"
    name = "Material_%d_%s" % (index, shader)
    flags = _int_attr(ir_mat, "flags", 0)
    diffuse, detail, normal = _texture_descriptors(
        ir_mat,
        resolver,
        source_format=source_format,
        texture_strategy=texture_strategy,
    )

    specular_intensity = _int_attr(ir_mat, "specular_intensity", 0)
    specular_strength = min(float(specular_intensity) / 255.0, 1.0) if specular_intensity > 0 else 0.0
    phong_shader = any(s in shader for s in ("PHONGT", "PHONGO", "BUMPPHONG", "ENVPHONG"))
    bump_shader = any(s in shader for s in ("DOT3", "PHONGT", "BUMP"))
    viewport_specular = specular_strength
    viewport_roughness = 1.0 - specular_strength * 0.7 if specular_strength > 0.0 else 1.0
    if phong_shader and viewport_specular <= 0.0:
        viewport_specular = 0.3
        viewport_roughness = 0.5

    luminosity = _int_attr(ir_mat, "luminosity", 0)
    luminosity_strength = min(float(luminosity) / 255.0, 1.0) if luminosity > 0 else 0.0
    emissive_type = _int_attr(ir_mat, "emissive_type", 0)
    u_tiling = _float_attr(ir_mat, "u_tiling", 0.0)
    v_tiling = _float_attr(ir_mat, "v_tiling", 0.0)
    effective_u_tiling = 1.0 if u_tiling == 0.0 else u_tiling
    effective_v_tiling = 1.0 if v_tiling == 0.0 else v_tiling
    has_custom_tiling = (
        (u_tiling != 0.0 and u_tiling != 1.0)
        or (v_tiling != 0.0 and v_tiling != 1.0)
    )

    bump_mode = ""
    bump_uses_alpha = False
    if normal.name and normal.path and normal.type != NORMAL_TYPE_MDT:
        bump_mode = "normal_texture"
        bump_uses_alpha = normal.type == NORMAL_TYPE_TGA_ALPHA
    elif bump_shader and diffuse.name and diffuse.path:
        bump_mode = "diffuse_alpha"
        bump_uses_alpha = True

    return MaterialDescriptor(
        index=index,
        shader=shader,
        name=name,
        diffuse=diffuse,
        detail=detail,
        normal=normal,
        flags=flags,
        alpha_threshold=_float_attr(ir_mat, "alpha_threshold", 0.0),
        blend_mode=_int_attr(ir_mat, "blend_mode", 0),
        alpha_test=bool(flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST),
        alpha_inverted=bool(flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT),
        two_sided=bool(flags & THREEDI_IR_MATERIAL_FLAG_TWO_SIDED),
        emissive=bool(flags & THREEDI_IR_MATERIAL_FLAG_EMISSIVE) or emissive_type != 0,
        emissive_type=emissive_type,
        glass=bool(_int_attr(ir_mat, "is_glass", 0)),
        reflect_color=_float4_attr(ir_mat, "reflect_color"),
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
        has_custom_tiling=has_custom_tiling,
        bump_mode=bump_mode,
        bump_uses_alpha=bump_uses_alpha,
        phong_shader=phong_shader,
        bump_shader=bump_shader,
        u_params=_uv_params(_attr(ir_mat, "u_params", None), ctrl_resolver),
        v_params=_uv_params(_attr(ir_mat, "v_params", None), ctrl_resolver),
        alpha_gen=_alpha_gen(_attr(ir_mat, "alpha_gen", None), ctrl_resolver),
        rgb_gen=_rgb_gen(_attr(ir_mat, "rgb_gen", None), ctrl_resolver),
        tex_anim=_tex_anim(_attr(ir_mat, "animation", None)),
    )


def describe_materials(ir, resolver=None, ctrl_resolver=None) -> List[MaterialDescriptor]:
    """Interpret all materials in a model IR."""

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


def material_user_props(desc: MaterialDescriptor) -> Dict[str, Any]:
    """Return common custom/export properties for host material objects."""

    props: Dict[str, Any] = {
        "ase_material_name": desc.name,
        "opennova_material_index": desc.index,
        "opennova_shader": desc.shader,
        "blend_mode": desc.blend_mode,
    }
    _texture_props(props, "diffuse", desc.diffuse, "ase_diffuse_bitmap")
    _texture_props(props, "detail", desc.detail, "ase_detail_bitmap")
    _texture_props(props, "normal", desc.normal, "ase_normal_bitmap")
    if desc.normal.name:
        props["ase_normal_type"] = desc.normal.type
        props["opennova_normal_type"] = desc.normal.type
    if desc.alpha_test:
        props["opennova_alpha_test"] = 1
        props["opennova_alpha_threshold"] = desc.alpha_threshold
    if desc.alpha_threshold > 0.0:
        props["alpha_threshold"] = desc.alpha_threshold
    if desc.emissive_type != 0:
        props["emissive_type"] = desc.emissive_type
    if desc.glass:
        props["reflect_color"] = desc.reflect_color
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
    opacity_maps = 0

    for desc in descriptors:
        _collect_texture_diag(desc.index, desc.diffuse, diffuse_paths, missing_diffuse)
        _collect_texture_diag(desc.index, desc.detail, detail_paths, missing_detail)
        _collect_texture_diag(desc.index, desc.normal, normal_paths, missing_normal)
        if desc.alpha_test and desc.diffuse.path:
            opacity_maps += 1

    return {
        "diffuse_paths": diffuse_paths,
        "missing_diffuse": missing_diffuse,
        "detail_paths": detail_paths,
        "missing_detail": missing_detail,
        "normal_paths": normal_paths,
        "missing_normal": missing_normal,
        "opacity_maps": opacity_maps,
    }


def ase_texture_names(desc: MaterialDescriptor, used_names: Dict[str, str]) -> Tuple[str, str]:
    """Return ASE MAP_DIFFUSE/MAP_OPACITY names using legacy truncation rules."""

    return (
        _fit_texture_name(used_names, _fix_tex_ext(desc.diffuse.name)),
        _fit_texture_name(used_names, _fix_tex_ext(desc.detail.name)),
    )


def _texture_descriptors(
    ir_mat,
    resolver,
    *,
    source_format: int | None = None,
    texture_strategy: str | None = None,
) -> Tuple[TextureDescriptor, TextureDescriptor, TextureDescriptor]:
    diffuse = TextureDescriptor("diffuse")
    detail = TextureDescriptor("detail")
    normal = TextureDescriptor("normal")

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
        desc = TextureDescriptor(
            role=_texture_role(slot, t_idx, bool(diffuse.name)),
            name=tex_name,
            path=_resolve_texture(
                tex_name,
                resolver,
                source_format=source_format,
                texture_strategy=texture_strategy,
                slot=slot,
                tex_type=_int_attr(tex, "type", 0),
                flags=_int_attr(tex, "flags", 0),
            ),
            slot=slot,
            type=_int_attr(tex, "type", 0),
            flags=_int_attr(tex, "flags", 0),
            frame=_int_attr(tex, "frame", 0),
        )
        if desc.role == "diffuse" and (slot == THREEDI_IR_TEX_SLOT_DIFFUSE or not diffuse.name):
            diffuse = desc
        elif desc.role == "detail":
            detail = desc
        elif desc.role == "normal":
            normal = desc

    return diffuse, detail, normal


def _texture_role(slot: int, texture_index: int, has_diffuse: bool) -> str:
    if slot == THREEDI_IR_TEX_SLOT_DIFFUSE or (texture_index == 0 and not has_diffuse):
        return "diffuse"
    if slot == THREEDI_IR_TEX_SLOT_DETAIL:
        return "detail"
    if slot == THREEDI_IR_TEX_SLOT_NORMAL:
        return "normal"
    return "unknown"


def _texture_props(props: Dict[str, Any], role: str, tex: TextureDescriptor, ase_prop: str) -> None:
    props["opennova_%s_texture_name" % role] = tex.name
    props["opennova_%s_texture_path" % role] = tex.path or ""
    props["opennova_%s_texture_missing" % role] = 1 if tex.missing else 0
    if tex.name:
        props[ase_prop] = tex.name
        props["opennova_has_%s_map" % role] = 1 if tex.path else 0


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
        return root + ext.upper()
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
