"""The add-on's clip sets: animation.py's export and anim_import.py's import,
authored from scratch in an empty scene (no assets) and run through the
opennova-3di the loader names:

    blender -b --factory-startup --python-exit-code 1 --python tests/blender/anim_test.py -- <opennova-3di.exe>

Each case builds its own model, writes into its own folder under a temporary
directory and checks one rule of the clip set: the clips are the Actions the
rows name, named after the table and their slot; the reset's last clip is the
bind and must start at rest, and a table without one gets a clip of the rest
pose; the loop, the length and the translation flag come from the Action; a
first-person rig's clips move BN01 by its own row and stand every event
still; triggers are markers; what retail cannot load is refused; import builds
Actions, merges rows and makes a static model's parts a rig, and never turns a
rest bone: a bind pointing a bone elsewhere becomes its bind frame, and the
clips pose and export as they did; a body's set stands the rig on a Root.
"""
import math
import os
import subprocess
import sys

import bpy
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addon_harness  # noqa: E402
import clip_scene  # noqa: E402
from clip_scene import (GUN_BONES, bolt_moves, bolt_turns, check, clip, fresh_scene, link,  # noqa: E402
                        make_armature, model_root, rest, set_rows)

addon, CLI = addon_harness.load()
animation, anim_import, rig = addon.animation, addon.anim_import, addon.rig
ExportError, ImportFailed = addon.o3dtext.ExportError, addon.o3dtext.ImportFailed
OUT = addon_harness.scratch("opennova_anim_test_")


def refusal(fn):
    return clip_scene.refusal(fn, ExportError, ImportFailed)


def gun(name="gun", at=(0.0, 0.0, 0.0)):
    return clip_scene.gun(OUT, name, at)


def export(model):
    return animation.export_animations(bpy.context, model)


def scene_of(path, name="set.o3a"):
    """A table or clip read back through `opennova-3di anim scene`."""
    out = os.path.join(os.path.dirname(path), name)
    subprocess.run([CLI, "anim", "scene", path, "-o", out], check=True, capture_output=True)
    return anim_import.read_o3a(out)


def clip_named(s, name):
    return next(c for c in s["clips"] if c["name"].lower() == name.lower())


# --- the clip set -------------------------------------------------------------

def test_clip_set_is_the_rows():
    fresh_scene()
    model, arm = gun()
    idle = clip(arm, "Idle", bolt_turns())
    fire = clip(arm, "Fire", bolt_moves())
    clip(arm, "Unused", bolt_turns(45.0))
    set_rows(model, [("anim_wpn_idle", [idle]), ("anim_wpn_fire", [fire])])
    _, notes = export(model)
    folder = os.path.dirname(model.o3d.adm_path)
    files = sorted(f for f in os.listdir(folder) if f.endswith((".bad", ".adm")))
    check(files == ["gun.adm", "gun_f.bad", "gun_i.bad", "gun_rst.bad"], "the clips are the rows' Actions, named "
          "after the table and their slot, with no NLA track", files)
    s = scene_of(model.o3d.adm_path)
    rows = [(k, [anim_import.clip_stem(v) for v in vs]) for k, vs in s["rows"]]
    check(rows == [("anim_reset", ["gun_rst"]), ("anim_wpn_idle", ["gun_i"]), ("anim_wpn_fire", ["gun_f"])],
          "the table gains a reset row first and names each clip file", rows)
    check(any("Unused" in n and "not exported" in n for n in notes), "an Action on no row is noted, not exported",
          notes)


def test_variant_names():
    fresh_scene()
    model, arm = gun()
    slots = {index: key for index, key in addon.catalog().slots}
    a, b, c = (clip(arm, n, bolt_turns(d)) for n, d in (("A", 10.0), ("B", 20.0), ("C", 30.0)))
    fire = clip(arm, "Fire", bolt_moves())
    set_rows(model, [(slots[1], [a, b]), (slots[12], [c]), ("anim_wpn_fire", [fire, a])])
    export(model)
    s = scene_of(model.o3d.adm_path)
    rows = {k: [anim_import.clip_stem(v) for v in vs] for k, vs in s["rows"]}
    check(rows[slots[1]] == ["gun_s1", "gun_s1_2"] and rows[slots[12]] == ["gun_s12"],
          "a numbered slot's second clip takes an underscore (s1_2, never slot 12's s12)", rows)
    check(rows["anim_wpn_fire"] == ["gun_f", "gun_s1"], "an Action on two rows is one file, named by its first row",
          rows)


def test_names_retail_can_pack():
    fresh_scene()
    model, arm = gun()
    draw = clip(arm, "Draw", bolt_turns())
    draw2 = clip(arm, "Draw2", bolt_turns(25.0))
    set_rows(model, [("anim_wpn_switchto", [draw, draw2])])
    model.o3d.adm_path = os.path.join(OUT, "names", "gun_long_table.adm")
    why = refusal(lambda: export(model))
    check(why is not None and "gun_long_table.adm" in why and "15" in why, "a table name over 15 characters is "
          "refused", why)
    model.o3d.adm_path = os.path.join(OUT, "names", "gunabcd.adm")
    why = refusal(lambda: export(model))
    check(why is not None and "Draw2" in why and "gunabcd_swt2.bad" in why and "shorten" in why,
          "a clip name over 15 characters is refused, naming the clip", why)
    set_rows(model, [("anim_wpn_switchto", [draw])])
    check(refusal(lambda: export(model)) is None, "a clip name of 15 characters exports (gunabcd_swt.bad)")


def test_rows_name_engine_slots():
    fresh_scene()
    model, arm = gun()
    idle = clip(arm, "Idle", bolt_turns())
    set_rows(model, [("anim_wpn_nonsense", [idle])])
    why = refusal(lambda: export(model))
    check(why is not None and "names no anim slot" in why, "a row naming no engine slot is refused", why)
    set_rows(model, [("anim_wpn_idle", [idle]), ("ANIM_WPN_IDLE", [idle])])
    why = refusal(lambda: export(model))
    check(why is not None and "both answer slot wpn_idle" in why, "two rows answering one slot are refused", why)


# --- the reset ----------------------------------------------------------------

def test_reset_from_the_rest_pose():
    fresh_scene()
    model, arm = gun()
    fire = clip(arm, "Fire", bolt_moves())
    turn = clip(arm, "Turn", bolt_turns())
    set_rows(model, [("anim_wpn_fire", [fire]), ("anim_wpn_idle", [turn])])
    export(model)
    s = scene_of(model.o3d.adm_path)
    reset = clip_named(s, "gun_rst")
    check(reset["frames"] == 1 and reset["fps"] == 30 and reset["flags"] == 0x3,
          "the reset a table lacks is one looping interval at 30 fps, translated as its set is",
          (reset["frames"], reset["fps"], hex(reset["flags"])))
    check(clip_named(s, "gun_f")["flags"] & 0x2 and not clip_named(s, "gun_i")["flags"] & 0x2,
          "a clip carries translations when a part moves off its rest offset, and only then")
    set_rows(model, [("anim_wpn_idle", [turn])])
    export(model)
    check(clip_named(scene_of(model.o3d.adm_path), "gun_rst")["flags"] == 0x1,
          "an untranslated set's reset carries no translation")


def test_reset_must_start_at_rest():
    fresh_scene()
    model, arm = gun()
    at_rest = clip(arm, "Bind", {0: {"BN02 Bolt": {"rot": ("X", 0.0)}}, 1: {"BN02 Bolt": {"rot": ("X", 0.0)}}})
    off = clip(arm, "Off", {0: {"BN03 Mag": {"rot": ("Z", 0.05)}}, 1: {"BN03 Mag": {"rot": ("Z", 0.05)}}})
    idle = clip(arm, "Idle", bolt_turns())
    set_rows(model, [("anim_reset", [at_rest]), ("anim_wpn_idle", [idle])])
    check(refusal(lambda: export(model)) is None, "a reset clip at rest exports")
    set_rows(model, [("anim_reset", [off]), ("anim_wpn_idle", [idle])])
    why = refusal(lambda: export(model))
    check(why is not None and "BN03 Mag" in why and "off its bind" in why, "a reset starting off rest is refused, "
          "naming the bone", why)
    set_rows(model, [("anim_reset", [off, at_rest]), ("anim_wpn_idle", [idle])])
    check(refusal(lambda: export(model)) is None, "only the reset row's LAST clip is the bind")
    set_rows(model, [("anim_reset", [at_rest, off]), ("anim_wpn_idle", [idle])])
    check(refusal(lambda: export(model)) is not None, "a reset row whose last clip is off rest is refused")


# --- a clip's own settings ----------------------------------------------------

def test_length_loop_rate_and_flag_8():
    fresh_scene()
    model, arm = gun()
    held = clip(arm, "Held", bolt_turns(), frame_range=(0, 20), cyclic=True, fps=24)
    held.o3d.raw_flag_8 = True
    keyed = clip(arm, "Keyed", {2: {"BN02 Bolt": {"rot": ("X", 0.0)}}, 7: {"BN02 Bolt": {"rot": ("X", 9.0)}}})
    set_rows(model, [("anim_wpn_idle", [held]), ("anim_wpn_fire", [keyed])])
    _, notes = export(model)
    s = scene_of(model.o3d.adm_path)
    c = clip_named(s, "gun_i")
    check(c["frames"] == 20 and c["flags"] & 0x1 and c["flags"] & 0x8 and c["fps"] == 24,
          "the manual frame range is the length, Cyclic the loop, the Clip rate the rate, Flag 8 carried",
          (c["frames"], hex(c["flags"]), c["fps"]))
    k = clip_named(s, "gun_f")
    check(k["frames"] == 5 and not k["flags"] & 0x1, "without a manual range the keyed range is the length",
          (k["frames"], hex(k["flags"])))
    check(any("Held 24 fps" in n and "30 fps" in n for n in notes), "a clip rate the scene does not play at is noted",
          notes)


def test_loop_rate_bound():
    fresh_scene()
    model, arm = gun()
    fast = clip(arm, "Fast", {0: {"BN02 Bolt": {"rot": ("X", 0.0)}}, 1: {"BN02 Bolt": {"rot": ("X", 5.0)}}},
                cyclic=True, fps=62)
    set_rows(model, [("anim_wpn_idle", [fast])])
    why = refusal(lambda: export(model))
    check(why is not None and "Fast" in why and "62" in why, "a loop at 62 x its frames fps or more is refused", why)
    fast.o3d.fps = 61
    check(refusal(lambda: export(model)) is None, "a loop under 62 x its frames fps exports")


def test_bone_scale_refused():
    fresh_scene()
    model, arm = gun()
    shrink = clip(arm, "Shrink", {0: {"BN03 Mag": {"scale": (1.0, 1.0, 1.0)}},
                                  3: {"BN03 Mag": {"scale": (0.0, 0.0, 0.0)}}})
    set_rows(model, [("anim_wpn_reload", [shrink])])
    why = refusal(lambda: export(model))
    check(why is not None and "BN03 Mag" in why and "scaled" in why, "a scaled bone is refused, naming it", why)


def test_first_person_events_stand_still():
    fresh_scene()
    model, arm = gun(at=(0.0, 0.0, 1.07))
    kick = clip(arm, "Kick", {0: {"BN01": {"loc": (0.0, 0.0, 0.0)}}, 5: {"BN01": {"loc": (0.0, 0.03, 0.0)}}})
    set_rows(model, [("anim_wpn_fire", [kick])])
    export(model)
    c = clip_named(scene_of(model.o3d.adm_path), "gun_f")
    tr = c["bones"][0]["tr"]
    check(c["flags"] & 0x2 and tr and abs(max(abs(v) for v in tr[5]) - 0.03) < 1e-5,
          "a rig without Root moves BN01 by its own translation row", tr[5] if tr else None)
    still = all(ev["velocity"] == (0.0, 0.0, 0.0) and abs(ev["bottom"] - 1.07) < 1e-5 and ev["bottom"] == ev["top"]
                for ev in c["events"])
    check(still, "a first-person clip's events stand still at the rig's height above Z = 0",
          c["events"][:2])


def test_root_measures_events():
    fresh_scene()
    root, lod = model_root("body")
    arm = make_armature(lod, [("Root", (0.0, 0.0, 0.0), (0.0, -0.2, 0.0), None),
                              ("BN01 Hips", (0.0, 0.0, 1.0), (0.0, 0.0, 1.1), "Root"),
                              ("BN02 Head", (0.0, 0.0, 1.6), (0.0, 0.0, 1.7), "BN01 Hips")], "body_rig")
    root.o3d.adm_path = os.path.join(OUT, "body", "body.adm")
    # The head moves off its rest offset too, so the clip carries translation
    # rows and the hips have one to leave empty.
    walk = clip(arm, "Walk", {0: {"Root": {"loc": (0.0, 0.0, 0.0)}, "BN02 Head": {"loc": (0.0, 0.0, 0.0)}},
                              10: {"Root": {"loc": (0.0, 1.0, 0.0)}, "BN02 Head": {"loc": (0.0, 0.02, 0.0)}}})
    set_rows(root, [("anim_walk_forward", [walk])])
    export(root)
    c = clip_named(scene_of(root.o3d.adm_path), "body_s1")
    ev = c["events"][0]
    check(abs(ev["bottom"] - 1.0) < 1e-5 and abs(ev["top"] - 1.6) < 1e-5 and abs(abs(ev["velocity"][0]) - 0.1) < 1e-5,
          "a rig with Root measures the step, bottom and top", ev)
    hips = c["bones"][0]["tr"]
    check(c["flags"] & 0x2 and len(hips) == 11 and all(tr == (0.0, 0.0, 0.0) for tr in hips) and
          c["bones"][1]["tr"][10] != (0.0, 0.0, 0.0), "the hips' travel on a rig with Root is the events', never a "
          "row", (hex(c["flags"]), hips[:3]))


def test_trigger_markers():
    fresh_scene()
    model, arm = gun()
    step = clip(arm, "Step", bolt_turns())
    step.pose_markers.new("FOOT_LEFT").frame = 2
    step.pose_markers.new("FIRE_PRIMARY").frame = 2
    step.pose_markers.new("foot_right").frame = 7
    step.pose_markers.new("contact").frame = 4
    set_rows(model, [("anim_wpn_idle", [step])])
    _, notes = export(model)
    events = clip_named(scene_of(model.o3d.adm_path), "gun_i")["events"]
    check(events[2]["trigger"] == 0x5 and events[7]["trigger"] == 0x2 and events[3]["trigger"] == 0,
          "markers named after event bits set them on their frame", [e["trigger"] for e in events])
    check(any("contact" in n for n in notes), "a marker naming no bit is noted", notes)
    step.pose_markers.new("FOOT_LEFT").frame = 40
    why = refusal(lambda: export(model))
    check(why is not None and "FOOT_LEFT" in why and "outside" in why, "a trigger past the clip is refused", why)


def test_slot_for_the_rig():
    fresh_scene()
    model, arm = gun()
    _, other_lod = model_root("other")
    other = make_armature(other_lod, GUN_BONES, "other_rig")
    shared = clip(other, "Shared", bolt_turns(60.0))
    arm.animation_data.action = shared
    rest(arm)
    for frame, degrees in ((0, 0.0), (10, 20.0)):
        pb = arm.pose.bones["BN02 Bolt"]
        pb.rotation_quaternion = Matrix.Rotation(math.radians(degrees), 3, "X").to_quaternion()
        pb.keyframe_insert("rotation_quaternion", frame=frame)
    arm.animation_data.action = None
    rest(arm)
    check(len(shared.slots) == 2, "an Action keyed on two rigs holds two slots", [s.identifier for s in shared.slots])
    set_rows(model, [("anim_wpn_idle", [shared])])
    export(model)
    key = clip_named(scene_of(model.o3d.adm_path), "gun_i")["bones"][1]["keys"][10]
    angle = math.degrees(2.0 * math.acos(min(1.0, abs(key[3]))))
    check(abs(angle - 20.0) < 1e-3, "each clip plays through its slot for the rig", angle)
    arm.name = "renamed_rig"
    why = refusal(lambda: export(model))
    check(why is not None and "slots" in why and "renamed_rig" in why, "an Action whose slots are none of the "
          "rig's is refused", why)


def test_shared_rig_has_no_set():
    fresh_scene()
    gun_root, arm = gun()
    arms, arms_lod = model_root("arms")
    mesh = bpy.data.meshes.new("arms")
    mesh.from_pydata([(0.0, 0.0, 0.0), (0.0, 0.1, 0.0), (0.1, 0.0, 0.0)], [], [(0, 1, 2)])
    ob = link(bpy.data.objects.new("arms_mesh", mesh), arms_lod)
    ob.vertex_groups.new(name="BN01").add([0, 1, 2], 1.0, "REPLACE")
    ob.modifiers.new("Armature", "ARMATURE").object = arm
    arms.o3d.adm_path = os.path.join(OUT, "arms", "arms.adm")
    why = refusal(lambda: export(arms))
    check(why is not None and "gun" in why and "deform" in why, "a model borrowing a gun's rig carries no set", why)
    check(animation.models_with_rigs(bpy.context.scene) == [gun_root], "Export All Animations passes the arms by")


# --- import ------------------------------------------------------------------

def authored_set():
    """A gun with a reset, an idle with triggers, a translated fire and a
    second idle variant, exported."""
    model, arm = gun()
    bind = clip(arm, "Bind", {0: {"BN02 Bolt": {"rot": ("X", 0.0)}}, 1: {"BN02 Bolt": {"rot": ("X", 0.0)}}},
                cyclic=True)
    idle = clip(arm, "Idle", bolt_turns(), frame_range=(0, 12), cyclic=True)
    idle.pose_markers.new("FOOT_LEFT").frame = 3
    idle2 = clip(arm, "Idle2", bolt_turns(-15.0), frame_range=(0, 12), cyclic=True)
    fire = clip(arm, "Fire", bolt_moves(), fps=45)
    set_rows(model, [("anim_reset", [bind]), ("anim_wpn_idle", [idle, idle2]), ("anim_wpn_fire", [fire])])
    export(model)
    return model, arm


def test_import_round_trip():
    fresh_scene()
    model, arm = authored_set()
    table = model.o3d.adm_path
    for action in list(bpy.data.actions):
        bpy.data.actions.remove(action)
    empty = bpy.data.actions.new("Empty")
    set_rows(model, [("anim_wpn_empty", [empty])])
    bpy.context.scene.render.fps = 24
    message, notes = anim_import.import_file(bpy.context, table, model)
    rows = [(r.key, [v.action.name for v in r.variants]) for r in model.o3d.rows]
    check(rows == [("anim_wpn_empty", ["Empty"]), ("anim_reset", ["gun_rst"]), ("anim_wpn_idle", ["gun_i", "gun_i2"]),
                   ("anim_wpn_fire", ["gun_f"])], "import merges the table's rows by slot, keeping the others", rows)
    idle = bpy.data.actions["gun_i"]
    check(idle.use_frame_range and tuple(idle.frame_range) == (0.0, 12.0) and idle.use_cyclic,
          "an imported clip's length is its manual frame range and its loop Cyclic")
    check([(m.name, m.frame) for m in idle.pose_markers] == [("FOOT_LEFT", 3)], "an imported trigger is a marker",
          [(m.name, m.frame) for m in idle.pose_markers])
    check(bpy.data.actions["gun_f"].o3d.fps == 45 and bpy.context.scene.render.fps == 30,
          "a clip keeps its rate and the scene takes the reset clip's")
    check(arm.animation_data.nla_tracks.__len__() == 0 and all(len(a.slots) == 1 for a in bpy.data.actions
                                                               if a.name.startswith("gun_")),
          "import makes an Action per clip, keyed for the rig, and no NLA track")
    check(not bind_frames(arm), "the rig a set was authored on takes no bind frame from it", bind_frames(arm))
    model.o3d.rows.remove(0)
    again = os.path.join(OUT, "again", "gun.adm")
    model.o3d.adm_path = again
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, again], capture_output=True, text=True)
    check(same.returncode == 0, "an imported set exports the same animation", same.stdout + same.stderr)
    message, notes = anim_import.import_file(bpy.context, table, model)
    check(any("replace" in n for n in notes) and "gun_i (replaced)" not in bpy.data.actions,
          "a set imported again replaces its clips in place", notes)


def static_model(name, bones):
    """A model of PN## parts at the bones' heads, in their hierarchy, each
    with a mesh."""
    model, lod = model_root(name)
    parts = {}
    for i, (bone, head, _, parent, *_roll) in enumerate(bones):
        part = link(bpy.data.objects.new(f"PN{i + 1:02d}", None), parts.get(parent, lod))
        part.matrix_world = Matrix.Translation(head)
        parts[bone] = part
        mesh = bpy.data.meshes.new(f"part{i}")
        mesh.from_pydata([(0.0, 0.0, 0.0), (0.02, 0.0, 0.0), (0.0, 0.02, 0.01)], [], [(0, 1, 2)])
        mesh.uv_layers.new(name="UVMap")
        ob = link(bpy.data.objects.new(f"{i + 1:02d} Mesh0", mesh), part)
        ob.matrix_world = Matrix.Translation(Vector(head) + Vector((0.01, 0.0, 0.0)))
    bpy.context.view_layer.update()
    model.o3d.output_path = os.path.join(OUT, name, "static", f"{name}.3di")
    return model


def bind_frames(arm):
    """The names of the rig's bones that store a bind frame."""
    return sorted(b.name for b in arm.data.bones if rig.bind_frame(b) is not None)


def off_the_rule(arm):
    """The part bones that do not point the way the add-on lays them: toward
    their first part child more than a millimetre away, else on along their
    parent's direction; and those pointing back at their parent."""
    off, back = [], []
    for b in arm.data.bones:
        if rig.bone_part(b) is None:
            continue
        axis = (b.tail_local - b.head_local).normalized()
        kids = sorted((k for k in b.children if rig.bone_part(k) is not None), key=rig.bone_part)
        far = [k for k in kids if (k.head_local - b.head_local).length > 1e-3]
        up = b.parent.head_local if b.parent is not None else None
        if far:
            want = (far[0].head_local - b.head_local).normalized()
        elif up is not None and (b.head_local - up).length > 1e-3:
            want = (b.head_local - up).normalized()
        else:
            want = Vector((0.0, 0.0, 1.0))
        if math.degrees(axis.angle(want)) > 1e-3:
            off.append(b.name)
        if up is not None and (b.head_local - up).length > 1e-3 and axis.dot((up - b.head_local).normalized()) > 0.0:
            back.append(b.name)
    return off, back


def deforms(model, arm, action):
    """Every part bone's deform, its pose times its rest's inverse in the
    model root's frame, on each frame of the clip (in part order): what the
    game draws the parts with."""
    data = arm.animation_data or arm.animation_data_create()
    rest(arm)
    data.action = action
    data.action_slot = action.slots[0]
    start, end = (int(round(x)) for x in action.frame_range)
    to_model = model.matrix_world.inverted() @ arm.matrix_world
    bones = sorted((pb for pb in arm.pose.bones if rig.bone_part(pb.bone) is not None),
                   key=lambda pb: rig.bone_part(pb.bone))
    out = []
    for f in range(start, end + 1):
        bpy.context.scene.frame_set(f)
        out.append([to_model @ pb.matrix @ pb.bone.matrix_local.inverted() @ to_model.inverted() for pb in bones])
    data.action = None
    rest(arm)
    return out


def worst_rotation(compared):
    """The worst key angle `anim compare` printed, in degrees."""
    return float(compared.stdout.split("worst rotation ")[1].split(" degrees")[0])


def test_import_makes_a_rig():
    fresh_scene()
    model, arm = authored_set()
    table = model.o3d.adm_path
    fresh_scene()
    model = static_model("gun", GUN_BONES)
    before = {ob.name: [ob.matrix_world @ v.co for v in ob.data.vertices] for ob in bpy.data.objects
              if ob.type == "MESH"}
    addon.export.export_model(bpy.context, model)
    first = model.o3d.output_path
    message, notes = anim_import.import_file(bpy.context, table, model)
    made = rig.rig_of(model)
    parts_left = [o.name for o in bpy.data.objects if o.name.startswith("PN")]
    check(made is not None and rig.model_of(made) is model and not parts_left,
          "clips imported onto a static model make its PN## parts a rig's bones", notes)
    parents = {ob.name: (ob.parent.name if ob.parent else None, ob.parent_bone) for ob in bpy.data.objects
               if ob.type == "MESH"}
    check(all(p == made.name and b.startswith("BN") for p, b in parents.values()),
          "every mesh hangs from its part's bone", parents)
    check(off_the_rule(made) == ([], []), "each bone points toward its first child, or on from its parent",
          off_the_rule(made))
    check(bind_frames(made) == ["BN01", "BN02 Bolt", "BN03 Mag"], "each bone whose bind is not its rest keeps a "
          "bind frame", bind_frames(made))
    bpy.context.view_layer.update()
    moved = max((ob.matrix_world @ v.co - w).length for ob in bpy.data.objects if ob.type == "MESH"
                for v, w in zip(ob.data.vertices, before[ob.name]))
    check(moved < 1e-6, "importing clips moves nothing", moved)
    model.o3d.output_path = os.path.join(OUT, "gun", "static", "rigged", "gun.3di")
    addon.export.export_model(bpy.context, model)
    same = subprocess.run([CLI, "compare", first, model.o3d.output_path], capture_output=True, text=True)
    check(same.returncode == 0, "the rigged model exports the model it was", same.stdout + same.stderr)
    model.o3d.adm_path = os.path.join(OUT, "gun", "static", "gun.adm")
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, model.o3d.adm_path], capture_output=True, text=True)
    check(same.returncode == 0, "its clips export the set imported", same.stdout + same.stderr)


# A pistol whose bones point where retail's binds point theirs, back at their
# parents (the slide, the hammer), one of them rolled (the magazine), and one
# the way the add-on lays a bone, toward its first child (the body).
PISTOL_BONES = [("BN01 Body", (0.0, 0.0, 0.0), (0.0, -0.1, 0.05), None),
                ("BN02 Slide", (0.0, -0.1, 0.05), (0.0, -0.05, 0.025), "BN01 Body"),
                ("BN03 Hammer", (0.0, -0.2, 0.08), (0.0, -0.15, 0.065), "BN02 Slide"),
                ("BN04 Mag", (0.0, -0.05, -0.05), (0.0, -0.05, -0.1), "BN01 Body", 90.0)]


def test_backward_binds_come_forward():
    fresh_scene()
    root, lod = model_root("pistol")
    arm = make_armature(lod, PISTOL_BONES, "pistol_rig")
    root.o3d.adm_path = os.path.join(OUT, "pistol", "pistol.adm")
    bind = clip(arm, "Bind", {0: {"BN02 Slide": {"rot": ("X", 0.0)}}, 1: {"BN02 Slide": {"rot": ("X", 0.0)}}},
                cyclic=True)
    idle = clip(arm, "Idle", {0: {"BN02 Slide": {"rot": ("X", 0.0)}, "BN03 Hammer": {"rot": ("Z", 0.0)}},
                              10: {"BN02 Slide": {"rot": ("X", 20.0)}, "BN03 Hammer": {"rot": ("Z", -30.0)}}},
                frame_range=(0, 12), cyclic=True)
    fire = clip(arm, "Fire", {0: {"BN02 Slide": {"loc": (0.0, 0.0, 0.0)}, "BN04 Mag": {"rot": ("Y", 0.0)}},
                              4: {"BN02 Slide": {"loc": (0.0, 0.03, 0.01)}, "BN04 Mag": {"rot": ("Y", 45.0)}}})
    reload = clip(arm, "Reload", {0: {"BN04 Mag": {"loc": (0.0, 0.0, 0.0), "rot": ("X", 0.0)}},
                                  8: {"BN04 Mag": {"loc": (0.02, -0.1, 0.0), "rot": ("X", -35.0)}}})
    rows = [("anim_reset", [bind]), ("anim_wpn_idle", [idle]), ("anim_wpn_fire", [fire]),
            ("anim_wpn_reload", [reload])]
    set_rows(root, rows)
    export(root)
    table = root.o3d.adm_path
    shown = {key: deforms(root, arm, actions[0]) for key, actions in rows}

    fresh_scene()
    model = static_model("pistol", PISTOL_BONES)
    message, notes = anim_import.import_file(bpy.context, table, model)
    made = rig.rig_of(model)
    check(off_the_rule(made) == ([], []), "a set whose binds point bones back at their parents comes in with each "
          "bone toward its child", off_the_rule(made))
    check(bind_frames(made) == ["BN02 Slide", "BN03 Hammer", "BN04 Mag"], "a bind frame only where the bind is not "
          "the bone's rest turn", bind_frames(made))
    worst = 0.0
    for key, _ in rows:
        action = next(v.action for r in model.o3d.rows if r.key == key for v in r.variants)
        for before, after in zip(shown[key], deforms(model, made, action)):
            for a, b in zip(before, after):
                worst = max(worst, max(abs(a[i][j] - b[i][j]) for i in range(3) for j in range(4)))
    check(worst < 1e-5, "each clip poses the parts as it did on the rig it was authored on", worst)
    model.o3d.adm_path = os.path.join(OUT, "pistol", "again", "pistol.adm")
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, model.o3d.adm_path], capture_output=True, text=True)
    check(same.returncode == 0 and worst_rotation(same) < 1e-3, "it re-exports the keys and binds it was "
          "imported from", same.stdout + same.stderr)
    next(r for r in model.o3d.rows if r.key == "anim_reset").variants[0].action = None
    model.o3d.adm_path = os.path.join(OUT, "pistol", "rest", "pistol.adm")
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, model.o3d.adm_path], capture_output=True, text=True)
    check(same.returncode == 0 and worst_rotation(same) < 1e-3, "a reset made of the rest pose keys the bind "
          "frames", same.stdout + same.stderr)


# A body: Root on the ground a metre under the hips (BN01, the model origin),
# and a head.
BODY_BONES = [("Root", (0.0, 0.0, -1.0), (0.0, -0.2, -1.0), None),
              ("BN01 Hips", (0.0, 0.0, 0.0), (0.0, 0.0, 0.6), "Root"),
              ("BN02 Head", (0.0, 0.0, 0.6), (0.0, 0.0, 0.65), "BN01 Hips")]


def model_frame(model):
    """Every mesh vertex in the model root's frame."""
    bpy.context.view_layer.update()
    into = model.matrix_world.inverted()
    return {ob.name: [into @ ob.matrix_world @ v.co for v in ob.data.vertices] for ob in bpy.data.objects
            if ob.type == "MESH"}


def moved(before, after):
    return max((a - b).length for name in before for a, b in zip(before[name], after[name]))


def test_body_set_stands_on_root():
    # A set that walks and bobs, authored on a rig with Root, imported onto a
    # static body: the rig made of its parts gets a Root on the ground under
    # the hips as its top bone, the model rises so that ground is Blender's,
    # nothing hung from a bone moves in the model, and the set exports the
    # animation it was.
    fresh_scene()
    root, lod = model_root("body")
    arm = make_armature(lod, BODY_BONES, "body_rig")
    root.o3d.adm_path = os.path.join(OUT, "bodyset", "body.adm")
    # The hips bob back to rest before the loop's last two frames, whose
    # events repeat its first (as every retail loop's), so no pose is lost.
    walk = clip(arm, "Walk", {0: {"Root": {"loc": (0.0, 0.0, 0.0)}, "BN01 Hips": {"loc": (0.0, 0.0, 0.0)}},
                              3: {"BN01 Hips": {"loc": (0.0, 0.05, 0.0)}}, 6: {"BN01 Hips": {"loc": (0.0, 0.0, 0.0)}},
                              10: {"Root": {"loc": (0.0, 1.0, 0.0)}, "BN01 Hips": {"loc": (0.0, 0.0, 0.0)}}},
                frame_range=(0, 10), cyclic=True)
    set_rows(root, [("anim_walk_forward", [walk])])
    export(root)
    table = root.o3d.adm_path
    events = clip_named(scene_of(table), "body_s1")["events"]
    check(abs(events[3]["bottom"] - 1.05) < 1e-5, "the set bobs", [e["bottom"] for e in events])

    fresh_scene()
    model = static_model("body", [("BN01 Hips", (0.0, 0.0, 0.0), (0.0, 0.0, 0.6), None),
                                  ("BN02 Head", (0.0, 0.0, 0.6), (0.0, 0.0, 0.65), "BN01 Hips")])
    before = model_frame(model)
    # A failure once Root is made puts the rig back as it was, Root gone and
    # everything hung from a bone where it hung.
    held = anim_import.Loader.action_for

    def refuse(*args, **kwargs):
        raise ImportFailed("refused")
    anim_import.Loader.action_for = refuse
    try:
        why = refusal(lambda: anim_import.import_file(bpy.context, table, model))
    finally:
        anim_import.Loader.action_for = held
    made = rig.rig_of(model)
    check(why == "refused" and made is not None and "Root" not in made.data.bones and
          model.matrix_world == Matrix.Identity(4) and moved(before, model_frame(model)) < 1e-6,
          "a failed import takes its Root and the model's rise back and moves nothing", why)

    message, notes = anim_import.import_file(bpy.context, table, model)
    bones = made.data.bones
    ground = bones.get("Root")
    check(ground is not None and ground.parent is None and bones["BN01 Hips"].parent == ground and
          abs((bones["BN01 Hips"].head_local - ground.head_local).length - 1.0) < 1e-5,
          "a rig without Root gets one as its top bone, the set's bottom under the hips",
          [(b.name, b.parent.name if b.parent else None) for b in bones])
    check(abs(model.matrix_world.translation.z - 1.0) < 1e-6, "the model rises until its ground is Blender's",
          model.matrix_world.translation)
    check(moved(before, model_frame(model)) < 1e-6, "nothing hung from a bone moves in the model",
          moved(before, model_frame(model)))
    model.o3d.adm_path = os.path.join(OUT, "bodyset", "again", "body.adm")
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, model.o3d.adm_path], capture_output=True, text=True)
    check(same.returncode == 0, "the set exports the animation it was imported from", same.stdout + same.stderr)


def test_edit_clip():
    fresh_scene()
    model, arm = gun()
    turn = clip(arm, "Turn", bolt_turns(), frame_range=(0, 10))
    mag = clip(arm, "Mag", {0: {"BN03 Mag": {"rot": ("Z", 0.0)}}, 6: {"BN03 Mag": {"rot": ("Z", 30.0)}}})
    set_rows(model, [("anim_wpn_idle", [turn]), ("anim_wpn_reload", [mag])])
    bpy.context.view_layer.objects.active = model
    arm.animation_data.use_nla = True
    fps = bpy.context.scene.render.fps
    check(bpy.ops.opennova_3di.edit_clip(row=0, variant=0) == {"FINISHED"}, "Edit Clip plays a row's clip")
    bpy.context.scene.frame_set(10)
    turned = arm.pose.bones["BN02 Bolt"].rotation_quaternion.angle
    check(arm.animation_data.action == turn and arm.animation_data.action_slot == turn.slots[0] and turned > 0.3,
          "the clip plays through its slot for the rig")
    bpy.context.view_layer.objects.active = model
    check(bpy.ops.opennova_3di.edit_clip(row=1, variant=0) == {"FINISHED"}, "Edit Clip plays another clip")
    scene = bpy.context.scene
    check(arm.pose.bones["BN02 Bolt"].rotation_quaternion.angle < 1e-6, "a channel the clip does not key goes back "
          "to rest", arm.pose.bones["BN02 Bolt"].rotation_quaternion)
    check(scene.use_preview_range and (scene.frame_preview_start, scene.frame_preview_end) == (0, 6) and
          scene.render.fps == fps and arm.animation_data.use_nla, "Edit Clip sets the preview range and leaves the "
          "scene's rate and the NLA alone")
    # Layout's Timeline is a Dope Sheet in its own mode; Animation's Dope
    # Sheet is one to show the clip in.
    screens = bpy.data.screens
    for name in ("Layout", "Animation"):
        with bpy.context.temp_override(screen=screens[name]):
            bpy.ops.opennova_3di.edit_clip(row=0, variant=0)
    modes = {name: [a.spaces.active.mode for a in screens[name].areas if a.type == "DOPESHEET_EDITOR"]
             for name in ("Layout", "Animation")}
    check(modes == {"Layout": ["TIMELINE"], "Animation": ["ACTION"]}, "Edit Clip shows the clip in a Dope Sheet's "
          "Action Editor and leaves the Timeline the Timeline", modes)


clip_scene.run_cases(globals(), "anim_test")
