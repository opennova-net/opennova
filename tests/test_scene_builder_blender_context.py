"""In-process bpy tests for ``scene_builder``.

These tests intentionally load ``bpy`` in the parent pytest process. The
standalone ``bpy`` package corrupts subsequent subprocess imports once it's
been loaded, so ``import bpy`` is deferred to inside each test function.
That lets the dispatcher-based integration tests (which spawn workers) run
first in the same pytest session without interference.
"""
from __future__ import annotations

from types import SimpleNamespace


def _fake_lw_skinned_ir():
    def vertex(position, bone_index):
        return SimpleNamespace(
            position=position,
            normal=(0.0, 1.0, 0.0),
            uv0=(0.0, 0.0),
            uv1=(0.0, 0.0),
            bone_weights=(1.0, 0.0, 0.0, 0.0),
            bone_indices=(bone_index, 0, 0, 0),
        )

    parts = [
        SimpleNamespace(
            parent_index=-1,
            abs_position=(10.0, 0.0, 0.0),
            rel_position=(10.0, 0.0, 0.0),
        ),
        SimpleNamespace(
            parent_index=0,
            abs_position=(12.0, 0.0, 0.0),
            rel_position=(2.0, 0.0, 0.0),
        ),
    ]
    prim = SimpleNamespace(
        part_index=1,
        material_index=0,
        index_offset=0,
        index_count=3,
        vertex_offset=0,
        bone_table_length=1,
        bone_table=(1,),
    )
    lod = SimpleNamespace(
        part_count=2,
        primitive_count=1,
        vertex_count=3,
        parts=parts,
        primitives=(prim,),
        indices=(0, 1, 2),
        vertices=(
            vertex((13.0, 0.0, 0.0), 0),
            vertex((13.0, 1.0, 0.0), 0),
            vertex((13.0, 0.0, 1.0), 0),
        ),
    )
    return SimpleNamespace(
        lod_count=1,
        lds=(lod,),
        lods=(lod,),
        mesh_type=3,
        material_count=0,
    )


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


def test_lw_skinned_mesh_uses_root_object_space_for_armature_binding() -> None:
    import bpy
    from mathutils import Vector
    from apps.importer.scene_builder import BlenderSceneBuilder
    from blender.math_utils import render_space

    bpy.ops.wm.read_homefile(use_empty=True)
    try:
        builder = BlenderSceneBuilder(
            _fake_lw_skinned_ir(),
            import_collisions=False,
            import_occlusion=False,
            import_lights=False,
        )
        builder.build_part_hierarchy("SkinnedLW")
        meshes = builder.create_basic_meshes("SkinnedLW")

        mesh_obj = next(obj for obj in meshes if len(obj.data.vertices) > 0)
        assert mesh_obj.parent == builder.root_object
        assert mesh_obj.data.vertices[0].co == render_space(Vector((13.0, 0.0, 0.0)))
    finally:
        bpy.ops.wm.read_homefile(use_empty=True)


def test_lw_animation_context_is_not_applied_until_saf_mapping_is_reversed() -> None:
    import bpy
    from apps.importer.scene_builder import BlenderSceneBuilder

    bpy.ops.wm.read_homefile(use_empty=True)
    try:
        builder = BlenderSceneBuilder(
            _fake_lw_skinned_ir(),
            import_collisions=False,
            import_occlusion=False,
            import_lights=False,
        )
        builder.build_part_hierarchy("SkinnedLW")
        builder.build_armature_from_parts("SkinnedLW")
        ctx = SimpleNamespace(
            movement_clips={
                "BrokenIfApplied": SimpleNamespace(
                    slot_id=1,
                    source_name="test.saf",
                    source_type="saf",
                    frame_count=1,
                    loop_frame=0,
                    frames=(SimpleNamespace(part_records=((0, 1234),), root_values=(0,) * 9),),
                )
            },
            clips={},
        )

        action_count = len(bpy.data.actions)
        builder.build_lw_animations_from_context(ctx)

        assert len(bpy.data.actions) == action_count
        assert len(builder.armature_object.animation_data.nla_tracks) == 0
        assert builder.armature_object.get("lw_animation_status") == "parsed_not_applied_pending_re"
    finally:
        bpy.ops.wm.read_homefile(use_empty=True)
