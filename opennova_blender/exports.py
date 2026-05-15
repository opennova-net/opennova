"""Blender-specific exports (.blend save).

Lives outside import_runner.py so import_runner stays focused on building
the bpy scene from a 3DI3 model.
"""
from __future__ import annotations

import logging
import os


_log = logging.getLogger(__name__)


def save_blend_scene(output_dir: str, name: str, *, checkpoint: bool = False) -> None:
    """Save the current bpy scene to a .blend file in ``output_dir``."""
    import bpy

    blend_path = os.path.join(output_dir, name + ".blend")
    if checkpoint:
        _log.debug("[CKPT] save_as_mainfile start (%s)", blend_path)
        for handler in _log.root.handlers:
            handler.flush()
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    if checkpoint:
        _log.info("[CKPT] save_as_mainfile done -> Wrote blend: %s", blend_path)
        for handler in _log.root.handlers:
            handler.flush()
    else:
        _log.info("Wrote blend: %s", blend_path)
