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

_log = logging.getLogger(__name__)
_headless_initialized = False


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
    import bpy
    bpy.ops.wm.read_homefile(use_empty=True)
    _headless_initialized = True


def save_blend(filepath: str) -> None:
    """Save the current Blender scene to a .blend file."""
    import bpy
    bpy.ops.wm.save_as_mainfile(filepath=str(filepath))
