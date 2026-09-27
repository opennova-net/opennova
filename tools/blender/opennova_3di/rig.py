# A model's parts: the one reading of a scene the export, the imports, the
# animation code and the panels share (docs/threedi/scene-naming-contract.md).
#
# A model root is an Empty whose children are its LOD roots; it is one .3di. A
# LOD root is an Empty with the custom property `_lod_index` (0 = the primary
# LOD). A LOD's parts come from one of two places:
#
#   PN## empties  a static model (a prop, a building, a vehicle whose parts
#                 turn by their PANM tracks): `PN##` is part ## (1-based in the
#                 name, 0-based here), its origin the pivot and the nearest
#                 PN## above it the parent (none: part 0, as retail stores a
#                 top part's parent and the root's own).
#   the rig       an animated or skinned model (a first-person gun, arms, a
#                 person): ONE Armature under LOD 0's root, whose bones named
#                 `BN##` (a label may follow: `BN16 L Hand`) are the parts:
#                 ## the part number, the bone head the pivot, the nearest BN##
#                 bone above it the parent (none: part 0). Any other bone (the
#                 `Root` on the ground, a control bone) is no part. Every LOD of
#                 the model reads its parts from that one rig: a later LOD holds
#                 meshes deforming with it, or PN## empties of its own.
#
# A first-person gun and its arms share the gun's rig: the arms model holds no
# armature, its meshes deform with the gun's rig through their Armature
# modifier, and its parts are the rig's bones 01..K, K the highest bone its
# weights use, since retail draws the arms with the gun's part matrices,
# paired by index [orig: Player_RenderFirstPersonViewModel @ 0x4DED60, the
# arms submit @ 0x4DF088]. The arms model root stands where the gun's does. A
# skinned model may also keep its geometry on a part of its own after the
# bones (its root's Mesh part setting, the retail layout of US01 and ArmsG),
# whose pivot is its skinned mesh's origin.
#
# An object belongs to a part (part_of): the part bone it is parented to, the
# root part when its Armature modifier deforms it (a skinned mesh), else the
# nearest PN## at or above it. Blender's `.001` duplicate suffixes are
# stripped before a name is read, and an object named `!...` is ignored.

import contextlib
import re

import bpy
from mathutils import Matrix, Vector

from .o3dtext import ExportError

BLENDER_SUFFIX = re.compile(r"\.\d{3,}$")
PART_RE = re.compile(r"^PN(\d{2})$")
BONE_RE = re.compile(r"^BN(\d{2})(?: (.*))?$")
# A vertex weight below this moves nothing (contract C1: a vertex needs one
# weight at or above it).
WEIGHT_EPS = 1e-4


def clean_name(name):
    """A name without Blender's `.001` duplicate suffix."""
    return BLENDER_SUFFIX.sub("", name)


def ignored(ob):
    """An object named `!...`, which no model reads."""
    return clean_name(ob.name).startswith("!")


# --- models and LODs --------------------------------------------------------

def is_lod_root(ob):
    return ob.type == "EMPTY" and "_lod_index" in ob


def lod_index(root):
    return int(root["_lod_index"])


def is_model_root(ob):
    """A model root: the Empty whose children are a model's LOD roots."""
    return ob.type == "EMPTY" and any(is_lod_root(c) for c in ob.children)


def model_roots(scene):
    return sorted((o for o in scene.objects if is_model_root(o)), key=lambda o: o.name)


def model_of(ob):
    """The model an object belongs to: the nearest model root at or above it."""
    while ob is not None:
        if is_model_root(ob):
            return ob
        ob = ob.parent
    return None


def lod_of(ob):
    """The LOD root at or above an object, within its model; None outside a LOD."""
    while ob is not None and not is_model_root(ob):
        if is_lod_root(ob):
            return ob
        ob = ob.parent
    return None


def active_model(context):
    """The model the active object belongs to, else the scene's only model."""
    model = model_of(context.object) if context.object is not None else None
    if model is None:
        roots = model_roots(context.scene)
        model = roots[0] if len(roots) == 1 else None
    return model


def descendants(ob):
    """Everything below an object, but not into another model parented there
    (that model is its own .3di)."""
    for child in ob.children:
        if is_model_root(child):
            continue
        yield child
        yield from descendants(child)


def lod_roots(model):
    """The model's LOD roots in LOD order; an ExportError when two share an
    index, the indices leave a gap or there is no LOD 0."""
    by_index = {}
    for ob in model.children:
        if not is_lod_root(ob):
            continue
        i = lod_index(ob)
        if i in by_index:
            raise ExportError(f"{model.name}: two LOD roots carry _lod_index {i}: {by_index[i].name}, {ob.name}")
        by_index[i] = ob
    order = sorted(by_index)
    if not order or order[0] != 0:
        raise ExportError(f"{model.name}: no LOD 0 root: add an Empty with the custom property _lod_index = 0 "
                          "under the model root (Add LOD)")
    if order != list(range(len(order))):
        raise ExportError(f"{model.name}: _lod_index values are not contiguous from 0: {order}")
    return [by_index[i] for i in order]


# --- the rig ----------------------------------------------------------------

def is_root_bone(name):
    """A rig's `Root` bone, in any case: the ground under a character, no part
    (the animation export stands the clips on it)."""
    return clean_name(name).lower() == "root"


def bone_part(bone):
    """The part index a BN## bone is, else None."""
    m = BONE_RE.match(clean_name(bone.name))
    return int(m.group(1)) - 1 if m else None


def parent_part(bone):
    """A part bone's parent part: the nearest BN## bone above it, past any bone
    that is no part; None at the top."""
    above = bone.parent
    while above is not None:
        index = bone_part(above)
        if index is not None:
            return index
        above = above.parent
    return None


def part_bones(rig):
    """{part index: Bone} of a rig's BN## bones; an ExportError when two bones
    are one part (Number Parts renumbers them) or one is BN00."""
    out = {}
    for bone in rig.data.bones:
        index = bone_part(bone)
        if index is None:
            continue
        if index < 0:
            raise ExportError(f"{rig.name}: bone '{bone.name}': parts start at BN01")
        if index in out:
            raise ExportError(f"{rig.name}: bones '{out[index].name}' and '{bone.name}' are both part "
                              f"{index + 1:02d} (Number Parts numbers them in hierarchy order)")
        out[index] = bone
    return out


def skin_rig(ob):
    """The armature an object's Armature modifier deforms it with, else None."""
    if ob.type != "MESH":
        return None
    return next((m.object for m in ob.modifiers if m.type == "ARMATURE" and m.object is not None), None)


def own_rig(root):
    """The armature under a LOD root (LOD 0's is its model's rig), else None;
    an ExportError for two."""
    rigs = [ob for ob in descendants(root) if ob.type == "ARMATURE" and not ignored(ob)]
    if len(rigs) > 1:
        raise ExportError(f"{root.name}: two armatures ({rigs[0].name}, {rigs[1].name}); a model has one rig")
    return rigs[0] if rigs else None


def rig_of(model):
    """The model's rig: the armature under its LOD 0 root, else the one its LOD
    0 meshes deform with (a first-person gun's, for its arms); None for a
    static model. It never raises: the panels ask on every redraw."""
    root = next((c for c in model.children if is_lod_root(c) and lod_index(c) == 0), None)
    if root is None:
        return None
    rigs = [ob for ob in descendants(root) if ob.type == "ARMATURE" and not ignored(ob)]
    if rigs:
        return rigs[0]
    return next((r for ob in descendants(root) if not ignored(ob) for r in [skin_rig(ob)] if r is not None), None)


def weighted_parts(meshes, bones):
    """The parts the meshes' weights use: their vertex groups named after a
    part bone ({part index: Bone}) that hold a weight of WEIGHT_EPS or more."""
    names = {b.name: i for i, b in bones.items()}
    used = set()
    for ob in meshes:
        groups = {g.index: names[g.name] for g in ob.vertex_groups if g.name in names}
        if not groups:
            continue
        for v in ob.data.vertices:
            for g in v.groups:
                if g.weight >= WEIGHT_EPS and g.group in groups:
                    used.add(groups[g.group])
    return used


# --- parts ------------------------------------------------------------------

def stored_parent(holder, index, above):
    """A part's parent: the hierarchy's (`above`), unless its Parent setting
    (a PN##'s or a bone's) stores what a hierarchy cannot: the part itself, or
    no part (-1), as retail models store them (Eturret's turret names itself,
    Excavatr's arm and Chair03X's pieces -1) and the runtime poses them apart
    from part 0."""
    mode = holder.o3d.part_parent
    if mode == "SELF":
        return index
    if mode == "NONE":
        return -1
    return above


class Part:
    """One part of a LOD: `index` and its `parent` part's (0-based), and what
    holds it: a PN## `empty`, a BN## `bone` of `rig`, or a skinned `mesh`
    (the mesh part, whose pivot is the mesh's origin)."""

    def __init__(self, index, parent, empty=None, bone=None, rig=None, mesh=None):
        self.index = index
        self.parent = parent
        self.empty = empty
        self.bone = bone
        self.rig = rig
        self.mesh = mesh

    @property
    def name(self):
        return (self.empty or self.bone or self.mesh).name

    def matrix_world(self):
        """The part's frame in the world, at rest: its origin is the pivot. A
        bone's is its rest matrix (its head the pivot), a mesh part's its
        mesh's origin with no turn."""
        if self.empty is not None:
            return self.empty.matrix_world
        if self.bone is not None:
            return self.rig.matrix_world @ self.bone.matrix_local
        return Matrix.Translation(self.mesh.matrix_world.translation)


class LodParts:
    """A LOD's parts (`parts`, in part order) and where they come from: `rig`
    (None for PN## parts), `shared` when the rig is another model's (a
    first-person gun's, for its arms), `skinned` (the LOD's meshes deforming
    with the rig) and `mesh_part` (the index of the part its skinned geometry
    is authored on when the model keeps one, else None)."""

    def __init__(self, root):
        self.root = root
        self.index = lod_index(root)
        self.model = model_of(root)
        self.rig = None
        self.shared = False
        self.parts = []
        self.skinned = []
        self.mesh_part = None


def lod_parts(root):
    """Read a LOD root's parts (LodParts); an ExportError when they cannot be
    read: two armatures, a rig under a later LOD, PN## empties beside a rig or
    beside skinned meshes, meshes deforming with two rigs, or a part number
    missing or given twice."""
    lod = LodParts(root)
    model = lod.model
    empties = {}
    rig = own_rig(root)
    for ob in descendants(root):
        if ignored(ob):
            continue
        if ob.type == "EMPTY":
            m = PART_RE.match(clean_name(ob.name))
            if m:
                index = int(m.group(1)) - 1
                if index < 0:
                    raise ExportError(f"{ob.name}: parts start at PN01")
                if index in empties:
                    raise ExportError(f"{root.name}: '{empties[index].name}' and '{ob.name}' are both part "
                                      f"{index + 1:02d}")
                empties[index] = ob
        elif skin_rig(ob) is not None:
            lod.skinned.append(ob)
    lod.skinned.sort(key=lambda ob: ob.name)
    if rig is not None and lod.index > 0:
        raise ExportError(f"{rig.name}: a model's rig lives under its LOD 0 root; a later LOD's meshes deform "
                          "with that rig")
    if empties and (rig is not None or lod.skinned):
        what = rig.name if rig is not None else lod.skinned[0].name
        raise ExportError(f"{root.name}: PN## parts beside a rig ({what}): a LOD's parts are PN## empties or its "
                          "rig's BN## bones")
    if empties:
        count = max(empties) + 1
        missing = [i + 1 for i in range(count) if i not in empties]
        if missing:
            raise ExportError(f"{root.name}: parts are not contiguous from PN01 (missing PN{missing[0]:02d})")
        for index in range(count):
            above = empties[index].parent
            while above is not None and above is not root and not (
                    above.type == "EMPTY" and PART_RE.match(clean_name(above.name))):
                above = above.parent
            parent = int(PART_RE.match(clean_name(above.name)).group(1)) - 1 \
                if above is not None and above is not root else 0
            lod.parts.append(Part(index, stored_parent(empties[index], index, parent), empty=empties[index]))
        return lod
    if rig is None and lod.skinned:
        rig = skin_rig(lod.skinned[0]) if lod.index == 0 else rig_of(model)
        if rig is None:
            raise ExportError(f"{lod.skinned[0].name}: deforms with {skin_rig(lod.skinned[0]).name}, but "
                              f"{model.name}'s LOD 0 holds no rig")
    if rig is None:
        return lod  # an empty LOD (retail ships them), or meshes on no part
    for ob in lod.skinned:
        if skin_rig(ob) != rig:
            raise ExportError(f"{ob.name}: deforms with {skin_rig(ob).name}, but {root.name}'s rig is {rig.name}")
    owner = lod_of(rig)
    if owner is None or lod_index(owner) != 0:
        raise ExportError(f"{rig.name}: a rig lives under a model's LOD 0 root")
    lod.rig = rig
    lod.shared = model_of(rig) is not model
    bones = part_bones(rig)
    count = max(bones) + 1 if bones else 0
    if lod.shared:
        # The arms' parts reach the highest bone their weights use, over every
        # LOD of the model.
        meshes = [ob for r in lod_roots(model) for ob in descendants(r)
                  if not ignored(ob) and skin_rig(ob) == rig]
        used = weighted_parts(meshes, bones)
        count = max(used) + 1 if used else 0
    missing = [i + 1 for i in range(count) if i not in bones]
    if missing:
        raise ExportError(f"{rig.name}: the part bones are not contiguous from BN01 (missing BN{missing[0]:02d}; "
                          "Number Parts numbers them)")
    for index in range(count):
        parent = parent_part(bones[index])
        if parent is not None and parent >= count:
            raise ExportError(f"{model.name}: its part {bones[index].name} hangs from {bones[parent].name}, past the "
                              f"{count} parts its weights reach on {rig.name}")
        lod.parts.append(Part(index, stored_parent(bones[index], index, parent if parent is not None else 0),
                              bone=bones[index], rig=rig))
    if lod.skinned and model.o3d.mesh_part:
        lod.mesh_part = count
        lod.parts.append(Part(count, 0, mesh=lod.skinned[0]))
    return lod


def part_of(ob):
    """The index of the part an object belongs to, else None: the part bone it
    is parented to (a bone that is no part holds none), the root part 0 when
    its Armature modifier deforms it (a skinned mesh; a model with a mesh part
    writes that geometry there), else the nearest PN## at or above it."""
    walk = ob
    while walk is not None and not is_lod_root(walk) and not is_model_root(walk):
        if walk.type == "EMPTY":
            m = PART_RE.match(clean_name(walk.name))
            if m:
                return int(m.group(1)) - 1
        if walk.parent is not None and walk.parent_type == "BONE":
            bone = walk.parent.data.bones.get(walk.parent_bone) if walk.parent.type == "ARMATURE" else None
            return bone_part(bone) if bone is not None else None
        if skin_rig(walk) is not None:
            return 0
        walk = walk.parent
    return None


# --- editing ----------------------------------------------------------------

def bone_label(bone):
    """What a bone's name says besides its part number: `L Hand` of
    `BN16 L Hand`, the whole name of a bone not yet numbered."""
    m = BONE_RE.match(clean_name(bone.name))
    if m is None:
        return bone.name
    return m.group(2) or ""


def number_parts(rig):
    """Number the rig's parts BN01, BN02, ... in hierarchy order: every bone
    before the bones below it, siblings in their present order (their number,
    then their name). The parts are the bones already numbered and every other
    bone that deforms, but the ground (`Root`); each keeps its label. Blender
    renames the vertex groups and the Action channels named after a bone with
    it. Returns how many bones were renamed; call it in Object Mode."""
    def is_part(bone):
        return bone_part(bone) is not None or (bone.use_deform and not is_root_bone(bone.name))

    def key(bone):
        index = bone_part(bone)
        return (index if index is not None else 1 << 30, bone.name)

    order = []

    def walk(bone):
        if is_part(bone):
            order.append(bone)
        for child in sorted(bone.children, key=key):
            walk(child)

    for bone in sorted((b for b in rig.data.bones if b.parent is None), key=key):
        walk(bone)
    if len(order) > 99:
        raise ExportError(f"{rig.name}: {len(order)} part bones; a part number has two digits (99 parts)")
    names = {}
    for i, bone in enumerate(order):
        label = bone_label(bone)
        names[bone.name] = f"BN{i + 1:02d}" + (f" {label}" if label else "")
    renames = [(old, new) for old, new in names.items() if old != new]
    # Through names no bone holds, so no rename meets a name another bone
    # still has (Blender would add a .001 suffix).
    taken = {b.name for b in rig.data.bones} | set(names.values())
    staged = []
    for n, (old, new) in enumerate(renames):
        temp = f"~number {n}"
        while temp in taken:
            temp += "~"
        taken.add(temp)
        rig.data.bones[old].name = temp
        staged.append((temp, new))
    for temp, new in staged:
        rig.data.bones[temp].name = new
    return len(renames)


@contextlib.contextmanager
def editing(context, arm):
    """The armature in Edit Mode for the block (its edit_bones), in the view
    layer the caller runs in; Object Mode after, the active object kept."""
    vl = context.view_layer
    held = vl.objects.active
    with context.temp_override(view_layer=vl, active_object=arm, object=arm, selected_objects=[arm]):
        vl.objects.active = arm
        bpy.ops.object.mode_set(mode="EDIT")
        try:
            yield arm.data.edit_bones
        finally:
            bpy.ops.object.mode_set(mode="OBJECT")
    if held is not None:
        vl.objects.active = held


def lay_bones(edit_bones, names, heads, parents):
    """New bones for parts: {part index: name}, their heads (armature space)
    and parent parts (None: a top bone). A bone's tail points at its first
    child part more than a millimetre away, else on along its parent's
    direction, else up; 5 cm when it points at no child. Returns
    {part index: edit bone}."""
    made = {i: edit_bones.new(names[i]) for i in sorted(names)}
    for i, eb in made.items():
        eb.head = heads[i]
        eb.use_connect = False
    for i, eb in made.items():
        if parents.get(i) is not None:
            eb.parent = made[parents[i]]
    for i, eb in made.items():
        kids = sorted(k for k, p in parents.items() if p == i and k != i)
        far = [Vector(heads[k]) for k in kids if (Vector(heads[k]) - Vector(heads[i])).length > 1e-3]
        up = parents.get(i)
        if far:
            eb.tail = far[0]
        elif up is not None and (Vector(heads[i]) - Vector(heads[up])).length > 1e-3:
            eb.tail = Vector(heads[i]) + (Vector(heads[i]) - Vector(heads[up])).normalized() * 0.05
        else:
            eb.tail = Vector(heads[i]) + Vector((0.0, 0.0, 0.05))
    return made


def bone_parent_matrix(arm, bone):
    """The world matrix a child parented to `bone` hangs from, at rest: the
    bone's tail (Blender parents an object to a bone's tail)."""
    return arm.matrix_world @ bone.matrix_local @ Matrix.Translation((0.0, bone.length, 0.0))


def hang(ob, arm, bone, world=None):
    """Parent `ob` to `bone` of `arm` where it stands, or at `world`. Blender
    holds an object's own matrix as location, rotation and scale, which a
    retail part frame is not quite (Mp5b_1st's scales run 0.9995 to 1.0005),
    but keeps a parent inverse whole: standing where it stands, the object
    keeps its own matrix and carries what it hung from before in its parent
    inverse; placed at `world`, the bone's inverse is the parent inverse and
    `world` its own matrix (the way Keep Transform parents)."""
    if world is None:
        above = ob.parent.matrix_world @ ob.matrix_parent_inverse if ob.parent is not None else Matrix.Identity(4)
        inverse = bone_parent_matrix(arm, bone).inverted() @ above
    else:
        inverse = bone_parent_matrix(arm, bone).inverted()
    ob.parent = arm
    ob.parent_type = "BONE"
    ob.parent_bone = bone.name
    ob.matrix_parent_inverse = inverse
    if world is not None:
        ob.matrix_basis = world


def make_rig(context, model):
    """Turn a static model into the rig shape in place, so clips can animate
    it: an Armature under its LOD 0 root whose bones BN01.. sit at the PN##
    pivots in the PN## hierarchy (each tail toward its first child part), each
    PN##'s part animation (tracks, flags, and its turn as the track frame) on
    its bone, and everything that sat on a part hung from that part's bone
    where it stood; the PN## empties go. Later LODs keep their own PN## parts.
    Returns (the armature, notes); an ExportError when LOD 0 holds no PN##
    parts or the model has a rig. Call it in Object Mode."""
    root = lod_roots(model)[0]
    lp = lod_parts(root)
    if lp.rig is not None:
        raise ExportError(f"{model.name} has a rig already ({lp.rig.name})")
    if not lp.parts or lp.parts[0].empty is None:
        raise ExportError(f"{model.name}: LOD 0 holds no PN## parts to make bones of")
    notes = []
    data = bpy.data.armatures.new(f"{clean_name(model.name)} Rig")
    arm = bpy.data.objects.new(data.name, data)
    for collection in root.users_collection:
        collection.objects.link(arm)
    arm.parent = root
    arm.matrix_parent_inverse = Matrix.Identity(4)
    arm.matrix_basis = Matrix.Identity(4)
    context.view_layer.update()
    to_arm = arm.matrix_world.inverted_safe()
    empties = {p.index: p.empty for p in lp.parts}
    index_of = {ob.name: i for i, ob in empties.items()}

    def above(ob):
        walk = ob.parent
        while walk is not None and walk is not root and walk.name not in index_of:
            walk = walk.parent
        return index_of.get(walk.name) if walk is not None else None

    parents = {i: above(ob) for i, ob in empties.items()}
    frames = {i: to_arm @ ob.matrix_world for i, ob in empties.items()}
    names = {i: f"BN{i + 1:02d}" for i in empties}
    with editing(context, arm) as edit_bones:
        lay_bones(edit_bones, names, {i: m.translation for i, m in frames.items()}, parents)
    for i, ob in empties.items():
        p, q = ob.o3d, arm.data.bones[names[i]].o3d
        q.panm_flags = p.panm_flags
        q.part_parent = p.part_parent
        for t in p.tracks:
            u = q.tracks.add()
            for field in ("target", "style", "register", "param", "rate", "start", "end", "axis"):
                setattr(u, field, getattr(t, field))
        q.frame = frames[i].to_3x3().normalized()
    context.view_layer.update()
    for i, ob in empties.items():
        bone = arm.data.bones[names[i]]
        for child in list(ob.children):
            if child.name not in index_of:
                hang(child, arm, bone)
    for ob in empties.values():
        bpy.data.objects.remove(ob)
    context.view_layer.update()
    return arm, notes


# How far an arms model's bones may sit from a gun's parts of the same index
# and still share the gun's rig: ArmsG's bone heads sit within 0.5 mm of
# 357_1st's part pivots, but each weapon places the hands itself: against
# REVVY's AKM_1st the same arms are 1.2 cm out on a finger joint, and that is
# still the pair retail draws.
PAIR_TOLERANCE = 0.025


def numbered(name, index):
    """An object's name carrying part number index + 1 where the naming
    contract takes one (a helper, a user point, a light, a volume, an
    occluder), else None."""
    from . import export
    raw = clean_name(name)
    nn = f"{index + 1:02d}"
    m = export.HELPER_RE.match(raw)
    if m:
        return f"_{nn} {m.group(2)}"
    m = export.POINT_RE.match(raw)
    if m:
        return f"UP{m.group(1)}{nn}" + (f" {m.group(3)}" if m.group(3) is not None else "")
    m = export.LIGHT_RE.match(raw)
    if m:
        return f"LP{nn}{m.group(2)}"
    m = export.VOLUME_RE.match(raw)
    if m:
        return f"{m.group(1)}{m.group(2)}{nn}{m.group(4)}-colonly"
    m = export.OCC_RE.match(raw)
    if m:
        return f"{m.group(1)}{nn}{m.group(3)}" + (f"-{m.group(4)}" if m.group(4) else "") + "-occonly"
    return None


def share_rig(context, arms, gun):
    """Make an arms model deform with a first-person gun's rig, as retail draws
    a gun's arms with the gun's part matrices, paired by index [orig:
    Player_RenderFirstPersonViewModel @ 0x4DED60, the arms submit @ 0x4DF088]:
    every mesh of the arms deforming with their own rig deforms with the gun's
    instead, its vertex groups renamed after the gun's bones of the same BN##
    number; a mesh hung from an arms bone is skinned wholly on that bone, and a
    helper there moves under its LOD root, its name carrying its part; the
    arms' own rig goes, and their root stands under the gun's root with no
    offset. Returns notes. An ExportError, with nothing changed, when either
    model lacks a rig of its own, when the arms' bones are not the gun's first
    K bones (the same parents, heads within PAIR_TOLERANCE in each model's
    frame), when their weights do not reach their last bone (a shared rig
    gives the arms the bones their weights reach), or when the arms would have
    more parts than the gun (every first-person bone buffer is the gun's)."""
    # Every matrix below is read as the scene now holds it.
    context.view_layer.update()
    own, theirs = rig_of(arms), rig_of(gun)
    if arms is gun:
        raise ExportError(f"{arms.name}: a model shares another model's rig")
    if own is None or model_of(own) is not arms:
        raise ExportError(f"{arms.name} has no rig of its own to give up")
    if theirs is None or model_of(theirs) is not gun:
        raise ExportError(f"{gun.name} has no rig of its own (Add Animation Rig makes one from its PN## parts)")
    bones, gun_bones = part_bones(own), part_bones(theirs)
    count = max(bones) + 1 if bones else 0
    if sorted(bones) != list(range(count)) or count == 0:
        raise ExportError(f"{own.name}: its part bones are not contiguous from BN01 (Number Parts numbers them)")
    root = lod_roots(arms)[0]
    meshes = [ob for r in lod_roots(arms) for ob in descendants(r) if not ignored(ob) and skin_rig(ob) == own]
    hung = [ob for ob in own.children if ob.parent_type == "BONE"]
    used = weighted_parts(meshes, bones)
    top = max(used) + 1 if used else 0
    if top != count:
        raise ExportError(f"{arms.name}: its weights reach BN{top:02d}, not its last bone BN{count:02d}: on "
                          f"{gun.name}'s rig its parts are the bones its weights reach")
    need = count + (1 if arms.o3d.mesh_part else 0)
    if need > len(gun_bones):
        raise ExportError(f"{arms.name}: {need} parts, more than the {len(gun_bones)} of {gun.name}: the game draws "
                          "the arms with the gun's part matrices, and a part past them has none")
    into_arms = arms.matrix_world.inverted_safe() @ own.matrix_world
    into_gun = gun.matrix_world.inverted_safe() @ theirs.matrix_world
    worst = 0.0
    for i in range(count):
        if i not in gun_bones:
            raise ExportError(f"{gun.name}: its rig lacks BN{i + 1:02d}")
        mine = stored_parent(bones[i], i, parent_part(bones[i]) or 0)
        other = stored_parent(gun_bones[i], i, parent_part(gun_bones[i]) or 0)
        if mine != other:
            raise ExportError(f"{arms.name}: bone {bones[i].name}'s parent is part {mine + 1:02d}, but "
                              f"{gun.name}'s {gun_bones[i].name}'s part {other + 1:02d}")
        d = ((into_arms @ bones[i].head_local) - (into_gun @ gun_bones[i].head_local)).length
        if d > PAIR_TOLERANCE:
            raise ExportError(f"{arms.name}: bone {bones[i].name} sits {d * 100:.1f} cm from {gun.name}'s "
                              f"{gun_bones[i].name} (at most {PAIR_TOLERANCE * 100:.1f} cm)")
        worst = max(worst, d)
    for ob in hung:
        bone = own.data.bones.get(ob.parent_bone)
        index = bone_part(bone) if bone is not None else None
        if index is None or index >= count:
            raise ExportError(f"{ob.name}: it hangs from {own.name}'s bone '{ob.parent_bone}', which no part of "
                              f"{gun.name}'s rig stands for")
        if ob.type != "MESH" and numbered(ob.name, index) is None:
            raise ExportError(f"{ob.name}: it hangs from {bone.name}; move it off the arms' rig first")
    # Nothing refused: now change the scene.
    names = {bones[i].name: gun_bones[i].name for i in range(count)}
    for ob in meshes:
        renames = [(g, names[g.name]) for g in ob.vertex_groups if g.name in names and g.name != names[g.name]]
        for n, (g, _) in enumerate(renames):
            g.name = f"~share {n}"
        for g, new in renames:
            g.name = new
        for m in ob.modifiers:
            if m.type == "ARMATURE" and m.object == own:
                m.object = theirs
    for ob in list(own.children):
        world = ob.matrix_world.copy()
        if ob.parent_type == "BONE":
            index = bone_part(own.data.bones[ob.parent_bone])
            if ob.type == "MESH" and skin_rig(ob) is None:
                name = gun_bones[index].name
                g = ob.vertex_groups.get(name) or ob.vertex_groups.new(name=name)
                g.add(list(range(len(ob.data.vertices))), 1.0, "REPLACE")
                ob.modifiers.new("Armature", "ARMATURE").object = theirs
            elif ob.type != "MESH":
                ob.name = numbered(ob.name, index)
        ob.parent = root
        ob.parent_type = "OBJECT"
        ob.matrix_parent_inverse = root.matrix_world.inverted_safe()
        ob.matrix_basis = world
    data = own.data
    bpy.data.objects.remove(own)
    if data.users == 0:
        bpy.data.armatures.remove(data)
    arms.parent = gun
    arms.matrix_parent_inverse = Matrix.Identity(4)
    arms.matrix_basis = Matrix.Identity(4)
    context.view_layer.update()
    return [f"{arms.name} deforms with {theirs.name}: its bones sat up to {worst * 1000:.2f} mm from "
            f"{gun.name}'s parts, whose pivots and track frames it now exports (the game draws the arms with the "
            "gun's part matrices, never their own)"]
