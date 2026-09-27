"""What the clip and weapon tests (anim_test.py, weapon_test.py) author from
scratch: a model root and its LOD 0, an Armature of BN## bones, Actions keyed
on it, the table's rows, and the checks each case reports. The file name
keeps it out of the *_test.py set that runs as tests.
"""
import math
import os

import bpy
from mathutils import Matrix

FAILURES = []


def check(ok, what, detail=""):
    print(("PASS " if ok else "FAIL ") + what + ("" if ok or not detail else f": {detail}"))
    if not ok:
        FAILURES.append(what)


def refusal(fn, *errors):
    """What `fn` was refused with (one of `errors`), or None when it ran."""
    try:
        fn()
    except errors as e:
        return str(e)
    return None


def run_cases(scope, name):
    """Every test_ function of `scope` in name order, each on its own: one
    that breaks is a failure and the others still run."""
    import traceback
    for case, fn in sorted((n, f) for n, f in scope.items() if n.startswith("test_") and callable(f)):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            check(False, f"{case} ran", repr(e))
    if FAILURES:
        raise SystemExit(f"{name}: {len(FAILURES)} checks failed: " + "; ".join(FAILURES))
    print(f"{name}: all checks passed")


def fresh_scene():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    for collection in (bpy.data.actions, bpy.data.armatures, bpy.data.meshes, bpy.data.cameras):
        for item in list(collection):
            collection.remove(item)
    scene = bpy.context.scene
    scene.render.fps, scene.render.fps_base = 30, 1.0
    scene.frame_set(0)


def link(ob, parent=None):
    bpy.context.scene.collection.objects.link(ob)
    ob.parent = parent
    return ob


def model_root(name, at=(0.0, 0.0, 0.0)):
    root = link(bpy.data.objects.new(name, None))
    root.location = at
    lod = link(bpy.data.objects.new(f"{name}_LOD0", None), root)
    lod["_lod_index"] = 0
    return root, lod


def make_armature(lod, bones, name="Rig"):
    """An Armature under the LOD root: bones [(name, head, tail, parent)]."""
    arm = link(bpy.data.objects.new(name, bpy.data.armatures.new(name)), lod)
    vl = bpy.context.view_layer
    with bpy.context.temp_override(active_object=arm, object=arm, selected_objects=[arm]):
        vl.objects.active = arm
        bpy.ops.object.mode_set(mode="EDIT")
        for bone, head, tail, parent in bones:
            eb = arm.data.edit_bones.new(bone)
            eb.head, eb.tail = head, tail
            if parent is not None:
                eb.parent = arm.data.edit_bones[parent]
        bpy.ops.object.mode_set(mode="OBJECT")
    vl.update()
    arm.animation_data_create()
    return arm


# A first-person gun's rig: no Root, BN01 at the model origin.
GUN_BONES = [("BN01", (0.0, 0.0, 0.0), (0.0, 0.1, 0.0), None),
             ("BN02 Bolt", (0.0, 0.05, 0.05), (0.0, 0.15, 0.05), "BN01"),
             ("BN03 Mag", (0.0, 0.1, -0.05), (0.0, 0.1, -0.15), "BN01")]


def gun(out, name="gun", at=(0.0, 0.0, 0.0)):
    """A first-person gun: a model root, its LOD 0 and a three-part rig, its
    table written into a folder of its own under `out`."""
    root, lod = model_root(name, at)
    arm = make_armature(lod, GUN_BONES, f"{name}_rig")
    root.o3d.adm_path = os.path.join(out, name, f"{name}.adm")
    return root, arm


def rest(arm):
    for pb in arm.pose.bones:
        pb.rotation_mode = "QUATERNION"
        pb.location = (0.0, 0.0, 0.0)
        pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)


def clip(arm, name, keys, frame_range=None, cyclic=False, fps=30):
    """An Action keyed linearly on the rig: {frame: {bone: {"rot": (axis,
    degrees), "loc": (x, y, z), "scale": (x, y, z)}}}, with a manual frame
    range when given."""
    action = bpy.data.actions.new(name)
    data = arm.animation_data
    data.action = action
    for frame, poses in keys.items():
        rest(arm)
        for bone, pose in poses.items():
            pb = arm.pose.bones[bone]
            if "rot" in pose:
                axis, degrees = pose["rot"]
                pb.rotation_quaternion = Matrix.Rotation(math.radians(degrees), 3, axis).to_quaternion()
                pb.keyframe_insert("rotation_quaternion", frame=frame)
            if "loc" in pose:
                pb.location = pose["loc"]
                pb.keyframe_insert("location", frame=frame)
            if "scale" in pose:
                pb.scale = pose["scale"]
                pb.keyframe_insert("scale", frame=frame)
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for curve in bag.fcurves:
                    for point in curve.keyframe_points:
                        point.interpolation = "LINEAR"
    if frame_range is not None:
        action.use_frame_range = True
        action.frame_start, action.frame_end = frame_range
    action.use_cyclic = cyclic
    action.o3d.fps = fps
    data.action = None
    rest(arm)
    return action


def bolt_turns(degrees=20.0, end=10):
    return {0: {"BN02 Bolt": {"rot": ("X", 0.0)}}, end: {"BN02 Bolt": {"rot": ("X", degrees)}}}


def bolt_moves(end=4):
    return {0: {"BN02 Bolt": {"loc": (0.0, 0.0, 0.0)}}, end: {"BN02 Bolt": {"loc": (0.0, -0.02, 0.0)}}}


def set_rows(model, rows):
    model.o3d.rows.clear()
    for key, actions in rows:
        row = model.o3d.rows.add()
        row.key = key
        for action in actions:
            row.variants.add().action = action
