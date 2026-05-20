"""3ds Max material creation for the OpenNova importer."""
from __future__ import annotations

import logging
import os
from typing import Any

from pyopennova.materials import (
    MATERIAL_BLEND_ADDITIVE,
    MATERIAL_BLEND_ALPHA,
    NORMAL_TYPE_MDT,
    NORMAL_TYPE_TGA_ALPHA,
    THREEDI_TEX_SLOT_DETAIL,
    THREEDI_TEX_SLOT_DIFFUSE,
    THREEDI_TEX_SLOT_NORMAL,
    THREEDI_TEX_SLOT_NORMAL_B,
    describe_material,
    material_user_props,
)

log = logging.getLogger(__name__)


def _rt():
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; opennova_max only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime


def create_material(
    mat_ir,
    resolver=None,
    ctrl_resolver=None,
    source_format=None,
    uv1_tiling_override=None,
) -> Any:
    """Create a Max StandardMaterial matching one 3DI3 material."""
    rt = _rt()
    desc = describe_material(
        mat_ir,
        resolver=resolver,
        ctrl_resolver=ctrl_resolver,
        source_format=source_format,
        uv1_tiling_override=uv1_tiling_override,
    )

    mat = rt.StandardMaterial(name=desc.name)
    fallback_color = rt.color(150, 150, 150)
    _try_set(mat, "diffuse", fallback_color)
    _try_set(mat, "diffuseColor", fallback_color)
    _try_set(mat, "showInViewport", True)
    _try_set(mat, "twoSided", bool(desc.two_sided))
    if desc.glass and any(desc.reflect_color[:3]):
        reflect_color = rt.color(
            _byte(desc.reflect_color[0]),
            _byte(desc.reflect_color[1]),
            _byte(desc.reflect_color[2]),
        )
        _try_set(mat, "ambient", reflect_color)
        _try_set(mat, "ambientColor", reflect_color)
    _try_set(mat, "specularLevel", desc.viewport_specular * 100.0)
    _try_set(mat, "glossiness", max(0.0, min(1.0, 1.0 - desc.viewport_roughness)) * 100.0)

    # Store the same metadata the Blender ASE exporter relies on. Native Max
    # ASE does not read these directly, but they keep the scene informative and
    # allow future custom exporters to consume one common property vocabulary.
    for key, value in material_user_props(desc).items():
        _set_user_prop(rt, mat, key, _prop_to_max_value(value))

    diffuse_assigned = _wire_diffuse(rt, mat, desc)

    _wire_alpha_test(rt, mat, desc)

    # Alpha-test materials use the precise threshold from the 3DI3 model; renderer_blend
    # opacity defaults (70 / 100) only apply when alpha-test isn't already set.
    is_alpha_blend = desc.blend_mode == 1 or desc.renderer_blend == MATERIAL_BLEND_ALPHA
    is_additive = desc.blend_mode == 2 or desc.renderer_blend == MATERIAL_BLEND_ADDITIVE
    if is_alpha_blend or is_additive:
        _set_user_prop(rt, mat, "opennova_renderer_blend", desc.renderer_blend)
        if not desc.alpha_test:
            _try_set(mat, "opacity", 70.0 if is_alpha_blend else 100.0)
    if is_additive or desc.emissive:
        _try_set(mat, "selfIllumAmount", 100.0)
    if desc.luminosity_strength > 0.0:
        _try_set(mat, "selfIllumAmount", desc.luminosity_strength * 100.0)

    _wire_detail(rt, mat, desc)
    _wire_normal_metadata(rt, mat, desc)
    _wire_bump(rt, mat, desc)

    return mat


def _wire_diffuse(rt, mat, desc) -> bool:
    """Wire the diffuse map slot. Returns True when a bitmap was assigned."""
    bitmap_path = desc.diffuse.path or desc.diffuse.name
    if not bitmap_path:
        return False
    bm = _create_bitmap_texture(rt, f"{desc.name}_diffuse", bitmap_path, desc)
    assigned = _assign_texture_map(rt, mat, "diffuseMap", "diffuseMapEnable", 2, bm)
    _set_user_prop(rt, mat, "opennova_has_diffuse_map", 1)
    _set_user_prop(rt, mat, "opennova_diffuse_bitmap_assigned", 1 if assigned else 0)
    _set_user_prop(rt, mat, "opennova_diffuse_bitmap_filename", _bitmap_filename(bm))
    try:
        rt.showTextureMap(mat, bm, True)
    except Exception:
        pass
    return True


def _wire_alpha_test(rt, mat, desc) -> None:
    """Drive opacity from desc.alpha_threshold + desc.alpha_inverted."""
    if not desc.alpha_test:
        return
    threshold = desc.alpha_threshold if not desc.alpha_inverted else (1.0 - desc.alpha_threshold)
    threshold = max(0.0, min(threshold, 1.0))
    _try_set(mat, "opacity", threshold * 100.0)
    _try_set(mat, "opacityType", 2)  # 2 = Cutoff (clip-style)
    _set_user_prop(rt, mat, "opennova_alpha_test", 1)
    _set_user_prop(rt, mat, "opennova_alpha_threshold", desc.alpha_threshold)
    _set_user_prop(rt, mat, "opennova_alpha_inverted", 1 if desc.alpha_inverted else 0)
    if desc.diffuse.path is not None:
        try:
            op = _create_bitmap_texture(rt, f"{desc.name}_opacity", desc.diffuse.path, desc)
            _assign_texture_map(rt, mat, "opacityMap", "opacityMapEnable", 7, op)
            _set_user_prop(rt, mat, "opennova_has_opacity_map", 1)
        except Exception:
            _set_user_prop(rt, mat, "opennova_has_opacity_map", 0)
    else:
        _set_user_prop(rt, mat, "opennova_has_opacity_map", 0)


def _wire_detail(rt, mat, desc) -> None:
    """Attach detail texture data to a native Max map slot.

    The current-scene ASE exporter reads this native slot back and emits the
    OED-compatible RGB Multiply diffuse/detail pair. This avoids relying on
    importer-authored user props.
    """
    detail_path = desc.detail.path or desc.detail.name
    if not detail_path:
        return
    detail = _create_bitmap_texture(rt, f"{desc.name}_detail", detail_path, desc)
    _set_bitmap_tiling(detail, desc.effective_u1_tiling, desc.effective_v1_tiling)
    _assign_texture_map(rt, mat, "selfIllumMap", "selfIllumMapEnable", 5, detail)


def _wire_normal_metadata(rt, mat, desc) -> None:
    """Stash normal/secondary-normal texture metadata as user props."""
    if desc.normal.name:
        _set_user_prop(rt, mat, "opennova_normal_texture_path", desc.normal.path or "")
        _set_user_prop(rt, mat, "opennova_normal_type", desc.normal.type)
        if desc.normal.type == NORMAL_TYPE_MDT:
            _set_user_prop(rt, mat, "opennova_has_normal_map", 0)
            _set_user_prop(rt, mat, "opennova_normal_map_unsupported", "mdt")
    if desc.secondary_normal.name:
        _set_user_prop(
            rt, mat, "opennova_secondary_normal_texture_path",
            desc.secondary_normal.path or "",
        )
        _set_user_prop(
            rt, mat, "opennova_secondary_normal_type", desc.secondary_normal.type,
        )


def _wire_bump(rt, mat, desc) -> None:
    """Wire normalMap (DOT3 tangent space) or bumpMap (height-based) per descriptor.

    Two modes from the descriptor:
      - normal_texture + non-MDT non-TGA-alpha type → tangent-space normal map
        goes to mat.normalMap (Max's dedicated normal-map slot).
      - normal_texture + TGA-alpha type → height-in-alpha bump goes to bumpMap
        with monoOutput=2.

    Phong/DOT3 materials without a slot-3 texture import flat-shaded. The
    earlier diffuse_alpha fabrication produced visible shading distortion on
    materials whose DDS alpha was opacity / unused rather than height.
    """
    if not desc.bump_mode:
        return
    if desc.bump_mode == "normal_texture":
        bump_tex = desc.normal if desc.normal.name else desc.secondary_normal
        bump_path = bump_tex.path
        bump_role = bump_tex.role
        if bump_path is None or not _is_max_bitmap_texture(bump_path):
            _set_user_prop(rt, mat, "opennova_has_normal_map", 0)
            return
        bm = _create_bitmap_texture(rt, f"{desc.name}_{bump_role}_bump", bump_path, desc)
        if bump_tex.type == NORMAL_TYPE_TGA_ALPHA:
            _try_set(bm, "monoOutput", 2)
            _assign_texture_map(rt, mat, "bumpMap", "bumpMapEnable", 9, bm)
            _try_set(mat, "bumpMapAmount", 30.0)
            _set_user_prop(rt, mat, "opennova_bump_map_mode", "tga_alpha")
        else:
            assigned = _assign_texture_map(
                rt, mat, "normalMap", "normalMapEnable", 9, bm,
            )
            _try_set(mat, "normalMapEnable", True)
            if not assigned:
                # Older Max versions without the dedicated normalMap slot fall
                # back to bumpMap so the user still sees something.
                _assign_texture_map(rt, mat, "bumpMap", "bumpMapEnable", 9, bm)
                _try_set(mat, "bumpMapAmount", 30.0)
                _set_user_prop(rt, mat, "opennova_bump_map_mode", "normal_dot3_fallback_bump")
            else:
                _set_user_prop(rt, mat, "opennova_bump_map_mode", "normal_dot3")
        _set_user_prop(rt, mat, "opennova_has_normal_map", 1)
    else:
        _set_user_prop(rt, mat, "opennova_has_normal_map", 0)
        _set_user_prop(rt, mat, "opennova_bump_map_mode", "metadata_only")


def create_diffuse_material(mat_ir, resolver=None, source_format=None) -> Any:
    """Backward-compatible wrapper used by older smoke tests."""
    return create_material(mat_ir, resolver=resolver, source_format=source_format)


def create_marker_material(name: str, color_rgb: tuple[float, float, float], alpha: float = 1.0) -> Any:
    """Create or return a simple colored Max material."""
    rt = _rt()
    try:
        existing = rt.sceneMaterials[name]
        if existing:
            return existing
    except Exception:
        pass

    mat = rt.StandardMaterial(name=name)
    _try_set(mat, "diffuseColor", rt.color(
        _byte(color_rgb[0]),
        _byte(color_rgb[1]),
        _byte(color_rgb[2]),
    ))
    _try_set(mat, "opacity", max(0.0, min(alpha, 1.0)) * 100.0)
    if alpha < 1.0:
        _try_set(mat, "twoSided", True)
    return mat


def create_multimaterial(name: str, material_ids: list[int], material_dict: dict[int, Any]) -> Any:
    """Create a Max MultiMaterial for one mesh's ordered material ids."""
    rt = _rt()
    if not material_ids:
        return None
    if len(material_ids) == 1:
        return material_dict.get(material_ids[0])

    max_material_ids = [_max_material_id(v) for v in material_ids]
    numsubs = max(max_material_ids) if max_material_ids else 0
    try:
        multi = rt.MultiMaterial(numsubs=numsubs)
    except Exception:
        try:
            multi = rt.multimaterial(numsubs=numsubs)
        except Exception:
            log.warning("Could not create MultiMaterial; falling back to first material")
            return material_dict.get(material_ids[0])

    multi.name = name
    _try_set(multi, "numsubs", numsubs)
    _set_user_prop(rt, multi, "opennova_submaterial_count", numsubs)
    _set_user_prop(rt, multi, "opennova_material_ids", ",".join(str(int(v)) for v in material_ids))
    _set_user_prop(rt, multi, "opennova_max_material_ids", ",".join(str(v) for v in max_material_ids))
    for mat_id in material_ids:
        mat = material_dict.get(mat_id)
        if mat is None:
            continue
        max_material_id = _max_material_id(mat_id)
        _set_user_prop(rt, mat, "opennova_max_material_id", max_material_id)
        assigned = False
        for setter in (
            lambda: rt.setSubMtl(multi, max_material_id, mat),
            lambda: multi.materialList.__setitem__(max_material_id, mat),
            lambda: multi.materialList.__setitem__(max_material_id - 1, mat),
        ):
            try:
                setter()
                assigned = True
                break
            except Exception:
                pass
        if not assigned:
            log.debug("Could not assign MultiMaterial slot %s", max_material_id)
    return multi


def _max_material_id(material_id: int) -> int:
    """Return the 1-based Max face/material id for a 0-based 3DI3 material id."""
    return max(1, int(material_id) + 1)


def collect_texture_diagnostics(mat_ir, resolver=None, source_format=None) -> dict[str, Any]:
    """Return resolved texture names/paths without touching Max material APIs."""
    desc = describe_material(mat_ir, resolver=resolver, source_format=source_format)
    return {
        "diffuse_name": desc.diffuse.name,
        "diffuse_path": desc.diffuse.path,
        "diffuse_missing": desc.diffuse.missing,
        "detail_name": desc.detail.name,
        "detail_path": desc.detail.path,
        "detail_missing": desc.detail.missing,
        "normal_name": desc.normal.name,
        "normal_path": desc.normal.path,
        "normal_missing": desc.normal.missing,
        "normal_type": desc.normal.type,
        "secondary_normal_name": desc.secondary_normal.name,
        "secondary_normal_path": desc.secondary_normal.path,
        "secondary_normal_missing": desc.secondary_normal.missing,
        "secondary_normal_type": desc.secondary_normal.type,
        "unknown_textures": [
            {
                "index": tex.texture_index,
                "name": tex.name,
                "slot": tex.slot,
                "type": tex.type,
                "flags": tex.flags,
            }
            for tex in desc.unknown_textures
        ],
    }


def _decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def _byte(value: float) -> int:
    return max(0, min(255, int(float(value) * 255.0)))


def _create_bitmap_texture(rt, name: str, path: str, desc=None):
    try:
        bm = rt.BitmapTexture(filename=path)
    except Exception:
        bm = rt.BitmapTexture()
    _try_set(bm, "filename", path)
    _try_set(bm, "fileName", path)
    _try_set(bm, "name", name)
    if desc is not None and getattr(desc, "has_custom_tiling", False):
        try:
            _try_set(bm.coords, "U_Tiling", desc.effective_u_tiling)
            _try_set(bm.coords, "V_Tiling", desc.effective_v_tiling)
            _try_set(bm.coords, "u_tiling", desc.effective_u_tiling)
            _try_set(bm.coords, "v_tiling", desc.effective_v_tiling)
        except Exception:
            pass
    return bm


def _set_bitmap_tiling(bitmap, u_tiling: float, v_tiling: float) -> None:
    try:
        _try_set(bitmap.coords, "U_Tiling", u_tiling)
        _try_set(bitmap.coords, "V_Tiling", v_tiling)
        _try_set(bitmap.coords, "u_tiling", u_tiling)
        _try_set(bitmap.coords, "v_tiling", v_tiling)
    except Exception:
        pass


def _bitmap_filename(bitmap) -> str:
    for attr in ("filename", "fileName"):
        try:
            value = getattr(bitmap, attr)
        except Exception:
            value = None
        if value:
            return str(value)
    return ""


def _assign_texture_map(rt, mat, map_attr: str, enable_attr: str, map_slot: int, bitmap) -> bool:
    assigned = _try_set(mat, map_attr, bitmap)
    assigned = _try_set_indexed(getattr(mat, "maps", None), map_slot, bitmap) or assigned
    assigned = _try_set_indexed(getattr(mat, "maps", None), map_slot - 1, bitmap) or assigned
    if not assigned:
        try:
            rt.setProperty(mat, map_attr, bitmap)
            assigned = True
        except Exception:
            pass

    enabled = _try_set(mat, enable_attr, True)
    enabled = _try_set_indexed(getattr(mat, "mapEnables", None), map_slot, True) or enabled
    enabled = _try_set_indexed(getattr(mat, "mapEnables", None), map_slot - 1, True) or enabled
    if not enabled:
        try:
            rt.setProperty(mat, enable_attr, True)
        except Exception:
            pass
    return assigned


def _store_texture_resolution(rt, mat, slot: str, texture_name: str, path: str | None) -> None:
    if not texture_name:
        _set_user_prop(rt, mat, f"opennova_{slot}_texture_name", "")
        _set_user_prop(rt, mat, f"opennova_{slot}_texture_path", "")
        _set_user_prop(rt, mat, f"opennova_{slot}_texture_missing", 0)
        return
    _set_user_prop(rt, mat, f"opennova_{slot}_texture_name", texture_name)
    _set_user_prop(rt, mat, f"opennova_{slot}_texture_path", path or "")
    _set_user_prop(rt, mat, f"opennova_{slot}_texture_missing", 0 if path else 1)


def _is_max_bitmap_texture(path: str) -> bool:
    return os.path.splitext(path)[1].lower() in {
        ".bmp",
        ".dds",
        ".jpg",
        ".jpeg",
        ".png",
        ".tga",
        ".tif",
        ".tiff",
    }


def _resolve_texture(texture_name: str, resolver) -> str | None:
    if not texture_name:
        return None
    if resolver is not None:
        try:
            resolved = resolver.resolve_texture(texture_name)
        except Exception as exc:  # pragma: no cover - defensive log path
            log.warning("Texture resolve failed for %r: %s", texture_name, exc)
            resolved = None
        if resolved is not None and os.path.isfile(str(resolved)):
            return str(resolved)
    if os.path.isfile(texture_name):
        return os.path.abspath(texture_name)
    return None


def _try_set(obj, attr: str, value) -> bool:
    try:
        setattr(obj, attr, value)
        return True
    except Exception:
        return False


def _try_set_indexed(seq, index: int, value) -> bool:
    if seq is None or index < 0:
        return False
    try:
        seq[index] = value
        return True
    except Exception:
        pass
    try:
        seq.__setitem__(index, value)
        return True
    except Exception:
        return False


def _set_user_prop(rt, obj, key: str, value) -> None:
    try:
        rt.setUserProp(obj, key, value)
    except Exception:
        pass


def _prop_to_max_value(value):
    if isinstance(value, (tuple, list)):
        return ",".join(str(v) for v in value)
    return value


def _store_generator_props(rt, mat, mat_ir, ctrl_resolver) -> None:
    if int(mat_ir.u_params.style) != 0:
        _set_user_prop(rt, mat, "uv_u_style", int(mat_ir.u_params.style))
        _set_user_prop(rt, mat, "uv_u_rate", float(mat_ir.u_params.gen_rate))
        _set_user_prop(rt, mat, "uv_u_phase", float(mat_ir.u_params.phase))
        _set_user_prop(rt, mat, "uv_u_start", float(mat_ir.u_params.start))
        _set_user_prop(rt, mat, "uv_u_end", float(mat_ir.u_params.end))
    if int(mat_ir.v_params.style) != 0:
        _set_user_prop(rt, mat, "uv_v_style", int(mat_ir.v_params.style))
        _set_user_prop(rt, mat, "uv_v_rate", float(mat_ir.v_params.gen_rate))
        _set_user_prop(rt, mat, "uv_v_phase", float(mat_ir.v_params.phase))
        _set_user_prop(rt, mat, "uv_v_start", float(mat_ir.v_params.start))
        _set_user_prop(rt, mat, "uv_v_end", float(mat_ir.v_params.end))
    if int(mat_ir.alpha_gen.style) != 0:
        _set_user_prop(rt, mat, "alpha_gen_style", int(mat_ir.alpha_gen.style))
        _set_user_prop(rt, mat, "alpha_gen_rate", float(mat_ir.alpha_gen.rate))
        _set_user_prop(rt, mat, "alpha_gen_phase", float(mat_ir.alpha_gen.phase))
        _set_user_prop(rt, mat, "alpha_gen_start", int(mat_ir.alpha_gen.start))
        _set_user_prop(rt, mat, "alpha_gen_end", int(mat_ir.alpha_gen.end))
    if int(mat_ir.rgb_gen.style) != 0:
        _set_user_prop(rt, mat, "rgb_gen_style", int(mat_ir.rgb_gen.style))
        _set_user_prop(rt, mat, "rgb_gen_rate", float(mat_ir.rgb_gen.rate))
        _set_user_prop(rt, mat, "rgb_gen_phase", float(mat_ir.rgb_gen.phase))
        sc = mat_ir.rgb_gen.start_color
        ec = mat_ir.rgb_gen.end_color
        _set_user_prop(rt, mat, "rgb_gen_start_color", f"{sc[0]},{sc[1]},{sc[2]},{sc[3]}")
        _set_user_prop(rt, mat, "rgb_gen_end_color", f"{ec[0]},{ec[1]},{ec[2]},{ec[3]}")
    if int(mat_ir.animation.num_frames) > 0:
        _set_user_prop(rt, mat, "tex_anim_frames", int(mat_ir.animation.num_frames))
        _set_user_prop(rt, mat, "tex_anim_type", int(mat_ir.animation.animation_type))
        _set_user_prop(rt, mat, "tex_anim_time", int(mat_ir.animation.cycle_frame_time))
    if int(mat_ir.alpha_threshold) > 0:
        _set_user_prop(rt, mat, "alpha_threshold", int(mat_ir.alpha_threshold))
    if int(mat_ir.emissive_type) != 0:
        _set_user_prop(rt, mat, "emissive_type", int(mat_ir.emissive_type))
    if bool(mat_ir.is_glass):
        rc = mat_ir.reflect_color
        _set_user_prop(rt, mat, "reflect_color", f"{rc[0]},{rc[1]},{rc[2]},{rc[3]}")

    if ctrl_resolver is None:
        return
    _store_ctrl(rt, mat, "rgb_gen_ctrlreg", int(mat_ir.rgb_gen.style), int(mat_ir.rgb_gen.reg), ctrl_resolver)
    _store_ctrl(rt, mat, "alpha_gen_ctrlreg", int(mat_ir.alpha_gen.style), int(mat_ir.alpha_gen.reg), ctrl_resolver)
    _store_ctrl(rt, mat, "uv_u_ctrlreg", int(mat_ir.u_params.style), int(mat_ir.u_params.reg), ctrl_resolver)
    _store_ctrl(rt, mat, "uv_v_ctrlreg", int(mat_ir.v_params.style), int(mat_ir.v_params.reg), ctrl_resolver)


def _store_ctrl(rt, mat, prop_name: str, style: int, reg: int, ctrl_resolver) -> None:
    if style <= 0x70 or reg < 0:
        return
    value = ctrl_resolver(reg)
    if value:
        _set_user_prop(rt, mat, prop_name, value)
