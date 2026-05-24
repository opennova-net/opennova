"""Current-scene Novalogic ASE export entry point for 3ds Max."""
from __future__ import annotations

import os
from typing import Any


class AseSceneExporter:
    """Export the active 3ds Max scene to one ASE file."""

    def __init__(self, rt):
        # type: (Any) -> None
        self.rt = rt

    def export_scene(self, filepath):
        # type: (str) -> bool
        if not filepath:
            raise ValueError("No output filepath specified")
        output_dir = os.path.dirname(filepath)
        if output_dir:
            os.makedirs(output_dir, exist_ok=True)
        return _export_native_ase(self.rt, filepath)


def export_scene_with_dialog():
    # type: () -> bool
    rt = _rt()
    filepath = _prompt_save_path(
        rt,
        caption="Export Novalogic ASE",
        types="Novalogic ASE (*.ase)|*.ase|All Files (*.*)|*.*|",
        extension=".ase",
    )
    if not filepath:
        return False
    return AseSceneExporter(rt).export_scene(filepath)


def _rt():
    # type: () -> Any
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover - only available inside Max
        raise RuntimeError(
            "pymxs is not available; 3ds Max scene export only runs inside 3ds Max."
        ) from exc
    return pymxs.runtime


def _prompt_save_path(rt, caption, types, extension):
    # type: (Any, str, str, str) -> str
    try:
        value = rt.getSaveFileName(caption=caption, types=types)
    except Exception:
        value = rt.execute(
            'getSaveFileName caption:"%s" types:"%s"'
            % (caption.replace('"', "'"), types.replace('"', "'"))
        )
    if value is None:
        return ""
    path = str(value)
    if not path:
        return ""
    root, ext = os.path.splitext(path)
    if not ext:
        path = root + extension
    return path


def _export_native_ase(rt, filepath):
    # type: (Any, str) -> bool
    try:
        result = rt.exportFile(
            filepath,
            rt.Name("noPrompt"),
            selectedOnly=False,
            using=rt.ASEEXP,
        )
    except Exception:
        result = rt.exportFile(filepath, rt.Name("noPrompt"), selectedOnly=False)
    if result is False:
        raise RuntimeError("3ds Max ASE export failed for %s" % filepath)
    return True
