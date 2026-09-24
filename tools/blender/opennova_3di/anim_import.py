# opennova-3di anim scene -> .o3a -> Blender Actions on a model's rig.
#
# The inverse of animation.py: every clip becomes one Action on its own NLA
# track, keyed on the rig's BN## bones, and the table becomes the model root's
# rows. Nothing is stashed so that a re-export reproduces the source: the bind,
# the bone positions, the capsule extents and the terminal duplicate key are the
# engine's derivations, and what the scene form cannot carry is reported.
#
# The rig's rest pose is the bind. A rig imported from a `.3di` carries only
# pivots, so its bones point wherever the model importer put them; this importer
# turns each rest bone onto the reset clip's first key, which is the bind the
# runtime composes against [orig: AnimChannel_ComputeBoneMatrices @0x410da0].
# The heads, the lengths and the weights do not move, so the model exports the
# same bytes, and a clip then shows in Blender the pose the game draws.

import os
import subprocess
import tempfile

import bpy
from mathutils import Matrix, Quaternion, Vector

from .animation import (ANIM_FLAG_BIT3, ANIM_FLAG_LOOP, ANIM_FLAG_TRANSLATION, RM_NAME, bone_rows,
                        rig_of, rm_of)
from .export import axis_basis, axis_map, clean_name


class ImportFailed(Exception):
    pass


def tokens(line):
    """The records' fields: bare tokens and "quoted names"."""
    out, i = [], 0
    while i < len(line):
        if line[i] in " \t":
            i += 1
            continue
        if line[i] == '"':
            end = line.find('"', i + 1)
            end = len(line) if end < 0 else end
            out.append(line[i + 1:end])
            i = end + 1
            continue
        end = i
        while end < len(line) and line[end] not in " \t":
            end += 1
        out.append(line[i:end])
        i = end
    return out


def strip_comment(line):
    quote = False
    for i, c in enumerate(line):
        if c == '"':
            quote = not quote
        if not quote and c == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i]
    return line


def read_o3a(path):
    """The .o3a clip set: its table rows and every clip's bones, keys,
    translations and events."""
    set_ = {"adm": "", "rows": [], "clips": []}
    clip = None
    bone = None
    with open(path, "r") as f:
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
                clip["capsule"] = (float(parts[1]), float(parts[2]))
            elif key == "bone":
                bone = {"parent": int(parts[1]), "pivot": tuple(float(x) for x in parts[2:5]),
                        "length": float(parts[5]), "name": parts[6] if len(parts) > 6 else "",
                        "keys": [], "durations": [], "tr": [], "position": None}
                clip["bones"].append(bone)
            elif bone is None:
                continue
            elif key == "k":
                bone["keys"].append(tuple(float(x) for x in parts[1:5]))
                bone["durations"].append(int(parts[5]) if len(parts) > 5 else 1)
            elif key == "tr":
                bone["tr"].append(tuple(float(x) for x in parts[1:4]))
            elif key == "bonepos":
                bone["position"] = tuple(float(x) for x in parts[1:4])
            elif key == "event":
                clip["events"].append({
                    "velocity": tuple(float(x) for x in parts[1:4]),
                    "trigger": int(parts[4], 0),
                    "extents": (float(parts[5]), float(parts[6])) if len(parts) > 6 else None,
                })
    return set_


def run_scene(context, path):
    """opennova-3di anim scene <path> -> a temporary .o3a, and its notes."""
    from . import cli_path
    cli = cli_path(context)
    if not os.path.isfile(cli):
        raise ImportFailed(f"opennova-3di not found at {cli}")
    tmp = tempfile.mkdtemp(prefix="opennova3di_")
    o3a = os.path.join(tmp, "set.o3a")
    result = subprocess.run([cli, "anim", "scene", path, "-o", o3a], capture_output=True, text=True)
    if result.returncode != 0:
        raise ImportFailed((result.stderr or result.stdout).strip()[:2000])
    notes = [l.split("scene drops ", 1)[1] for l in result.stderr.splitlines() if "scene drops " in l]
    return o3a, notes


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


class Loader:
    def __init__(self, context, model, set_, op=None):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.set = set_
        self.op = op
        self.notes = []
        self.basis = axis_basis(self.scene.o3d.forward)
        self.to_blender = lambda m: self.basis @ Vector(m)

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    def mission_rot(self, rotation):
        """A Blender rotation (3x3, in the model root's frame) as a mission-axes
        quaternion: what animation.AnimExporter reads back."""
        return (self.basis.transposed() @ rotation @ self.basis).to_quaternion()

    def blender_rot(self, quat):
        """A mission-axes key as a Blender rotation in the model root's frame."""
        m = Quaternion((quat[3], quat[0], quat[1], quat[2])).to_matrix()
        return self.basis @ m @ self.basis.transposed()

    # --- the rig ------------------------------------------------------------
    def reset_clip(self):
        """The clip the table's reset row names, which carries the bind; else
        the first clip."""
        for key, variants in self.set["rows"]:
            if "reset" in key.lower() and variants:
                stem = os.path.splitext(variants[-1])[0].lower()
                for clip in self.set["clips"]:
                    if clip["name"].lower() == stem:
                        return clip
        return self.set["clips"][0] if self.set["clips"] else None

    def name_bones(self, arm, bones, clip):
        """The bone names the clip carries. A model's part table has none, so a
        rig imported from a `.3di` calls its bones BN## alone; the clip labels
        them (BN16 L Hand), and the vertex groups follow."""
        meshes = [ob for ob in bpy.data.objects
                  if ob.type == "MESH" and ob.find_armature() == arm]
        for i, pb in enumerate(bones):
            if i >= len(clip["bones"]):
                break
            name = clip["bones"][i]["name"]
            if not name or name == pb.name:
                continue
            old = pb.name
            bone = arm.data.bones.get(old)
            if bone is None:
                continue
            bone.name = name
            for ob in meshes:
                group = ob.vertex_groups.get(old)
                if group is not None:
                    group.name = name
        return bone_rows(arm)

    def align_rest(self, arm, bones, clip):
        """Turn each rest bone onto the reset clip's first key: the rig's rest
        pose is the bind. Heads and lengths keep their places."""
        rows = clip["bones"]
        vl = self.scene.view_layers[0]
        held = self.context.view_layer.objects.active
        with self.context.temp_override(scene=self.scene, view_layer=vl, active_object=arm,
                                        object=arm, selected_objects=[arm]):
            vl.objects.active = arm
            bpy.ops.object.mode_set(mode="EDIT")
            arm_rot = arm.matrix_world.to_3x3().normalized()
            for i, pb in enumerate(bones):
                if i >= len(rows):
                    break
                eb = arm.data.edit_bones.get(pb.name)
                if eb is None:
                    continue
                # The key is the bone's rotation in the model's frame; the rest
                # bone's own space is the armature's.
                m = arm_rot.inverted() @ self.blender_rot(rows[i]["keys"][0])
                length = eb.length if eb.length > 1e-6 else 0.05
                eb.tail = eb.head + m @ Vector((0.0, length, 0.0))
                eb.align_roll(m @ Vector((0.0, 0.0, 1.0)))
            bpy.ops.object.mode_set(mode="OBJECT")
        if held is not None:
            vl.objects.active = held

    # --- the clips ----------------------------------------------------------
    def action_for(self, clip, arm, bones, rest):
        name = clip["name"]
        action = bpy.data.actions.new(name)
        data = arm.animation_data or arm.animation_data_create()
        held = (data.action, data.use_nla)
        data.use_nla = False
        data.action = action
        action.o3d.fps = float(clip["fps"])
        action.o3d.loop = bool(clip["flags"] & ANIM_FLAG_LOOP)
        action.o3d.translation = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        action.o3d.raw_flag_8 = bool(clip["flags"] & ANIM_FLAG_BIT3)
        action.o3d.frames = clip["frames"]
        rows = clip["bones"]
        if len(rows) > len(bones):
            self.note(f"{name}: the clip carries {len(rows)} bones, the rig {len(bones)}; the extra "
                      "channels are dropped")
        samples = max((len(b["keys"]) for b in rows), default=0)
        translated = bool(clip["flags"] & ANIM_FLAG_TRANSLATION)
        sparse = [i for i, b in enumerate(rows) if len(b["keys"]) != clip["frames"] + 1]
        if sparse:
            self.note(f"{name}: bones {sparse[:6]} key fewer times than the clip's frame count; "
                      "Blender keys every frame, so a re-export densifies them")
        arm_rot = arm.matrix_world.to_3x3().normalized()
        order = {pb.name: i for i, pb in enumerate(bones)}
        # The bind every key is measured against is the RIG's rest pose, one
        # bind for the whole set, as the runtime pins the reset clip's records
        # once per entity [orig: AnimMap_RegisterEntity @0x40bb60].
        bind = [self.mission_rot(arm_rot @ m.to_3x3().normalized()) for m in rest]
        # The pose that shows what the game draws: the key measured against the
        # bind, then the rest kinematics for the head.
        for f in range(samples):
            world = []
            for i, pb in enumerate(bones):
                if i >= len(rows):
                    world.append(arm.matrix_world @ rest[i])
                    continue
                keys = rows[i]["keys"]
                key = keys[min(f, len(keys) - 1)]
                pose = bind[i].inverted() @ Quaternion((key[3], key[0], key[1], key[2])) @ bind[i]
                rot = arm_rot.inverted() @ (self.basis @ pose.to_matrix() @ self.basis.transposed())
                parent = pb.parent
                if parent is None:
                    head = (arm.matrix_world @ rest[i]).translation
                else:
                    j = order[parent.name]
                    follow = (arm.matrix_world @ rest[j]).inverted() @ (arm.matrix_world @ rest[i])
                    head = (world[j] @ follow).translation
                if translated and rows[i]["tr"]:
                    tr = rows[i]["tr"][min(f, len(rows[i]["tr"]) - 1)]
                    head = head + self.to_blender(tr)
                world.append(Matrix.Translation(head) @ (arm.matrix_world.to_3x3() @ rot).to_4x4())
            self.write_frame(arm, bones, rest, world, f, translated)
        action.o3d.capsule_keys = bool(self.write_events(arm, clip, samples))
        linear(action)
        data.action, data.use_nla = held
        return action

    def write_frame(self, arm, bones, rest, world, frame, translated):
        """One frame's pose, keyed on the rig's bone channels."""
        inverse = arm.matrix_world.inverted()
        order = {pb.name: i for i, pb in enumerate(bones)}
        for i, pb in enumerate(bones):
            local = inverse @ world[i]
            parent = pb.parent
            if parent is None:
                basis = rest[i].inverted() @ local
            else:
                j = order[parent.name]
                follow = rest[j].inverted() @ rest[i]
                basis = (inverse @ world[j] @ follow).inverted() @ local
            pb.rotation_mode = "QUATERNION"
            pb.rotation_quaternion = basis.to_quaternion()
            pb.keyframe_insert("rotation_quaternion", frame=frame)
            if translated:
                pb.location = basis.translation
                pb.keyframe_insert("location", frame=frame)

    def write_events(self, arm, clip, samples):
        """The root track and the trigger word, on the clip's own Action: the
        `!RM` bone walks the summed event steps, and the rig carries the keyed
        trigger word."""
        events = clip["events"]
        if not events:
            self.note(f"{clip['name']}: the clip carries no event record (no root motion, no capsule)")
            return
        rm = rm_of(arm)
        rest = rm.bone.matrix_local.to_3x3().normalized().inverted() if rm is not None else None
        at = Vector((0.0, 0.0, 0.0))
        # The extents the clip carries are keyed beside the trigger: the rule
        # retail's own tool measured them by is not witnessed, and the engine's
        # derivation from the pose lands within centimetres of it.
        carry = any(ev["extents"] is not None for ev in events)
        for f in range(samples):
            ev = events[min(f, len(events) - 1)]
            at = at + self.to_blender(ev["velocity"])
            if rm is not None:
                rm.location = rest @ at
                rm.keyframe_insert("location", frame=f)
            arm.o3d.anim_trigger = ev["trigger"]
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
        vl = self.scene.view_layers[0]
        with self.context.temp_override(scene=self.scene, view_layer=vl, active_object=arm,
                                        object=arm, selected_objects=[arm]):
            vl.objects.active = arm
            bpy.ops.object.mode_set(mode="EDIT")
            eb = arm.data.edit_bones.new(RM_NAME)
            eb.head = Vector((0.0, 0.0, 0.0))
            eb.tail = Vector((0.0, 0.2, 0.0))
            bpy.ops.object.mode_set(mode="OBJECT")

    # --- the run ------------------------------------------------------------
    def run(self):
        arm = rig_of(self.model)
        if arm is None:
            raise ImportFailed(f"{self.model.name}: animations need a rig (an Armature of BN## bones "
                               "under its LOD 0 root)")
        bones = bone_rows(arm)
        clips = self.set["clips"]
        if not clips:
            raise ImportFailed("the clip set holds no clip")
        reset = self.reset_clip()
        if reset is not None:
            bones = self.name_bones(arm, bones, reset)
        if reset is not None and (self.op is None or self.op.align_rest):
            self.align_rest(arm, bones, reset)
            self.context.view_layer.update()
        rest = [pb.bone.matrix_local.copy() for pb in bones]

        self.ensure_rm(arm)
        data = arm.animation_data or arm.animation_data_create()
        for clip in clips:
            action = self.action_for(clip, arm, bones, rest)
            track = data.nla_tracks.new()
            track.name = clip["name"]
            track.strips.new(clip["name"], 0, action)

        # The table: its rows in file order, each variant an Action.
        props = self.model.o3d
        props.rows.clear()
        if self.set["adm"]:
            props.adm_path = f"//{self.set['adm']}"
        for key, variants in self.set["rows"]:
            row = props.rows.add()
            row.key = key
            for variant in variants:
                stem = os.path.splitext(variant)[0]
                action = next((a for a in bpy.data.actions if a.name.lower() == stem.lower()), None)
                if action is None:
                    self.note(f"the row '{key}' names '{variant}', which the set does not hold")
                    continue
                row.variants.add().action = action
        return f"{len(clips)} clips on {arm.name}", self.notes


def import_file(context, path, model=None, op=None):
    """Read a .adm table (or a lone .bad) onto a model's rig."""
    o3a, notes = run_scene(context, path)
    set_ = read_o3a(o3a)
    model = model or active_model(context)
    if model is None:
        raise ImportFailed("select an object of the model whose rig these clips animate")
    loader = Loader(context, model, set_, op)
    message, own = loader.run()
    return message, notes + own


def active_model(context):
    from .export import model_of
    ob = context.active_object
    return model_of(ob) if ob is not None else None
