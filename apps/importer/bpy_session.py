"""Headless bpy session management.

Call init_headless() once at startup before any import operations.
Requires the standalone `bpy` package: pip install bpy
"""
import logging
import bpy

_log = logging.getLogger(__name__)
_headless_initialized = False


def _ckpt(msg: str) -> None:
    _log.debug("[CKPT] %s", msg)
    for h in logging.root.handlers:
        h.flush()


def init_headless() -> None:
    """Initialize a clean, empty Blender scene for headless use.

    Calls read_homefile exactly once per process to set up the bpy context
    (view layer, active_object, etc.). Safe to call from multiple worker
    threads — subsequent calls are no-ops.
    """
    global _headless_initialized
    if _headless_initialized:
        _log.debug("init_headless: already initialized, skipping read_homefile")
        return
    _ckpt("read_homefile start")
    bpy.ops.wm.read_homefile(use_empty=True)
    _headless_initialized = True
    _ckpt("read_homefile done")


def save_blend(filepath: str) -> None:
    """Save the current Blender scene to a .blend file."""
    bpy.ops.wm.save_as_mainfile(filepath=str(filepath))


def new_scene() -> None:
    """Reset bpy to a clean empty state between imports.

    Uses manual data-block removal rather than read_homefile, which segfaults
    on the second call on Windows with the standalone bpy package.
    Requires init_headless() to have been called once first.
    """
    _ckpt("purge objects start")
    for blk in list(bpy.data.objects):     bpy.data.objects.remove(blk,      do_unlink=True)
    _ckpt("purge meshes start")
    for blk in list(bpy.data.meshes):      bpy.data.meshes.remove(blk,       do_unlink=True)
    _ckpt("purge armatures start")
    for blk in list(bpy.data.armatures):   bpy.data.armatures.remove(blk,    do_unlink=True)
    _ckpt("purge actions start")
    for blk in list(bpy.data.actions):     bpy.data.actions.remove(blk,      do_unlink=True)
    _ckpt("purge materials start")
    for blk in list(bpy.data.materials):   bpy.data.materials.remove(blk,    do_unlink=True)
    _ckpt("purge images start")
    for blk in list(bpy.data.images):      bpy.data.images.remove(blk,       do_unlink=True)
    _ckpt("purge textures start")
    for blk in list(bpy.data.textures):    bpy.data.textures.remove(blk,     do_unlink=True)
    _ckpt("purge cameras start")
    for blk in list(bpy.data.cameras):     bpy.data.cameras.remove(blk,      do_unlink=True)
    _ckpt("purge lights start")
    for blk in list(bpy.data.lights):      bpy.data.lights.remove(blk,       do_unlink=True)
    _ckpt("purge curves start")
    for blk in list(bpy.data.curves):      bpy.data.curves.remove(blk,       do_unlink=True)
    _ckpt("purge collections start")
    # Skip the scene's master collection; only remove user-created ones.
    master = bpy.context.scene.collection
    for blk in list(bpy.data.collections):
        if blk != master:
            bpy.data.collections.remove(blk, do_unlink=True)
    _ckpt("new_scene purge complete")
