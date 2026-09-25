# A model's animations -> .o3a clip-set text -> opennova-3di anim build.
#
# A clip set belongs to a skinned model: the rig is its LOD 0 armature, and
# retail pairs a clip's channels with the model's parts BY INDEX, so a clip
# authored here plays on that rig (and on the retail rig it matches).
#
#   clips      the Actions on the armature's NLA tracks, in track order. An
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
# The key a channel stores is `rest * pose * rest^-1` of the bone's rotation in
# the model's own frame: the engine composes a channel against the bind as
# `bind^-1 * key` [orig: AnimChannel_ComputeBoneMatrices @0x410da0], the bind is
# the reset clip's first key, and a rig's rest pose IS its bind, so the pose a
# clip shows here is the pose the game draws. Nothing else is stored: the bind,
# the bone positions, the capsule extents and the terminal duplicate key are the
# engine's own derivations (formats/bad/bad_build.h), recomputed on every export.

import os
import re
import subprocess

import bpy
from mathutils import Matrix, Quaternion, Vector

from .export import BONE_RE, ExportError, axis_basis, axis_map, clean_name, is_lod_root, model_roots

ANIM_FLAG_LOOP = 0x1
ANIM_FLAG_TRANSLATION = 0x2
ANIM_FLAG_BIT3 = 0x8
# The .adm slot key namespace (AnimMap_ParseConfigLine @0x40cb60 keeps rows
# whose first token starts with anim_).
SLOT_RE = re.compile(r"^anim_[A-Za-z0-9_]+$")
# The root track: a bone of the rig, outside the BN## parts, whose per-frame
# step is the clip's event velocity. It rides the clip's own Action, so one
# Action holds a clip whole.
RM_NAME = "!RM"


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


def clip_actions(arm):
    """The clip set: every Action on the rig's NLA tracks, in track order, and
    each one once."""
    data = arm.animation_data
    if data is None:
        return []
    out = []
    for track in data.nla_tracks:
        for strip in track.strips:
            if strip.action is not None and strip.action not in out:
                out.append(strip.action)
    return out


def clip_range(action):
    """A clip's frame range: the Action's own, as whole frames."""
    start, end = action.frame_range
    return int(round(start)), int(round(end))


def quoted(name):
    if name and all(c not in ' \t"#' for c in name):
        return name
    return '"' + name.replace('"', "") + '"'


def fmt(*values):
    out = []
    for v in values:
        if isinstance(v, float):
            s = f"{v:.9g}"
            out.append("0" if s == "-0" else s)
        else:
            out.append(str(v))
    return " ".join(out)


class AnimExporter:
    def __init__(self, context, model):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.props = model.o3d
        self.settings = self.scene.o3d
        self.to_mission = axis_map(self.settings.forward)
        self.basis = axis_basis(self.settings.forward)
        self.space = None
        self.notes = []

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    # --- frames -------------------------------------------------------------
    def world(self, ob):
        return ob.matrix_world if self.space is None else self.space @ ob.matrix_world

    def mission(self, v):
        return self.to_mission(v)

    def mission_rot(self, rotation):
        """A Blender rotation (3x3, in the model root's frame) as a mission-axes
        quaternion: the basis change the vector map is, applied to a rotation
        (export.Exporter.frame_of turns a part frame the same way)."""
        return (self.basis.transposed() @ rotation @ self.basis).to_quaternion()

    # --- the set ------------------------------------------------------------
    def rows(self):
        out = []
        for row in self.props.rows:
            key = row.key.strip()
            if not SLOT_RE.match(key):
                raise ExportError(f"'{key}' is not an anim slot key (anim_<name>)")
            variants = [clean_name(v.action.name) for v in row.variants if v.action is not None]
            if not variants:
                raise ExportError(f"the row '{key}' names no clip")
            out.append((key, variants))
        if not out:
            raise ExportError("the model has no .adm row (a clip set needs at least the reset row)")
        return out

    def bone_lines(self, pb, index, rest, pose, translations):
        parent = -1
        if pb.parent is not None:
            m = BONE_RE.match(clean_name(pb.parent.name))
            if m is None:
                raise ExportError(f"{pb.name}: its parent '{pb.parent.name}' is not a BN## bone")
            parent = int(m.group(1)) - 1
            if parent >= index:
                raise ExportError(f"{pb.name}: its parent is not a lower part")
        pivot = self.mission(rest[index].translation)
        lines = [f"bone {parent} {fmt(*pivot, pb.bone.length)} {quoted(clean_name(pb.name))}"]
        for q in pose[index]:
            lines.append(" k " + fmt(q.x, q.y, q.z, q.w))
        for t in translations[index] if translations is not None else []:
            lines.append(" tr " + fmt(*t))
        return lines

    def sample(self, action, bones, rest):
        """Walk the clip: every bone's key per frame, its translation from the
        rest kinematics, and the frame's root step and trigger word."""
        start, end = clip_range(action)
        frames = list(range(start, end + 1))
        keys = [[] for _ in bones]
        translations = [[] for _ in bones]
        steps = []
        triggers = []
        extents = []
        arm = self.arm
        rm = rm_of(arm)
        rm_rest = (self.world(arm) @ rm.bone.matrix_local).translation if rm is not None else None
        data = arm.animation_data or arm.animation_data_create()
        held = (data.action, data.use_nla)
        data.use_nla = False
        data.action = action
        rest_rot = [m.to_3x3().normalized() for m in rest]
        order = {pb.name: i for i, pb in enumerate(bones)}
        try:
            for frame in frames:
                self.scene.frame_set(frame)
                arm_world = self.world(arm)
                arm_rot = arm_world.to_3x3().normalized()
                posed = [arm_world @ pb.matrix for pb in bones]
                for i, pb in enumerate(bones):
                    pose_rot = self.mission_rot(arm_rot @ pb.matrix.to_3x3().normalized())
                    bind = self.mission_rot(arm_rot @ rest_rot[i])
                    keys[i].append(bind @ pose_rot @ bind.inverted())
                    # The bone's own displacement: where its head sits, less
                    # where the rest offset alone would put it.
                    parent = pb.parent
                    if parent is None:
                        base = (arm_world @ rest[i]).translation
                    else:
                        j = order[parent.name]
                        follow = (arm_world @ rest[j]).inverted() @ (arm_world @ rest[i])
                        base = (posed[j] @ follow).translation
                    translations[i].append(self.mission(posed[i].translation - base))
                if rm is not None:
                    steps.append(self.mission((arm_world @ rm.matrix).translation - rm_rest))
                else:
                    steps.append((0.0, 0.0, 0.0))
                triggers.append(int(arm.o3d.anim_trigger))
                extents.append((arm.o3d.capsule_bottom, arm.o3d.capsule_top))
        finally:
            data.action, data.use_nla = held
        # The event velocity is the frame's step, the first measured from the
        # model origin (a clip's root track starts there, and the importer sums
        # the steps back into it).
        events = []
        for f in range(len(frames)):
            before = steps[f - 1] if f > 0 else (0.0, 0.0, 0.0)
            events.append((tuple(steps[f][k] - before[k] for k in range(3)), triggers[f], extents[f]))
        return keys, translations, events, len(frames)

    def clip_lines(self, action, bones, rest):
        props = action.o3d
        flags = 0
        if props.loop:
            flags |= ANIM_FLAG_LOOP
        if props.translation:
            flags |= ANIM_FLAG_TRANSLATION
        if props.raw_flag_8:
            flags |= ANIM_FLAG_BIT3
        keys, translations, events, samples = self.sample(action, bones, rest)
        if samples < 2:
            raise ExportError(f"the clip '{action.name}' holds one frame; a clip needs two (its "
                              "frame count is the interval count, so it stores one key more)")
        # The header counts intervals: the keys are the samples, and the block
        # of translations one fewer. A clip may state a longer length, whose
        # extra frames hold its last key.
        frames = max(samples - 1, int(props.frames))
        lines = [f"clip {quoted(clean_name(action.name))}",
                 f"fps {max(1, int(round(props.fps)))}",
                 f"flags 0x{flags:x}", f"frames {frames}"]
        rows = translations if props.translation else None
        if rows is not None:
            # One row per frame: a clip longer than its keys repeats the last.
            rows = [(t + [t[-1]] * frames)[:frames] for t in rows]
        for i, pb in enumerate(bones):
            lines += self.bone_lines(pb, i, rest, keys, rows)
        while len(events) < frames + 1:
            events.append(((0.0, 0.0, 0.0), events[-1][1], events[-1][2]))
        for step, trigger, extent in events[:frames + 1]:
            line = "event " + fmt(*step) + f" 0x{trigger & 0xFFFFFFFF:x}"
            if props.capsule_keys:
                line += " " + fmt(*extent)
            lines.append(line)
        return lines

    # --- the run ------------------------------------------------------------
    def run(self):
        model = self.model.name
        out_path = bpy.path.abspath(self.props.adm_path)
        if not out_path.lower().endswith(".adm"):
            raise ExportError(f"{model}: the animation output path must end in .adm")
        if not os.path.isabs(out_path):
            raise ExportError(f"{model}: save the .blend first or give an absolute output path (a "
                              "'//' path is relative to the saved file)")
        self.arm = rig_of(self.model)
        if self.arm is None:
            raise ExportError(f"{model}: animations need a rig (an Armature of BN## bones under "
                              "its LOD 0 root)")
        self.context.view_layer.update()
        root = self.model.matrix_world
        if root != Matrix.Identity(4):
            self.space = root.inverted_safe()
        bones = bone_rows(self.arm)
        rows = self.rows()
        actions = clip_actions(self.arm)
        if not actions:
            raise ExportError(f"{model}: the rig holds no clip (push each Action onto its own NLA "
                              "track)")
        named = {v for _, variants in rows for v in variants}
        for name in sorted(named):
            if not any(clean_name(a.name) == name for a in actions):
                raise ExportError(f"{model}: the table names '{name}', which is not a clip on the rig")
        for action in actions:
            if clean_name(action.name) not in named:
                self.note(f"the clip '{action.name}' is on no table row, so nothing plays it")

        # The rest pose is the bind every key is measured against.
        held = self.arm.data.pose_position
        self.arm.data.pose_position = "POSE"
        self.context.view_layer.update()
        rest = [pb.bone.matrix_local.copy() for pb in bones]
        held_frame = self.scene.frame_current
        try:
            text = ["o3a 1", f"adm {quoted(os.path.basename(out_path))}"]
            for key, variants in rows:
                text.append("row " + quoted(key) + "".join(" " + quoted(v) for v in variants))
            for action in actions:
                text += self.clip_lines(action, bones, rest)
        finally:
            self.arm.data.pose_position = held
            self.scene.frame_set(held_frame)

        out_dir = os.path.dirname(out_path)
        os.makedirs(out_dir, exist_ok=True)
        o3a_path = os.path.splitext(out_path)[0] + ".o3a"
        with open(o3a_path, "w", newline="\n") as f:
            f.write("\n".join(text) + "\n")

        from . import cli_path
        cli = cli_path(self.context)
        if not os.path.isfile(cli):
            raise ExportError(f"opennova-3di not found at {cli}")
        result = subprocess.run([cli, "anim", "build", o3a_path, "-o", out_path],
                               capture_output=True, text=True)
        if result.returncode != 0:
            raise ExportError((result.stderr or result.stdout).strip()[:2000])
        for line in result.stderr.splitlines():
            if "note: " in line:
                self.note(line.split("note: ", 1)[1])
        return f"{result.stdout.strip()} ({len(actions)} clips, {len(bones)} bones)", self.notes


def export_animations(context, model):
    """Export one model's clip set; returns the summary line and the notes."""
    return AnimExporter(context, model).run()


def models_with_rigs(scene):
    return [m for m in model_roots(scene) if rig_of(m) is not None]
