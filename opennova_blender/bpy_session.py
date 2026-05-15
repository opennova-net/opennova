"""Headless bpy session management.

Each batch import runs in its own subprocess (see ``apps.importer.dispatcher``),
so this module's only job is to call ``read_homefile`` exactly once on the
worker side. The cross-import ``new_scene()`` purge is gone - process
isolation makes scene reset unnecessary.

``bpy`` is imported lazily inside each function. The standalone ``bpy as
module`` package corrupts subsequent subprocess imports if it has been
imported in the parent process, so any code path that doesn't actually need
bpy must avoid loading this module's dependencies at import time.

Requires the standalone ``bpy`` package: ``pip install bpy``.
"""
import logging
import sys

_log = logging.getLogger(__name__)
_headless_initialized = False


def _is_inherited_bpy_script_path(path: str) -> bool:
    normalized = path.replace("\\", "/").lower()
    return "/site-packages/bpy/" in normalized and "/scripts/" in normalized


def _drop_inherited_bpy_script_paths() -> None:
    """Remove standalone bpy script paths copied from a parent process."""
    sys.path[:] = [
        path for path in sys.path
        if not _is_inherited_bpy_script_path(str(path))
    ]


def init_headless() -> None:
    """Initialize a clean, empty Blender scene for headless use.

    Calls ``read_homefile`` exactly once per process to set up the bpy
    context (view layer, active_object, window, etc.). Subsequent calls in
    the same process are no-ops; that path is mostly defensive since each
    worker subprocess only handles one import.
    """
    global _headless_initialized
    if _headless_initialized:
        _log.debug("init_headless: already initialized, skipping read_homefile")
        return
    if "bpy" not in sys.modules:
        _drop_inherited_bpy_script_paths()
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)
    _headless_initialized = True


def save_blend(filepath: str) -> None:
    """Save the current Blender scene to a .blend file."""
    import bpy
    bpy.ops.wm.save_as_mainfile(filepath=str(filepath))
