"""Small 3ds Max scene output helpers."""
from __future__ import annotations

import os
from typing import Any


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
