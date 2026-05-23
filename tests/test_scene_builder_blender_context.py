"""bpy-backed scene_builder tests.

The standalone bpy package corrupts later worker subprocess imports if it has
been loaded in the parent pytest process, so these checks run in child Python
processes and keep the parent process Blender-free.
"""
from __future__ import annotations

from tests.blender_subprocess import run_blender_python


BLENDER_VERSION_SCRIPT = """
import bpy

assert (5, 0, 0) <= bpy.app.version < (6, 0, 0)
"""


SET_OBJECT_MODE_SCRIPT = """
import bpy

from apps.importer.scene_builder import _set_object_mode


bpy.ops.wm.read_homefile(use_empty=True)

armature_data = bpy.data.armatures.new("ContextArmature")
armature_obj = bpy.data.objects.new("ContextSkeleton", armature_data)
bpy.context.collection.objects.link(armature_obj)
bpy.context.view_layer.update()

for obj in bpy.context.scene.objects:
    obj.select_set(False)

try:
    _set_object_mode(armature_obj, "EDIT")
    assert armature_obj.mode == "EDIT"

    bone = armature_data.edit_bones.new("BN01")
    bone.head = (0.0, 0.0, 0.0)
    bone.tail = (0.0, 0.0, 0.1)

    _set_object_mode(armature_obj, "OBJECT")
    assert armature_obj.mode == "OBJECT"
finally:
    if getattr(armature_obj, "mode", "OBJECT") != "OBJECT":
        _set_object_mode(armature_obj, "OBJECT")
    bpy.ops.wm.read_homefile(use_empty=True)
"""


def test_bpy_dependency_is_blender_5() -> None:
    run_blender_python(BLENDER_VERSION_SCRIPT)


def test_set_object_mode_uses_explicit_context() -> None:
    run_blender_python(SET_OBJECT_MODE_SCRIPT)
