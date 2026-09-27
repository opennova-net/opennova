"""The add-on's weapon side (weapon.py and its operators), authored from
scratch in an empty scene (no assets, no retail weapon.def) and run through
the opennova-3di the loader names:

    blender -b --factory-startup --python-exit-code 1 --python tests/blender/weapon_test.py -- <opennova-3di.exe>

A row's weapon action is its slot's; the model's weapon entries, its rows'
timing markers and its Hip and Aim cameras make the `weapon timing` request
(docs/anim/weapon-timing-format.md); export writes the edits beside the table
and never holds the clips back for a missing fire row; Merge into weapon.def
sets them in a copy of a def; the preview lives in memory; assigning a clip
leaves its loop alone; every clip of one action must time alike.
"""
import hashlib
import math
import os
import sys
import tempfile

import bpy
from mathutils import Euler

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addon_harness  # noqa: E402
import clip_scene  # noqa: E402
from clip_scene import bolt_moves, bolt_turns, check, clip, fresh_scene, link, set_rows  # noqa: E402

addon, CLI = addon_harness.load()
animation, weapon = addon.animation, addon.weapon
ExportError = addon.o3dtext.ExportError
OUT = tempfile.mkdtemp(prefix="opennova_weapon_test_")


def refusal(fn):
    return clip_scene.refusal(fn, ExportError)


def marked(action, **frames):
    """Timing markers on a clip: Shot=, Eject=, Active=, Ready=."""
    names = {"Shot": weapon.SHOT, "Eject": weapon.EJECT, "Active": weapon.ACTIVE, "Ready": weapon.READY}
    for key, frame in frames.items():
        weapon.set_marker(action, names[key], frame)
    return action


def entry(model, name, mode, rpm):
    e = model.o3d.weapons.add()
    e.name, e.mode, e.rpm = name, mode, rpm
    return e


def armed_gun(name="gun"):
    """A gun whose table times idle, fire, recoil, reload and draw, played by
    an automatic and a semi-automatic entry."""
    model, arm = clip_scene.gun(OUT, name)
    idle = clip(arm, "Idle", bolt_turns(5.0, 30), cyclic=True)
    fire = marked(clip(arm, "Fire", bolt_moves(6)), Shot=0)
    recoil = marked(clip(arm, "Recoil", bolt_moves(4)), Eject=0, Ready=1)
    reload = marked(clip(arm, "Reload", bolt_turns(40.0, 60)), Active=45, Ready=60)
    draw = clip(arm, "Draw", bolt_turns(15.0, 12))
    set_rows(model, [("anim_wpn_idle", [idle]), ("anim_wpn_fire", [fire]), ("anim_wpn_recoil", [recoil]),
                     ("anim_wpn_reload", [reload]), ("anim_wpn_switchto", [draw])])
    entry(model, "WPN_TEST", "auto", 600.0)
    entry(model, "WPN_TEST_SEMI", "semi", 400.0)
    return model, arm


def camera(name, model, at, turn=(90.0, 0.0, 180.0)):
    """A camera under the model root at `at`, looking along the model's
    forward (Blender's -Y) unless turned otherwise."""
    cam = link(bpy.data.objects.new(name, bpy.data.cameras.new(name)), model)
    cam.location = at
    cam.rotation_euler = Euler([math.radians(a) for a in turn], "XYZ")
    bpy.context.view_layer.update()
    return cam


def request_lines(model):
    return weapon.request_text(model).splitlines()


# --- roles --------------------------------------------------------------------

def test_role_from_the_row_key():
    check(weapon.action_of("anim_wpn_fire") == "fire" and weapon.action_of("ANIM_WPN_EMPTY_IDLE") == "emptyidle",
          "a row's weapon action is its slot's, in any case")
    check(weapon.action_of("anim_wpn_overheated") is None and weapon.action_of("anim_reset") is None,
          "overheated has no slot, and other slots are no weapon action")
    suffixes = [s for s, _, _ in weapon.actions()]
    check("overheated" not in suffixes and len(suffixes) == 11, "the catalog offers the eleven timed actions",
          suffixes)
    roles = [r[0] for r in addon.weapon_roles(None, bpy.context)]
    check(roles[0] == "reset" and "overheated" not in roles and len(roles) == 12,
          "Assign Weapon Action offers reset and the eleven", roles)


def test_assign_keeps_the_loop():
    fresh_scene()
    model, arm = clip_scene.gun(OUT)
    idle = clip(arm, "Idle", bolt_turns(), cyclic=True)
    other = clip(arm, "Other", bolt_turns(9.0))
    arm.animation_data.action = idle
    bpy.context.view_layer.objects.active = arm
    check(bpy.ops.opennova_3di.assign_weapon_action(role="idle") == {"FINISHED"}, "Assign Weapon Action runs")
    rows = [(r.key, [v.action.name for v in r.variants]) for r in model.o3d.rows]
    check(rows == [("anim_wpn_idle", ["Idle"])] and idle.use_cyclic, "assigning makes the row and leaves the loop "
          "as the clip has it", rows)
    check([(m.name, m.frame) for m in idle.pose_markers] == [(weapon.READY, 10)], "idle gets its Ready marker",
          [(m.name, m.frame) for m in idle.pose_markers])
    replaced = weapon.assign(model, other, "idle")
    check(replaced == ["Idle"] and [v.action for v in model.o3d.rows[0].variants] == [other],
          "assigning again replaces the row's clip, never appends", replaced)
    check(weapon.assign(model, other, "idle") == [] and len(model.o3d.rows[0].variants) == 1,
          "assigning the same clip changes nothing")
    weapon.assign(model, idle, "emptyidle")
    check(model.o3d.rows[-1].key == "anim_wpn_empty_idle", "emptyidle's row is anim_wpn_empty_idle")


# --- the request ----------------------------------------------------------------

def test_request():
    fresh_scene()
    model, arm = armed_gun()
    model.o3d.hip_camera = camera("Hip", model, (0.05, -0.3, 0.2))
    lines = request_lines(model)
    want = ["weapon_timing 2", "action idle 0.0 1.0", "action fire 0.0 0.0",
            "action recoil 0.0 0.03333333333333333", "action reload 1.5 0.5",
            "action switchto 0.0 0.0", "entry WPN_TEST auto 0.1", "entry WPN_TEST_SEMI semi 0.15"]
    check(lines[:len(want)] == want, "the request times each row's action from its markers, one entry per weapon",
          lines)
    view = lines[-1].split()
    eye = [float(v) for v in view[2:]]
    check(view[:2] == ["view", "pos"] and all(abs(a - b) < 1e-6 for a, b in zip(eye, (0.3, 0.05, 0.2))),
          "the Hip camera is the pos eye, in the model's mission axes", lines[-1])
    model.o3d.weapons.clear()
    check(weapon.request_text(model) is None, "a model with no weapon entry is no weapon")


def test_markers_that_time_an_action():
    fresh_scene()
    model, arm = armed_gun()
    fire = model.o3d.rows[1].variants[0].action
    weapon.set_marker(fire, weapon.READY, 6)
    why = refusal(lambda: weapon.request_text(model))
    check(why is not None and "do not time fire" in why and "target rate" in why,
          "fire takes no Ready marker: its recovery is each entry's rate", why)
    fire.pose_markers.remove(fire.pose_markers[weapon.READY])
    draw = model.o3d.rows[4].variants[0].action
    weapon.set_marker(draw, weapon.ACTIVE, 5)
    why = refusal(lambda: weapon.request_text(model))
    check(why is not None and "switch timer" in why, "draw takes no marker: the switch timer paces it", why)
    draw.pose_markers.remove(draw.pose_markers[weapon.ACTIVE])
    reload = model.o3d.rows[3].variants[0].action
    weapon.set_marker(reload, weapon.ACTIVE, 90)
    why = refusal(lambda: weapon.request_text(model))
    check(why is not None and "inside the clip" in why, "an active boundary past the clip is refused", why)
    reload.pose_markers.new("ON:Typo")
    why = refusal(lambda: weapon.request_text(model))
    check(why is not None and "unknown timing marker" in why, "an unknown timing marker is refused", why)


def test_variants_time_alike():
    fresh_scene()
    model, arm = armed_gun()
    long_idle = clip(arm, "LongIdle", bolt_turns(8.0, 45), cyclic=True)
    model.o3d.rows[0].variants.add().action = long_idle
    why = refusal(lambda: weapon.request_text(model))
    check(why is not None and "time idle differently" in why, "idle clips of different windows are refused (any may "
          "be the one playing)", why)
    weapon.set_marker(long_idle, weapon.READY, 30)
    check(refusal(lambda: weapon.request_text(model)) is None, "idle clips whose Ready markers agree in seconds time "
          "alike")


# --- the CLI ------------------------------------------------------------------

def test_compile_and_preview():
    fresh_scene()
    model, arm = armed_gun()
    lines, notes = weapon.preview(bpy.context, model)
    preview = weapon.last_preview(model)
    names = [e["name"] for e in preview["entries"]]
    check(names == ["WPN_TEST", "WPN_TEST_SEMI"] and abs(preview["entries"][0]["rpm"] - 625.0) < 1e-6,
          "each entry is measured in its own mode (600 asked, 6 ticks: 625)", [(e["name"], e["rpm"])
                                                                                for e in preview["entries"]])
    check(lines[0][0].startswith("WPN_TEST (auto): 625.0 RPM") and "target 600" in lines[0][0],
          "the preview shows the rate got against the target", lines[0])
    check(any(t.startswith("reload: DELAYSTART 94, DELAYEND 31") for t, _ in lines),
          "the preview shows each action's delays", [t for t, _ in lines])
    check("weapon_preview" not in model.o3d.bl_rna.properties and not bpy.data.texts,
          "the preview is kept in memory, never in the scene")
    model.o3d.weapons[0].rpm = 500.0
    check(weapon.last_preview(model) is None, "a changed request makes the preview stale")


def test_short_one_shot_noted():
    fresh_scene()
    model, arm = armed_gun()
    long_fire = marked(clip(arm, "LongFire", bolt_moves(44)), Shot=0)
    model.o3d.rows[1].variants[0].action = long_fire
    lines, notes = weapon.preview(bpy.context, model)
    fire_lines = [(t, w) for t, w in lines if t.startswith("fire:")]
    check(fire_lines and fire_lines[0][1] and "of 1.467 s" in fire_lines[0][0],
          "a one-shot clip that shows far less than its length is flagged", fire_lines)
    check(any("fire clip shows" in n for n in notes), "and noted", notes)


def test_export_writes_edits():
    fresh_scene()
    model, arm = armed_gun()
    model.o3d.aim_camera = camera("Aim", model, (0.0, -0.25, 0.22))
    summary, notes = animation.export_animations(bpy.context, model)
    path = weapon.edits_path(model.o3d.adm_path)
    text = open(path, encoding="utf-8").read() if os.path.isfile(path) else ""
    check(os.path.basename(path) == "gun_weapon_edits.txt" and "gun_weapon_edits.txt" in summary,
          "export writes the edits beside the table", summary)
    check("entry WPN_TEST auto" in text and "entry WPN_TEST_SEMI semi" in text and "action fire anim anim_wpn_fire"
          in text and "tpos" in text and "function" not in text.lower() and "sound" not in text.lower(),
          "the edits set only ANIM, the delays and the view keys, per entry", text)


def test_missing_fire_writes_the_clips():
    fresh_scene()
    model, arm = armed_gun("nofire")
    animation.export_animations(bpy.context, model)
    model.o3d.rows.remove(1)
    edits = weapon.edits_path(model.o3d.adm_path)
    os.remove(edits)
    summary, notes = animation.export_animations(bpy.context, model)
    folder = os.path.dirname(model.o3d.adm_path)
    check(os.path.isfile(os.path.join(folder, "nofire_i.bad")) and not os.path.isfile(edits),
          "a table without a fire row still writes its clips, and no edits")
    check(any("no anim_wpn_fire row" in n for n in notes), "and says why", notes)
    open(edits, "w").close()
    summary, notes = animation.export_animations(bpy.context, model)
    check(any("earlier export" in n for n in notes), "an edits file an earlier export left is named as such", notes)


def test_camera_off_forward_noted():
    fresh_scene()
    model, arm = armed_gun()
    model.o3d.aim_camera = camera("Aim", model, (0.0, -0.25, 0.22), turn=(90.0, 0.0, 170.0))
    notes = weapon.NoteList()
    weapon.request_text(model, notes)
    check(any("Aim" in n and "degrees off" in n for n in notes.notes), "a camera looking off the model's forward is "
          "noted: only its place is used", notes.notes)


def test_merge_into_a_copy():
    fresh_scene()
    model, arm = armed_gun()
    folder = os.path.join(OUT, "merge")
    os.makedirs(folder, exist_ok=True)
    source = os.path.join(folder, "weapon.def")
    original = ("weapon \"WPN_TEST\"\r\n\tFLAGS\tAUTO\r\n\tACTION\t\"FIRE\"\r\n\tDELAYEND\t9\r\n"
                "\tFUNCTION\tWPN_STD_FIRE\r\n\tEND\r\nend\r\n\r\n"
                "weapon \"WPN_TEST_SEMI\"\r\n\tACTION\t\"FIRE\"\r\n\tDELAYEND\t9\r\n\tEND\r\nend\r\n")
    with open(source, "w", encoding="latin-1", newline="") as f:
        f.write(original)
    target = os.path.join(folder, "weapon_merged.def")
    said, notes = weapon.merge(bpy.context, model, source, target)
    merged = open(target, encoding="latin-1", newline="").read()
    check(open(source, encoding="latin-1", newline="").read() == original, "the def read is left as it was")
    check("WPN_STD_FIRE" in merged and "DELAYEND\t9" not in merged and "anim_wpn_reload" in merged.lower(),
          "the copy takes the keys and keeps the rest", merged)
    why = refusal(lambda: weapon.merge(bpy.context, model, source, source))
    check(why is not None and "new file" in why, "the merge never writes over the def it reads", why)
    os.remove(target)
    bpy.context.view_layer.objects.active = model
    check(bpy.ops.opennova_3di.merge_weapon_def("EXEC_DEFAULT", filepath=source) == {"FINISHED"} and
          os.path.isfile(target), "Merge into weapon.def writes <def>_merged.def beside the def by default")
    check(bpy.ops.opennova_3di.preview_weapon() == {"FINISHED"} and weapon.last_preview(model) is not None,
          "Preview Game Timing keeps the preview")
    model.o3d.weapons[0].name = "WPN_ABSENT"
    why = refusal(lambda: weapon.merge(bpy.context, model, source, target))
    check(why is not None and "WPN_ABSENT" in why, "an entry the def lacks is refused", why)


def folder_hashes(model):
    folder = os.path.dirname(model.o3d.adm_path)
    return {n: hashlib.sha256(open(os.path.join(folder, n), "rb").read()).hexdigest() for n in os.listdir(folder)
            if n.endswith((".bad", ".adm", ".txt"))}


def test_refused_timing_writes_nothing():
    fresh_scene()
    model, arm = armed_gun("refused")
    animation.export_animations(bpy.context, model)
    before = folder_hashes(model)
    weapon.set_marker(model.o3d.rows[1].variants[0].action, weapon.SHOT, 40)
    why = refusal(lambda: animation.export_animations(bpy.context, model))
    check(why is not None and folder_hashes(model) == before, "timing the author must fix writes nothing, the clips "
          "included", why)


def test_reopened_scene_exports_the_same():
    fresh_scene()
    model, arm = armed_gun("reopen")
    model.o3d.hip_camera = camera("Hip", model, (0.0, -0.3, 0.2))
    animation.export_animations(bpy.context, model)
    before = folder_hashes(model)
    path = os.path.join(OUT, "reopen.blend")
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path)
    model = bpy.data.objects["reopen"]
    animation.export_animations(bpy.context, model)
    check(folder_hashes(model) == before and len(before) == 8, "a saved scene exports the same files: the clip set "
          "and its timing live in the scene", sorted(before))


clip_scene.run_cases(globals(), "weapon_test")
