"""3ds Max scene output helpers."""
from __future__ import annotations

import os
from typing import Any


def write_outputs(
    model,
    output_dir: str,
    output_name: str,
    builder=None,
    resolver=None,
    *,
    write_max: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
    write_anims: bool = False,
    copy_textures: bool = True,
) -> list[str]:
    """Write selected outputs for the current Max scene.

    ``.ase`` is intentionally written through the Max current-scene exporter,
    not through the DCC-neutral IR ASE writer. That keeps Max/Blender parity
    tied to what each DCC plugin actually exports. ``.bad``/``.adm`` animations
    are likewise sourced from the live skeleton (off by default; enable for
    batch/headless parity with the interactive Anims export).
    """
    del builder
    os.makedirs(output_dir, exist_ok=True)
    written: list[str] = []
    if write_3dp:
        written.extend(_write_project_outputs(model, output_dir, output_name))
    if write_ase:
        written.append(export_ase_scene(output_dir, output_name))
    if write_max:
        written.append(save_max_scene(output_dir, output_name))
    if write_anims:
        written.extend(_write_anim_outputs(output_dir, output_name))
    if copy_textures and resolver is not None:
        written.extend(_copy_texture_outputs(model, output_dir, resolver))
    return written


def _write_anim_outputs(output_dir: str, output_name: str) -> list[str]:
    from .anim_scene_exporter import AnimSceneExporter

    adm_path = os.path.join(output_dir, output_name + ".adm")
    if AnimSceneExporter(_rt()).export(adm_path):
        return [adm_path]
    return []


def _write_project_outputs(model, output_dir: str, output_name: str) -> list[str]:
    from pyopennova.project_writer import write_3dp_from_3di3

    tdp_path = os.path.join(output_dir, output_name + ".3dp")
    return list(write_3dp_from_3di3(model, tdp_path))


def _copy_texture_outputs(model, output_dir: str, resolver) -> list[str]:
    from pyopennova.texture_outputs import copy_model_textures

    return list(copy_model_textures(model, output_dir, resolver=resolver))


def reset_scene():
    # type: () -> None
    rt = _rt()
    try:
        rt.resetMaxFile(rt.Name("noPrompt"))
    except Exception:
        rt.execute("resetMaxFile #noPrompt")


def save_max_scene(output_dir, name):
    # type: (str, str) -> str
    os.makedirs(output_dir, exist_ok=True)
    path = os.path.join(output_dir, name + ".max")
    rt = _rt()
    try:
        result = rt.saveMaxFile(path, quiet=True)
    except TypeError:
        result = rt.saveMaxFile(path)
    except Exception:
        result = rt.execute("saveMaxFile %s quiet:true" % _maxscript_string(path))
    if result is False:
        raise RuntimeError("saveMaxFile returned false for %s" % path)
    return path


def export_ase_scene(output_dir, name):
    # type: (str, str) -> str
    from .ase_scene_exporter import AseSceneExporter

    os.makedirs(output_dir, exist_ok=True)
    path = os.path.join(output_dir, name + ".ase")
    AseSceneExporter(_rt()).export_scene(path)
    return path


def _rt():
    # type: () -> Any
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover - only available inside Max
        raise RuntimeError("pymxs is not available; opennova_max only runs inside 3ds Max.") from exc
    return pymxs.runtime


def _maxscript_string(value):
    # type: (str) -> str
    escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return '"%s"' % escaped
