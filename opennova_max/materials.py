"""3ds Max material creation for the OpenNova importer."""
from __future__ import annotations

import logging
import os
from typing import Any

from pyopennova.materials import (
    NORMAL_TYPE_MDT,
    THREEDI_IR_TEX_SLOT_DETAIL,
    THREEDI_IR_TEX_SLOT_DIFFUSE,
    THREEDI_IR_TEX_SLOT_NORMAL,
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


def create_material(mat_ir, resolver=None, ctrl_resolver=None) -> Any:
    """Create a Max StandardMaterial matching one IR material."""
    rt = _rt()
    desc = describe_material(mat_ir, resolver=resolver, ctrl_resolver=ctrl_resolver)

    mat = rt.StandardMaterial(name=desc.name)
    fallback_color = rt.color(150, 150, 150)
    _try_set(mat, "diffuse", fallback_color)
    _try_set(mat, "diffuseColor", fallback_color)
    _try_set(mat, "showInViewport", True)
    _try_set(mat, "twoSided", desc.two_sided)
    _try_set(mat, "specularLevel", desc.viewport_specular * 100.0)
    _try_set(mat, "glossiness", max(0.0, min(1.0, 1.0 - desc.viewport_roughness)) * 100.0)

    # Store the same metadata the Blender ASE exporter relies on. Native Max
    # ASE does not read these directly, but they keep the scene informative and
    # allow future custom exporters to consume one common property vocabulary.
    for key, value in material_user_props(desc).items():
        _set_user_prop(rt, mat, key, _prop_to_max_value(value))

    if desc.diffuse.path is not None:
        bm = _create_bitmap_texture(rt, f"{desc.name}_diffuse", desc.diffuse.path, desc)
        _try_set(mat, "diffuseMap", bm)
        _try_set(mat, "diffuseMapEnable", True)
        _set_user_prop(rt, mat, "opennova_has_diffuse_map", 1)
        try:
            rt.showTextureMap(mat, bm, True)
        except Exception:
            pass
    elif desc.diffuse.name:
        _set_user_prop(rt, mat, "opennova_has_diffuse_map", 0)

    if desc.alpha_test:
        _try_set(mat, "opacity", 100.0)
        _try_set(mat, "opacityType", 2)
        _set_user_prop(rt, mat, "opennova_alpha_test", 1)
        _set_user_prop(rt, mat, "opennova_alpha_threshold", desc.alpha_threshold)
        if desc.diffuse.path is not None:
            try:
                op = _create_bitmap_texture(rt, f"{desc.name}_opacity", desc.diffuse.path, desc)
                _try_set(mat, "opacityMap", op)
                _try_set(mat, "opacityMapEnable", True)
                _set_user_prop(rt, mat, "opennova_has_opacity_map", 1)
            except Exception:
                _set_user_prop(rt, mat, "opennova_has_opacity_map", 0)
                pass
        else:
            _set_user_prop(rt, mat, "opennova_has_opacity_map", 0)

    if desc.blend_mode in (1, 2):
        _try_set(mat, "opacity", 70.0 if desc.blend_mode == 1 else 100.0)
    if desc.blend_mode == 2 or desc.emissive:
        _try_set(mat, "selfIllumAmount", 100.0)
    if desc.luminosity_strength > 0.0:
        _try_set(mat, "selfIllumAmount", desc.luminosity_strength * 100.0)

    if desc.detail.name:
        _set_user_prop(rt, mat, "opennova_detail_texture_path", desc.detail.path or "")
        if desc.detail.path is not None:
            _set_user_prop(rt, mat, "opennova_has_detail_map", 1)
            _set_user_prop(rt, mat, "opennova_detail_map_mode", "metadata_only")
        else:
            _set_user_prop(rt, mat, "opennova_has_detail_map", 0)

    if desc.normal.name:
        _set_user_prop(rt, mat, "opennova_normal_texture_path", desc.normal.path or "")
        _set_user_prop(rt, mat, "opennova_normal_type", desc.normal.type)
        if desc.normal.type == NORMAL_TYPE_MDT:
            _set_user_prop(rt, mat, "opennova_has_normal_map", 0)
            _set_user_prop(rt, mat, "opennova_normal_map_unsupported", "mdt")

    if desc.bump_mode:
        if desc.bump_mode == "normal_texture":
            bump_path = desc.normal.path
            bump_name = "normal"
        else:
            bump_path = desc.diffuse.path
            bump_name = "diffuse_alpha"
        if bump_path is not None and _is_max_bitmap_texture(bump_path):
            normal_bm = _create_bitmap_texture(rt, f"{desc.name}_{bump_name}_bump", bump_path, desc)
            _try_set(mat, "bumpMap", normal_bm)
            _try_set(mat, "bumpMapEnable", True)
            _try_set(mat, "bumpMapAmount", 30.0)
            _set_user_prop(rt, mat, "opennova_has_normal_map", 1)
        else:
            _set_user_prop(rt, mat, "opennova_has_normal_map", 0)

    return mat


def create_diffuse_material(mat_ir, resolver=None) -> Any:
    """Backward-compatible wrapper used by older smoke tests."""
    return create_material(mat_ir, resolver=resolver)


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

    try:
        multi = rt.MultiMaterial(numsubs=len(material_ids))
    except Exception:
        try:
            multi = rt.multimaterial(numsubs=len(material_ids))
        except Exception:
            log.warning("Could not create MultiMaterial; falling back to first material")
            return material_dict.get(material_ids[0])

    multi.name = name
    _try_set(multi, "numsubs", len(material_ids))
    _set_user_prop(rt, multi, "opennova_submaterial_count", len(material_ids))
    _set_user_prop(rt, multi, "opennova_material_ids", ",".join(str(int(v)) for v in material_ids))
    _set_user_prop(rt, multi, "opennova_max_material_ids", ",".join(str(_max_material_id(v)) for v in material_ids))
    for slot, mat_id in enumerate(material_ids, start=1):
        mat = material_dict.get(mat_id)
        if mat is None:
            continue
        max_material_id = _max_material_id(mat_id)
        _set_user_prop(rt, mat, "opennova_max_material_id", max_material_id)
        assigned = False
        for setter in (
            lambda: rt.setSubMtl(multi, slot, mat),
            lambda: multi.materialList.__setitem__(slot, mat),
            lambda: multi.materialList.__setitem__(slot - 1, mat),
        ):
            try:
                setter()
                assigned = True
                break
            except Exception:
                pass
        if not assigned:
            log.debug("Could not assign MultiMaterial slot %s", slot)

        for setter in (
            lambda: multi.materialIDList.__setitem__(slot - 1, max_material_id),
            lambda: multi.materialIDList.__setitem__(slot, max_material_id),
        ):
            try:
                setter()
                break
            except Exception:
                pass
    return multi


def _max_material_id(material_id: int) -> int:
    """Return the 1-based Max face/material id for a 0-based IR material id."""
    return max(1, int(material_id) + 1)


def collect_texture_diagnostics(mat_ir, resolver=None) -> dict[str, Any]:
    """Return resolved texture names/paths without touching Max material APIs."""
    desc = describe_material(mat_ir, resolver=resolver)
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
    }


def _decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def _byte(value: float) -> int:
    return max(0, min(255, int(float(value) * 255.0)))


def _create_bitmap_texture(rt, name: str, path: str, desc=None):
    bm = rt.BitmapTexture(filename=path)
    bm.name = name
    if desc is not None and getattr(desc, "has_custom_tiling", False):
        try:
            _try_set(bm.coords, "U_Tiling", desc.effective_u_tiling)
            _try_set(bm.coords, "V_Tiling", desc.effective_v_tiling)
            _try_set(bm.coords, "u_tiling", desc.effective_u_tiling)
            _try_set(bm.coords, "v_tiling", desc.effective_v_tiling)
        except Exception:
            pass
    return bm


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


def _try_set(obj, attr: str, value) -> None:
    try:
        setattr(obj, attr, value)
    except Exception:
        pass


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
