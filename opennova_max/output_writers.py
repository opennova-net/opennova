"""3ds Max output writers for ASE and OpenNova project files."""
from __future__ import annotations

import os

from pyopennova.host_outputs import write_host_neutral_outputs
from pyopennova.texture_outputs import copy_model_textures


def _rt():
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; opennova_max only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime


def reset_scene() -> None:
    """Reset the active Max scene without prompting."""
    rt = _rt()
    try:
        rt.resetMaxFile(rt.Name("noPrompt"))
    except Exception:
        rt.execute("resetMaxFile #noPrompt")


def save_max_scene(output_dir: str, name: str) -> str:
    """Save the current 3ds Max scene as ``<name>.max`` in ``output_dir``."""
    os.makedirs(output_dir, exist_ok=True)
    path = os.path.join(output_dir, name + ".max")
    rt = _rt()
    try:
        result = rt.saveMaxFile(path, quiet=True)
        if result is False:
            raise RuntimeError(f"saveMaxFile returned false for {path}")
    except TypeError:
        result = rt.saveMaxFile(path)
        if result is False:
            raise RuntimeError(f"saveMaxFile returned false for {path}")
    except Exception:
        rt.execute(f"saveMaxFile {_maxscript_string(path)} quiet:true")
    return path


def write_outputs(
    ir,
    output_dir: str,
    name: str,
    builder,
    *,
    write_ase: bool = True,
    write_3dp: bool = True,
    write_max: bool = True,
    copy_textures: bool = True,
) -> list[str]:
    """Write selected Max outputs and return the paths produced."""
    if ir is None:
        raise ValueError("write_outputs requires a model data")
    os.makedirs(output_dir, exist_ok=True)
    written: list[str] = []

    if write_ase or write_3dp:
        written.extend(write_host_neutral_outputs(
            ir,
            output_dir,
            name,
            write_ase=write_ase,
            write_3dp=write_3dp,
            include_collisions=getattr(builder, "import_collisions", True),
            include_occlusion=getattr(builder, "import_occlusion", True),
            include_lights=getattr(builder, "import_lights", True),
            bad_file=getattr(builder, "bad_file", None),
            collision_lod_index=None,
        ))

    if write_max:
        written.append(save_max_scene(output_dir, name))

    if copy_textures:
        written.extend(copy_model_textures(
            ir,
            output_dir,
            resolver=getattr(builder, "resolver", None),
        ))

    return written


def _maxscript_string(value: str) -> str:
    escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'
