from __future__ import annotations

import bpy

from apps.importer.scene_builder import _set_object_mode


def test_bpy_dependency_is_blender_5() -> None:
    assert (5, 0, 0) <= bpy.app.version < (6, 0, 0)


def test_set_object_mode_uses_explicit_context() -> None:
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
