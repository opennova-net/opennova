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
Actions, merges rows and makes a static model's parts a rig.
"""
import math
import os
import subprocess
import sys
import tempfile

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
OUT = tempfile.mkdtemp(prefix="opennova_anim_test_")


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
    check(why is not None and "BN03 Mag" in why and "rest pose" in why, "a reset starting off rest is refused, "
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
    walk = clip(arm, "Walk", {0: {"Root": {"loc": (0.0, 0.0, 0.0)}}, 10: {"Root": {"loc": (0.0, 1.0, 0.0)}}})
    set_rows(root, [("anim_walk_forward", [walk])])
    export(root)
    c = clip_named(scene_of(root.o3d.adm_path), "body_s1")
    ev = c["events"][0]
    check(abs(ev["bottom"] - 1.0) < 1e-5 and abs(ev["top"] - 1.6) < 1e-5 and abs(abs(ev["velocity"][0]) - 0.1) < 1e-5,
          "a rig with Root measures the step, bottom and top", ev)
    check(all(tr == (0.0, 0.0, 0.0) for tr in c["bones"][0]["tr"]) if c["bones"][0]["tr"] else True,
          "the hips' travel on a rig with Root is the events', never a row")


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
    model.o3d.rows.remove(0)
    again = os.path.join(OUT, "again", "gun.adm")
    model.o3d.adm_path = again
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, again], capture_output=True, text=True)
    check(same.returncode == 0, "an imported set exports the same animation", same.stdout + same.stderr)
    message, notes = anim_import.import_file(bpy.context, table, model)
    check(any("replace" in n for n in notes) and "gun_i (replaced)" not in bpy.data.actions,
          "a set imported again replaces its clips in place", notes)


def static_gun():
    """The gun as a static model: PN## parts, each with a mesh."""
    model, lod = model_root("gun")
    parts = {}
    for i, (name, head, _, parent) in enumerate(GUN_BONES):
        part = link(bpy.data.objects.new(f"PN{i + 1:02d}", None), parts.get(parent, lod))
        part.matrix_world = Matrix.Translation(head)
        parts[name] = part
        mesh = bpy.data.meshes.new(f"part{i}")
        mesh.from_pydata([(0.0, 0.0, 0.0), (0.02, 0.0, 0.0), (0.0, 0.02, 0.01)], [], [(0, 1, 2)])
        mesh.uv_layers.new(name="UVMap")
        ob = link(bpy.data.objects.new(f"{i + 1:02d} Mesh0", mesh), part)
        ob.matrix_world = Matrix.Translation(Vector(head) + Vector((0.01, 0.0, 0.0)))
    bpy.context.view_layer.update()
    model.o3d.output_path = os.path.join(OUT, "static", "gun.3di")
    return model


def test_import_makes_a_rig():
    fresh_scene()
    model, arm = authored_set()
    table = model.o3d.adm_path
    fresh_scene()
    model = static_gun()
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
    bpy.context.view_layer.update()
    moved = max((ob.matrix_world @ v.co - w).length for ob in bpy.data.objects if ob.type == "MESH"
                for v, w in zip(ob.data.vertices, before[ob.name]))
    check(moved < 1e-6, "turning the rest onto the bind moves nothing", moved)
    model.o3d.output_path = os.path.join(OUT, "static", "rigged", "gun.3di")
    addon.export.export_model(bpy.context, model)
    same = subprocess.run([CLI, "compare", first, model.o3d.output_path], capture_output=True, text=True)
    check(same.returncode == 0, "the rigged model exports the model it was", same.stdout + same.stderr)
    model.o3d.adm_path = os.path.join(OUT, "static", "gun.adm")
    export(model)
    same = subprocess.run([CLI, "anim", "compare", table, model.o3d.adm_path], capture_output=True, text=True)
    check(same.returncode == 0, "its clips export the set imported", same.stdout + same.stderr)


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


clip_scene.run_cases(globals(), "anim_test")
