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
#   !RM        a bone of the rig outside its BN## parts, keyed per frame: its
#              per-frame step is the clip's event velocity (the body animates in
#              place and the engine moves the entity by these).
#   trigger    the armature's keyed `o3d.anim_trigger` word: the footstep,
#              fire and foley bits the body consumes (opennova-3di catalog
#              prints them). A clip that carries its own capsule extents keys
#              `o3d.capsule_bottom` and `o3d.capsule_top` beside it; otherwise
#              the engine derives them from the pose.
#
# A channel key is the bone's rotation in the model's own frame. The runtime
# binds every clip of a table to the reset clip: that clip's first key is the
# bind, and what a bone deforms by is `key * bind^-1` [orig:
# AnimChannel_ComputeBoneMatrices @0x410da0 over the bind AnimMap_RegisterEntity
# @0x40bb60 pins]. A rig's rest pose IS that bind, so the pose a clip shows here
# is the pose the game draws. Each clip is sampled on its own: every channel it
# does not key sits at rest, the root at the origin, the trigger and capsule at
# zero, and a bone that follows another model's parts (the drive) follows its
# own clip instead. Nothing else is stored: the bind, the bone positions, the
# capsule extents and the terminal duplicate key are the engine's own
# derivations (formats/bad/bad_build.h), recomputed on every export.

import os
import re

import bpy

from . import assembly
from .export import BONE_RE, clean_name, is_lod_root, model_roots
from .o3dtext import ExportError, ModelSpace, cli_notes, fmt, quoted, run_cli, scratch

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


# The root track: a bone of the rig, outside the BN## parts, whose per-frame
# step is the clip's event velocity. It rides the clip's own Action, so one
# Action holds a clip whole.
RM_NAME = "!RM"
# A pose bone's own channels: what a clip keys, and what sits at rest where it
# does not.
POSE_CHANNELS = ("location", "rotation_quaternion", "rotation_euler", "rotation_axis_angle", "scale")
POSE_REST = ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (1.0, 1.0, 1.0))
# The rig's keyed event channels, zero where a clip does not key them.
EVENT_PROPS = ("anim_trigger", "capsule_bottom", "capsule_top")


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


def rm_of(arm):
    """The rig's root-track bone, if it has one."""
    return arm.pose.bones.get(RM_NAME) if arm is not None else None


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
    """A bone's part parent: its nearest BN## ancestor, past any `!` control
    bone between them, as the model export reads a part's parent; None for a
    root part."""
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


class AnimExporter:
    def __init__(self, context, model):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.props = model.o3d
        self.settings = self.scene.o3d
        self.space = None  # set in run(): the model root's frame, as the model export reads it
        self.notes = []

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    # --- the set ------------------------------------------------------------
    def rows(self):
        out = []
        for row in self.props.rows:
            key = row.key.strip()
            if not SLOT_KEY_RE.match(key):
                raise ExportError(f"'{key}' is not an anim slot key: a row names its slot by what follows the "
                                  "key's first five characters (anim_reset, anim_idle_5), with no blank or quote")
            variants = [clean_name(v.action.name) for v in row.variants if v.action is not None]
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
        for name in EVENT_PROPS:
            setattr(self.arm.o3d, name, 0)

    def sample(self, action, strip, bones, rest, start, frames):
        """Walk the clip, frames + 1 samples from its first frame: every bone's
        key per frame, its translation from the rest kinematics, and the frame's
        root step, trigger word and capsule extents."""
        keys = [[] for _ in bones]
        translations = [[] for _ in bones]
        steps = []
        triggers = []
        extents = []
        arm = self.arm
        rm = rm_of(arm)
        rm_rest = (self.space.world(arm) @ rm.bone.matrix_local).translation if rm is not None else None
        order = {pb.name: i for i, pb in enumerate(bones)}
        above = [order[p.name] if p is not None else None for p in (part_bone(pb) for pb in bones)]
        data = arm.animation_data
        slotted = hasattr(data, "action_slot")
        held = (data.action, data.use_nla, data.action_slot if slotted else None)
        try:
            data.use_nla = False
            data.action = action
            if slotted:
                # The strip's slot, not the one assigning the Action picks: an
                # Action authored on another rig names its channels under that
                # rig's slot, and without it nothing here would move.
                data.action_slot = strip.action_slot
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
                    j = above[i]
                    if j is None:
                        base = (arm_world @ rest[i]).translation
                    else:
                        base = (posed[j] @ rest[j].inverted() @ rest[i]).translation
                    translations[i].append(self.space.mission(posed[i].translation - base))
                if rm is not None:
                    steps.append(self.space.mission((arm_world @ rm.matrix).translation - rm_rest))
                else:
                    steps.append((0.0, 0.0, 0.0))
                triggers.append(trigger_word(arm.o3d.anim_trigger))
                extents.append((arm.o3d.capsule_bottom, arm.o3d.capsule_top))
        finally:
            data.action, data.use_nla = held[0], held[1]
            if slotted and held[0] is not None and held[2] is not None:
                data.action_slot = held[2]
        # The event velocity is the frame's step, the first measured from the
        # model origin (a clip's root track starts there, and the importer sums
        # the steps back into it).
        events = []
        for f in range(frames + 1):
            before = steps[f - 1] if f > 0 else (0.0, 0.0, 0.0)
            events.append((tuple(steps[f][k] - before[k] for k in range(3)), triggers[f], extents[f]))
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
        keys, translations, events = self.sample(action, strip, bones, rest, start, frames)
        lines = [f"clip {quoted(clean_name(action.name))}",
                 f"fps {max(1, int(round(props.fps)))}",
                 f"flags 0x{flags:x}", f"frames {frames}"]
        # Every channel carries frames + 1 rows: a key, and under the
        # translation flag a displacement, per frame; the runtime blends row f
        # with row f + 1, so the last is read [orig: BoneAnim_TransformBones
        # @0x410360].
        rows = translations if props.translation else None
        for i, pb in enumerate(bones):
            lines += self.bone_lines(pb, i, rest, keys, rows)
        for step, trigger, extent in events:
            line = "event " + fmt(*step) + f" 0x{trigger:x}"
            if props.capsule_keys:
                line += " " + fmt(*extent)
            lines.append(line)
        return lines

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
        self.space = ModelSpace(self.model, self.settings.forward)
        bones = bone_rows(self.arm)
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
            if not any(clean_name(a.name) == name for a, _ in strips):
                raise ExportError(f"{model}: the table names '{name}', which is not a clip on the rig")
        for action, _ in strips:
            if clean_name(action.name) not in named:
                self.note(f"the clip '{action.name}' is on no table row, so nothing plays it")

        # The rest pose is the bind every key is measured against. A bone that
        # follows another model's parts (assembly.drive: arms on a gun) is
        # display only, so the drive is muted while the clips are read.
        held_position = self.arm.data.pose_position
        held_pose = [(pb, [tuple(getattr(pb, c)) for c in POSE_CHANNELS]) for pb in self.arm.pose.bones]
        held_events = [(name, getattr(self.arm.o3d, name)) for name in EVENT_PROPS]
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
            for name, value in held_events:
                setattr(self.arm.o3d, name, value)
            self.arm.data.pose_position = held_position
            self.scene.frame_set(held_frame)

        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        # The clip-set text is the CLI's input only, as the model export's
        # scene text is: nothing lands beside the table but its clips.
        with scratch() as tmp:
            o3a_path = os.path.join(tmp, "set.o3a")
            with open(o3a_path, "w", newline="\n", encoding="utf-8") as f:
                f.write("\n".join(text) + "\n")
            result = run_cli(self.context, ["anim", "build", o3a_path, "-o", out_path], ExportError,
                             hide=((o3a_path, "clip set text"),))
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
