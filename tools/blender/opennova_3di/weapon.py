# A first-person weapon's timing. The model root's weapon entries (the
# weapon.def entries that play its clips, each with its fire mode and target
# rate), its table's weapon rows (an action's clips answer the action's own
# anim slot) and the timing markers on each row's clips compile, through
# `opennova-3di weapon timing`, into the keys each entry's ACTION blocks need
# (ANIM, DELAYSTART, DELAYEND) and the view positions its Hip and Aim cameras
# give (POS, TPOS), measured with the engine's own weapon FSM;
# `opennova-3di weapon merge` sets them in a copy of a weapon.def
# (docs/anim/weapon-timing-format.md; ADR 0047 decision 14). Nothing here
# reads imported provenance or an existing weapon.def.
#
# A row's action is its slot's: the catalog pairs each weapon action with the
# slot of its own clips, anim_wpn_<action> (emptyidle's is
# anim_wpn_empty_idle) [orig: g_AnimStateNameTable @0x8135f0, 241..251].
# OVERHEATED has no slot, and nothing ever queues it, so it is not offered.
#
# Action-local markers time an action from its clip's first frame, in seconds
# of the clip at its own rate:
#   ON:Shot        fire: the shot pose (none: the first frame). Fire's
#                  recovery is each entry's, from its target rate.
#   ON:Eject       recoil: the casing and refire decision (none: the first
#                  frame).
#   ON:Active End  reload, empty, switchrank, scopeup, scopedown: the end of
#                  the action's active phase (none: Ready, else the clip's end).
#   ON:Ready       the end of the recovery after that boundary, and an idle's
#                  reseed window (none: the clip's end).
# Draw and holster take none: their handlers pace themselves on the switch
# timer. Every clip of one action must time alike: a ring serves its LAST clip
# first and steps on every play and loop wrap [orig: AnimMap_RegisterBoneNode
# @0x40c385; AnimMap_AdvanceToNextAnim @0x40bdf0], so any may be playing.

import json
import math
import os
import re

from mathutils import Vector

from . import animation
from .o3dtext import ExportError, ModelSpace, Notes, cli_notes, fmt, run_cli, scratch

SHOT, EJECT, ACTIVE, READY = "ON:Shot", "ON:Eject", "ON:Active End", "ON:Ready"
MARKERS = (SHOT, EJECT, ACTIVE, READY)
SWITCHES = {"switchto", "switchfrom"}
IDLES = {"idle", "emptyidle"}
# The markers that time each action; any other action takes Active End and
# Ready.
TIMED_BY = {"fire": {SHOT}, "recoil": {EJECT, READY}, "idle": {READY}, "emptyidle": {READY},
            "switchto": set(), "switchfrom": set()}
# An entry name fits the def's 32-byte name field [orig:
# WeaponDefs_ParseLineCallback, strncpy 32 @0x543737].
ENTRY_NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,31}$")
# A one-shot clip the viewmodel shows less of than this share is noted.
SHOWN_SHARE = 0.5
# A camera looking further than this (degrees) off the model's forward is
# noted: the game looks along the model's forward from the eye.
VIEW_ANGLE = 1.0


class NoteList(Notes):
    """Notes gathered outside an export run."""

    def __init__(self):
        self.notes = []


def actions():
    """The timeable weapon actions [(suffix, slot index, key)] in the engine's
    order, from `opennova-3di catalog`; an ExportError when it cannot be
    read."""
    from . import catalog, catalog_error
    table = catalog(retry=True).actions
    if not table:
        raise ExportError(f"no weapon action table: {catalog_error() or 'opennova-3di catalog printed none'}")
    return table


def action_key(suffix):
    """The row key a weapon action's clips answer."""
    return next(key for s, _, key in actions() if s == suffix)


def action_of(key, table=None):
    """The weapon action whose slot a row key names, else None."""
    slot = animation.slot_of(key.strip())
    return next((s for s, _, k in (table if table is not None else actions()) if animation.slot_of(k) == slot), None)


def clip_bounds(action):
    """A clip's first and last frame and its rate."""
    start, end = animation.clip_range(action)
    if end <= start:
        raise ExportError(f"{action.name}: a clip needs at least two frames")
    return start, end, action.o3d.fps


def markers(action):
    """The Action's timing markers {name: frame}."""
    found = {}
    for marker in action.pose_markers:
        if not marker.name.startswith("ON:"):
            continue
        if marker.name not in MARKERS:
            raise ExportError(f"{action.name}: unknown timing marker '{marker.name}' ("
                              + ", ".join(MARKERS) + " time a weapon action)")
        if marker.name in found:
            raise ExportError(f"{action.name}: two {marker.name} markers")
        found[marker.name] = marker.frame
    return found


def phase_times(action, suffix):
    """An action's timing from one of its clips, in seconds: the time from
    its entry to the marked pose (the active phase) and the recovery after
    it."""
    start, end, fps = clip_bounds(action)
    points = markers(action)
    allowed = TIMED_BY.get(suffix, {ACTIVE, READY})
    extra = sorted(points.keys() - allowed)
    if extra:
        why = " (draw and holster pace themselves on the switch timer)" if suffix in SWITCHES else \
            " (fire's recovery is each weapon entry's target rate)" if suffix == "fire" and READY in extra else ""
        raise ExportError(f"{action.name}: {', '.join(extra)} do not time {suffix}{why}")
    if suffix in SWITCHES:
        return 0.0, 0.0
    if suffix == "fire":
        boundary = points.get(SHOT, start)
    elif suffix == "recoil":
        boundary = points.get(EJECT, start)
    elif suffix in IDLES:
        boundary = start
    else:
        boundary = points.get(ACTIVE, points.get(READY, end))
    # Ready may lie past the keyed pose, a held recovery; the active boundary
    # names a pose the clip holds.
    if not start <= boundary <= end:
        raise ExportError(f"{action.name}: its active boundary (frame {boundary}) must lie inside the clip "
                          f"({start}..{end})")
    if suffix == "fire":
        return (boundary - start) / fps, 0.0
    ready = points.get(READY, end)
    if ready < boundary:
        raise ExportError(f"{action.name}: its Ready marker (frame {ready}) comes before its active boundary "
                          f"(frame {boundary})")
    return (boundary - start) / fps, (ready - boundary) / fps


def eye(model, camera, notes=None):
    """A camera's place in the model root's frame, in mission axes (metres):
    the eye the view position is measured from."""
    space = ModelSpace(model)
    world = space.world(camera)
    look = space.mission(world.to_3x3() @ Vector((0.0, 0.0, -1.0)))
    angle = math.degrees(math.acos(max(-1.0, min(1.0, look[0] / max(1e-12, math.sqrt(sum(c * c for c in look)))))))
    if notes is not None and angle > VIEW_ANGLE:
        notes.note(f"{camera.name} looks {angle:.1f} degrees off {model.name}'s forward: the game looks along the "
                   "model's forward from the eye, so only its place is used")
    return space.mission(world.translation)


def request_text(model, notes=None):
    """The `weapon timing` request for a model; None when it is no weapon (it
    has no weapon entry) or its table has no fire row (`notes` hears why)."""
    props = model.o3d
    if not props.weapons:
        return None
    table = actions()
    authored = {}
    for row in props.rows:
        suffix = action_of(row.key, table)
        if suffix is None:
            continue
        clips = [v.action for v in row.variants if v.action is not None]
        if not clips:
            raise ExportError(f"the row '{row.key.strip()}' names no clip")
        times = [phase_times(a, suffix) for a in clips]
        for a, t in zip(clips[1:], times[1:]):
            if any(abs(x - y) > 1e-9 for x, y in zip(t, times[0])):
                raise ExportError(f"{row.key.strip()}: {clips[0].name} and {a.name} time {suffix} differently "
                                  f"({times[0][0]:.4g} s + {times[0][1]:.4g} s against {t[0]:.4g} s + {t[1]:.4g} s): "
                                  "the game serves a ring's clips in turn, so any may be playing; align their "
                                  "markers in seconds")
        authored[suffix] = times[0]
    if "fire" not in authored:
        if notes is not None:
            notes.note(f"{model.name} has weapon entries but no {action_key('fire')} row, so no weapon.def edits are "
                       "written: its fire clip times every entry's shot")
        return None
    lines = ["weapon_timing 2"]
    for suffix, _, _ in table:
        if suffix in authored:
            active, recovery = authored[suffix]
            lines.append(f"action {suffix} {active!r} {recovery!r}")
    names = {}
    for entry in props.weapons:
        name = entry.name.strip()
        if not ENTRY_NAME_RE.match(name):
            raise ExportError(f"{model.name}: the weapon entry '{name}' is not a weapon.def entry name (1 to 31 "
                              "letters, digits, _ - or .)")
        if name.lower() in names:
            raise ExportError(f"{model.name}: the weapon entries '{names[name.lower()]}' and '{name}' are one entry")
        names[name.lower()] = name
        lines.append(f"entry {name} {entry.mode} {60.0 / entry.rpm!r}")
    for which, camera in (("pos", props.hip_camera), ("tpos", props.aim_camera)):
        if camera is not None:
            lines.append(f"view {which} {fmt(*eye(model, camera, notes))}")
    return "\n".join(lines) + "\n"


def compile_timing(context, model, notes):
    """The model's weapon timing through `opennova-3di weapon timing`:
    (the request, the preview the CLI printed, the edits text), or None when
    the model is no weapon or has no fire row (`notes` hears why)."""
    source = request_text(model, notes)
    if source is None:
        return None
    with scratch() as tmp:
        input_path, output_path = os.path.join(tmp, "timing.txt"), os.path.join(tmp, "edits.txt")
        with open(input_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(source)
        result = run_cli(context, ["weapon", "timing", input_path, "-o", output_path], ExportError, timeout=30,
                         hide=((input_path, "weapon timing"),))
        try:
            preview = json.loads(result.stdout)
        except (ValueError, TypeError) as e:
            raise ExportError("opennova-3di weapon timing printed no preview") from e
        with open(output_path, encoding="utf-8") as f:
            edits = f.read()
    for note in cli_notes(result, "note: "):
        notes.note(note)
    for text in short_windows(model, preview):
        notes.note(text)
    return source, preview, edits


def clip_lengths(model):
    """{action: (its clip's length in seconds, whether it loops)} of the weapon
    rows' first clips."""
    out = {}
    try:
        table = actions()
    except ExportError:
        return out
    for row in model.o3d.rows:
        suffix = action_of(row.key, table)
        clip = next((v.action for v in row.variants if v.action is not None), None)
        if suffix is not None and clip is not None:
            start, end = animation.clip_range(clip)
            out[suffix] = ((end - start) / max(1, clip.o3d.fps), clip.use_cyclic)
    return out


def short_windows(model, preview):
    """A note per one-shot clip the viewmodel shows less than SHOWN_SHARE of:
    the game replaces a clip at the next action, so the rest never shows."""
    lengths = clip_lengths(model)
    out = []
    for entry in preview.get("entries", []):
        for row in entry.get("rows", []):
            shown, clip = row.get("shown_s", -1), lengths.get(row.get("action"))
            if clip is None or shown < 0 or clip[1] or shown >= SHOWN_SHARE * clip[0]:
                continue
            out.append(f"{entry['name']}: its {row['action']} clip shows {shown:.3f} s of its {clip[0]:.3f} s before "
                       "the next action replaces it")
    return out


def preview_lines(model, preview):
    """What a preview says, one (text, warning) per line: per entry its rate
    against the target, and per action its delays and how much of its clip
    shows."""
    targets = {e.name.strip().lower(): e.rpm for e in model.o3d.weapons}
    lengths = clip_lengths(model)
    out = []
    for entry in preview.get("entries", []):
        target = targets.get(entry["name"].lower())
        line = f"{entry['name']} ({entry['mode']}): {entry['rpm']:.1f} RPM, {entry['cycle_ticks']} ticks a shot"
        out.append((line + (f" (target {target:g})" if target is not None else ""), False))
        for row in entry.get("rows", []):
            text = f"{row['action']}: DELAYSTART {row['delaystart']}, DELAYEND {row['delayend']}"
            shown, clip = row.get("shown_s", -1), lengths.get(row["action"])
            warn = False
            if shown >= 0 and clip is not None:
                text += f", shows {shown:.3f} of {clip[0]:.3f} s"
                warn = not clip[1] and shown < SHOWN_SHARE * clip[0]
            out.append((text, warn))
    return out


# The last preview of each model by name, with the request it measured: kept
# in memory only, and stale once the request changes.
_previews = {}


def preview(context, model):
    """Compile the model's timing and keep the preview: (its lines, notes)."""
    notes = NoteList()
    compiled = compile_timing(context, model, notes)
    if compiled is None:
        raise ExportError(notes.notes[0] if notes.notes else f"{model.name} has no weapon entry")
    _previews[model.name_full] = (compiled[0], compiled[1])
    return preview_lines(model, compiled[1]), notes.notes


def last_preview(model):
    """The model's last preview while its request is unchanged, else None."""
    held = _previews.get(model.name_full)
    if held is None:
        return None
    try:
        current = request_text(model)
    except ExportError:
        return None
    return held[1] if current == held[0] else None


def edits_path(adm_path):
    """Where export writes the weapon.def edits: beside the table."""
    return os.path.splitext(adm_path)[0] + "_weapon_edits.txt"


def write_edits(path, text):
    """The edits, written whole or not at all (called once the clip set has
    built)."""
    part = path + ".part"
    try:
        with open(part, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(part, path)
    finally:
        if os.path.exists(part):
            os.remove(part)


def merge(context, model, def_path, out_path):
    """The model's weapon.def edits set in a copy of `def_path`, written to
    `out_path` by `opennova-3di weapon merge`: (what it said, notes)."""
    if not os.path.isfile(def_path):
        raise ExportError(f"no weapon.def at {def_path}")
    if os.path.normcase(os.path.abspath(out_path)) == os.path.normcase(os.path.abspath(def_path)):
        raise ExportError("the merged weapon.def is a new file: choose another name than the one read")
    notes = NoteList()
    compiled = compile_timing(context, model, notes)
    if compiled is None:
        raise ExportError(notes.notes[0] if notes.notes else f"{model.name} has no weapon entry")
    with scratch() as tmp:
        edits = os.path.join(tmp, "edits.txt")
        with open(edits, "w", encoding="utf-8", newline="\n") as f:
            f.write(compiled[2])
        result = run_cli(context, ["weapon", "merge", def_path, edits, "-o", out_path], ExportError, timeout=30,
                         hide=((edits, "weapon edits"),))
    for note in cli_notes(result, "note: "):
        notes.note(note)
    return result.stdout.strip(), notes.notes


def assign(model, action, suffix):
    """Make `action` the clip of the row answering `suffix`'s slot (`reset`:
    the anim_reset row), adding the row: returns the names of the clips it
    replaced. The clip's own settings (its loop above all) are left alone:
    retail's idles rely on the clip's loop flag."""
    key = "anim_reset" if suffix == "reset" else action_key(suffix)
    slot = animation.slot_of(key)
    rows = model.o3d.rows
    row = next((r for r in rows if animation.slot_of(r.key.strip()) == slot), None)
    if row is None:
        row = rows.add()
        row.key = key
    held = [v.action for v in row.variants if v.action is not None]
    if held == [action]:
        return []
    row.variants.clear()
    row.variants.add().action = action
    return [a.name for a in held if a != action]


def set_marker(action, name, frame):
    marker = action.pose_markers.get(name) or action.pose_markers.new(name)
    marker.frame = int(frame)
    return marker


def initialize_markers(action, suffix):
    """An action's timing markers where the clip has none, at their
    defaults: fire's Shot on its first frame, recoil's Eject a frame before
    its end, the others' Active End at the end, and Ready at the end."""
    if suffix in SWITCHES or suffix == "reset":
        return
    start, end, _ = clip_bounds(action)
    wanted = {"fire": [(SHOT, start)], "recoil": [(EJECT, max(start, end - 1)), (READY, end)]}.get(
        suffix, [(READY, end)] if suffix in IDLES else [(ACTIVE, end), (READY, end)])
    for name, frame in wanted:
        if action.pose_markers.get(name) is None:
            set_marker(action, name, frame)
