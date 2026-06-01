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


def test_sampled_action_places_child_heads_with_non_identity_rest_axes() -> None:
    import bpy
    from mathutils import Vector

    from apps.importer.scene_builder import BlenderSceneBuilder, _set_object_mode
    from pyopennova.animation_build import (
        IDENTITY_QUAT,
        SampledBoneFrame,
        SampledClip,
        SampledFrame,
    )

    bpy.ops.wm.read_homefile(use_empty=True)

    armature_data = bpy.data.armatures.new("SampledArmature")
    armature_obj = bpy.data.objects.new("Skeleton", armature_data)
    bpy.context.collection.objects.link(armature_obj)

    _set_object_mode(armature_obj, "EDIT")
    root = armature_data.edit_bones.new("BN01")
    root.head = (0.0, 0.0, 0.0)
    root.tail = (0.0, 0.0, 1.0)

    child = armature_data.edit_bones.new("BN02")
    child.head = (0.0, 0.0, 1.0)
    child.tail = (0.0, 0.0, 2.0)
    child.parent = root
    child.use_connect = False
    _set_object_mode(armature_obj, "OBJECT")
    armature_obj.animation_data_create()

    builder = BlenderSceneBuilder.__new__(BlenderSceneBuilder)
    builder._bone_infos = [
        ("BN01", -1, (0.0, 0.0, 0.0), None, 0.0),
        ("BN02", 0, (0.0, 0.0, 1.0), None, 0.0),
    ]
    builder._cache_rest_local_transforms(armature_obj, builder._bone_infos)

    clip = SampledClip(
        name="sampled",
        bad_name="",
        flags=0,
        fps=30,
        frame_count=1,
        start_frame=1,
        end_frame=1,
        is_reset=False,
        source_format="lw",
        frames=(
            SampledFrame(
                frame_index=0,
                frame=1,
                bones=(
                    SampledBoneFrame(
                        bone_index=0,
                        name="BN01",
                        parent_index=-1,
                        world_rotation=IDENTITY_QUAT,
                        world_position=(0.0, 0.0, 0.0),
                        local_rotation=IDENTITY_QUAT,
                        local_position=(0.0, 0.0, 0.0),
                        source_rotation_xyzw=(0.0, 0.0, 0.0, 1.0),
                    ),
                    SampledBoneFrame(
                        bone_index=1,
                        name="BN02",
                        parent_index=0,
                        world_rotation=IDENTITY_QUAT,
                        world_position=(0.0, 0.0, 1.0),
                        local_rotation=IDENTITY_QUAT,
                        local_position=(0.0, 0.0, 1.0),
                        source_rotation_xyzw=(0.0, 0.0, 0.0, 1.0),
                    ),
                ),
            ),
        ),
    )

    action = builder._build_action_from_sampled_clip(clip, armature_obj)
    armature_obj.animation_data.action = action
    if hasattr(action, "slots") and len(action.slots) > 0:
        armature_obj.animation_data.action_slot = action.slots[0]

    bpy.context.scene.frame_set(1)
    bpy.context.view_layer.update()

    child_head = armature_obj.matrix_world @ armature_obj.pose.bones["BN02"].matrix.translation
    assert (child_head - Vector((0.0, 0.0, 1.0))).length < 1e-4
