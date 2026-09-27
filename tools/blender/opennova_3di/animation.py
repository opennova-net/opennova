# A model's animations -> .o3a clip-set text -> opennova-3di anim build.
#
# A clip set belongs to a model's rig: a skinned model's LOD 0 armature, or a
# rigid model's `!Rig` (anim_import.py builds one whose bones mirror its parts).
# Retail pairs a clip's channels with the model's parts BY INDEX, so a clip
# authored here plays on that rig (and on the retail rig it matches).
#
#   clips      the Actions on the armature's NLA tracks, in track order, each
#              played through its strip's action slot (Blender 4.4 and up). An
#              Action's name is the .bad file stem; its own properties carry
#              the loop and translation flags and the frame rate.
#   the table  the model root's `.adm` rows: a slot key and its clip ring, in
#              the order the file stores (the engine serves a row from its LAST
#              variant back). The reset row names the clip whose bind every
#              other clip is measured against.
#   Root       a bone named `Root` (any case), outside the BN## parts: the
#              ground under the character, the rig's top-level bone with the
#              hips below it. Its travel over the ground is the body's. A rig
#              without one (a first-person rig) stands on Blender's ground
#              plane, world Z = 0 (our convention: the ground is not in a clip).
#   the hips   BN01, the model origin (part 0).
#   the head   the bone the model root's `head_bone` names; unnamed, the one
#              bone whose name ends in `head` (BN15 Head), and none when no
#              bone or several do (a first-person rig has none).
#   trigger    the armature's keyed `o3d.anim_trigger` word: the footstep,
#              fire and foley bits the body consumes (opennova-3di catalog
#              prints them).
#
# A frame's event is measured from the pose, never keyed:
#   bottom     the hips' height above the ground,
#   top        the head's height above the ground (the bottom without a head),
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
# A channel key is the bone's rotation in the model's own frame. The runtime
# binds every clip of a table to the reset clip: that clip's first key is the
# bind, and what a bone deforms by is `key * bind^-1` [orig:
# AnimChannel_ComputeBoneMatrices @0x410da0 over the bind AnimMap_RegisterEntity
# @0x40bb60 pins]. A rig's rest pose IS that bind, so the pose a clip shows here
# is the pose the game draws. Each clip is sampled on its own: every channel it
# does not key sits at rest (Root and the hips too), the trigger at zero, and a
# bone that follows another model's parts (the drive) follows its own clip
# instead. Nothing else is stored: the bind, the bone positions and the
# terminal duplicate key are the engine's own derivations
# (formats/bad/bad_build.h), recomputed on every export.

import os
import re

import bpy
from mathutils import Vector

from . import assembly
from .export import BONE_RE, clean_name, is_lod_root, is_root_bone, model_roots
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


def slot_of(key):
    """The anim slot a row key names, lower case; empty for a key of five
    characters or fewer, which names none."""
    return key[5:].lower() if len(key) > 5 else ""


# A pose bone's own channels: what a clip keys, and what sits at rest where it
# does not.
POSE_CHANNELS = ("location", "rotation_quaternion", "rotation_euler", "rotation_axis_angle", "scale")
POSE_REST = ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (1.0, 1.0, 1.0))


def trigger_word(value):
    """The trigger property as the event's 32-bit word."""
    return int(value) & 0xFFFFFFFF


def trigger_value(word):
    """An event's 32-bit trigger word as the signed property Blender holds:
    a word with bit 31 set (the 0xffffffff of a version 0 clip) is negative."""
    word &= 0xFFFFFFFF
    return word - (1 << 32) if word >= 1 << 31 else word


def rig_of(model):
    """A model's rig: the armature under its lowest LOD root."""
    roots = sorted((c for c in model.children if is_lod_root(c)), key=lambda o: o.get("_lod_index", 0))
    for root in roots:
        for child in root.children:
            if child.type == "ARMATURE":
                return child
    return None


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


def bone_rows(arm):
    """The rig's BN## pose bones in part order, and an error for a gap."""
    rows = {}
    for pb in arm.pose.bones:
        m = BONE_RE.match(clean_name(pb.name))
        if m is None:
            continue
        index = int(m.group(1)) - 1
        if index in rows:
            raise ExportError(f"{arm.name}: two bones are part {index + 1}")
        rows[index] = pb
    if not rows:
        raise ExportError(f"{arm.name}: the rig has no BN## bones")
    if sorted(rows) != list(range(len(rows))):
        raise ExportError(f"{arm.name}: the BN## bones are not contiguous from BN01")
    return [rows[i] for i in range(len(rows))]


def part_bone(pb):
    """A bone's part parent: its nearest BN## ancestor, past Root and any `!`
    control bone between them, as the model export reads a part's parent; None
    for a root part."""
    p = pb.parent
    while p is not None and BONE_RE.match(clean_name(p.name)) is None:
        p = p.parent
    return p


def clip_strips(arm):
    """The clip set: every Action on the rig's NLA tracks, in track order and
    each once, with the strip that plays it (its action slot names the
    Action's channels for this rig on Blender 4.4 and up)."""
    data = arm.animation_data
    if data is None:
        return []
    out = []
    for track in data.nla_tracks:
        for strip in track.strips:
            if strip.action is not None and all(strip.action != a for a, _ in out):
                out.append((strip.action, strip))
    return out


def clip_actions(arm):
    """The clip set's Actions, in track order."""
    return [action for action, _ in clip_strips(arm)]


def clip_range(action):
    """A clip's frame range: the Action's own, as whole frames."""
    start, end = action.frame_range
    return int(round(start)), int(round(end))


class AnimExporter(Notes):
    def __init__(self, context, model):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.props = model.o3d
        self.settings = self.scene.o3d
        self.space = None  # set in run(): the model root's frame, as the model export reads it
        self.root = None  # set in run(): the rig's Root and head bones, where it has them
        self.head = None
        self.placed = None  # set in run(): where the model root stands (a rig without Root stands on Z = 0)
        self.notes = []

    # --- the set ------------------------------------------------------------
    def clip_name(self, action):
        return self.props.clip_prefix + clean_name(action.name)

    def rows(self):
        out = []
        for row in self.props.rows:
            key = row.key.strip()
            if not SLOT_KEY_RE.match(key):
                raise ExportError(f"'{key}' is not an anim slot key: a row names its slot by what follows the "
                                  "key's first five characters (anim_reset, anim_idle_5), with no blank or quote")
            variants = [self.clip_name(v.action) for v in row.variants if v.action is not None]
            if not variants:
                raise ExportError(f"the row '{key}' names no clip")
            out.append((key, variants))
        if not out:
            raise ExportError("the model has no .adm row (a clip set needs at least the reset row)")
        # Retail cannot load a table without a reset row: it reads slot 0's
        # head without a test [orig: AnimMap_LoadAdmFile @0x40cc40, @0x40ce11..
        # 0x40ce16], and the CLI refuses one.
        if not any(slot_of(key) == "reset" for key, _ in out):
            raise ExportError("the table has no reset row (a key naming slot reset, as anim_reset does), which "
                              "the game cannot load: its last clip is the bind every clip is measured against")
        return out

    def bone_lines(self, pb, index, rest, pose, translations):
        parent = -1
        above = part_bone(pb)
        if above is not None:
            parent = int(BONE_RE.match(clean_name(above.name)).group(1)) - 1
            if parent >= index:
                raise ExportError(f"{pb.name}: its parent is not a lower part")
        pivot = self.space.mission((self.space.world(self.arm) @ rest[index]).translation)
        lines = [f"bone {parent} {fmt(*pivot, pb.bone.length)} {quoted(clean_name(pb.name))}"]
        for q in pose[index]:
            lines.append(" k " + fmt(q.x, q.y, q.z, q.w))
        for t in translations[index] if translations is not None else []:
            lines.append(" tr " + fmt(*t))
        return lines

    def reset_pose(self):
        """Every channel a clip may key, at rest: a clip exports what it keys
        over the rig's rest, never what the clip sampled before it left."""
        for pb in self.arm.pose.bones:
            for channel, value in zip(POSE_CHANNELS, POSE_REST):
                setattr(pb, channel, value)
        self.arm.o3d.anim_trigger = 0

    def sample(self, action, strip, bones, rest, start, frames, loop):
        """Walk the clip, frames + 1 samples from its first frame: every bone's
        key per frame, its translation from the rest kinematics, and the frame's
        event, measured from the pose (the hips' step, bottom and top) with its
        keyed trigger word."""
        keys = [[] for _ in bones]
        translations = [[] for _ in bones]
        hips, grounds, heads, triggers = [], [], [], []
        arm = self.arm
        order = {pb.name: i for i, pb in enumerate(bones)}
        above = [order[p.name] if p is not None else None for p in (part_bone(pb) for pb in bones)]
        # The strip's slot, not the one assigning the Action picks: an Action
        # authored on another rig names its channels under that rig's slot,
        # and without it nothing here would move.
        with playing(arm.animation_data, action, getattr(strip, "action_slot", None)):
            self.reset_pose()
            for frame in range(start, start + frames + 1):
                self.scene.frame_set(frame)
                arm_world = self.space.world(arm)
                arm_rot = arm_world.to_3x3().normalized()
                posed = [arm_world @ pb.matrix for pb in bones]
                for i, pb in enumerate(bones):
                    # A channel key IS the bone's rotation in the model's frame:
                    # the runtime carries the bind as the skeleton's REST and
                    # poses it with the key, so the deform is `key * bind^-1`
                    # [orig: BoneAnim_BuildWorldMatrices @0x40c400 over the bind
                    # the reset clip pins, AnimMap_RegisterEntity @0x40bb60].
                    keys[i].append(self.space.mission_rotation(arm_rot @ pb.matrix.to_3x3().normalized())
                                   .to_quaternion())
                    # The bone's own displacement: where its head sits, less
                    # where the rest offset from its part parent would put it.
                    # The hips' own is the event (the step and the bottom),
                    # never a row.
                    if i == 0:
                        translations[i].append((0.0, 0.0, 0.0))
                        continue
                    j = above[i]
                    if j is None:
                        base = (arm_world @ rest[i]).translation
                    else:
                        base = (posed[j] @ rest[j].inverted() @ rest[i]).translation
                    translations[i].append(self.space.mission(posed[i].translation - base))
                at = posed[0].translation.copy()
                hips.append(at)
                grounds.append(ground_under(self.placed, (arm_world @ self.root.head) if self.root else None, at))
                heads.append((arm_world @ self.head.head) if self.head is not None else None)
                triggers.append(trigger_word(arm.o3d.anim_trigger))
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
        tail = measured[0] if loop else ((0.0, 0.0, 0.0), bottoms[frames - 1], tops[frames - 1])
        events = []
        for f in range(frames + 1):
            step, bottom, top = measured[f] if f < frames - 1 else tail
            events.append((step, triggers[f], bottom, top))
        return keys, translations, events

    def clip_lines(self, action, strip, bones, rest):
        props = action.o3d
        flags = 0
        if props.loop:
            flags |= ANIM_FLAG_LOOP
        if props.translation:
            flags |= ANIM_FLAG_TRANSLATION
        if props.raw_flag_8:
            flags |= ANIM_FLAG_BIT3
        # The header counts intervals: the Action's own range, or a longer
        # length the clip states, whose extra frames hold the Action's last pose
        # (Blender holds every channel past its last key).
        start, end = clip_range(action)
        frames = max(end - start, int(props.frames))
        if frames < 1:
            raise ExportError(f"the clip '{action.name}' holds one frame; a clip needs two (its "
                              "frame count is the interval count, so it stores one key more)")
        keys, translations, events = self.sample(action, strip, bones, rest, start, frames, props.loop)
        lines = [f"clip {quoted(self.clip_name(action))}",
                 f"fps {max(1, int(round(props.fps)))}",
                 f"flags 0x{flags:x}", f"frames {frames}"]
        # Every channel carries frames + 1 rows: a key, and under the
        # translation flag a displacement, per frame; the runtime blends row f
        # with row f + 1, so the last is read [orig: BoneAnim_TransformBones
        # @0x410360].
        rows = translations if props.translation else None
        for i, pb in enumerate(bones):
            lines += self.bone_lines(pb, i, rest, keys, rows)
        for step, trigger, bottom, top in events:
            lines.append("event " + fmt(*step) + f" 0x{trigger:x} " + fmt(bottom, top))
        return lines

    def read(self, out_path):
        """The clip-set text, read from the scene; the rig's bones and clips."""
        model = self.model.name
        self.space = ModelSpace(self.model, self.settings.forward)
        bones = bone_rows(self.arm)
        self.root = root_of(self.arm)
        self.head = head_of(self.model, self.arm)
        heads = head_candidates(self.arm)
        if not self.props.head_bone and len(heads) > 1:
            self.note("several bones end in 'head' (" + ", ".join(pb.name for pb in heads) + "), so none is "
                      "the head and each clip's top is its bottom: choose one as the model's head bone")
        rows = self.rows()
        strips = clip_strips(self.arm)
        if not strips:
            raise ExportError(f"{model}: the rig holds no clip (push each Action onto its own NLA "
                              "track)")
        for action, strip in strips:
            if hasattr(strip, "action_slot") and strip.action_slot is None:
                raise ExportError(f"{model}: the NLA strip playing '{action.name}' has no action slot "
                                  "(choose one in the strip's properties)")
        named = {v for _, variants in rows for v in variants}
        for name in sorted(named):
            if not any(self.clip_name(a) == name for a, _ in strips):
                raise ExportError(f"{model}: the table names '{name}', which is not a clip on the rig")
        for action, _ in strips:
            if self.clip_name(action) not in named:
                self.note(f"the clip '{action.name}' is on no table row, so nothing plays it")

        # The rest pose is the bind every key is measured against. A bone that
        # follows another model's parts (assembly.drive: arms on a gun) is
        # display only, so the drive is muted while the clips are read.
        held_position = self.arm.data.pose_position
        held_pose = [(pb, [tuple(getattr(pb, c)) for c in POSE_CHANNELS]) for pb in self.arm.pose.bones]
        held_trigger = self.arm.o3d.anim_trigger
        drives = [con for pb in self.arm.pose.bones for con in pb.constraints
                  if con.name == assembly.DRIVE and not con.mute]
        held_frame = self.scene.frame_current
        rest = [pb.bone.matrix_local.copy() for pb in bones]
        try:
            self.arm.data.pose_position = "POSE"
            for con in drives:
                con.mute = True
            self.context.view_layer.update()
            text = ["o3a 1", f"adm {quoted(os.path.basename(out_path))}"]
            for key, variants in rows:
                text.append("row " + quoted(key) + "".join(" " + quoted(v) for v in variants))
            for action, strip in strips:
                text += self.clip_lines(action, strip, bones, rest)
        finally:
            for con in drives:
                con.mute = False
            for pb, values in held_pose:
                for channel, value in zip(POSE_CHANNELS, values):
                    setattr(pb, channel, value)
            self.arm.o3d.anim_trigger = held_trigger
            self.arm.data.pose_position = held_position
            self.scene.frame_set(held_frame)
        return text, bones, strips

    # --- the run ------------------------------------------------------------
    def run(self):
        model = self.model.name
        out_path = bpy.path.abspath(self.props.adm_path or adm_default(self.model))
        if not out_path.lower().endswith(".adm"):
            raise ExportError(f"{model}: the animation output path must end in .adm")
        if not os.path.isabs(out_path):
            raise ExportError(f"{model}: save the .blend first or give an absolute output path (a "
                              "'//' path is relative to the saved file)")
        self.arm = rig_of(self.model)
        if self.arm is None:
            raise ExportError(f"{model}: animations need a rig (an Armature of BN## bones under "
                              "its LOD 0 root)")
        data = self.arm.animation_data
        if data is not None and data.use_tweak_mode:
            raise ExportError(f"{model}: {self.arm.name} is in NLA tweak mode; leave it (Tab in the "
                              "NLA editor) before exporting its clips")
        self.context.view_layer.update()
        self.placed = self.model.matrix_world.copy()
        # Read at the world origin, the model's own frame exactly
        # (at_world_origin); the ground of a rig without Root is where the
        # model stands in the scene.
        with at_world_origin(self.context, self.model):
            text, bones, strips = self.read(out_path)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        # The clip-set text is the CLI's input only, as the model export's
        # scene text is: nothing lands beside the table but its clips.
        result = export_text(self.context, ["anim", "build"], text, "set.o3a", "clip set text", out_path)
        for note in cli_notes(result, "note: "):
            self.note(note)
        return f"{result.stdout.strip()} ({len(strips)} clips, {len(bones)} bones)", self.notes


def adm_default(model):
    """A model's table path when its root names none: beside the .blend,
    after the model."""
    return f"//{clean_name(model.name)}.adm"


def export_animations(context, model):
    """Export one model's clip set; returns the summary line and the notes."""
    return AnimExporter(context, model).run()


def models_with_rigs(scene):
    return [m for m in model_roots(scene) if rig_of(m) is not None]


def clip_set_gap(model):
    """Why a rigged model has no clip set to export, or None when it has one:
    a table row and a clip on its rig."""
    rows = len(model.o3d.rows) > 0
    clips = bool(clip_actions(rig_of(model)))
    if rows and clips:
        return None
    if not rows and not clips:
        return "no clip set (no .adm row, no clip on its rig)"
    return "no .adm row" if not rows else "no clip on its rig"
