"""Weapon authoring from Actions, local markers and explicit animation rows.

Nothing here reads imported provenance or cached delays. The native tool
compiles current authoring data and measures the engine's real weapon FSM.
"""
import hashlib
import json
import os
import re

import bpy

from . import animation
from .o3dtext import ExportError, run_cli, scratch

ROLES = [("NONE", "Not a weapon action", "Export this animation row without a weapon ACTION block")]
ROLES += [(name, label, meaning) for name, label, meaning in (
    ("idle", "Idle", "Ready for a fresh semi-auto trigger press"),
    ("emptyidle", "Empty idle", "Hold with an empty magazine"),
    ("fire", "Fire", "Shot marker and firing cadence"),
    ("recoil", "Recoil", "Casing eject and recovery before the next decision"),
    ("reload", "Reload", "Reload animation window; ammo refills on entry"),
    ("empty", "Dry fire", "Empty-magazine click"),
    ("switchto", "Draw", "Draw action; the engine also applies its switch timer"),
    ("switchfrom", "Holster", "Holster action; the engine also applies its switch timer"),
    ("switchrank", "Switch fire mode", "Switch rank action"),
    ("scopeup", "Aim in", "Raise the scope"),
    ("scopedown", "Aim out", "Lower the scope"),
)]
SHOT, EJECT, ACTIVE, READY = "ON:Shot", "ON:Eject", "ON:Active End", "ON:Ready"
MARKERS = (SHOT, EJECT, ACTIVE, READY)
FIXED_ROLES = {"switchto", "switchfrom"}
# Idle holds repeat on their Ready window; they have no active boundary.
IDLE_ROLES = {"idle", "emptyidle"}
IDENTIFIER = re.compile(r"^[A-Za-z0-9_.-]{1,63}$")


def clip_bounds(action):
    start, end = animation.clip_range(action)
    end = start + max(end - start, action.o3d.frames)
    if end <= start:
        raise ExportError(f"{action.name}: a weapon clip needs at least two frames")
    return start, end, max(1, int(round(action.o3d.fps)))


def markers(action):
    found = {}
    for marker in action.pose_markers:
        if not marker.name.startswith("ON:"):
            continue
        if marker.name not in MARKERS:
            raise ExportError(f"{action.name}: unknown timing marker '{marker.name}' (use the timing buttons)")
        if marker.name in found:
            raise ExportError(f"{action.name}: duplicate timing marker '{marker.name}'")
        found[marker.name] = marker.frame
    return found


def phase_times(action, role, cadence):
    start, end, fps = clip_bounds(action)
    points = markers(action)
    if role in FIXED_ROLES:
        if points:
            raise ExportError(f"{action.name}: draw/holster use the fixed switch timer; remove its ON: timing markers")
        return 0.0, 0.0
    allowed = {READY}
    if role == "fire":
        allowed.add(SHOT)
        boundary = points.get(SHOT, start)
    elif role == "recoil":
        allowed.add(EJECT)
        boundary = points.get(EJECT, start)
    elif role in IDLE_ROLES:
        boundary = start
    else:
        allowed.add(ACTIVE)
        boundary = points.get(ACTIVE, points.get(READY, end))
    if points.keys() - allowed:
        raise ExportError(f"{action.name}: markers {', '.join(sorted(points.keys() - allowed))} do not apply to {role}")
    # Ready can extend past the keyed pose for an explicit held recovery, but
    # Shot/Eject/Active End must identify a pose the exported clip contains.
    if not start <= boundary <= end:
        raise ExportError(f"{action.name}: its active boundary must lie inside the exported clip ({start}..{end})")
    ready = points.get(READY, end)
    if role == "fire" and cadence == "RPM":
        ready = boundary  # cadence has exactly one source; native solves it
    elif ready < boundary:
        raise ExportError(f"{action.name}: Ready must not precede its active boundary")
    if role == "fire" and cadence == "MARKER" and READY not in points:
        raise ExportError(f"{action.name}: add a Ready marker or choose Target RPM")
    active = (boundary - start) / fps
    recovery = (ready - boundary) / fps
    if role == "fire" and cadence == "MARKER" and recovery <= 0:
        raise ExportError(f"{action.name}: Ready must be later than Shot")
    return active, recovery


def token(value, what, empty=False):
    if not value and empty:
        return '""'
    if not IDENTIFIER.fullmatch(value):
        raise ExportError(f"{what}: use an identifier under 64 characters (letters, digits, _, - or .)")
    return value


def request_text(model):
    props = model.o3d
    rig = animation.rig_of(model)
    if rig is None:
        raise ExportError("weapon timing needs an authored rig and Actions")
    clips = set(animation.clip_actions(rig))
    lines = ["weapon_timing 1", "mode " + props.weapon_mode]
    roles, fire = set(), None
    for row in props.rows:
        role = row.weapon_role
        if role == "NONE":
            continue
        if role in roles:
            raise ExportError(f"{model.name}: more than one row is assigned to {role}")
        roles.add(role)
        actions = [v.action for v in row.variants if v.action is not None]
        if not actions:
            raise ExportError(f"{row.key}: assign a clip to its {role} action")
        if any(a not in clips for a in actions):
            raise ExportError(f"{row.key}: keep each Action on an NLA track (Assign Weapon Action does this)")
        times = [phase_times(a, role, props.weapon_cadence) for a in actions]
        # Idle variants may have different lengths, as the table's first
        # served variant defines the initial idle window. Gameplay boundaries
        # of other variants must agree: an ACTION has only one pair of delays.
        if role not in IDLE_ROLES and any(
                any(abs(x - y) > 1e-8 for x, y in zip(t, times[0])) for t in times[1:]):
            raise ExportError(f"{row.key}: variants disagree on {role} timing; align their markers in seconds")
        active, recovery = times[0]
        if role == "fire":
            fire = (active, recovery)
        fields = ["action", role, token(row.key, "animation slot"), f"{active:.12g}", f"{recovery:.12g}"]
        fields += [token(getattr(row, attr).strip(), attr, True) for attr in
                   ("weapon_sound", "weapon_end_sound", "weapon_particle", "weapon_userpoint")]
        lines.append(" ".join(fields))
    if fire is None:
        raise ExportError("assign an animation row to Fire before previewing weapon timing")
    cycle = 60 / props.weapon_rpm if props.weapon_cadence == "RPM" else fire[1]
    lines.insert(2, f"cycle {cycle:.12g}")
    return "\n".join(lines) + "\n"


def requested_cycle(source):
    """The shot period, in seconds, a request text asks for."""
    return float(next(line.split()[1] for line in source.splitlines() if line.startswith("cycle ")))


def signature(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def compile_timing(context, model):
    """Fresh source -> native plan and snippet; preview and export use this."""
    source = request_text(model)
    with scratch() as tmp:
        input_path, output_path = os.path.join(tmp, "timing.txt"), os.path.join(tmp, "actions.txt")
        with open(input_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(source)
        result = run_cli(context, ["weapon", "timing", input_path, "-o", output_path], ExportError, timeout=30,
                         hide=((input_path, "weapon timing"),))
        try:
            preview = json.loads(result.stdout)
        except (ValueError, TypeError) as e:
            raise ExportError("the native weapon timing tool returned an invalid preview") from e
        with open(output_path, encoding="utf-8") as f:
            snippet = f.read()
    preview["signature"] = signature(source)
    preview["requested_rpm"] = 60 / requested_cycle(source)
    return preview, snippet


def output_path(adm_path):
    return os.path.splitext(adm_path)[0] + "_weapon_actions.txt"


def write_snippet(path, snippet):
    # Called only after the complete animation set validates and builds.
    part = path + ".part"
    try:
        with open(part, "w", encoding="utf-8", newline="\n") as f:
            f.write(snippet)
        os.replace(part, path)
    finally:
        if os.path.exists(part):
            os.remove(part)


def stash_action(rig, action):
    if action in animation.clip_actions(rig):
        return
    track = rig.animation_data.nla_tracks.new()
    track.name = action.name
    strip = track.strips.new(action.name, int(action.frame_range[0]), action)
    if hasattr(strip, "action_slot"):
        strip.action_slot = rig.animation_data.action_slot
    track.mute = True
    action.use_fake_user = True


def assign_action(model, action, role):
    rig = animation.rig_of(model)
    stash_action(rig, action)
    key = "anim_reset" if role == "RESET" else "anim_wpn_" + ("empty_idle" if role == "emptyidle" else role)
    row = next((r for r in model.o3d.rows if r.key == key), None)
    if row is None:
        row = model.o3d.rows.add()
        row.key = key
    row.weapon_role = "NONE" if role == "RESET" else role
    if not any(v.action == action for v in row.variants):
        row.variants.add().action = action
    if role != "RESET":
        model.o3d.weapon_enabled = True
    return row


def set_marker(action, name, frame):
    marker = action.pose_markers.get(name) or action.pose_markers.new(name)
    marker.frame = int(frame)
    return marker


def initialize_markers(action, role):
    if role in FIXED_ROLES:
        return
    start, end, _ = clip_bounds(action)
    for name, frame in ((SHOT if role == "fire" else EJECT if role == "recoil" else ACTIVE,
                         start if role == "fire" else max(start, end-1) if role == "recoil" else end), (READY, end)):
        if role in IDLE_ROLES and name != READY:
            continue
        if action.pose_markers.get(name) is None:
            set_marker(action, name, frame)
