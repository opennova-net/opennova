# A model's clip set -> .o3a clip-set text -> opennova-3di anim build.
#
# A clip set belongs to a model's rig (rig.rig_of): the Armature under its LOD
# 0 root whose BN## bones are the model's parts. Retail pairs a clip's
# channels with a model's parts BY INDEX [orig: BoneAnim_BuildWorldMatrices
# @0x40c400], so a clip authored here plays on that rig and on any rig that
# matches it. A model whose meshes deform with another model's rig (the arms
# beside a first-person gun) has no set of its own: the gun's set poses both.
#
#   the table  the model root's rows, each an anim slot and the clips that
#              answer it, in the order the file stores them. A row names its
#              slot by what follows its key's first five characters, one of
#              the engine's 252 (`opennova-3di catalog` prints them) [orig:
#              AnimMap_FindSlotByName @0x40cfa0], and the game drops a row
#              naming none [orig: AnimMap_ParseConfigLine @0x40cba4]. A row of
#              several clips is a ring the game serves from its LAST clip back,
#              one step at every play and every loop wrap [orig:
#              AnimMap_RegisterBoneNode @0x40c385; AnimMap_PlayAnimBySlot
#              @0x40bdb4; AnimMap_AdvanceToNextAnim @0x40bdf0].
#   the clips  the Actions the rows name, each written once, each played
#              through its slot for the rig; an Action no row names is not
#              exported. A clip's file is named after the table and the slot it
#              answers (clip_file), within the 15 characters a retail archive
#              entry holds [orig: PFF_FindEntry @0x7685d0, the 16-byte name
#              field and its NUL]; the Action's own name is the author's.
#   the reset  the row of slot `reset` (anim_reset). Its LAST clip is the bind
#              every clip is measured against, since each clip on it replaces
#              the one before [orig: AnimMap_RegisterBoneNode @0x40c38b], and
#              the rig's rest pose IS that bind (below): a reset clip must
#              start at rest, and a table whose reset row names no Action gets
#              a clip of the rest pose, one looping interval at 30 fps, as
#              retail's resets are.
#   a clip     is as long as the Action's manual frame range (else its keyed
#              range), loops when the Action is Cyclic, and plays at the
#              Action's Clip rate. It carries translations when a part moves
#              off its rest offset from its parent, and the reset carries them
#              whenever a clip does: a part moves only when the playing clip
#              AND the bind are translated [orig:
#              AnimChannel_ComputeBoneMatrices @0x410da0, the bind's flag test
#              @0x410de7]. Its event triggers are the Action's markers named
#              after the engine's trigger bits (FOOT_LEFT, FIRE_PRIMARY, ...).
#   Root       a bone named `Root` (any case), outside the BN## parts: the
#              ground under a body, the rig's top bone with the hips below it.
#              Its travel over the ground is the body's.
#   the hips   BN01, the model origin (part 0).
#   the head   the bone the model root's `head_bone` names; unnamed, the one
#              bone whose name ends in `head` (BN15 Head), and none when no
#              bone or several do.
#
# On a rig with Root a frame's event is measured from the pose, never keyed:
#   bottom     the hips' height above Root,
#   top        the head's height above Root (the bottom without a head),
#   velocity   the hips' step to the next frame: across the ground in the
#              model root's frame, and up by the change in bottom,
# and the last two events are the exporter's, not samples of the last two
# poses: a loop repeats event 0 in both, a one-shot stands still in both at
# frame_count - 1's bottom and top (every one of the 477 retail clips: 273
# loops, 204 one-shots). The runtime moves the entity by the step (forward
# vel.z, lateral vel.x of a frame), stands its origin `bottom` above the
# ground, takes the vertical from the change in bottom and reads top as the
# capsule's top [orig: AnimMap_UpdateEntity @0x40b5f0]. What retail's own
# exporter measured is read off the corpus under OPENNOVA_JO_ASSETS, not
# witnessed in code: over the 30 person tables (185,661 frames) top is the
# head bone's height within 1 cm in 91% of frames (median 0.2 mm), a planted
# foot slides back by the step (|slide| / |step| median 1.000 over the run
# clips), bone 0 never carries a translation row (0 of the 202 translated
# clips), and 118 of the 178 clips whose bottom moves store its change as the
# vertical step (60 store 0, which the runtime replaces).
#
# A rig without Root is a first-person rig: its clips pose the model where it
# stands. BN01 moves by its own translation row, and every event stands still,
# no step and bottom and top both the hips' rest height above Blender's ground
# (Z = 0) where the model stands, which is what retail's first-person clips
# carry (zero velocity on all 204 registrations, the rig's origin height the
# bottom of 201; docs/anim/adm-bad-format-re.md). Nothing reads them: the one
# event reader runs on a body's two channels, never on the viewmodel's [orig:
# AnimMap_UpdateEntity @0x40b5f0, called only from AnimMap_UpdateDualChannels
# @0x40b8c0].
#
# A channel key is the bone's rotation in the model's own frame. The runtime
# binds every clip of a table to the reset clip: that clip's first key is the
# bind, and what a bone deforms by is `key * bind^-1` [orig:
# AnimChannel_ComputeBoneMatrices @0x410da0 over the bind AnimMap_RegisterEntity
# @0x40bb60 pins]. A rig's rest pose IS that bind, so the pose a clip shows here
# is the pose the game draws. Each clip is sampled on its own: every channel it
# does not key sits at rest (Root and the hips too). Nothing else is stored:
# the bind, the bone positions and the terminal duplicate key are the engine's
# own derivations (formats/bad/bad_build.h), recomputed on every export.

import math
import os
import re

import bpy
from mathutils import Vector

from .rig import clean_name, is_root_bone, model_of, model_roots, parent_part, part_bones, rig_of
from .o3dtext import (ExportError, ModelSpace, Notes, at_world_origin, cli_notes, export_text, fmt, playing,
                      quoted)

ANIM_FLAG_LOOP = 0x1
ANIM_FLAG_TRANSLATION = 0x2
ANIM_FLAG_BIT3 = 0x8
# A row names its anim slot by its key past the first five characters,
# whatever they are, without case: `anim_reset`, `ANIM_RESET` and `xxxx_reset`
# are all slot 0 [orig: AnimMap_FindSlotByName @0x40cfa0, stricmp on key + 5].
# Every retail table spells its keys anim_<name>.
SLOT_KEY_RE = re.compile(r"^[^\s\"]{6,}$")
# A name a retail archive holds: 15 characters and the name field's NUL [orig:
# PFF_FindEntry @0x7685d0; PFF_CompareSearchNameToEntry @0x76826e]; a table's
# stem, which its clips are named after, is a bare name with no dot.
MAX_FILE_NAME = 15
FILE_STEM_RE = re.compile(r"^[A-Za-z0-9_-]+$")
# Each weapon slot's clip file code (the add-on's naming); any other slot's is
# `s<slot index>`.
SLOT_CODES = {"reset": "rst", "wpn_idle": "i", "wpn_empty_idle": "ei", "wpn_fire": "f", "wpn_recoil": "rc",
              "wpn_reload": "r", "wpn_empty": "e", "wpn_switchto": "swt", "wpn_switchfrom": "swf",
              "wpn_switchrank": "swr", "wpn_scopeup": "su", "wpn_scopedown": "sd"}
# Retail's per-frame bone scratch holds 64 bones, and so do the first-person
# viewmodel's matrix arrays, with no test on the animated path [orig:
# BoneSystem_Init @0x410170 from AnimMap_Init @0x40be52;
# Player_RenderFirstPersonViewModel @0x4ded60].
MAX_BONES = 64
# A channel steps fps / 62 / frames of its clip a tick [orig:
# AnimChannel_InitFromData @0x410560]; a loop wraps by one length only [orig:
# AnimChannel_AdvancePlayback @0x40b199], so one at 62 x frames fps or more
# runs past its rows.
TICK_STEPS = 62
# The reset clip the table lacks: one looping interval at 30 fps (the shape of
# 20 of 38 retail first-person resets and of the person resets but its length).
RESET_FPS = 30
# How far a reset clip's first frame may turn from the rest pose (degrees),
# how far a part may stray from its rest offset and still carry no
# translation, and how far a bone's scale may stray from 1 (metres, a ratio:
# the float noise of a posed rig lies well inside each).
REST_ANGLE = 0.01
TRANSLATION_EPS = 1e-5
SCALE_EPS = 1e-4


def slot_of(key):
    """The anim slot a row key names, lower case; empty for a key of five
    characters or fewer, which names none."""
    return key[5:].lower() if len(key) > 5 else ""


def slot_table():
    """{slot name: slot index} of the engine's 252 anim slots, from
    `opennova-3di catalog`; an ExportError when it cannot be read."""
    from . import catalog, catalog_error
    slots = catalog().slots
    if not slots:
        raise ExportError(f"no anim slot table: {catalog_error() or 'opennova-3di catalog printed none'}")
    return {slot_of(key): index for index, key in slots}


def trigger_bits():
    """{NAME: mask} of the engine's animation event bits, from `opennova-3di
    catalog`; an ExportError when it cannot be read."""
    from . import catalog, catalog_error
    bits = catalog().triggers
    if not bits:
        raise ExportError(f"no event trigger table: {catalog_error() or 'opennova-3di catalog printed none'}")
    return {name.upper(): mask for mask, name in bits}


def turn_between(a, b):
    """The angle between two rotations (quaternions), in degrees, in double
    precision and stable near zero, where the acos of their dot turns a single
    precision rounding into hundredths of a degree: 4 atan2(|a - b|, |a + b|)
    with b in a's hemisphere."""
    a = (a.w, a.x, a.y, a.z)
    b = (b.w, b.x, b.y, b.z)
    if sum(x * y for x, y in zip(a, b)) < 0.0:
        b = tuple(-y for y in b)
    apart = math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    together = math.sqrt(sum((x + y) ** 2 for x, y in zip(a, b)))
    return math.degrees(4.0 * math.atan2(apart, together))


def clip_file(stem, code, variant):
    """The file stem of a row's `variant`th clip (0-based): the table's stem,
    the slot's code and, from the second clip on, its number, after an
    underscore when the code ends in a digit (s12's second clip is s12_2,
    never slot 122's s122)."""
    if variant == 0:
        return f"{stem}_{code}"
    return f"{stem}_{code}{'_' if code[-1].isdigit() else ''}{variant + 1}"


def clip_range(action):
    """A clip's frames: the Action's manual frame range, else its keyed range,
    as whole frames."""
    start, end = action.frame_range
    return int(round(start)), int(round(end))


def clip_slot(action, arm):
    """The slot an Action plays on the rig through: the one made for the rig
    (its identifier names it), else the Action's only slot. An ExportError for
    an Action that keys nothing, or holds several slots and none is the
    rig's."""
    slots = list(action.slots)
    own = next((s for s in slots if s.identifier == "OB" + arm.name), None)
    if own is not None:
        return own
    if len(slots) == 1:
        return slots[0]
    if not slots:
        raise ExportError(f"the clip '{action.name}' keys nothing")
    raise ExportError(f"the clip '{action.name}' holds {len(slots)} slots ("
                      + ", ".join(s.name_display for s in slots) + f") and none is {arm.name}'s: assign it to "
                      f"{arm.name} in the Action editor and pick its slot there")


def table_path(model):
    """The model's table: its absolute path and its stem, a name retail can
    pack (the table's clips are named after it)."""
    path = bpy.path.abspath(model.o3d.adm_path or adm_default(model))
    name = os.path.basename(path)
    stem, ext = os.path.splitext(name)
    if ext.lower() != ".adm":
        raise ExportError(f"{model.name}: the animation output path must end in .adm")
    if not os.path.isabs(path):
        raise ExportError(f"{model.name}: save the .blend first or give an absolute output path (a '//' path is "
                          "relative to the saved file)")
    if not FILE_STEM_RE.match(stem) or len(name) > MAX_FILE_NAME:
        raise ExportError(f"{model.name}: the table's name '{name}' is not one a retail archive can hold: at most "
                          f"{MAX_FILE_NAME} characters, its stem letters, digits, _ and - (its clips are named after "
                          "it, <table>_<slot code>.bad, so a short one leaves them room)")
    return path, stem


def root_of(arm, failure=ExportError):
    """The rig's Root bone (a pose bone), if it has one."""
    roots = [pb for pb in arm.pose.bones if is_root_bone(pb.name)]
    if len(roots) > 1:
        raise failure(f"{arm.name}: two bones are Root ({', '.join(pb.name for pb in roots)})")
    return roots[0] if roots else None


def head_candidates(arm):
    """The rig's bones whose name ends in `head`, without case."""
    return [pb for pb in arm.pose.bones if clean_name(pb.name).lower().endswith("head")]


def head_of(model, arm, failure=ExportError):
    """The rig's head bone (a pose bone): the one the model root's `head_bone`
    names, else the one bone whose name ends in `head`; None when no bone or
    several do."""
    name = model.o3d.head_bone
    if name:
        pb = arm.pose.bones.get(name)
        if pb is None:
            raise failure(f"{model.name}: the head bone '{name}' is not a bone of {arm.name}")
        return pb
    heads = head_candidates(arm)
    return heads[0] if len(heads) == 1 else None


def ground_under(placed, root_head, point):
    """The ground under `point` (the model root's frame), in that frame: the
    Root bone's head, or on a rig without one the point of Blender's ground
    plane (world Z = 0) straight below it, for a root standing at `placed`."""
    if root_head is not None:
        return root_head
    w = placed @ point
    return placed.inverted_safe() @ Vector((w.x, w.y, 0.0))


def rig_bones(arm, failure=ExportError):
    """The rig's part bones (pose bones) in part order, and each one's part
    parent (None for BN01; a part bone under no other takes part 0, as the
    model does). A failure for a gap, a parent numbered after its child or
    more bones than retail's 64."""
    bones = part_bones(arm)
    if not bones:
        raise failure(f"{arm.name}: the rig has no BN## bones")
    count = max(bones) + 1
    missing = [i + 1 for i in range(count) if i not in bones]
    if missing:
        raise failure(f"{arm.name}: the part bones are not contiguous from BN01 (missing BN{missing[0]:02d}; "
                      "Number Parts numbers them)")
    if count > MAX_BONES:
        raise failure(f"{arm.name}: {count} part bones; a clip poses at most {MAX_BONES}, all retail's bone arrays "
                      "hold")
    pose = [arm.pose.bones[bones[i].name] for i in range(count)]
    above = [None]
    for i in range(1, count):
        p = parent_part(bones[i])
        p = 0 if p is None else p
        if p >= i:
            raise failure(f"{bones[i].name}: its parent part {bones[p].name} is numbered after it; parts number "
                          "parents first (Number Parts)")
        above.append(p)
    return pose, above


POSE_CHANNELS = ("location", "rotation_quaternion", "rotation_euler", "rotation_axis_angle", "scale")
POSE_REST = ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (1.0, 1.0, 1.0))


def rest_pose(arm):
    """Every channel a clip may key, at rest, so a clip shows what it keys over
    the rig's rest (export and Edit Clip alike)."""
    for pb in arm.pose.bones:
        for channel, value in zip(POSE_CHANNELS, POSE_REST):
            setattr(pb, channel, value)


class Clip:
    """One clip the set writes: its file stem, its Action (None: the rest
    pose, the reset the table lacks), the row that names it first, and the
    slot it plays through (set once the rig is known)."""

    def __init__(self, name, action, key):
        self.name = name
        self.action = action
        self.key = key
        self.slot = None


class Sampled:
    """A clip as the export read it: its header and its keys, translation rows
    and events per frame, and whether any part moved off its rest offset."""

    def __init__(self, fps, frames, loop, raw8):
        self.fps = fps
        self.frames = frames
        self.loop = loop
        self.raw8 = raw8
        self.keys = []
        self.translations = []
        self.events = []
        self.translated = False


class AnimExporter(Notes):
    def __init__(self, context, model):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.props = model.o3d
        self.notes = []
        self.arm = None
        self.space = None  # set in read(): the model root's frame, as the model export reads it
        self.root = None  # set in read(): the rig's Root and head bones, where it has them
        self.head = None
        self.placed = None  # set in run(): where the model root stands (a rig without Root stands on Z = 0)

    # --- the set ------------------------------------------------------------
    def plan(self, stem):
        """The table as it is written: its rows [(key, [clip file stems])] (a
        reset row first when the model has none), its clips [Clip] in row
        order, each once, and the bind (the reset row's last clip)."""
        slots = slot_table()
        rows = []
        seen = {}
        for row in self.props.rows:
            key = row.key.strip()
            if not SLOT_KEY_RE.match(key):
                raise ExportError(f"'{key}' is not an anim slot key: a row names its slot by what follows the key's "
                                  "first five characters (anim_reset, anim_idle_1), with no blank or quote")
            slot = slot_of(key)
            if slot not in slots:
                raise ExportError(f"the row '{key}' names no anim slot, and the game drops such a row: a row's slot "
                                  "is what follows its key's first five characters, one of the engine's 252 "
                                  "(`opennova-3di catalog` lists them)")
            if slot in seen:
                raise ExportError(f"the rows '{seen[slot]}' and '{key}' both answer slot {slot}: put their clips on "
                                  "one row")
            seen[slot] = key
            actions = [v.action for v in row.variants if v.action is not None]
            if not actions and slot != "reset":
                raise ExportError(f"the row '{key}' names no clip")
            rows.append((key, slot, actions))
        if not rows:
            raise ExportError(f"{self.model.name} has no table row (add one per anim slot its clips answer)")
        if "reset" not in seen:
            rows.insert(0, ("anim_reset", "reset", []))
        clips, made, table = [], {}, []
        for key, slot, actions in rows:
            code = SLOT_CODES.get(slot, f"s{slots[slot]}")
            names = []
            if not actions:
                clips.append(Clip(clip_file(stem, code, 0), None, key))
                names.append(clips[-1].name)
            for n, action in enumerate(actions):
                if action not in made:
                    made[action] = Clip(clip_file(stem, code, n), action, key)
                    clips.append(made[action])
                names.append(made[action].name)
            table.append((key, names))
        for clip in clips:
            name = clip.name + ".bad"
            if len(name) > MAX_FILE_NAME:
                what = f"the clip '{clip.action.name}'" if clip.action is not None else "the rest pose's reset clip"
                raise ExportError(f"{what} ({clip.key}) would be {name}, {len(name)} characters: a retail archive "
                                  f"holds a file name of at most {MAX_FILE_NAME}, so shorten the table's name "
                                  f"({stem}.adm)")
        reset = next(names for key, names in table if slot_of(key) == "reset")
        bind = next(c for c in clips if c.name == reset[-1])
        return table, clips, bind

    def check_clips(self, clips):
        """Every clip's slot for the rig, and what the author should hear
        about the set: clip rates the scene does not play at, and Actions
        keyed for the rig that no row names."""
        for clip in clips:
            if clip.action is not None:
                clip.slot = clip_slot(clip.action, self.arm)
        render = self.scene.render
        scene_fps = render.fps / render.fps_base
        odd = [c.action for c in clips if c.action is not None and abs(c.action.o3d.fps - scene_fps) > 1e-6]
        if odd:
            self.note(f"the scene plays at {scene_fps:g} fps and the game plays each clip at its Clip rate: "
                      + ", ".join(f"{a.name} {a.o3d.fps} fps" for a in odd[:6]) + (" ..." if len(odd) > 6 else ""))
        named = {c.action for c in clips}
        ident = "OB" + self.arm.name
        spare = sorted(a.name for a in bpy.data.actions
                       if a not in named and any(s.identifier == ident for s in a.slots))
        if spare:
            self.note(f"{len(spare)} Actions keyed for {self.arm.name} are on no row, so they are not exported: "
                      + ", ".join(spare[:6]) + (" ..." if len(spare) > 6 else ""))

    # --- a clip -------------------------------------------------------------
    def triggers(self, action, start, frames):
        """The clip's event trigger words: each marker named after an event
        bit sets that bit on its frame's event."""
        words = [0] * (frames + 1)
        bits = None
        for marker in action.pose_markers:
            name = marker.name.strip()
            if name.startswith("ON:"):
                continue  # a weapon timing marker (weapon.py)
            if bits is None:
                bits = trigger_bits()
            mask = bits.get(name.upper())
            if mask is None:
                self.note(f"{action.name}: the marker '{marker.name}' is neither an event trigger ("
                          + ", ".join(bits) + ") nor a weapon timing marker, so it exports nothing")
                continue
            f = marker.frame - start
            if not 0 <= f <= frames:
                raise ExportError(f"{action.name}: its {name} marker at frame {marker.frame} lies outside the clip "
                                  f"({start}..{start + frames})")
            words[f] |= mask
        return words

    def sample(self, clip, bones, above, rest):
        """Walk the clip, frames + 1 samples from its first frame: every
        bone's key per frame, its translation row from the rest kinematics,
        and the frame's event. The rest pose's reset is sampled with the rig
        at rest."""
        action = clip.action
        if action is None:
            s = Sampled(RESET_FPS, 1, True, False)
            start = self.scene.frame_current
            words = [0, 0]
        else:
            start, end = clip_range(action)
            s = Sampled(action.o3d.fps, end - start, action.use_cyclic, action.o3d.raw_flag_8)
            if s.frames < 1:
                raise ExportError(f"the clip '{action.name}' holds one frame; a clip needs two (its frame count is "
                                  "the interval count, so it stores one key more): give it a manual frame range")
            if s.loop and s.fps >= TICK_STEPS * s.frames:
                raise ExportError(f"the loop '{action.name}' is {s.frames} frames at {s.fps} fps: the game steps a "
                                  f"clip fps / {TICK_STEPS} frames a tick, so it would pass the whole loop within one "
                                  f"tick and read past its keys; keep a loop's rate under {TICK_STEPS} x its frames")
            words = self.triggers(action, start, s.frames)
        arm = self.arm
        s.keys = [[] for _ in bones]
        s.translations = [[] for _ in bones]
        hips, grounds, heads = [], [], []
        position = arm.data.pose_position
        with playing(arm.animation_data, action, clip.slot):
            rest_pose(arm)
            if action is None:
                arm.data.pose_position = "REST"
            try:
                for frame in range(start, start + s.frames + 1):
                    self.scene.frame_set(frame)
                    arm_world = self.space.world(arm)
                    arm_rot = arm_world.to_3x3().normalized()
                    posed = [arm_world @ pb.matrix for pb in bones]
                    for i, pb in enumerate(bones):
                        m = pb.matrix.to_3x3()
                        stretch = max(abs(m.col[k].length - 1.0) for k in range(3))
                        if stretch > SCALE_EPS or m.determinant() < 0.0:
                            what = action.name if action is not None else "the rest pose"
                            raise ExportError(f"{what}: {pb.name} is scaled at frame {frame}: a clip turns "
                                              "and moves parts, and the game reads a scaled bone as a turn (clear its "
                                              "scale keys, and hide a part some other way)")
                        # A channel key IS the bone's rotation in the model's
                        # frame: the runtime carries the bind as the
                        # skeleton's REST and poses it with the key, so the
                        # deform is `key * bind^-1` [orig:
                        # BoneAnim_BuildWorldMatrices @0x40c400 over the bind
                        # the reset clip pins, AnimMap_RegisterEntity
                        # @0x40bb60].
                        s.keys[i].append(self.space.mission_rotation(arm_rot @ m.normalized()).to_quaternion())
                        # The bone's own displacement: where its head sits,
                        # less where the rest offset from its part parent
                        # would put it. On a rig with Root the hips' own is
                        # the event (the step and the bottom), never a row.
                        if i == 0 and self.root is not None:
                            s.translations[i].append((0.0, 0.0, 0.0))
                            continue
                        j = above[i]
                        base = (arm_world @ rest[i]).translation if j is None else \
                            (posed[j] @ rest[j].inverted() @ rest[i]).translation
                        row = self.space.mission(posed[i].translation - base)
                        s.translated = s.translated or max(abs(c) for c in row) > TRANSLATION_EPS
                        s.translations[i].append(row)
                    at = posed[0].translation.copy()
                    hips.append(at)
                    if self.root is not None:
                        grounds.append(arm_world @ self.root.head)
                        heads.append((arm_world @ self.head.head) if self.head is not None else None)
            finally:
                arm.data.pose_position = position
        s.events = self.events(s, hips, grounds, heads, words, bones, rest)
        return s

    def events(self, s, hips, grounds, heads, words, bones, rest):
        """A clip's events: measured from the pose on a rig with Root, else
        standing still at the hips' rest height above Blender's ground."""
        frames = s.frames
        if self.root is None:
            hip = (self.space.world(self.arm) @ rest[0]).translation
            height = (hip - ground_under(self.placed, None, hip)).z
            return [((0.0, 0.0, 0.0), words[f], height, height) for f in range(frames + 1)]
        bottoms = [(hips[f] - grounds[f]).z for f in range(frames + 1)]
        tops = [(heads[f] - grounds[f]).z if heads[f] is not None else bottoms[f] for f in range(frames + 1)]
        measured = []
        for f in range(frames):
            step = hips[f + 1] - hips[f]
            step = Vector((step.x, step.y, bottoms[f + 1] - bottoms[f]))
            measured.append((self.space.mission(step), bottoms[f], tops[f]))
        # The last two events are the exporter's, not samples of the last two
        # poses (every one of the 477 retail clips): a loop repeats event 0 in
        # both, a one-shot stands still in both at frame_count - 1's bottom
        # and top; each keeps its own frame's trigger word. The runtime lerps
        # event trunc(frame_count * t) with the next [orig:
        # AnimChannel_InterpolateKeyframe @0x40b230], so the last interval
        # reads that one event throughout.
        tail = measured[0] if s.loop else ((0.0, 0.0, 0.0), bottoms[frames - 1], tops[frames - 1])
        out = []
        for f in range(frames + 1):
            step, bottom, top = measured[f] if f < frames - 1 else tail
            out.append((step, words[f], bottom, top))
        return out

    def check_bind(self, clip, s, bones, rest_keys):
        """The reset clip's first frame is the bind, and the rig's rest pose
        must be it: each part within REST_ANGLE of its rest turn."""
        for i, pb in enumerate(bones):
            angle = turn_between(s.keys[i][0], rest_keys[i])
            if angle > REST_ANGLE:
                raise ExportError(f"the reset clip '{clip.action.name}' starts {angle:.3f} degrees off the rest pose "
                                  f"at {pb.name}: its first frame is the bind the game measures every clip against, "
                                  "and the rig's rest pose must be that bind. Key the reset at rest, or leave the "
                                  "reset row without a clip for one of the rest pose")

    def clip_lines(self, clip, s, translated, bones, above, rest):
        flags = (ANIM_FLAG_LOOP if s.loop else 0) | (ANIM_FLAG_TRANSLATION if translated else 0) | \
            (ANIM_FLAG_BIT3 if s.raw8 else 0)
        # The header counts intervals; every channel carries frames + 1 rows,
        # a key and under the translation flag a displacement per frame. The
        # runtime blends row f with row f + 1, so the last is read [orig:
        # BoneAnim_TransformBones @0x410360].
        lines = [f"clip {quoted(clip.name)}", f"fps {s.fps}", f"flags 0x{flags:x}", f"frames {s.frames}"]
        world = self.space.world(self.arm)
        for i, pb in enumerate(bones):
            parent = -1 if i == 0 else above[i]
            pivot = self.space.mission((world @ rest[i]).translation)
            lines.append(f"bone {parent} {fmt(*pivot, pb.bone.length)} {quoted(clean_name(pb.name))}")
            for q in s.keys[i]:
                lines.append(" k " + fmt(q.x, q.y, q.z, q.w))
            for t in s.translations[i] if translated else []:
                lines.append(" tr " + fmt(*t))
        for step, trigger, bottom, top in s.events:
            lines.append("event " + fmt(*step) + f" 0x{trigger:x} " + fmt(bottom, top))
        return lines

    def read(self, table, clips, bind, out_path):
        """The clip-set text, read from the scene, and the rig's bone count."""
        self.space = ModelSpace(self.model)
        bones, above = rig_bones(self.arm)
        self.root = root_of(self.arm)
        if self.root is not None:
            self.head = head_of(self.model, self.arm)
            heads = head_candidates(self.arm)
            if not self.props.head_bone and len(heads) > 1:
                self.note("several bones end in 'head' (" + ", ".join(pb.name for pb in heads) + "), so none is "
                          "the head and each clip's top is its bottom: choose one as the model's head bone")
        rest = [pb.bone.matrix_local.copy() for pb in bones]
        arm_rot = self.space.world(self.arm).to_3x3().normalized()
        rest_keys = [self.space.mission_rotation(arm_rot @ r.to_3x3().normalized()).to_quaternion() for r in rest]
        # Every clip is read over the rig's rest; the pose, the frame and the
        # rig's pose position come back after.
        arm = self.arm
        held_position = arm.data.pose_position
        held_pose = [(pb, [tuple(getattr(pb, c)) for c in POSE_CHANNELS]) for pb in arm.pose.bones]
        held_frame = self.scene.frame_current
        try:
            arm.data.pose_position = "POSE"
            self.context.view_layer.update()
            sampled = [(clip, self.sample(clip, bones, above, rest)) for clip in clips]
        finally:
            for pb, values in held_pose:
                for channel, value in zip(POSE_CHANNELS, values):
                    setattr(pb, channel, value)
            arm.data.pose_position = held_position
            self.scene.frame_set(held_frame)
        for clip, s in sampled:
            if clip is bind and clip.action is not None:
                self.check_bind(clip, s, bones, rest_keys)
        translated = any(s.translated for _, s in sampled)
        text = ["o3a 1", f"adm {quoted(os.path.basename(out_path))}"]
        for key, names in table:
            text.append("row " + quoted(key) + "".join(" " + quoted(n) for n in names))
        for clip, s in sampled:
            text += self.clip_lines(clip, s, s.translated or (clip is bind and translated), bones, above, rest)
        return text, len(bones)

    # --- the run ------------------------------------------------------------
    def run(self):
        model = self.model
        out_path, stem = table_path(model)
        self.arm = rig_of(model)
        if self.arm is None:
            raise ExportError(f"{model.name}: clips animate a rig, the BN## bones of an Armature under its LOD 0 "
                              "root (Add Animation Rig turns its PN## parts into one)")
        owner = model_of(self.arm)
        if owner is not model:
            raise ExportError(f"{model.name}: its meshes deform with {owner.name}'s rig ({self.arm.name}), whose "
                              f"clips pose it: export {owner.name}'s clip set")
        data = self.arm.animation_data or self.arm.animation_data_create()
        if data.use_tweak_mode:
            raise ExportError(f"{model.name}: {self.arm.name} is in NLA tweak mode; leave it (Tab in the NLA "
                              "editor) before exporting its clips")
        table, clips, bind = self.plan(stem)
        self.check_clips(clips)
        self.context.view_layer.update()
        self.placed = model.matrix_world.copy()
        # The weapon edits compile before anything is written, so timing an
        # author must fix writes nothing; a table without a fire row still
        # writes its clips (weapon imports this module, hence the local
        # import).
        from . import weapon
        compiled = weapon.compile_timing(self.context, model, self)
        # Read at the world origin, the model's own frame exactly
        # (at_world_origin); the ground of a rig without Root is where the
        # model stands in the scene.
        with at_world_origin(self.context, model):
            text, count = self.read(table, clips, bind, out_path)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        # The clip-set text is the CLI's input only, as the model export's
        # scene text is: nothing lands beside the table but its clips and the
        # weapon edits.
        result = export_text(self.context, ["anim", "build"], text, "set.o3a", "clip set text", out_path)
        for note in cli_notes(result, "note: "):
            self.note(note)
        summary = f"{result.stdout.strip()} ({len(clips)} clips, {count} bones)"
        path = weapon.edits_path(out_path)
        if compiled is not None:
            weapon.write_edits(path, compiled[2])
            summary += f"; weapon.def edits in {os.path.basename(path)}"
        elif self.props.weapons and os.path.isfile(path):
            self.note(f"{os.path.basename(path)} beside the table is an earlier export's, not this one's")
        return summary, self.notes


def adm_default(model):
    """A model's table path when its root names none: beside the .blend,
    after the model."""
    return f"//{clean_name(model.name)}.adm"


def export_animations(context, model):
    """Export one model's clip set; returns the summary line and the notes."""
    return AnimExporter(context, model).run()


def clip_rig(model):
    """The model's own rig (not one its meshes borrow), else None."""
    arm = rig_of(model)
    return arm if arm is not None and model_of(arm) is model else None


def models_with_rigs(scene):
    """The models with a rig of their own, which may carry a clip set."""
    return [m for m in model_roots(scene) if clip_rig(m) is not None]


def clip_set_gap(model):
    """Why a rigged model has no clip set to export, or None when it has one."""
    return None if len(model.o3d.rows) > 0 else "no table row"
