# opennova-3di anim scene -> .o3a -> Blender Actions on a model's rig.
#
# The inverse of animation.py: every clip becomes one Action keyed for the
# model's rig, as long as its manual frame range, Cyclic when it loops, at its
# Clip rate, with a marker per event trigger bit, and the table's rows merge
# into the model root's: a row answering a slot the model already has takes
# the set's clips, and the others stay. Nothing is stashed so that a re-export
# reproduces the source: the bind, the bone positions and the terminal
# duplicate key are the engine's derivations, the events are the pose's own
# (animation.py), and what the scene form cannot carry is reported.
#
# A model with no rig (a first-person gun imported from its .3di, its parts
# PN## empties) gets one first: its parts become the BN## bones of an Armature
# under its LOD 0 root and everything on a part hangs from its bone
# (rig.make_rig, what Add Animation Rig runs). A model whose meshes deform
# with another model's rig (arms on a gun) takes no clips: the gun's set
# poses both.
#
# The rig's rest pose is the bind. The runtime binds every clip of a table to
# the reset clip and composes each key as `key * bind^-1`, the bind being that
# clip's first key [orig: AnimChannel_ComputeBoneMatrices @0x410da0 over the
# bind AnimMap_RegisterEntity @0x40bb60 pins]. A rig made from a `.3di`'s
# parts carries only pivots, so its bones point wherever it put them; the
# first table imported onto a rig that holds no clip yet turns each rest bone
# onto that bind. The heads, the lengths, the weights and the place of
# everything hung from a bone do not move, so the model still exports the same
# model, and a clip then shows in Blender the pose the game draws. A rig that
# already holds clips keeps its rest (their Actions are keyed against it), and
# a lone `.bad`, or a table with no reset row, names no bind at all.
#
# A clip is sampled the way the runtime evaluates it: one pose per frame of the
# header's length, frames 0..frame_count, each bone's key found by walking its
# key durations and blending inside the window, the last key held past them.
#
# The events stand the body on the ground. A set whose events step or bob (a
# body's) keys the hips (BN01) at each frame's bottom above the ground and a
# `Root` bone at the ground along the summed steps, so a planted foot stays
# put; a rig without Root gets one under the hips, the bind clip's first
# bottom below them, as its top bone. A set whose events stand still (every
# first-person set) needs none: its ground is Blender's Z = 0 and its hips
# keep their rest. A model root still at the world origin rises so the ground
# is Z = 0, and so do the models whose meshes deform with its rig (arms on a
# gun): display only, since export reads a model standing at the origin. A
# stored top and a stored vertical step are not carried: export measures the
# head bone's height and the bottom's change, and a clip whose top stands more
# than 3 cm from the head's (from the bottom on a rig without a head) is
# noted.

import math
import re

import bpy
from bpy_extras import anim_utils
from mathutils import Matrix, Quaternion, Vector

from .animation import (ANIM_FLAG_BIT3, ANIM_FLAG_LOOP, ANIM_FLAG_TRANSLATION, ground_under, head_of, rig_bones,
                        root_of, slot_of)
from .rig import (BONE_RE, active_model, bone_parent_matrix, editing, lod_parts, lod_roots, make_rig, model_of,
                  model_roots, rig_of)
from .o3dtext import ExportError, ImportFailed, Notes, axis_basis, blender_axes, import_text, num, strip_comment, tokens

# A clip's own bone name that is a part's label with its BN## in lower case
# (22 of the 82 retail tables name a weapon's own bones `bn38 bone`).
LOWER_BONE_RE = re.compile(r"^bn(\d{2})(?: (.*))?$", re.IGNORECASE)
# The Root bone import makes, how far a stored top may stand from the head
# bone's height before a clip is noted (the corpus holds 97% of person frames
# within it), and how far a set's bottoms may spread and still stand still.
ROOT_NAME = "Root"
TOP_TOLERANCE = 0.03
STILL = 1e-4


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


def fcurve_path(bone, channel):
    return f'pose.bones["{bpy.utils.escape_identifier(bone)}"].{channel}'


class Loader(Notes):
    def __init__(self, context, model, set_, op=None):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.set = set_
        self.op = op
        self.notes = []
        self.basis = axis_basis()
        self.to_blender = blender_axes()
        # What the run changed, each with its inverse: a failure puts the scene
        # back as it found it (but for a rig it made of the model's parts,
        # which is the model's shape from then on).
        self.undo = []

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

    # --- the rig ------------------------------------------------------------
    def rig(self):
        """The model's rig, made of its PN## parts when it has none."""
        arm = rig_of(self.model)
        if arm is not None:
            owner = model_of(arm)
            if owner is not self.model:
                raise ImportFailed(f"{self.model.name}: its meshes deform with {owner.name}'s rig ({arm.name}), "
                                   f"whose clips pose it: import the clips onto {owner.name}")
            return arm
        try:
            parts = lod_parts(lod_roots(self.model)[0]).parts
            if not parts:
                raise ImportFailed(f"{self.model.name}: its LOD 0 has no parts for clips to pose")
            arm, notes = make_rig(self.context, self.model)
        except ExportError as e:
            raise ImportFailed(str(e)) from e
        for note in notes:
            self.note(note)
        self.note(f"{self.model.name}'s PN## parts are now the bones of {arm.name}, everything on a part hung from "
                  "its bone (as Add Animation Rig does)")
        return arm

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

    def fresh(self, arm):
        """A rig no clip is keyed against yet: the model's rows name no Action
        and the rig plays none."""
        rows = any(v.action is not None for r in self.model.o3d.rows for v in r.variants)
        return not rows and (arm.animation_data is None or arm.animation_data.action is None)

    def name_bones(self, arm, bones, clip):
        """The bone names the clip carries. A model's part table has none, so a
        rig made of a `.3di`'s parts calls its bones BN## alone; the clip labels
        them (BN16 L Hand), and the vertex groups follow."""
        meshes = [ob for ob in bpy.data.objects if ob.type == "MESH" and ob.find_armature() == arm]
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
        return rig_bones(arm, ImportFailed)[0]

    def hung(self, arm):
        """The objects hung from the rig's bones, each with its bone's frame at
        rest and its place, parent inverse and offset, before an edit of the
        rig's rest; the rig stays at rest until rehang takes its pose position
        back."""
        position = arm.data.pose_position
        arm.data.pose_position = "REST"
        self.context.view_layer.update()
        out = []
        for ob in arm.children:
            bone = arm.data.bones.get(ob.parent_bone) if ob.parent_type == "BONE" else None
            if bone is not None:
                out.append((ob, bone.name, bone_parent_matrix(arm, bone), ob.matrix_world.copy(),
                            ob.matrix_parent_inverse.copy(), ob.location.copy()))
        return position, out

    def rehang(self, arm, position, hung):
        """Each hung object back where it stood after an edit of the rig's
        rest: its parent inverse takes up its bone's turn, keeping the object's
        own matrix (hang). Through the edited bone the place still comes back a
        float step or so off, which moves a 16.16 word a step (a user point's
        place): its own offset takes up the difference, one step at a time,
        until the place is the bits it was."""
        for ob, name, before, _, inverse, _ in hung:
            after = bone_parent_matrix(arm, arm.data.bones[name])
            ob.matrix_parent_inverse = after.inverted() @ before @ inverse
        for _ in range(8):
            self.context.view_layer.update()
            off = [(ob, world.translation - ob.matrix_world.translation) for ob, _, _, world, _, _ in hung]
            off = [(ob, d) for ob, d in off if d.length > 0.0]
            if not off:
                break
            for ob, d in off:
                ob.location += (ob.matrix_world @ ob.matrix_basis.inverted()).to_3x3().inverted() @ d
        arm.data.pose_position = position
        self.context.view_layer.update()

    def unhang(self, hung):
        """The hung objects' parent inverses and offsets as they were."""
        for ob, _, _, _, inverse, location in hung:
            ob.matrix_parent_inverse = inverse
            ob.location = location

    def align_rest(self, arm, bones, clip):
        """Turn each rest bone onto the bind, the reset clip's first key: the
        rig's rest pose is the bind. Heads and lengths keep their places, and
        so does everything hung from a turned bone: the model is unchanged."""
        rows = clip["bones"]
        to_arm = self.arm_space(arm).inverted()
        position, hung = self.hung(arm)
        held = {}
        with editing(self.context, arm) as edit_bones:
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
        self.rehang(arm, position, hung)

        def restore():
            with editing(self.context, arm) as edit_bones:
                for name, (tail, roll) in held.items():
                    eb = edit_bones.get(name)
                    if eb is not None:
                        eb.tail = tail
                        eb.roll = roll
            self.unhang(hung)
        self.undo.append(restore)

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
        position, hung = self.hung(arm)
        with editing(self.context, arm) as edit_bones:
            eb = edit_bones.new(ROOT_NAME)
            eb.head = edit_bones[bones[0].name].head - up * bottom
            eb.tail = eb.head + forward.normalized() * 0.2
            eb.align_roll(up)
            below = edit_bones[top_name]
            below.use_connect = False
            below.parent = eb
            made = eb.name
        self.rehang(arm, position, hung)

        def remove():
            with editing(self.context, arm) as edit_bones:
                below = edit_bones.get(top_name)
                if below is not None:
                    below.parent = None
                eb = edit_bones.get(made)
                if eb is not None:
                    edit_bones.remove(eb)
            self.unhang(hung)
        self.undo.append(remove)
        return root_of(arm, ImportFailed)

    # --- the clips ----------------------------------------------------------
    def shape_notes(self, clip, bones):
        """What a clip keys that Blender's one-pose-per-frame channels cannot
        hold as it is stored."""
        name, frames = clip["name"], clip["frames"]
        spans = [sum(b["durations"]) for b in clip["bones"]]
        past = [i for i, s in enumerate(spans) if s > frames + 1]
        held = [i for i, s in enumerate(spans) if s < frames + 1]
        spread = [i for i, b in enumerate(clip["bones"]) if any(d > 1 for d in b["durations"])]
        if len(clip["bones"]) > len(bones):
            self.note(f"{name}: the clip carries {len(clip['bones'])} bones, the rig {len(bones)}; the extra "
                      "channels are dropped")
        elif len(clip["bones"]) < len(bones):
            self.note(f"{name}: the clip carries {len(clip['bones'])} bones, the rig {len(bones)}; a re-export "
                      "keys the others at rest")
        if past:
            self.note(f"{name}: bones {past[:6]} key past the clip's {frames} frames, which it never "
                      "plays; those keys are not imported")
        if spread:
            self.note(f"{name}: bones {spread[:6]} hold a key over several frames; Blender keys each "
                      "frame with the pose the runtime blends there, so a re-export keys every frame")
        if held:
            self.note(f"{name}: bones {held[:6]} stop keying before the clip's last frame and hold "
                      "their last key; a re-export keys those frames too")
        if clip["flags"] & ANIM_FLAG_TRANSLATION and not any(
                any(c != 0.0 for t in b["tr"] for c in t) for b in clip["bones"]):
            self.note(f"{name}: the clip carries the translation flag and no translation, which a re-export leaves "
                      "off (the reset still carries it when another clip moves a part)")

    def poses(self, clip, arm, bones, above, rest, stand):
        """The clip's pose on every frame, as the game draws it: each bone's
        armature-space matrix, the Root's, and the worst gap between a stored
        top and the head's height."""
        rows = clip["bones"]
        events = clip["events"]
        translated = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        to_arm = self.arm_space(arm).inverted()
        root, head = stand["root"], stand["head"]
        disp, arm_model = stand["disp"], stand["arm_model"]
        travel = Vector((0.0, 0.0, 0.0))
        worst = (0.0, 0)
        frames = []
        for f in range(clip["frames"] + 1):
            ev = events[f] if f < len(events) else None
            lift = travel.copy()
            if ev is not None:
                lift.z += ev["bottom"] - stand["height"]
            posed = []
            for i in range(len(bones)):
                if i >= len(rows):
                    posed.append(rest[i])
                    continue
                key = key_at(rows[i]["keys"], rows[i]["durations"], f)
                rot = to_arm @ self.blender_rot(key)
                j = above[i]
                at = rest[i].translation if j is None else (posed[j] @ rest[j].inverted() @ rest[i]).translation
                if i == 0:
                    at = at + disp @ lift
                tr = rows[i]["tr"]
                if translated and tr:
                    at = at + to_arm @ self.to_blender(tr[min(f, len(tr) - 1)])
                posed.append(Matrix.Translation(at) @ rot.to_4x4())
            root_pose = Matrix.Translation(disp @ travel) @ stand["root_rest"] if root is not None else None
            frames.append((posed, root_pose))
            if ev is not None:
                hips = arm_model @ posed[0].translation
                top = ev["bottom"] + ((arm_model @ posed[head].translation) - hips).z \
                    if head is not None else ev["bottom"]
                if abs(ev["top"] - top) > worst[0]:
                    worst = (abs(ev["top"] - top), f)
                step = self.to_blender(ev["velocity"])
                travel += Vector((step.x, step.y, 0.0))
        return frames, worst

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

    def action_for(self, clip, arm, bones, above, rest, stand):
        """One clip as an Action keyed for the rig: each bone's rotation every
        frame against its Blender parent's pose, the hips' and Root's place
        (which carry the events) and, under the translation flag, every bone's
        place; its frame range, loop, rate and flag 8 from the header, its
        event triggers as markers."""
        name = clip["name"]
        translated = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        self.shape_notes(clip, bones)
        frames, worst = self.poses(clip, arm, bones, above, rest, stand)
        root = stand["root"]
        keyed = bones + ([root] if root is not None else [])
        values = {pb.name: ([], []) for pb in keyed}
        for posed, root_pose in frames:
            anchors = {pb.name: (rest[i], posed[i]) for i, pb in enumerate(bones)}
            if root is not None:
                anchors[root.name] = (stand["root_rest"], root_pose)
            for pb in keyed:
                rest_m, pose = anchors[pb.name]
                parent = pb.parent
                if parent is None:
                    basis = rest_m.inverted() @ pose
                else:
                    parent_pose = self.carried(parent, anchors)
                    basis = (parent_pose @ parent.bone.matrix_local.inverted() @ rest_m).inverted() @ pose
                locations, rotations = values[pb.name]
                q = basis.to_quaternion()
                # Neighbouring keys in one hemisphere, so Blender's per-channel
                # blend between frames takes the short way (export samples whole
                # frames and reads either sign alike).
                if rotations and rotations[-1].dot(q) < 0.0:
                    q.negate()
                rotations.append(q)
                locations.append(basis.translation.copy())
        action = bpy.data.actions.new(name)
        self.undo.append(lambda: bpy.data.actions.remove(action))
        action.use_frame_range = True
        action.frame_start, action.frame_end = 0, clip["frames"]
        action.use_cyclic = bool(clip["flags"] & ANIM_FLAG_LOOP)
        action.o3d.fps = max(1, min(255, clip["fps"]))
        action.o3d.raw_flag_8 = bool(clip["flags"] & ANIM_FLAG_BIT3)
        slot = action.slots.new(id_type="OBJECT", name=arm.name)
        bag = anim_utils.action_ensure_channelbag_for_slot(action, slot)
        linear = bpy.types.Keyframe.bl_rna.properties["interpolation"].enum_items["LINEAR"].value
        count = len(frames)

        def curve(bone, channel, index, series):
            fc = bag.fcurves.new(fcurve_path(bone, channel), index=index, group_name=bone)
            fc.keyframe_points.add(count)
            fc.keyframe_points.foreach_set("co", [v for f, x in enumerate(series) for v in (float(f), x)])
            fc.keyframe_points.foreach_set("interpolation", [linear] * count)
            fc.update()

        for pb in keyed:
            locations, rotations = values[pb.name]
            if pb is not root:
                pb.rotation_mode = "QUATERNION"
                for k in range(4):
                    curve(pb.name, "rotation_quaternion", k, [q[k] for q in rotations])
            if pb is root or pb is bones[0] or translated:
                for k in range(3):
                    curve(pb.name, "location", k, [t[k] for t in locations])
        self.write_triggers(action, clip)
        # A known gap, not a fault: retail's exporter measured most tops at the
        # head, but not all (a death fall, a crawl; 64 of the 204 first-person
        # registrations carry a top above the bottom by a rule nothing has
        # witnessed), and export writes the rig's own measure.
        head = stand["head"]
        if worst[0] > TOP_TOLERANCE and head is not None:
            self.note(f"{name}: its top stands up to {worst[0] * 100:.1f} cm from the head bone's height "
                      f"({bones[head].name}, frame {worst[1]}), which a re-export writes")
        elif worst[0] > TOP_TOLERANCE:
            self.note(f"{name}: its top stands up to {worst[0] * 100:.1f} cm above its bottom (frame {worst[1]}) "
                      "by a rule nothing has witnessed; a rig without a head bone re-exports the bottom")
        return action

    def write_triggers(self, action, clip):
        """The clip's trigger words as markers, one per bit per frame, named
        after the engine's event bits (opennova-3di catalog)."""
        name = clip["name"]
        events = clip["events"]
        if not events:
            self.note(f"{name}: the clip carries no event record (no step, bottom or trigger word)")
            return
        if clip["version"] == 0:
            # A version 0 event has no trigger word (the text states 0), and
            # the reader gives it 0xffffffff, every bit.
            self.note(f"{name}: a version 0 clip, whose events carry no trigger word (the game reads every bit "
                      "set); it re-exports as version 1 with the bits its markers set")
            return
        from . import catalog
        bits = catalog().triggers
        known = 0
        for mask, _ in bits:
            known |= mask
        stray = 0
        for f, ev in enumerate(events[:clip["frames"] + 1]):
            word = ev["trigger"] & 0xFFFFFFFF
            stray |= word & ~known
            for mask, bit in bits:
                if word & mask:
                    action.pose_markers.new(bit).frame = f
        if stray:
            self.note(f"{name}: its events set trigger bits no engine table names (0x{stray:x}), which a marker "
                      "cannot carry; a re-export leaves them off")

    # --- the ground ---------------------------------------------------------
    def place(self, arm, bones, bottom):
        """Stand the model on Blender's ground plane, display only: a model
        root at the world origin rises until the ground its set stands on (its
        Root's head, else the bind clip's first bottom below the hips) is Z = 0,
        and so does each model whose meshes deform with its rig (arms on a gun),
        so a first-person gun and its arms overlay a body at the hips. Every
        export reads a model standing at the origin (o3dtext.at_world_origin)."""
        if bottom is None or not at_origin(self.model):
            return
        root = root_of(arm, ImportFailed)
        ground = (arm.matrix_world @ root.bone.head_local).z if root is not None else \
            (arm.matrix_world @ bones[0].bone.head_local).z - bottom
        if ground == 0.0:
            return
        movers = [self.model] + [m for m in model_roots(self.scene)
                                 if m is not self.model and m.parent is None and rig_of(m) == arm and at_origin(m)]

        def move(at):
            for m in movers:
                m.matrix_world = at
            self.context.view_layer.update()
        self.undo.append(lambda: move(Matrix.Identity(4)))
        move(Matrix.Translation((0.0, 0.0, -ground)))

    def stand(self, arm, bones, rest, root, bottom):
        """How the clips stand the rig on the ground: the Root and its rest,
        the hips' rest height above the ground, the model root's frame against
        the rig's, and the head bone's place among the bones."""
        arm_model = self.model.matrix_world.inverted_safe() @ arm.matrix_world
        root_rest = root.bone.matrix_local.copy() if root is not None else None
        hips = arm_model @ rest[0].translation
        ground = ground_under(self.model.matrix_world, (arm_model @ root_rest.translation) if root is not None else
                              None, hips)
        height = (hips - ground).z
        if root is None and bottom is not None and abs(height - bottom) > 0.001:
            # Measured from the bind rather than the ground, so the bind clip's
            # first frame is the rest pose wherever the model stands.
            self.note(f"the hips stand {height:.3f} above Blender's ground (Z = 0), the set {bottom:.3f}: a rig "
                      f"without a Root bone stands on Z = 0, so a re-export writes {height:.3f} as every clip's "
                      "bottom (place the model so its hips stand the set's height)")
            height = bottom
        head = None
        if root is not None:
            try:
                pb = head_of(self.model, arm, ImportFailed)
            except ImportFailed as e:
                self.note(f"{e}; each top is checked against the bottom")
                pb = None
            head = next((i for i, b in enumerate(bones) if pb is not None and b.name == pb.name), None)
        return {"root": root, "root_rest": root_rest, "height": height, "arm_model": arm_model,
                "disp": arm_model.to_3x3().inverted_safe(), "head": head}

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
                      "rig's rest pose stays as it is, and export writes a reset clip of it")
        first = bind if bind is not None else clips[0]
        bottom = first["events"][0]["bottom"] if first["events"] else None
        events = [ev for c in clips for ev in c["events"]]
        travels = any(ev["velocity"][0] != 0.0 or ev["velocity"][1] != 0.0 for ev in events)
        bobs = bool(events) and max(ev["bottom"] for ev in events) - min(ev["bottom"] for ev in events) > STILL
        arm = self.rig()
        data = arm.animation_data
        if data is not None and data.use_tweak_mode:
            raise ImportFailed(f"{arm.name} is in NLA tweak mode; leave it (Tab in the NLA editor) before "
                               "importing clips onto it")
        bones, _ = rig_bones(arm, ImportFailed)
        fresh = self.fresh(arm)
        if fresh:
            bones = self.name_bones(arm, bones, bind if bind is not None else clips[0])
        if bind is not None and fresh and (self.op is None or self.op.align_rest):
            self.align_rest(arm, bones, bind)
        elif bind is not None and not fresh:
            self.note(f"{arm.name} already holds clips keyed against its rest pose, so its rest stays; "
                      f"{bind['name']}'s bind shows only on a rig without clips")
        # A set that steps or bobs is a body's, which stands on a Root; a set
        # that stands still (every first-person set) needs none.
        root = self.ensure_root(arm, bones, bottom) if (travels or bobs) and bottom is not None else \
            root_of(arm, ImportFailed)
        bones, above = rig_bones(arm, ImportFailed)
        self.place(arm, bones, bottom)
        rest = [pb.bone.matrix_local.copy() for pb in bones]
        stand = self.stand(arm, bones, rest, root, bottom)
        # An Action a row of the model names under a clip's name (the set
        # imported again) steps aside for the new one, which takes its place
        # everywhere once the import has succeeded.
        held = {}
        for row in self.model.o3d.rows:
            for v in row.variants:
                if v.action is not None:
                    held.setdefault(v.action.name.lower(), v.action)
        made, replaced = {}, []
        for clip in clips:
            old = held.get(clip["name"].lower())
            if old is not None and old not in (a for a, _ in replaced):
                self.set_aside(old)
            action = self.action_for(clip, arm, bones, above, rest, stand)
            made[clip["name"].lower()] = action
            if old is not None:
                replaced.append((old, action))
        self.merge_rows(made)
        reference = bind if bind is not None else clips[0]
        render = self.scene.render
        held_rate = (render.fps, render.fps_base)

        def rate(fps, base):
            render.fps, render.fps_base = fps, base
        self.undo.append(lambda: rate(*held_rate))
        rate(max(1, reference["fps"]), 1.0)
        self.retire(replaced)
        return f"{len(clips)} clips on {arm.name}", self.notes

    def merge_rows(self, made):
        """The set's table merged into the model's rows by slot: a row the
        model has takes the set's clips, a new one is added, and every other
        row stays. A lone .bad carries no table and leaves the rows alone."""
        props = self.model.o3d
        if not self.set["rows"]:
            self.note("the set carries no table, so the model's rows stay; add the clip to a row for the game to "
                      "play it")
            return
        if self.set["adm"] and not props.adm_path:
            props.adm_path = f"//{self.set['adm']}"
        for key, variants in self.set["rows"]:
            slot = slot_of(key)
            row = next((r for r in props.rows if slot_of(r.key.strip()) == slot), None)
            if row is None:
                row = props.rows.add()
                row.key = key
            row.variants.clear()
            for variant in variants:
                action = made.get(clip_stem(variant).lower())
                if action is None:
                    self.note(f"the row '{key}' names '{variant}', which the set does not hold")
                    continue
                row.variants.add().action = action

    def set_aside(self, action):
        """An Action the set replaces, renamed out of the way while its
        successor takes the name (put back if the import fails)."""
        name = action.name
        action.name = f"{name} (replaced)"
        self.undo.append(lambda: setattr(action, "name", name))

    def retire(self, replaced):
        """The replaced Actions gone once the import has succeeded, every use
        of each (a row of any model's table) moved to its successor."""
        for old, new in replaced:
            old.user_remap(new)
            bpy.data.actions.remove(old)
        if replaced:
            self.note("the rows named clips of this set's names (" + ", ".join(sorted(n.name for _, n in replaced))
                      + "); the set's replace them")


def rename(arm, name, old):
    bone = arm.data.bones.get(name)
    if bone is not None:
        bone.name = old


def import_file(context, path, model=None, op=None):
    """Read a .adm table (or a lone .bad) onto a model's rig."""
    model = model or active_model(context)
    if model is None:
        raise ImportFailed("select an object of the model whose rig these clips animate")
    set_, notes = import_text(context, ["anim", "scene"], path, "set.o3a", read_o3a)
    loader = Loader(context, model, set_, op)
    message, own = loader.run()
    return message, notes + own
