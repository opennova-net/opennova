# opennova-3di anim scene -> .o3a -> Blender Actions on a model's rig.
#
# The inverse of animation.py: every clip becomes one Action on its own NLA
# track, keyed on the rig's BN## bones (replacing a clip the rig already holds
# under its name), and the table becomes the model root's rows. Nothing is
# stashed so that a re-export reproduces the source: the bind, the bone
# positions and the terminal duplicate key are the engine's derivations, the
# events are the pose's own (animation.py), and what the scene form cannot
# carry is reported.
#
# The rig's rest pose is the bind. The runtime binds every clip of a table to
# the reset clip and composes each key as `key * bind^-1`, the bind being that
# clip's first key [orig: AnimChannel_ComputeBoneMatrices @0x410da0 over the
# bind AnimMap_RegisterEntity @0x40bb60 pins]. A rig imported from a `.3di`
# carries only pivots, so its bones point wherever the model importer put
# them; the first table imported onto a rig that holds no clip yet turns each
# rest bone onto that bind. The heads, the lengths and the weights do not move,
# so the model still exports the same model, and a clip then shows in Blender
# the pose the game draws. A rig that already holds clips keeps its rest (their
# Actions are keyed against it), and a lone `.bad`, or a table with no reset
# row, names no bind at all (the game cannot load such a table, and export
# refuses one).
#
# A clip is sampled the way the runtime evaluates it: one pose per frame of the
# header's length, frames 0..frame_count, each bone's key found by walking its
# key durations and blending inside the window, the last key held past them.
#
# The events stand the body on the ground. Each frame keys the hips (BN01) at
# the frame's bottom above the ground and, when the set travels (an event
# steps across the ground), keys a `Root` bone at the ground along the summed
# steps, so a planted foot stays put. A rig without Root gets one under the
# hips, the bind clip's first bottom below them, as its top-level bone. A set
# that never steps (every first-person set) needs none: its ground is
# Blender's Z = 0. A model root still at the world origin rises so the ground
# is Z = 0, and so do the models whose bones follow it (arms on a gun): display
# only, since export reads a model standing at the origin. A stored top and a
# stored vertical step are not carried: export measures the head bone's height
# and the bottom's change, and a clip whose top stands more than 3 cm from the
# head's (from the bottom on a rig without a head) is noted.

import contextlib
import math
import os
import re

import bpy
from mathutils import Matrix, Quaternion, Vector

from . import assembly
from .animation import (ANIM_FLAG_BIT3, ANIM_FLAG_LOOP, ANIM_FLAG_TRANSLATION, bone_rows, clip_actions,
                        ground_under, head_of, part_bone, rig_of, root_of, slot_of, trigger_value)
from .export import (ATTACH_RE, BONE_RE, CENTER_RE, PART_RE, Exporter, active_model, clean_name, descendants,
                     is_lod_root, model_roots)
from .o3dtext import (ImportFailed, axis_basis, blender_axes, cli_notes, num, run_cli, scratch, strip_comment,
                      tokens)

# A rigid model's part follows its `!Rig` bone through this constraint.
FOLLOW = "O3D follow"
# A clip's own bone name that is a part's label with its BN## in lower case
# (22 of the 82 retail tables name a weapon's own bones `bn38 bone`).
LOWER_BONE_RE = re.compile(r"^bn(\d{2})(?: (.*))?$", re.IGNORECASE)
# The Root bone import makes, and how far a stored top may stand from the head
# bone's height before a clip is noted (the corpus holds 97% of person frames
# within it).
ROOT_NAME = "Root"
TOP_TOLERANCE = 0.03


def read_o3a(path):
    """The .o3a clip set: its table rows and every clip's header, bones, keys
    with their durations, translations and events (a step, the trigger word,
    the bottom and top)."""
    set_ = {"adm": "", "rows": [], "clips": []}
    clip = None
    bone = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            parts = tokens(strip_comment(raw.rstrip("\n")))
            if not parts:
                continue
            key = parts[0]
            if key == "o3a":
                continue
            if key == "adm":
                set_["adm"] = parts[1] if len(parts) > 1 else ""
            elif key == "row":
                set_["rows"].append((parts[1], parts[2:]))
            elif key == "clip":
                clip = {"name": parts[1], "fps": 30, "flags": 0, "frames": 0, "version": 1,
                        "bones": [], "events": []}
                set_["clips"].append(clip)
                bone = None
            elif clip is None:
                continue
            elif key in ("fps", "frames", "version", "flags"):
                clip[key] = int(parts[1], 0)
            elif key == "bone":
                bone = {"parent": int(parts[1]), "name": parts[6] if len(parts) > 6 else "",
                        "keys": [], "durations": [], "tr": []}
                clip["bones"].append(bone)
            elif bone is None and key != "event":
                continue
            elif key == "k":
                bone["keys"].append(tuple(num(x) for x in parts[1:5]))
                bone["durations"].append(max(1, int(parts[5])) if len(parts) > 5 else 1)
            elif key == "tr":
                bone["tr"].append(tuple(num(x) for x in parts[1:4]))
            elif key == "event":
                clip["events"].append({
                    "velocity": tuple(num(x) for x in parts[1:4]),
                    "trigger": int(parts[4], 0),
                    "bottom": num(parts[5]),
                    "top": num(parts[6]),
                })
    return set_


def run_scene(context, path):
    """The clip set's text, read, and the CLI's notes."""
    with scratch() as tmp:
        o3a = os.path.join(tmp, "set.o3a")
        result = run_cli(context, ["anim", "scene", path, "-o", o3a], ImportFailed)
        set_ = read_o3a(o3a)
    return set_, cli_notes(result, "scene drops ")


def linear(action):
    """Every keyframe interpolates linearly, as the runtime's own slerp between
    two keys does [orig: Math_QuaternionSlerp @0x615e20]."""
    for group in getattr(action, "layers", []):
        for strip in group.strips:
            for bag in getattr(strip, "channelbags", []):
                for curve in bag.fcurves:
                    for point in curve.keyframe_points:
                        point.interpolation = "LINEAR"
    for curve in getattr(action, "fcurves", []):
        for point in curve.keyframe_points:
            point.interpolation = "LINEAR"


def slerp(a, b, t):
    """Two stored keys blended as the runtime blends them: the short arc, plain
    linear weights when the keys lie within 1 - dot <= 0.01, and no
    normalization [orig: Math_QuaternionSlerp @0x615e20, as
    runtime/anim/anim_sample.cpp quat_slerp ports it]."""
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = tuple(-y for y in b)
        dot = -dot
    if 1.0 - dot <= 0.0099999998:
        wa, wb = 1.0 - t, t
    else:
        omega = math.acos(min(dot, 1.0))
        inv = 1.0 / math.sin(omega)
        wa, wb = math.sin((1.0 - t) * omega) * inv, inv * math.sin(t * omega)
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def key_at(keys, durations, frame):
    """A bone's stored key at a frame, as the runtime finds it: the key whose
    duration window holds the frame, blended toward the next key (the last
    key's next is key 0) by the frame's place in the window, and the last key
    held past the summed durations [orig: BoneAnim_FindKeyframeAtTime @0x410220,
    as runtime/anim/anim_sample.cpp sample_bone_world_rot ports it]."""
    if len(keys) == 1:
        return keys[0]
    at = 0
    for i, key in enumerate(keys):
        if at + durations[i] > frame:
            blend = (frame - at) / durations[i]
            return key if blend == 0.0 else slerp(key, keys[(i + 1) % len(keys)], blend)
        at += durations[i]
    return keys[-1]


def clip_stem(variant):
    """The clip a row variant names: the variant without a `.bad` ending (any
    case), as formats/bad/bad_build.cpp bad_build_clip_stem reads it."""
    return variant[:-4] if len(variant) > 4 and variant.lower().endswith(".bad") else variant


def at_origin(ob):
    return ob.matrix_world == Matrix.Identity(4)


def bone_name(index, name):
    """The rig bone for part `index`, named from the clip's own bone name: kept
    when it already reads BN## with that index, otherwise BN## and the label (a
    lower-case `bn38 bone` becomes `BN38 bone`, which the model export and
    compare read as the same bone)."""
    m = BONE_RE.match(name)
    if m is not None and int(m.group(1)) == index + 1:
        return name
    m = LOWER_BONE_RE.match(name)
    label = (m.group(2) or "") if m is not None and int(m.group(1)) == index + 1 else name
    return f"BN{index + 1:02d} {label}".strip()


class Loader:
    def __init__(self, context, model, set_, op=None):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.set = set_
        self.op = op
        self.notes = []
        self.basis = axis_basis(self.scene.o3d.forward)
        self.to_blender = blender_axes(self.scene.o3d.forward)
        # What the run changed, each with its inverse: a failure puts the scene
        # back as it found it (never a half-built rig).
        self.undo = []

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    def blender_rot(self, quat):
        """A mission-axes key as a Blender rotation in the model root's frame.
        Normalized, as the runtime reads a channel key (anim_sample.cpp
        bad_channel_quat): two keys blended on the linear path fall short of
        unit length (|q|^2 >= 0.995), and the matrix of such a quaternion is no
        rotation."""
        m = Quaternion((quat[3], quat[0], quat[1], quat[2])).normalized().to_matrix()
        return self.basis @ m @ self.basis.transposed()

    def arm_space(self, arm):
        """The rig's rotation in the model root's frame, which is the frame a key
        is in (export reads it back through the same root)."""
        return (self.model.matrix_world.inverted_safe() @ arm.matrix_world).to_3x3().normalized()

    @contextlib.contextmanager
    def editing(self, arm):
        """The rig in edit mode for the block, in the view layer the import
        runs in (the rig may be in no other)."""
        vl = self.context.view_layer
        held = vl.objects.active
        with self.context.temp_override(scene=self.scene, view_layer=vl, active_object=arm,
                                        object=arm, selected_objects=[arm]):
            vl.objects.active = arm
            bpy.ops.object.mode_set(mode="EDIT")
            try:
                yield arm.data.edit_bones
            finally:
                bpy.ops.object.mode_set(mode="OBJECT")
        if held is not None:
            vl.objects.active = held

    # --- the rig ------------------------------------------------------------
    def bind_clip(self):
        """The clip every clip of the table composes against: the reset row's
        (a key naming slot 0, `reset`, past its first five characters; the last
        such row) LAST variant, because each reset variant replaces the slot's
        head instead of joining a ring [orig: AnimMap_FindSlotByName @0x40cfa0,
        stricmp on the key + 5; AnimMap_RegisterBoneNode @0x40C2D0, slot 0
        self-rings @0x40c38b; AnimMap_RegisterEntity @0x40bb60 pins its clip
        @0x40bbe3], as formats/bad/bad_build.cpp bad_build_reset_stem ports it.
        None for a table with no reset row, a reset clip the set lacks, or a set
        with no table (a lone .bad)."""
        reset = None
        for key, variants in self.set["rows"]:
            if variants and slot_of(key) == "reset":
                reset = variants
        if reset is None:
            return None
        stem = clip_stem(reset[-1]).lower()
        return next((c for c in self.set["clips"] if c["name"].lower() == stem), None)

    def name_bones(self, arm, bones, clip):
        """The bone names the clip carries. A model's part table has none, so a
        rig imported from a `.3di` calls its bones BN## alone; the clip labels
        them (BN16 L Hand), and the vertex groups follow."""
        meshes = [ob for ob in bpy.data.objects
                  if ob.type == "MESH" and ob.find_armature() == arm]
        for i, pb in enumerate(bones):
            if i >= len(clip["bones"]):
                break
            name = bone_name(i, clip["bones"][i]["name"])
            old = pb.name
            bone = arm.data.bones.get(old)
            if name == old or bone is None:
                continue
            bone.name = name
            for ob in meshes:
                group = ob.vertex_groups.get(old)
                if group is not None:
                    group.name = name
            # By name: edit mode rebuilds the bones, so a held Bone goes stale.
            self.undo.append(lambda name=name, old=old: rename(arm, name, old))
        return bone_rows(arm)

    def align_rest(self, arm, bones, clip):
        """Turn each rest bone onto the bind, the reset clip's first key: the
        rig's rest pose is the bind. Heads and lengths keep their places, and
        so does an object hung from a turned bone (a skinned part's `~PPx
        attach` helper, whose place is a CXLT row): the model is unchanged."""
        rows = clip["bones"]
        to_arm = self.arm_space(arm).inverted()
        position, hung = self.hung_from(arm, {pb.name for pb in bones[:len(rows)]})
        held = {}
        with self.editing(arm) as edit_bones:
            for i, pb in enumerate(bones[:len(rows)]):
                eb = edit_bones.get(pb.name)
                if eb is None:
                    continue
                held[eb.name] = (eb.tail.copy(), eb.roll)
                # The key is the bone's rotation in the model's frame; the rest
                # bone's own space is the armature's.
                m = to_arm @ self.blender_rot(rows[i]["keys"][0])
                length = eb.length if eb.length > 1e-6 else 0.05
                eb.tail = eb.head + m @ Vector((0.0, length, 0.0))
                eb.align_roll(m @ Vector((0.0, 0.0, 1.0)))
        self.keep_places(arm, position, hung)

        def restore():
            with self.editing(arm) as edit_bones:
                for name, (tail, roll) in held.items():
                    eb = edit_bones.get(name)
                    if eb is not None:
                        eb.tail = tail
                        eb.roll = roll
            for ob, _, basis in hung:
                ob.matrix_basis = basis
        self.undo.append(restore)

    def hung_from(self, arm, names):
        """The objects hung from the named bones, each with its place at rest
        and its own transform, before an edit of the rig's rest; the rig stays
        at rest until keep_places, which takes the pose position back."""
        position = arm.data.pose_position
        arm.data.pose_position = "REST"
        self.context.view_layer.update()
        return position, [(ob, ob.matrix_world.copy(), ob.matrix_basis.copy()) for ob in arm.children
                          if ob.parent_type == "BONE" and ob.parent_bone in names]

    def keep_places(self, arm, position, hung):
        """Each hung object back at its place after an edit of the rig's rest
        (a skinned part's `~PPx attach` helper, whose place is a CXLT row).
        Through an edited bone's matrix the place comes back a float step or
        so off, which moves a CXLT row a 16.16 step: its own offset takes up
        the difference, one step at a time, until the place is the bits it
        was."""
        self.context.view_layer.update()
        for ob, world, _ in hung:
            ob.matrix_world = world
        for _ in range(8):
            self.context.view_layer.update()
            off = [(ob, world.translation - ob.matrix_world.translation) for ob, world, _ in hung]
            off = [(ob, d) for ob, d in off if d.length > 0.0]
            if not off:
                break
            for ob, d in off:
                ob.location += (ob.matrix_world @ ob.matrix_basis.inverted()).to_3x3().inverted() @ d
        arm.data.pose_position = position
        self.context.view_layer.update()

    def redrive(self):
        """A skinned model whose bones follow another model's parts (arms on a
        gun) rides sockets made at its rest; make them again at the rest the
        rig has now (assembly.drive), the parts at theirs."""
        rig = self.model.o3d.drive_rig
        if rig is None:
            return
        held = [(a.data, a.data.pose_position) for a in assembly.armatures(rig)]
        for data, _ in held:
            data.pose_position = "REST"
        self.context.view_layer.update()
        try:
            assembly.drive(self.model, rig)
        finally:
            for data, position in held:
                data.pose_position = position
            self.context.view_layer.update()

    # --- the clips ----------------------------------------------------------
    def shape_notes(self, clip):
        """What a clip keys that Blender's one-pose-per-frame channels cannot
        hold as it is stored."""
        name, frames = clip["name"], clip["frames"]
        spans = [sum(b["durations"]) for b in clip["bones"]]
        past = [i for i, s in enumerate(spans) if s > frames + 1]
        held = [i for i, s in enumerate(spans) if s < frames + 1]
        spread = [i for i, b in enumerate(clip["bones"]) if any(d > 1 for d in b["durations"])]
        if past:
            self.note(f"{name}: bones {past[:6]} key past the clip's {frames} frames, which it never "
                      "plays; those keys are not imported")
        if spread:
            self.note(f"{name}: bones {spread[:6]} hold a key over several frames; Blender keys each "
                      "frame with the pose the runtime blends there, so a re-export keys every frame")
        if held:
            self.note(f"{name}: bones {held[:6]} stop keying before the clip's last frame and hold "
                      "their last key; a re-export keys those frames too")
        if clip["version"] == 0:
            self.note(f"{name}: a version 0 clip, whose events carry no trigger word (the reader "
                      "gives each 0xffffffff); it re-exports as version 1 with that word")

    def action_for(self, clip, arm, bones, rest, stand):
        name = clip["name"]
        action = bpy.data.actions.new(name)
        self.undo.append(lambda: bpy.data.actions.remove(action))
        action.o3d.fps = float(clip["fps"])
        action.o3d.loop = bool(clip["flags"] & ANIM_FLAG_LOOP)
        action.o3d.translation = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        action.o3d.raw_flag_8 = bool(clip["flags"] & ANIM_FLAG_BIT3)
        rows = clip["bones"]
        if len(rows) > len(bones):
            self.note(f"{name}: the clip carries {len(rows)} bones, the rig {len(bones)}; the extra "
                      "channels are dropped")
        self.shape_notes(clip)
        frames = clip["frames"]
        events = clip["events"]
        translated = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        to_arm = self.arm_space(arm).inverted()
        order = {pb.name: i for i, pb in enumerate(bones)}
        above = [order[p.name] if p is not None else None for p in (part_bone(pb) for pb in bones)]
        root, head = stand["root"], stand["head"]
        disp, arm_model = stand["disp"], stand["arm_model"]
        travel = Vector((0.0, 0.0, 0.0))
        worst = (0.0, 0)
        data = arm.animation_data or arm.animation_data_create()
        slotted = hasattr(data, "action_slot")
        held = (data.action, data.use_nla, data.action_slot if slotted else None)
        try:
            data.use_nla = False
            data.action = action
            # The pose that shows what the game draws: a key IS the bone's
            # rotation in the model's frame, and the rig's rest pose is the bind
            # the runtime measures it against, so the key poses the bone
            # directly. The head follows its part parent's rest offset, and
            # the hips stand the frame's bottom above the ground, the summed
            # steps along it.
            for f in range(frames + 1):
                ev = events[f] if f < len(events) else None
                lift = travel.copy()
                if ev is not None:
                    lift.z += ev["bottom"] - stand["height"]
                posed = []
                for i, pb in enumerate(bones):
                    if i >= len(rows):
                        posed.append(rest[i])
                        continue
                    key = key_at(rows[i]["keys"], rows[i]["durations"], f)
                    rot = to_arm @ self.blender_rot(key)
                    j = above[i]
                    at = rest[i].translation if j is None else \
                        (posed[j] @ rest[j].inverted() @ rest[i]).translation
                    if i == 0:
                        at = at + disp @ lift
                    tr = rows[i]["tr"]
                    if translated and tr:
                        at = at + to_arm @ self.to_blender(tr[min(f, len(tr) - 1)])
                    posed.append(Matrix.Translation(at) @ rot.to_4x4())
                anchors = {pb.name: (rest[i], posed[i]) for i, pb in enumerate(bones)}
                if root is not None:
                    anchors[root.name] = (stand["root_rest"],
                                          Matrix.Translation(disp @ travel) @ stand["root_rest"])
                self.write_frame(bones, root, anchors, f, translated)
                if ev is not None:
                    hips = arm_model @ posed[0].translation
                    top = ev["bottom"] + ((arm_model @ posed[head].translation) - hips).z \
                        if head is not None else ev["bottom"]
                    if abs(ev["top"] - top) > worst[0]:
                        worst = (abs(ev["top"] - top), f)
                    step = self.to_blender(ev["velocity"])
                    travel += Vector((step.x, step.y, 0.0))
            self.write_triggers(arm, clip, frames)
            linear(action)
            slot = data.action_slot if slotted else None
        finally:
            data.action, data.use_nla = held[0], held[1]
            if slotted and held[0] is not None and held[2] is not None:
                data.action_slot = held[2]
        # A known gap, not a fault: retail's exporter measured most tops at the
        # head, but not all (a death fall, a crawl; 64 of the 204 first-person
        # registrations carry a top above the bottom by a rule nothing has
        # witnessed), and export writes the rig's own measure.
        if worst[0] > TOP_TOLERANCE and head is not None:
            self.note(f"{name}: its top stands up to {worst[0] * 100:.1f} cm from the head bone's height "
                      f"({bones[head].name}, frame {worst[1]}), which a re-export writes")
        elif worst[0] > TOP_TOLERANCE:
            self.note(f"{name}: its top stands up to {worst[0] * 100:.1f} cm above its bottom (frame {worst[1]}) "
                      "by a rule nothing has witnessed; a rig without a head bone re-exports the bottom")
        return action, slot

    @staticmethod
    def carried(bone, anchors):
        """A bone's pose where the frame keys it, else its rest carried by the
        nearest keyed bone above it (its rest when there is none): a control
        bone the clip does not key rides the bone it hangs from."""
        walk = bone
        while walk is not None and walk.name not in anchors:
            walk = walk.parent
        if walk is None:
            return bone.bone.matrix_local
        rest, pose = anchors[walk.name]
        return pose @ rest.inverted() @ bone.bone.matrix_local

    def write_frame(self, bones, root, anchors, frame, translated):
        """One frame's pose, keyed on the rig's bone channels: each bone's basis
        against its Blender parent's pose. Every bone keys its rotation; the
        hips and Root their place, which carries the events; the others their
        place only under the translation flag."""
        for pb in bones + ([root] if root is not None else []):
            rest, pose = anchors[pb.name]
            parent = pb.parent
            if parent is None:
                basis = rest.inverted() @ pose
            else:
                parent_pose = self.carried(parent, anchors)
                basis = (parent_pose @ parent.bone.matrix_local.inverted() @ rest).inverted() @ pose
            if pb is not root:
                pb.rotation_mode = "QUATERNION"
                pb.rotation_quaternion = basis.to_quaternion()
                pb.keyframe_insert("rotation_quaternion", frame=frame)
            if pb is root or pb is bones[0] or translated:
                pb.location = basis.translation
                pb.keyframe_insert("location", frame=frame)

    def write_triggers(self, arm, clip, frames):
        """The clip's trigger words, keyed on the rig on the clip's own
        Action."""
        events = clip["events"]
        if not events:
            self.note(f"{clip['name']}: the clip carries no event record (no step, bottom or trigger word)")
            return
        for f in range(frames + 1):
            ev = events[min(f, len(events) - 1)]
            # A version 0 event has no trigger word (the text states 0) and the
            # reader gives it 0xffffffff, the word the game plays.
            arm.o3d.anim_trigger = trigger_value(0xFFFFFFFF if clip["version"] == 0 else ev["trigger"])
            arm.keyframe_insert("o3d.anim_trigger", frame=f)

    # --- the ground ---------------------------------------------------------
    def place(self, arm, bones, bottom):
        """Stand the model on Blender's ground plane, display only: a model
        root at the world origin rises until the ground its set stands on (its
        Root's head, else the bind clip's first bottom below the hips) is Z = 0,
        and so does each model whose bones follow it (arms on a gun), so a
        first-person gun and its arms overlay a body at the hips. It rises once
        its rig is set up at the origin, where every export reads it
        (o3dtext.at_world_origin), and what binds to where things stand (a
        part's `O3D follow`, the arms' sockets) binds again. A model whose own
        bones follow another stays where it is."""
        if bottom is None or self.model.o3d.drive_rig is not None or not at_origin(self.model):
            return
        root = root_of(arm, ImportFailed)
        ground = (arm.matrix_world @ root.bone.head_local).z if root is not None else \
            (arm.matrix_world @ bones[0].bone.head_local).z - bottom
        if ground == 0.0:
            return
        movers = [self.model] + [m for m in model_roots(self.scene)
                                 if m.o3d.drive_rig == self.model and at_origin(m)]

        def move(at):
            for m in movers:
                m.matrix_world = at
            self.context.view_layer.update()
            self.rebind(arm)
            for m in movers[1:]:
                assembly.drive(m, self.model)
        self.undo.append(lambda: move(Matrix.Identity(4)))
        move(Matrix.Translation((0.0, 0.0, -ground)))

    def rebind(self, arm):
        """Each part's `O3D follow` measured again from its bone's rest where
        the rig now stands (a Child Of also carries what moves its part's own
        parent, so a bind from elsewhere would move the part twice)."""
        for ob in descendants(self.model):
            for con in ob.constraints:
                if con.name == FOLLOW and con.target == arm:
                    con.inverse_matrix = (arm.matrix_world @ arm.data.bones[con.subtarget].matrix_local).inverted()

    def ensure_root(self, arm, bones, bottom):
        """The rig's Root, made on first need: a bone on the ground, `bottom`
        below the hips, lying along the model's forward, and the rig's top
        bone, the hips' top ancestor now below it."""
        root = root_of(arm, ImportFailed)
        if root is not None:
            return root
        disp = (self.model.matrix_world.inverted_safe() @ arm.matrix_world).to_3x3().inverted_safe()
        up = disp @ Vector((0.0, 0.0, 1.0))
        forward = disp @ self.to_blender((1.0, 0.0, 0.0))
        top = bones[0].bone
        while top.parent is not None:
            top = top.parent
        top_name = top.name
        # A bone below a new parent has its rest rebuilt through the parent's,
        # a float step off: what hangs from any bone keeps its place.
        position, hung = self.hung_from(arm, {b.name for b in arm.data.bones})
        with self.editing(arm) as edit_bones:
            eb = edit_bones.new(ROOT_NAME)
            eb.head = edit_bones[bones[0].name].head - up * bottom
            eb.tail = eb.head + forward.normalized() * 0.2
            eb.align_roll(up)
            below = edit_bones[top_name]
            below.use_connect = False
            below.parent = eb
            made = eb.name
        self.keep_places(arm, position, hung)

        def remove():
            with self.editing(arm) as edit_bones:
                below = edit_bones.get(top_name)
                if below is not None:
                    below.parent = None
                eb = edit_bones.get(made)
                if eb is not None:
                    edit_bones.remove(eb)
            for ob, _, basis in hung:
                ob.matrix_basis = basis
        self.undo.append(remove)
        return root_of(arm, ImportFailed)

    def stand(self, arm, bones, rest, root, bottom):
        """How the clips stand the rig on the ground: the Root and its rest,
        the hips' rest height above the ground, the model root's frame against
        the rig's, and the head bone's place among the bones."""
        arm_model = self.model.matrix_world.inverted_safe() @ arm.matrix_world
        root_rest = root.bone.matrix_local.copy() if root is not None else None
        hips = arm_model @ rest[0].translation
        ground = ground_under(self.model.matrix_world, (arm_model @ root_rest.translation) if root is not None else None,
                              hips)
        height = (hips - ground).z
        if root is None and bottom is not None and abs(height - bottom) > 0.001:
            # Measured from the bind rather than the ground, so the bind clip's
            # first frame is the rest pose wherever the model stands.
            self.note(f"the hips stand {height:.3f} above Blender's ground (Z = 0), the set {bottom:.3f}: "
                      "a rig without a Root bone stands on Z = 0, so a re-export measures its bottoms from "
                      "there (place the model so its hips stand the set's height, or give the rig a Root)")
            height = bottom
        try:
            head = head_of(self.model, arm, ImportFailed)
        except ImportFailed as e:
            self.note(f"{e}; each top is checked against the bottom")
            head = None
        index = next((i for i, pb in enumerate(bones) if head is not None and pb.name == head.name), None)
        return {"root": root, "root_rest": root_rest, "height": height, "arm_model": arm_model,
                "disp": arm_model.to_3x3().inverted_safe(), "head": index}

    # --- a rigid model ------------------------------------------------------
    def rigid_rig(self, clip):
        """A rigid model's animation rig: an Armature named `!Rig` whose BN##
        bones mirror the model's parts, one per channel the clip carries. The
        model export ignores it (its name starts with `!`) and still reads the
        PN## empties, which follow their bones (attach_parts), so a first-person
        weapon's own clips are authored the same way a body's are."""
        roots = sorted((c for c in self.model.children if is_lod_root(c)),
                       key=lambda o: o.get("_lod_index", 0))
        if not roots:
            raise ImportFailed(f"{self.model.name}: the model has no LOD root")
        root = roots[0]
        parts, centers = {}, {}
        for ob in descendants(root):
            m = PART_RE.match(clean_name(ob.name))
            if m is not None:
                parts[int(m.group(1)) - 1] = ob
            m = CENTER_RE.match(clean_name(ob.name))
            if m is not None:
                centers[int(m.group(1)) - 1] = ob
        if not parts:
            raise ImportFailed(f"{self.model.name}: the model has neither a rig nor PN## parts")
        rows = clip["bones"]
        if len(rows) > len(parts):
            self.note(f"the clip carries {len(rows)} channels, the model {len(parts)} parts; the "
                      "extra channels are dropped")
        count = min(len(rows), len(parts))
        # A part's pivot: its `_## center` helper, else its origin (the model
        # export reads it the same way).
        pivots = {i: (centers[i] if i in centers else parts[i]).matrix_world.translation.copy()
                  for i in parts}
        index_of = {ob.name: i for i, ob in parts.items()}
        above = {}
        for i, ob in parts.items():
            walk = ob.parent
            while walk is not None and walk.name not in index_of:
                walk = walk.parent
            above[i] = index_of.get(walk.name, 0) if walk is not None else 0
        data = bpy.data.armatures.new("!Rig")
        arm = bpy.data.objects.new("!Rig", data)
        self.model.users_collection[0].objects.link(arm)

        def remove():
            bpy.data.objects.remove(arm)
            bpy.data.armatures.remove(data)
        self.undo.append(remove)
        arm.parent = root
        arm.matrix_parent_inverse = Matrix.Identity(4)
        arm.matrix_world = root.matrix_world
        with self.editing(arm) as edit_bones:
            bones = {}
            for i in range(count):
                eb = edit_bones.new(bone_name(i, rows[i]["name"]))
                eb.head = arm.matrix_world.inverted() @ pivots[i]
                eb.tail = eb.head + Vector((0.0, 0.05, 0.0))
                eb.use_connect = False
                bones[i] = eb
            for i in range(count):
                parent = rows[i]["parent"]
                if 0 <= parent < i:
                    bones[i].parent = bones[parent]
        self.pending = (arm, root, parts, centers, count, pivots, above)
        return arm

    def attach_parts(self):
        """Each part follows its bone's step away from rest: a Child Of on the
        bone, measured from the bone's rest. The rig's Rest Position mutes it
        (a driver), so at rest, where the model export reads every rig, a part
        is the very matrix it was authored as and the model exports the same
        bytes; a part hung from its bone would be taken apart into location,
        rotation and scale instead, and a retail part frame is not orthonormal
        (Mp5b_1st's scales run 0.9995 to 1.0005). A following part hangs from
        the LOD root, so it moves once, not again through a part above it, and
        keeps the model hierarchy in a `~PPx attach` helper: its own when it
        has one, else a new one at its pivot. In the collision LOD a helper is
        also the part's attach point (export.py), and a new one on the part's
        origin reads back as the very pivot, the row the builder would derive,
        so the model still exports the same bytes."""
        arm, root, parts, centers, count, pivots, above = self.pending
        self.pending = None
        # By NAME through the part order: Blender keeps its bones sorted by name,
        # so the collection's order is not the part order once the clip has
        # labelled them.
        by_part = bone_rows(arm)
        # Every part's parent matrix (with its parent inverse) before any part
        # moves: a part keeps it as its parent inverse, so its matrix is the
        # same product it was.
        into_root = root.matrix_world.inverted_safe()
        chain = {i: into_root @ parts[i].parent.matrix_world @ parts[i].matrix_parent_inverse for i in range(count)
                 if parts[i].parent is not None and parts[i].parent != root}
        for i in range(count):
            ob = parts[i]
            state = (ob.parent, ob.parent_type, ob.parent_bone, ob.matrix_parent_inverse.copy())
            self.undo.append(lambda ob=ob, state=state: restore_parent(ob, state))
            if i in chain:
                ob.parent = root
                ob.matrix_parent_inverse = chain[i]
            bone = by_part[i]
            follow = ob.constraints.new("CHILD_OF")
            follow.name = FOLLOW
            follow.target = arm
            follow.subtarget = bone.name
            follow.inverse_matrix = (arm.matrix_world @ bone.bone.matrix_local).inverted()
            self.undo.append(lambda ob=ob, follow=follow: drop_follow(ob, follow))
            rest = follow.driver_add("mute").driver
            rest.type = "SCRIPTED"
            var = rest.variables.new()
            var.name = "rest"
            var.type = "SINGLE_PROP"
            var.targets[0].id_type = "ARMATURE"
            var.targets[0].id = arm.data
            var.targets[0].data_path = "pose_position"
            rest.expression = "rest"
            # A `_## center` below the part is its pivot: the helper sits on
            # it with no offset of its own, so it reads back as the very
            # pivot (one elsewhere is reached through its world matrix).
            centre = centers.get(i)
            on = centre if centre is not None and Exporter.owning_part(centre) == i else ob
            if i == 0 or any(ATTACH_RE.match(clean_name(c.name)) and Exporter.helper_part(c) == i
                             for c in ob.children_recursive):
                continue
            helper = bpy.data.objects.new(f"~{above[i] + 1:02d} attach", None)
            helper.empty_display_size = 0.02
            self.model.users_collection[0].objects.link(helper)
            self.undo.append(lambda helper=helper: bpy.data.objects.remove(helper))
            helper.parent = on
            helper.matrix_parent_inverse = Matrix.Identity(4)
            if centre is not None and on is ob:
                helper.matrix_world = Matrix.Translation(pivots[i])

    # --- the run ------------------------------------------------------------
    def run(self):
        try:
            return self.load()
        except BaseException:
            for undo in reversed(self.undo):
                try:
                    undo()
                except Exception:
                    pass  # already gone with what it hung from; put back the rest
            self.context.view_layer.update()
            raise

    def load(self):
        clips = self.set["clips"]
        if not clips:
            raise ImportFailed("the clip set holds no clip")
        bind = self.bind_clip()
        if bind is None and self.set["rows"]:
            self.note("the table has no reset row naming a clip of the set, which the game cannot load: the "
                      "rig's rest pose stays as it is, and export refuses the table until a row names one")
        self.pending = None
        first = bind if bind is not None else clips[0]
        bottom = first["events"][0]["bottom"] if first["events"] else None
        travels = any(ev["velocity"][0] != 0.0 or ev["velocity"][1] != 0.0 for c in clips for ev in c["events"])
        arm = rig_of(self.model)
        if arm is not None:
            data = arm.animation_data
            if data is not None and data.use_tweak_mode:
                raise ImportFailed(f"{arm.name} is in NLA tweak mode; leave it (Tab in the NLA editor) "
                                   "before importing clips onto it")
            bones = bone_rows(arm)
            order = {pb.name: i for i, pb in enumerate(bones)}
            for i, pb in enumerate(bones):
                above = part_bone(pb)
                if above is not None and order[above.name] >= i:
                    raise ImportFailed(f"{pb.name}: its parent is not a lower part")
        if arm is None:
            # A rigid model (a first-person weapon): its clips get an animation
            # rig whose bones mirror its parts, built from the bind clip.
            arm = self.rigid_rig(bind if bind is not None else clips[0])
            self.context.view_layer.update()
        fresh = not clip_actions(arm)
        bones = bone_rows(arm)
        if fresh:
            bones = self.name_bones(arm, bones, bind if bind is not None else clips[0])
        if bind is not None and fresh and (self.op is None or self.op.align_rest):
            if self.model.o3d.drive_rig is not None:
                # Undone after the rest is put back, so the sockets sit at it.
                self.undo.append(self.redrive)
            self.align_rest(arm, bones, bind)
            self.redrive()
            self.context.view_layer.update()
        elif bind is not None and not fresh:
            self.note(f"{arm.name} already holds clips keyed against its rest pose, so its rest "
                      f"stays; {bind['name']}'s bind shows only on a rig without clips")
        if self.pending is not None:
            self.attach_parts()
            self.context.view_layer.update()
        root = self.ensure_root(arm, bones, bottom) if travels and bottom is not None else \
            root_of(arm, ImportFailed)
        bones = bone_rows(arm)
        self.place(arm, bones, bottom)
        rest = [pb.bone.matrix_local.copy() for pb in bones]
        stand = self.stand(arm, bones, rest, root, bottom)
        data = arm.animation_data or arm.animation_data_create()
        # A clip the rig already holds under a clip's name (the set imported
        # again) is replaced in its place: two clips of one name would write
        # one .bad, and the export refuses them. The old one steps aside until
        # the import has succeeded.
        held = {}
        for track in data.nla_tracks:
            for strip in track.strips:
                if strip.action is not None:
                    held.setdefault(clean_name(strip.action.name).lower(), []).append((strip.action, track, strip))
        replaced = []
        made = {}
        for clip in clips:
            olds = held.get(clip["name"].lower(), [])
            for old in {a for a, _, _ in olds}:
                self.set_aside(old)
            action, slot = self.action_for(clip, arm, bones, rest, stand)
            made[clip["name"].lower()] = action
            track = data.nla_tracks.new(prev=olds[0][1] if olds else None)
            self.undo.append(lambda track=track: data.nla_tracks.remove(track))
            track.name = clip["name"]
            strip = track.strips.new(clip["name"], 0, action)
            if slot is not None and strip.action_slot is None:
                strip.action_slot = slot
            replaced += [(old, old_track, old_strip, action) for old, old_track, old_strip in olds]

        # The table: its rows in file order, each variant an Action. A set with
        # no table (a lone .bad) leaves the model's rows as they are.
        props = self.model.o3d
        if not self.set["rows"]:
            self.note("the set carries no table, so the model's rows stay; add the clip to a row "
                      "for the game to play it")
        else:
            props.rows.clear()
            if self.set["adm"]:
                props.adm_path = f"//{self.set['adm']}"
            for key, variants in self.set["rows"]:
                row = props.rows.add()
                row.key = key
                for variant in variants:
                    action = made.get(clip_stem(variant).lower())
                    if action is None:
                        self.note(f"the row '{key}' names '{variant}', which the set does not hold")
                        continue
                    row.variants.add().action = action
        self.retire(data, replaced)
        return f"{len(clips)} clips on {arm.name}", self.notes

    def set_aside(self, action):
        """A clip the set replaces, renamed out of the way while its successor
        takes the name (put back if the import fails)."""
        name = action.name
        action.name = f"{name} (replaced)"
        self.undo.append(lambda: setattr(action, "name", name))

    def retire(self, data, replaced):
        """The replaced clips gone, once the import has succeeded: their strips
        (and a track left empty), then every use of each (a row of another
        model's table, a lone .bad's row here) moved to its successor."""
        for _, track, strip, _ in replaced:
            track.strips.remove(strip)
            if len(track.strips) == 0:
                data.nla_tracks.remove(track)
        gone = {}
        for old, _, _, new in replaced:
            gone.setdefault(old, new)
        for old, new in gone.items():
            old.user_remap(new)
            bpy.data.actions.remove(old)
        if gone:
            names = ", ".join(sorted({new.name for new in gone.values()}))
            self.note(f"the rig held clips of this set's names ({names}); the set's replace them")


def drop_follow(ob, follow):
    follow.driver_remove("mute")
    ob.constraints.remove(follow)


def restore_parent(ob, state):
    parent, parent_type, parent_bone, inverse = state
    ob.parent = parent
    ob.parent_type = parent_type
    ob.parent_bone = parent_bone
    ob.matrix_parent_inverse = inverse


def rename(arm, name, old):
    bone = arm.data.bones.get(name)
    if bone is not None:
        bone.name = old


def import_file(context, path, model=None, op=None):
    """Read a .adm table (or a lone .bad) onto a model's rig."""
    model = model or active_model(context)
    if model is None:
        raise ImportFailed("select an object of the model whose rig these clips animate")
    set_, notes = run_scene(context, path)
    loader = Loader(context, model, set_, op)
    message, own = loader.run()
    return message, notes + own
