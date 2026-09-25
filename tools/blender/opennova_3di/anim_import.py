# opennova-3di anim scene -> .o3a -> Blender Actions on a model's rig.
#
# The inverse of animation.py: every clip becomes one Action on its own NLA
# track, keyed on the rig's BN## bones (replacing a clip the rig already holds
# under its name), and the table becomes the model root's rows. Nothing is
# stashed so that a re-export reproduces the source: the bind, the bone
# positions, the capsule extents and the terminal duplicate key are the
# engine's derivations, and what the scene form cannot carry is reported.
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
# row, names no bind at all (the game then binds each clip to its own first
# key).
#
# A clip is sampled the way the runtime evaluates it: one pose per frame of the
# header's length, frames 0..frame_count, each bone's key found by walking its
# key durations and blending inside the window, the last key held past them.

import contextlib
import math
import os
import re

import bpy
from mathutils import Matrix, Quaternion, Vector

from . import assembly
from .animation import (ANIM_FLAG_BIT3, ANIM_FLAG_LOOP, ANIM_FLAG_TRANSLATION, RM_NAME, bone_rows,
                        clip_actions, part_bone, rig_of, rm_of, trigger_value)
from .export import (ATTACH_RE, BONE_RE, CENTER_RE, PART_RE, Exporter, active_model, clean_name, descendants,
                     is_lod_root)
from .o3dtext import (ImportFailed, axis_basis, blender_axes, cli_notes, num, run_cli, scratch, strip_comment,
                      tokens)

# A rigid model's part follows its `!Rig` bone through this constraint.
FOLLOW = "O3D follow"
# A clip's own bone name that is a part's label with its BN## in lower case
# (22 of the 82 retail tables name a weapon's own bones `bn38 bone`).
LOWER_BONE_RE = re.compile(r"^bn(\d{2})(?: (.*))?$", re.IGNORECASE)


def read_o3a(path):
    """The .o3a clip set: its table rows and every clip's header, bones, keys
    with their durations, translations and events."""
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
                        "capsule": None, "bones": [], "events": []}
                set_["clips"].append(clip)
                bone = None
            elif clip is None:
                continue
            elif key in ("fps", "frames", "version", "flags"):
                clip[key] = int(parts[1], 0)
            elif key == "capsule":
                clip["capsule"] = (num(parts[1]), num(parts[2]))
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
                    "extents": (num(parts[5]), num(parts[6])) if len(parts) > 6 else None,
                })
    # A clip's `capsule` record is the pair every event carries.
    for clip in set_["clips"]:
        for ev in clip["events"]:
            if ev["extents"] is None:
                ev["extents"] = clip["capsule"]
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
            if len(key) > 5 and variants and key[5:].lower() == "reset":
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
        rig's rest pose is the bind. Heads and lengths keep their places."""
        rows = clip["bones"]
        to_arm = self.arm_space(arm).inverted()
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

        def restore():
            with self.editing(arm) as edit_bones:
                for name, (tail, roll) in held.items():
                    eb = edit_bones.get(name)
                    if eb is not None:
                        eb.tail = tail
                        eb.roll = roll
        self.undo.append(restore)

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

    def action_for(self, clip, arm, bones, rest):
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
        translated = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        to_arm = self.arm_space(arm).inverted()
        order = {pb.name: i for i, pb in enumerate(bones)}
        above = [order[p.name] if p is not None else None for p in (part_bone(pb) for pb in bones)]
        data = arm.animation_data or arm.animation_data_create()
        slotted = hasattr(data, "action_slot")
        held = (data.action, data.use_nla, data.action_slot if slotted else None)
        try:
            data.use_nla = False
            data.action = action
            # The pose that shows what the game draws: a key IS the bone's
            # rotation in the model's frame, and the rig's rest pose is the bind
            # the runtime measures it against, so the key poses the bone
            # directly. The head follows its part parent's rest offset.
            for f in range(frames + 1):
                posed = []
                for i, pb in enumerate(bones):
                    if i >= len(rows):
                        posed.append(rest[i])
                        continue
                    key = key_at(rows[i]["keys"], rows[i]["durations"], f)
                    rot = to_arm @ self.blender_rot(key)
                    j = above[i]
                    head = rest[i].translation if j is None else \
                        (posed[j] @ rest[j].inverted() @ rest[i]).translation
                    tr = rows[i]["tr"]
                    if translated and tr:
                        head = head + to_arm @ self.to_blender(tr[min(f, len(tr) - 1)])
                    posed.append(Matrix.Translation(head) @ rot.to_4x4())
                self.write_frame(bones, rest, above, posed, f, translated)
            action.o3d.capsule_keys = self.write_events(arm, clip, frames)
            linear(action)
            slot = data.action_slot if slotted else None
        finally:
            data.action, data.use_nla = held[0], held[1]
            if slotted and held[0] is not None and held[2] is not None:
                data.action_slot = held[2]
        return action, slot

    def write_frame(self, bones, rest, above, posed, frame, translated):
        """One frame's pose, keyed on the rig's bone channels: each bone's basis
        against its Blender parent's pose, a `!` control bone's being its rest
        under the nearest part bone (the clip keys no control bone)."""
        order = {pb.name: i for i, pb in enumerate(bones)}
        for i, pb in enumerate(bones):
            parent = pb.parent
            if parent is None:
                basis = rest[i].inverted() @ posed[i]
            else:
                j = order.get(parent.name)
                parent_rest = parent.bone.matrix_local
                if j is not None:
                    parent_pose = posed[j]
                elif above[i] is None:
                    parent_pose = parent_rest
                else:
                    a = above[i]
                    parent_pose = posed[a] @ rest[a].inverted() @ parent_rest
                basis = (parent_pose @ parent_rest.inverted() @ rest[i]).inverted() @ posed[i]
            pb.rotation_mode = "QUATERNION"
            pb.rotation_quaternion = basis.to_quaternion()
            pb.keyframe_insert("rotation_quaternion", frame=frame)
            if translated:
                pb.location = basis.translation
                pb.keyframe_insert("location", frame=frame)

    def write_events(self, arm, clip, frames):
        """The root track and the trigger word, on the clip's own Action: the
        `!RM` bone walks the summed event steps, and the rig carries the keyed
        trigger word. Returns whether the clip carries its capsule extents."""
        events = clip["events"]
        if not events:
            self.note(f"{clip['name']}: the clip carries no event record (no root motion, no capsule)")
            return False
        rm = rm_of(arm)
        # A step is a displacement in the model's frame; the bone's location is
        # in its own rest frame, under the rig's.
        to_rm = (self.arm_space(arm) @ rm.bone.matrix_local.to_3x3().normalized()).inverted() \
            if rm is not None else None
        at = Vector((0.0, 0.0, 0.0))
        # The extents the clip carries are keyed beside the trigger: the rule
        # retail's own tool measured them by is not witnessed, and the engine's
        # derivation from the pose lands within centimetres of it.
        carry = any(ev["extents"] is not None for ev in events)
        for f in range(frames + 1):
            ev = events[min(f, len(events) - 1)]
            if f < len(events):
                at = at + self.to_blender(ev["velocity"])
            if rm is not None:
                rm.location = to_rm @ at
                rm.keyframe_insert("location", frame=f)
            arm.o3d.anim_trigger = trigger_value(ev["trigger"])
            arm.keyframe_insert("o3d.anim_trigger", frame=f)
            if carry:
                bottom, top = ev["extents"] or (0.0, 0.0)
                arm.o3d.capsule_bottom = bottom
                arm.o3d.capsule_top = top
                arm.keyframe_insert("o3d.capsule_bottom", frame=f)
                arm.keyframe_insert("o3d.capsule_top", frame=f)
        return carry

    def ensure_rm(self, arm):
        """The rig's root-track bone, made on first need: a bone at the model
        origin, outside the BN## parts."""
        if rm_of(arm) is not None:
            return
        with self.editing(arm) as edit_bones:
            eb = edit_bones.new(RM_NAME)
            eb.head = Vector((0.0, 0.0, 0.0))
            eb.tail = Vector((0.0, 0.2, 0.0))

        def remove():
            with self.editing(arm) as edit_bones:
                eb = edit_bones.get(RM_NAME)
                if eb is not None:
                    edit_bones.remove(eb)
        self.undo.append(remove)

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
        # Every part's parent matrix before any part moves: a part keeps it as
        # its parent inverse, so its matrix is the same product it was.
        into_root = root.matrix_world.inverted_safe()
        chain = {i: into_root @ parts[i].parent.matrix_world for i in range(count)
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
            self.note("the table binds no clip (no anim_reset row naming a clip of the set): the game binds "
                      "each clip to its own first key, and the rig's rest pose stays as it is")
        self.pending = None
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
        else:
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
        rest = [pb.bone.matrix_local.copy() for pb in bones]

        self.ensure_rm(arm)
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
            action, slot = self.action_for(clip, arm, bones, rest)
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
