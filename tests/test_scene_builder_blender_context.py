"""In-process bpy tests for ``scene_builder``.

These tests intentionally load ``bpy`` in the parent pytest process. The
standalone ``bpy`` package corrupts subsequent subprocess imports once it's
been loaded, so ``import bpy`` is deferred to inside each test function.
That lets the dispatcher-based integration tests (which spawn workers) run
first in the same pytest session without interference.
"""
from __future__ import annotations


def test_bpy_dependency_is_blender_5() -> None:
    import bpy
    assert (5, 0, 0) <= bpy.app.version < (6, 0, 0)


def test_set_object_mode_uses_explicit_context() -> None:
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
