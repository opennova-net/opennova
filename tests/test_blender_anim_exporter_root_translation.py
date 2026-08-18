"""The .bad root translation channel is measured against the ROOT'S OWN reset position.

The model table the runtime pairs .bad channels with is rebuilt root-relative (row 0 sits at
the model origin, every pivot and vertex is offset from it), so a rig authored with its root
away from the armature origin -- a viewmodel whose hips rest at standing height -- must not
export that authoring offset as a per-frame root translation: it would lift the whole rig by
that offset in-game (retail viewmodels carry a zero root translation). Only real root motion
inside a clip is a translation.

In-process ``bpy`` test (see test_scene_builder_blender_context.py for the import caveat).
"""
from __future__ import annotations

import math


def _build_rig(bpy, root_head):
    bpy.ops.wm.read_homefile(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 30
    arm = bpy.data.armatures.new("RootRelRig")
    obj = bpy.data.objects.new("RootRelRig", arm)
    scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    root = arm.edit_bones.new("BN01 root")
    root.head = root_head
    root.tail = (root_head[0], root_head[1], root_head[2] + 0.2)
    child = arm.edit_bones.new("BN02 child")
    child.head = root.tail
    child.tail = (root.tail[0], root.tail[1], root.tail[2] + 0.2)
    child.parent = root
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in obj.pose.bones:
        pb.rotation_mode = "XYZ"
    return obj


def _make_actions(bpy, obj, root_step):
    """anim_reset = the rest pose; anim_wpn_idle moves the root by root_step (Blender x) at frame 4."""
    ad = obj.animation_data_create()
    reset = bpy.data.actions.new("anim_reset")
    idle = bpy.data.actions.new("anim_wpn_idle")
    for act, last, moving in ((reset, 1, False), (idle, 4, True)):
        ad.action = act
        if hasattr(ad, "action_slot") and len(act.slots):
            ad.action_slot = act.slots[0]
        root = obj.pose.bones["BN01 root"]
        for f in range(0, last + 1):
            root.location = (root_step * f / last if moving else 0.0, 0.0, 0.0)
            root.rotation_euler = (0.0, 0.0, 0.0)
            root.keyframe_insert("location", frame=f)
            root.keyframe_insert("rotation_euler", frame=f)
            obj.pose.bones["BN02 child"].keyframe_insert("rotation_euler", frame=f)
        act.use_frame_range = True
        act.frame_start = 0
        act.frame_end = last
    ad.action = None
    return reset, idle


def _root_translation_track(bpy, obj, reset, idle):
    from blender import anim_exporter as ae

    exporter = ae.NovalogicAnimExporter(bpy.context, obj)
    reset_clip = ae._ClipData(action=reset, action_name="anim_reset", bad_name="RST", start_frame=0,
                              end_frame=1, is_reset=True, flags=ae.ANIM_FLAG_TRANSLATION)
    idle_clip = ae._ClipData(action=idle, action_name="anim_wpn_idle", bad_name="1i", start_frame=0,
                             end_frame=4, is_reset=False, flags=ae.ANIM_FLAG_TRANSLATION)
    exporter.configure_from_reset_clip(reset_clip)
    old = exporter._set_eval_action(idle)
    try:
        _channels, translations, _events = exporter._extract_channels(idle_clip, True)
    finally:
        exporter._restore_eval_action(old)
    old = exporter._set_eval_action(reset)
    try:
        _channels, reset_translations, _events = exporter._extract_channels(reset_clip, True)
    finally:
        exporter._restore_eval_action(old)
    return [row[0] for row in translations], [row[0] for row in reset_translations]


def _norm(v):
    return math.sqrt(sum(c * c for c in v))


def test_off_origin_root_exports_zero_translation_at_reset() -> None:
    import bpy

    obj = _build_rig(bpy, (0.0, 0.05, 1.01))   # hips authored at standing height
    reset, idle = _make_actions(bpy, obj, root_step=0.1)
    idle_root, reset_root = _root_translation_track(bpy, obj, reset, idle)

    # The authoring offset is not a translation: reset and idle frame 0 are the rest.
    assert all(_norm(t) < 1e-5 for t in reset_root), reset_root
    assert _norm(idle_root[0]) < 1e-5, idle_root[0]
    # Real root motion inside the clip survives, measured from the root's rest (Blender +x is BAD +x).
    assert abs(idle_root[4][0] - 0.1) < 1e-5 and abs(idle_root[4][1]) < 1e-5 and abs(idle_root[4][2]) < 1e-5, idle_root[4]


def test_root_at_origin_is_unchanged() -> None:
    import bpy

    obj = _build_rig(bpy, (0.0, 0.0, 0.0))
    reset, idle = _make_actions(bpy, obj, root_step=0.1)
    idle_root, reset_root = _root_translation_track(bpy, obj, reset, idle)
    assert all(_norm(t) < 1e-5 for t in reset_root), reset_root
    assert abs(idle_root[4][0] - 0.1) < 1e-5, idle_root[4]
