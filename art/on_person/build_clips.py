"""The person's third-person clips, keyed on the KINE control rig and baked
onto on_person through its sockets.

    blender art/on_player/on_player.blend -b --python art/on_person/build_clips.py -- <reference dir> [clip ...]
    blender art/on_person/work/on_person.blend -b --python art/on_person/build_clips.py -- <reference dir> [clip ...]

Every entry of clips.json names a source clip (src/source_index.json,
index_sources.py), the retail clip it stands in for, and how to fit it. The
source is retargeted onto `KINE Operator` (an Action with a KINE slot, so it
can be edited there), fitted to retail's clip (its ground speed for a loop,
its length for a one-shot; read from <reference dir> at build time,
retail_spec.py), and baked onto `on_person Rig` (the same Action's person
slot), which every row that played the retail clip then plays. A row no
entry fills is left out of the table: it plays the reset clip.

The scene is saved to work/on_person.blend (never tracked: the sources'
terms are not cleared for the repository).

The retarget: per KINE bone a source bone (or none), and a calibration that
turns KINE's rest onto the source's rest bone by bone (each bone aimed down
its source bone, the hands and feet framed by their knuckles and toes, a
bone's correction carried to its children). A frame's source turn from its
rest, applied to that calibrated KINE bone, is KINE's pose; a KINE bone the
source lacks between two it has takes the half-way turn, any other rides its
parent. The hips stand at the source's height in KINE's measure, over
retail's travel.
"""
import json
import math
import os
import sys

import bpy
from bpy_extras import anim_utils
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import retail_spec  # noqa: E402

WORK = os.path.join(HERE, "work", "on_person.blend")
FPS = 30
KINE = "KINE Operator"
MODEL = "on_person"
TABLE = "US01.adm"
# How far a loop may be sped up or slowed to meet retail's ground speed before
# its feet are left to slide.
RATE_MIN, RATE_MAX = 0.8, 1.35
# How far the hips may be raised or lowered to keep the lower foot where the
# source's stands (KINE units, cm).
LIFT = 8.0
# How far a loop's hips may be raised or lowered (KINE units, the legs
# re-solved under them) to stand its head at retail's height: the first-person
# eye is the head bone.
EYE_FIT = 20.0

FINGERS = [f"{f}_0{i}" for f in ("thumb", "index", "middle", "ring", "pinky") for i in (1, 2, 3)]
METACARPALS = [f"{f}_metacarpal" for f in ("index", "middle", "ring", "pinky")]


def _ue(spine, neck):
    m = {"pelvis": "pelvis", "head": "head"}
    m.update(spine)
    m.update(neck)
    for s in "lr":
        for b in ("clavicle", "upperarm", "lowerarm", "hand", "thigh", "calf", "foot", "ball") + tuple(FINGERS):
            m[f"{b}_{s}"] = f"{b}_{s}"
    return m


UE5 = _ue({f"spine_0{i}": f"spine_0{i}" for i in range(1, 6)}, {"neck_01": "neck_01", "neck_02": "neck_02"})
for _s in "lr":
    UE5.update({f"{m}_{_s}": f"{m}_{_s}" for m in METACARPALS})
UE4 = _ue({"spine_01": "spine_01", "spine_03": "spine_02", "spine_05": "spine_03"}, {"neck_01": "neck_01"})
MOTUS = {"pelvis": "Hips", "spine_01": "Spine", "spine_03": "Spine1", "spine_05": "Spine2", "neck_01": "Neck",
         "head": "Head"}
for _s, _S in (("l", "Left"), ("r", "Right")):
    MOTUS.update({f"clavicle_{_s}": f"{_S}Shoulder", f"upperarm_{_s}": f"{_S}Arm", f"lowerarm_{_s}": f"{_S}ForeArm",
                  f"hand_{_s}": f"{_S}Hand", f"thigh_{_s}": f"{_S}UpLeg", f"calf_{_s}": f"{_S}Leg",
                  f"foot_{_s}": f"{_S}Foot", f"ball_{_s}": f"{_S}ToeBase"})
    for _f in ("thumb", "index", "middle", "ring", "pinky"):
        for _i in (1, 2, 3):
            MOTUS[f"{_f}_0{_i}_{_s}"] = f"{_S}Hand{_f.capitalize()}{_i}"
KINDS = {"infima": UE5, "kine": UE4, "kubold": UE4, "motus": MOTUS}

# A KINE bone the source may lack, between two it has: the half-way turn.
BETWEEN = {"spine_02": ("spine_01", "spine_03"), "spine_04": ("spine_03", "spine_05"),
           "neck_02": ("neck_01", "head")}
# Each bone's aim: the KINE bone whose head it points at (the next one the
# source has is used where it lacks this one).
AIM = {"spine_01": "spine_02", "spine_02": "spine_03", "spine_03": "spine_04", "spine_04": "spine_05",
       "spine_05": "neck_01", "neck_01": "neck_02", "neck_02": "head"}
for _s in "lr":
    AIM.update({f"clavicle_{_s}": f"upperarm_{_s}", f"upperarm_{_s}": f"lowerarm_{_s}",
                f"lowerarm_{_s}": f"hand_{_s}", f"thigh_{_s}": f"calf_{_s}", f"calf_{_s}": f"foot_{_s}"})
    for _f in ("thumb", "index", "middle", "ring", "pinky"):
        AIM[f"{_f}_01_{_s}"] = f"{_f}_02_{_s}"
        AIM[f"{_f}_02_{_s}"] = f"{_f}_03_{_s}"
        if _f != "thumb":
            AIM[f"{_f}_metacarpal_{_s}"] = f"{_f}_01_{_s}"
# The arms a rifle hold replaces: everything below the clavicles.
ARM_ROOTS = ("clavicle_l", "clavicle_r")
# The bones a hold steadies toward its own (forward, level) head.
HEAD = ("neck_01", "neck_02", "head")
CHEST = "spine_05"
# The forearm twist bones the hand's roll is shared out to.
TWISTS = [(f"lowerarm_twist_0{i}_{s}", f"lowerarm_{s}", f"hand_{s}") for s in "lr" for i in (1, 2)]


def addon():
    """The OpenNova 3DI add-on: the installed one, else this checkout's
    (tools/blender), with ON3DI_CLI naming the opennova-3di it runs."""
    import addon_utils
    import importlib
    for name in ("bl_ext.user_default.opennova_3di", "opennova_3di"):
        try:
            module = importlib.import_module(name)
            break
        except ImportError:
            continue
    else:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "blender"))
        addon_utils.enable("opennova_3di", default_set=True)
        module = importlib.import_module("opennova_3di")
        name = "opennova_3di"
    if os.environ.get("ON3DI_CLI"):
        bpy.context.preferences.addons[module.__name__].preferences.cli_path = os.environ["ON3DI_CLI"]
    return module


def rot(m):
    return m.to_3x3().normalized()


def frame_of(aim, side):
    x = aim.normalized()
    y = (side - x * side.dot(x)).normalized()
    return Matrix((x, y, x.cross(y))).transposed()


def unscaled(m):
    loc, q, _ = m.decompose()
    return Matrix.Translation(loc) @ q.to_matrix().to_4x4()


# --- a source ----------------------------------------------------------------------

class Source:
    """A source clip in the scene: its armature playing its Action, removed on
    close()."""

    def __init__(self, key, index):
        row = index[key]
        self.pack = key.split("/")[0]
        self.map = KINDS[self.pack]
        scene = bpy.context.scene
        self.held = (scene.render.fps, scene.render.fps_base, scene.frame_start, scene.frame_end, scene.frame_current)
        before = {c: set(getattr(bpy.data, c)) for c in ("objects", "meshes", "armatures", "actions", "materials",
                                                          "images", "collections")}
        path = row["path"]
        if bpy.context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        if path.lower().endswith(".fbx"):
            bpy.ops.import_scene.fbx(filepath=path)
        else:
            with bpy.data.libraries.load(path) as (src, dst):
                dst.objects = [n for n in src.objects if n == "Armature"]
            for ob in dst.objects:
                scene.collection.objects.link(ob)
        self.made = {c: [d for d in getattr(bpy.data, c) if d not in before[c]] for c in before}
        self.arm = next(o for o in self.made["objects"] if o.type == "ARMATURE" and o.animation_data
                        and o.animation_data.action)
        self.start, self.end, self.fps = row["start"], row["end"], row["fps"]
        scene.render.fps, scene.render.fps_base = self.held[0], self.held[1]
        bpy.context.view_layer.update()
        self.map = {k: v for k, v in self.map.items() if v in self.arm.data.bones}

    def world(self, name):
        return self.arm.matrix_world @ self.arm.pose.bones[name].matrix

    def rest(self, name):
        return self.arm.matrix_world @ self.arm.data.bones[name].matrix_local

    def at(self, frame):
        # the source's own frame, at the scene's rate the importer set
        whole = math.floor(frame)
        bpy.context.scene.frame_set(int(whole), subframe=float(frame - whole))

    def close(self):
        scene = bpy.context.scene
        for collection in self.made:
            for data in self.made[collection]:
                try:
                    getattr(bpy.data, collection).remove(data)
                except ReferenceError:
                    pass
        scene.render.fps, scene.render.fps_base = self.held[0], self.held[1]
        scene.frame_start, scene.frame_end = self.held[2], self.held[3]
        scene.frame_set(self.held[4])


# --- the retarget ------------------------------------------------------------------

class Kine:
    """KINE Operator's rest: bone order (parents first), rest frames and heads
    in its armature space (cm, the ground at z = 0)."""

    def __init__(self, kine):
        self.ob = kine
        self.bones = [b for b in kine.data.bones if not b.name.startswith("PS ")]
        self.order = [b.name for b in self.bones]
        self.parent = {b.name: (b.parent.name if b.parent else None) for b in self.bones}
        self.rest = {b.name: b.matrix_local.copy() for b in self.bones}
        self.R = {n: rot(m) for n, m in self.rest.items()}
        self.P = {n: m.translation.copy() for n, m in self.rest.items()}
        self.offset = {n: (self.rest[p].inverted() @ self.rest[n]) if p else self.rest[n]
                       for n, p in self.parent.items()}


def calibrate(src, k):
    """Per KINE bone, A = C @ R_rest: its rest frame turned so that it stands
    as the source's bone does at the source's rest."""
    m = src.map
    S = {n: src.rest(s) for n, s in m.items()}
    C = {}
    for name in k.order:
        p = k.parent[name]
        cp = C.get(p, Matrix.Identity(3))
        side = name[-1]
        if name in m and name.startswith("hand_"):
            def hand_frame(head, mid, idx, pky):
                return frame_of(mid - head, pky - idx)
            ks = [k.P[f"{b}_01_{side}"] for b in ("middle", "index", "pinky")]
            ss = [S[f"{b}_01_{side}"].translation for b in ("middle", "index", "pinky")] \
                if all(f"{b}_01_{side}" in S for b in ("middle", "index", "pinky")) else None
            if ss is not None:
                fk = hand_frame(k.P[name], *ks)
                fs = hand_frame(S[name].translation, *ss)
                C[name] = fs @ fk.transposed()
                continue
        if name in m and name.startswith("foot_") and f"ball_{side}" in S:
            up = Vector((0.0, 0.0, 1.0))
            dk = k.P[f"ball_{side}"] - k.P[name]
            ds = S[f"ball_{side}"].translation - S[name].translation
            fk = frame_of(Vector((dk.x, dk.y, 0.0)), up)
            fs = frame_of(Vector((ds.x, ds.y, 0.0)), up)
            C[name] = fs @ fk.transposed()
            continue
        if name in m and name in AIM:
            target = AIM[name]
            while target is not None and target not in m:
                target = AIM.get(target)
            if target is not None:
                v = cp @ (k.P[target] - k.P[name])
                u = S[target].translation - S[name].translation
                if v.length > 1e-6 and u.length > 1e-9:
                    C[name] = v.rotation_difference(u).to_matrix() @ cp
                    continue
        C[name] = cp
    A = {n: C[n] @ k.R[n] for n in k.order}
    rest_s = {n: rot(S[n]) for n in m}
    return A, rest_s


def source_turns(src, k, A, rest_s):
    """The KINE bones' armature-space turns at the source's current frame."""
    D = {n: rot(src.world(s)) @ rest_s[n].transposed() for n, s in src.map.items()}
    T = {}
    # (the bones turned from the source, as against riding their parents:
    # absolute(src) names them)
    for name in k.order:
        p = k.parent[name]
        if name in D:
            T[name] = D[name] @ A[name]
        elif name in BETWEEN and all(e in D for e in BETWEEN[name]):
            a, c = BETWEEN[name]
            T[name] = D[a].to_quaternion().slerp(D[c].to_quaternion(), 0.5).to_matrix() @ A[name]
        elif p is not None:
            T[name] = T[p] @ k.R[p].transposed() @ k.R[name]
        else:
            T[name] = k.R[name].copy()
    for tw, fa, hd in TWISTS:
        if tw not in T:
            continue
        rel = (T[fa].transposed() @ T[hd]) @ (k.R[fa].transposed() @ k.R[hd]).transposed()
        axis = (k.R[fa].transposed() @ (k.P[hd] - k.P[fa])).normalized()
        q = rel.to_quaternion()
        angle = 2.0 * math.atan2(Vector((q.x, q.y, q.z)).dot(axis), q.w)
        if angle > math.pi:
            angle -= 2.0 * math.pi
        if angle < -math.pi:
            angle += 2.0 * math.pi
        length = (k.P[hd] - k.P[fa]).length
        share = min(max((k.P[tw] - k.P[fa]).dot((k.P[hd] - k.P[fa]).normalized()) / length, 0.0), 1.0)
        T[tw] = T[fa] @ Matrix.Rotation(angle * share, 3, axis) @ k.R[fa].transposed() @ k.R[tw]
    return T


def posed(k, T, hips):
    """Armature-space matrices from the turns, the hips at `hips`, every other
    head carried by its parent from its rest offset."""
    M = {}
    for name in k.order:
        p = k.parent[name]
        if name == "pelvis":
            at = hips
        elif p is None:
            at = k.P[name]
        else:
            at = (M[p] @ k.offset[name]).translation
        M[name] = Matrix.Translation(at) @ T[name].to_4x4()
    return M


def mirror_name(name):
    for a, b in (("_l", "_r"), ("_r", "_l")):
        if name.endswith(a):
            return name[:-2] + b
    return name


def mirrored(k, T):
    """The turns of the pose mirrored left for right (across KINE's
    middle, X = 0): each bone takes its counterpart's turn reflected,
    carried by the reflection between their rest frames."""
    S = Matrix.Diagonal((-1.0, 1.0, 1.0))
    out = {}
    for name in k.order:
        m = mirror_name(name)
        if m not in T or m not in k.R:
            out[name] = T[name]
            continue
        out[name] = S @ T[m] @ k.R[m].transposed() @ S @ k.R[name]
    return out


def hold_turns(key, frame, ctx):
    """A rifle hold's KINE turns: the source's pose at `frame`, cached."""
    cache = ctx.setdefault("holds", {})
    if (key, frame) not in cache:
        src = Source(key, ctx["index"])
        try:
            A, rest_s = calibrate(src, ctx["k"])
            src.at(frame)
            cache[(key, frame)] = source_turns(src, ctx["k"], A, rest_s)
        finally:
            src.close()
    return cache[(key, frame)]


def arm_bones(k):
    """The clavicles and every KINE bone below them."""
    out = set(ARM_ROOTS)
    for name in k.order:
        if k.parent[name] in out:
            out.add(name)
    return out


def layered(k, T, hold, arms, steady):
    """The turns with the arms the hold's, carried by the chest (each arm
    bone keeps its turn from the chest as the hold has it), and the neck and
    head steadied `steady` of the way toward the hold's."""
    T = dict(T)
    chest = T[CHEST] @ hold[CHEST].transposed()
    for name in k.order:
        if name in arms:
            T[name] = chest @ hold[name]
    if steady > 0.0:
        for name in HEAD:
            T[name] = T[name].to_quaternion().slerp(hold[name].to_quaternion(), steady).to_matrix()
    return T


def absolute(src):
    """The KINE bones whose turn comes from the source (the others ride)."""
    return set(src.map) | set(BETWEEN) | {t[0] for t in TWISTS}


def reach_up(k, M):
    """How far the hips may rise with each foot kept where M has it (KINE
    units): the legs' straightest reach, less half a centimetre."""
    out = math.inf
    for s in "lr":
        th, ca, ft = f"thigh_{s}", f"calf_{s}", f"foot_{s}"
        span = (k.P[ca] - k.P[th]).length + (k.P[ft] - k.P[ca]).length - 0.5
        d = M[ft].translation - M[th].translation
        flat = d.x * d.x + d.y * d.y
        if flat >= span * span:
            return 0.0
        out = min(out, math.sqrt(span * span - flat) + d.z)
    return out


def leg_ik(k, T, M, rise, fixed):
    """The turns with the hips `rise` higher (KINE units) and each foot kept
    where M has it: the thigh and calf re-aimed in the knee's own plane, the
    foot keeping its turn; the bones riding them follow."""
    T = dict(T)
    up = Vector((0.0, 0.0, rise))
    changed = set()
    for s in "lr":
        th, ca, ft = f"thigh_{s}", f"calf_{s}", f"foot_{s}"
        hip = M[th].translation + up
        knee0 = M[ca].translation + up
        ankle = M[ft].translation
        l1, l2 = (k.P[ca] - k.P[th]).length, (k.P[ft] - k.P[ca]).length
        to = ankle - hip
        d = min(max(to.length, abs(l1 - l2) + 0.01), l1 + l2 - 0.01)
        u = to.normalized()
        v = (knee0 - hip) - u * (knee0 - hip).dot(u)
        if v.length < 1e-6:
            continue
        v.normalize()
        a = math.acos(min(max((l1 * l1 + d * d - l2 * l2) / (2.0 * l1 * d), -1.0), 1.0))
        knee = hip + (u * math.cos(a) + v * math.sin(a)) * l1
        r1 = (knee0 - hip).rotation_difference(knee - hip).to_matrix()
        carried = r1 @ (ankle + up - knee0)
        r2 = carried.rotation_difference(hip + u * d - knee).to_matrix()
        T[th] = r1 @ T[th]
        T[ca] = r2 @ r1 @ T[ca]
        changed |= {th, ca}
    for name in k.order:
        p = k.parent[name]
        if p in changed and name not in fixed:
            T[name] = T[p] @ k.R[p].transposed() @ k.R[name]
            changed.add(name)
    return T


# --- fitting ------------------------------------------------------------------------

def own_speed(src, first, last, scale):
    """The source's ground speed (its units a second, times scale) and
    heading: the hips' pace over the planted foot, so a clip made in place
    measures as one made moving."""
    m = src.map
    feet = [m.get(f"ball_{s}") or m.get(f"foot_{s}") for s in "lr"]
    hips = m["pelvis"]
    n = max(8, int(round((last - first) * 2)))
    pts = []
    for i in range(n + 1):
        src.at(first + (last - first) * i / n)
        pts.append((src.world(hips).translation.copy(), [src.world(f).translation.copy() for f in feet]))
    lows = [min(p[1][j].z for p in pts) for j in range(2)]
    dt = (last - first) / src.fps / n
    vel = Vector((0.0, 0.0, 0.0))
    count = 0
    for i in range(n):
        j = 0 if pts[i][1][0].z <= pts[i][1][1].z else 1
        if pts[i][1][j].z > lows[j] + 0.03:  # the sources stand in metres
            continue
        hv = pts[i + 1][0] - pts[i][0]
        fv = pts[i + 1][1][j] - pts[i][1][j]
        vel += (hv - fv) / dt
        count += 1
    if count == 0:
        return 0.0, Vector((0.0, 0.0, 0.0))
    vel /= count
    vel.z = 0.0
    return vel.length * scale, vel.normalized() if vel.length > 1e-9 else vel


def footfalls(heights_l, heights_r, loop):
    """The frames each foot lands on: where it comes down to within 3.5 cm of
    its lowest after standing higher."""
    out = {}
    for marker, heights in (("FOOT_LEFT", heights_l), ("FOOT_RIGHT", heights_r)):
        hs = heights[:-1] if loop else heights
        low = min(hs)
        if max(hs) - low < 5.0:
            continue
        down = [h < low + 3.5 for h in hs]
        out[marker] = [f for f in range(len(hs)) if down[f] and not down[f - 1] and (loop or f > 0)]
    return out


# --- the scene ----------------------------------------------------------------------

def socket_pairs(kine, person):
    """(person bone, socket rest, KINE bone) for every person bone with a socket."""
    out = []
    for b in kine.data.bones:
        if b.name.startswith("PS ") and b.name[3:] in person.data.bones:
            out.append((b.name[3:], b.matrix_local.copy(), b.parent.name))
    return out


def key_slot(action, rig, frames, bones, with_location):
    """Key rig's slot of `action` from per-frame {bone: armature matrix}."""
    slot = next((s for s in action.slots if s.name_display == rig.name), None)
    if slot is not None:
        action.slots.remove(slot)
    slot = action.slots.new(id_type="OBJECT", name=rig.name)
    bag = anim_utils.action_ensure_channelbag_for_slot(action, slot)
    rest = {b.name: b.matrix_local for b in rig.data.bones}
    parent = {b.name: (b.parent.name if b.parent else None) for b in rig.data.bones}
    n = len(frames)
    linear = bpy.types.Keyframe.bl_rna.properties["interpolation"].enum_items["LINEAR"].value
    for name in bones:
        locs, rots = [], []
        p = parent[name]
        for fm in frames:
            if p is None:
                basis = rest[name].inverted() @ fm[name]
            else:
                basis = (rest[p].inverted() @ rest[name]).inverted() @ fm[p].inverted() @ fm[name]
            loc, q, _ = basis.decompose()
            loc = Vector([0.0 if abs(x) < 0.05 else x for x in loc])
            if rots and q.dot(rots[-1]) < 0.0:
                q.negate()
            locs.append(loc)
            rots.append(q)
        channels = [("rotation_quaternion", rots, 4)]
        if with_location(name):
            channels.append(("location", locs, 3))
        for prop, vals, size in channels:
            for i in range(size):
                fc = bag.fcurves.new(f'pose.bones["{name}"].{prop}', index=i, group_name=name)
                fc.keyframe_points.add(n)
                fc.keyframe_points.foreach_set("co", [v for f in range(n) for v in (float(f), vals[f][i])])
                fc.keyframe_points.foreach_set("interpolation", [linear] * n)
                fc.update()
    return slot


def build(name, entry, ctx):
    k, kine, person, a = ctx["k"], ctx["kine"], ctx["person"], ctx["addon"]
    spec = ctx["clips"][entry["retail"].lower()]
    hold, arms = None, None
    arms_key = entry.get("arms", ctx["arms"])
    if arms_key:
        hold = hold_turns(arms_key, ctx["index"][arms_key]["start"], ctx)
        arms = arm_bones(k)
    src = Source(entry["source"], ctx["index"])
    try:
        first, last = entry.get("range", (src.start, src.end))
        seconds = (last - first) / src.fps
        loop = spec["loop"]
        natural = max(2, round(seconds * FPS))
        A, rest_s = calibrate(src, k)
        hips_s = src.map["pelvis"]
        scale = k.P["pelvis"].z / src.rest(hips_s).translation.z  # KINE cm per source unit
        s_p = ctx["person_scale"]  # metres a KINE unit, on the person
        step = Vector(entry.get("step", (spec["step"][1], -spec["step"][0]))) / s_p  # retail: x forward, y left
        step = Vector((step.x, step.y, 0.0))
        speed = spec["speed"] if "step" not in entry else step.length * s_p * FPS
        report = ""
        if "frames" in entry:
            count = entry["frames"]
        elif loop and speed > 0.05:
            own, heading = own_speed(src, first, last, scale * s_p)
            rate = min(max(speed / own, entry.get("rate_min", RATE_MIN)), entry.get("rate_max", RATE_MAX)) \
                if own > 0.05 else 1.0
            count = max(4, round(natural / rate))
            report = (f"own {own:.2f} m/s, retail {speed:.2f}, rate x{rate:.2f}, slide "
                      f"{speed / (own * rate) - 1.0:+.0%}" if own > 0.05 else "source stands")
        elif not loop and entry.get("fit", "length") == "length":
            count = max(2, spec["frames"])
        else:
            count = max(2, round(natural / entry.get("rate", 1.0)))
        ankles = [src.map.get(f"foot_{s}") for s in "lr"]
        h0x = 0.0
        ankle_rest = [src.rest(n).translation.z for n in ankles]
        k_ankle = [k.P[f"foot_{s}"].z for s in "lr"]
        frames = []
        for i in range(count + 1):
            src.at(first + (last - first) * i / count)
            T = source_turns(src, k, A, rest_s)
            if hold is not None:
                T = layered(k, T, hold, arms, entry.get("steady", ctx["steady"]))
            h = src.world(hips_s).translation.copy()
            az = [src.world(n).translation.z for n in ankles]
            if entry.get("mirror"):
                T, h, az = mirrored(k, T), Vector((2.0 * h0x - h.x, h.y, h.z)) if i else h, az[::-1]
                if i == 0:
                    h0x = h.x
            frames.append((T, h, az))
        if loop:
            frames[-1] = frames[0]
        h0 = frames[0][1]
        drift_total = (frames[-1][1] - h0) * scale
        posed_k = []
        for f, (T, h, az) in enumerate(frames):
            travel = step * f
            if not loop and entry.get("own_travel"):
                d = (h - h0) * scale
                travel = Vector((d.x, d.y, 0.0))
            elif not loop and not entry.get("pinned"):
                # one-shot: the source's own drift, evened to retail's whole travel
                d = (h - h0) * scale
                travel = Vector((d.x, d.y, 0.0)) + (step * count - Vector((drift_total.x, drift_total.y, 0.0))) * f / count
            hips = Vector((travel.x, travel.y, h.z * scale))
            M = posed(k, T, hips)
            want = min(z / r * kz for z, r, kz in zip(az, ankle_rest, k_ankle))
            have = min(M[f"foot_{s}"].translation.z for s in "lr")
            lift = min(max(want - have, -LIFT), LIFT) if not entry.get("no_lift") else 0.0
            if lift:
                hips = hips + Vector((0.0, 0.0, lift))
                M = posed(k, T, hips)
            posed_k.append((T, hips, M))
        fixed = absolute(src)
    finally:
        src.close()
    # the head at retail's height: the hips raised or lowered over planted
    # feet, so the first-person eye (the head bone) stands where retail's does
    head_sock = ctx["sockets"][ctx["head"]]
    ground = ctx["ground"]

    def head_heights(posed_list):
        return [((M["head"] @ k.rest["head"].inverted() @ head_sock[0]).translation.z - ground) * s_p
                for _, _, M in posed_list]

    heads = head_heights(posed_k)
    rise = 0.0
    if entry.get("eye", loop and "frames" not in entry):
        want = (spec["top"][0] + spec["top"][1]) / 2.0
        have = sum(heads) / len(heads)
        rise = min(max((want - have) / s_p, -EYE_FIT), EYE_FIT, min(reach_up(k, M) for _, _, M in posed_k))
        if abs(rise) > 0.2:
            posed_k = [(T2, hips + Vector((0.0, 0.0, rise)), None) for T2, hips, _ in
                       [(leg_ik(k, T, M, rise, fixed), hips, M) for T, hips, M in posed_k]]
            posed_k = [(T, hips, posed(k, T, hips)) for T, hips, _ in posed_k]
            heads = head_heights(posed_k)
        else:
            rise = 0.0
    posed_k = [M for _, _, M in posed_k]
    # the Action: KINE's slot, then the person's from it through the sockets
    action = bpy.data.actions.get(f"P_{name}")
    if action is None:
        action = bpy.data.actions.new(f"P_{name}")
    action.use_fake_user = True
    twists = {t[0] for t in TWISTS}
    keyed = ["pelvis"] + [n for n in k.order if n != "pelvis" and (n in src.map or n in BETWEEN or n in twists)]
    key_slot(action, kine, posed_k, keyed, lambda n: n == "pelvis")
    pframes = []
    for M in posed_k:
        P = {}
        for b in person.data.bones:
            sock = ctx["sockets"].get(b.name)
            if sock is not None:
                socket_rest, kb = sock
                P[b.name] = unscaled(M[kb] @ k.rest[kb].inverted() @ socket_rest)
            elif b.parent is None:
                P[b.name] = b.matrix_local.copy()
            else:
                P[b.name] = P[b.parent.name] @ b.parent.matrix_local.inverted() @ b.matrix_local
        pframes.append(P)
    key_slot(action, person, pframes, [b.name for b in person.data.bones], lambda n: True)
    action.frame_range = (0, count)
    action.use_frame_range = True
    action.use_cyclic = loop
    action.o3d.fps = FPS
    action.o3d.raw_flag_8 = spec["bit3"]
    # event triggers: footsteps where a foot lands (when retail's clip has
    # them), the others at the same share of the clip
    for mk in list(action.pose_markers):
        action.pose_markers.remove(mk)
    names = ctx["triggers"]
    if spec["triggers"].get(0x1) or spec["triggers"].get(0x2):
        hl = [M["foot_l"].translation.z for M in posed_k]
        hr = [M["foot_r"].translation.z for M in posed_k]
        for marker, at in footfalls(hl, hr, loop).items():
            for f in at:
                action.pose_markers.new(marker).frame = f
    for mask, at in spec["triggers"].items():
        if mask in (0x1, 0x2) or mask not in names:
            continue
        for f in at:
            action.pose_markers.new(names[mask]).frame = min(count, round(f * count / max(1, spec["frames"])))
    bottoms = [(M["pelvis"].translation.z - ground) * s_p for M in posed_k]
    print(f"on_person: {name}: {entry['source']} -> {spec['name']}: {count} frames "
          f"{'loop' if loop else 'one-shot'}; hips {min(bottoms):.2f}..{max(bottoms):.2f} m "
          f"(retail {spec['bottom'][0]:.2f}..{spec['bottom'][1]:.2f}), head {min(heads):.2f}..{max(heads):.2f} "
          f"(retail {spec['top'][0]:.2f}..{spec['top'][1]:.2f}{f', hips {rise * s_p:+.2f}' if rise else ''}) {report}")
    return action


def rows_for(entry, rows):
    stem = entry["retail"].lower()
    out = [key for key, variants in rows if any(v.lower() == stem for v in variants)]
    return out + [r for r in entry.get("also", []) if r not in out]


def run(reference, names=()):
    a = addon()
    kine = bpy.data.objects[KINE]
    model = bpy.data.objects[MODEL]
    person = a.rig.rig_of(model)
    with open(os.path.join(HERE, "clips.json"), encoding="utf-8") as f:
        entries = json.load(f)
    with open(os.path.join(HERE, "src", "source_index.json"), encoding="utf-8") as f:
        index = json.load(f)
    rows, clips = retail_spec.spec(a.o3dtext.cli_path(), reference)
    bpy.context.scene.render.fps, bpy.context.scene.render.fps_base = FPS, 1.0
    for pb in kine.pose.bones:
        pb.rotation_mode = "QUATERNION"
        pb.matrix_basis = Matrix.Identity(4)
    for pb in person.pose.bones:
        pb.rotation_mode = "QUATERNION"
        for c in pb.constraints:
            c.enabled = False
    ctx = {
        "k": Kine(kine), "kine": kine, "person": person, "addon": a, "index": index, "clips": clips,
        "arms": entries.get("_arms", {}).get("source"), "steady": entries.get("_arms", {}).get("steady", 0.0),
        "sockets": {p: (r, kb) for p, r, kb in socket_pairs(kine, person)},
        "person_scale": person.matrix_world.to_scale()[0],
        "head": next(b.name for b in person.data.bones if b.name.startswith("BN15")),
        "ground": next(b.head_local.z for b in person.data.bones if b.name.lower() == "root"),
        "triggers": {mask: name for mask, name in a.catalog(retry=True).triggers},
    }
    built = {}
    for name, entry in entries.items():
        if name.startswith("_") or (names and name not in names):
            continue
        built[name] = build(name, entry, ctx)
    # the table: every row that played a retail clip an entry stands in for
    have = {r.key: r for r in model.o3d.rows}
    for name, entry in entries.items():
        if name.startswith("_"):
            continue
        action = built.get(name) or bpy.data.actions.get(f"P_{name}")
        if action is None or entry.get("table") is False:
            continue
        for key in rows_for(entry, rows):
            r = have.get(key)
            if r is None:
                r = model.o3d.rows.add()
                r.key = key
                have[key] = r
            while len(r.variants):
                r.variants.remove(0)
            r.variants.add().action = action
    order = [key for key, _ in rows]
    print(f"on_person: {len(built)} clips built; the table fills {len(have)} of retail's {len(order)} rows")
    for ob, act in ((kine, None), (person, None)):
        if ob.animation_data:
            ob.animation_data.action = act
    return built


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        raise SystemExit("name the folder holding retail's loose US01.adm and its clips")
    run(args[0], args[1:])
    os.makedirs(os.path.dirname(WORK), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=WORK, compress=True, relative_remap=True)
    print("SAVED", WORK)
