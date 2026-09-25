# Several models seen together, as the game assembles them. Display only:
# every model still exports alone, in its own model root's frame, and export
# reads skinned meshes in their rest pose, so nothing here reaches a .3di.
#
#   Arms on a gun  The first-person gun (weapon.def gfx1) is a rigid model
#                  whose parts 01-37 are the arm rig; retail draws the gun and
#                  the player's skinned arms with ONE array of bone matrices
#                  built from the gun's parts, paired by index [orig:
#                  Player_RenderFirstPersonViewModel @ 0x4ded60: the gun's
#                  part table @ 0x4def59, its Entity_BuildBoneWorldMatrices
#                  call @ 0x4df028, the arms submit reusing bone_matrices @
#                  0x4df088]. Here each BN## bone follows the gun's PN## empty
#                  of the same index, so posing the gun's parts poses the arms;
#                  a gun whose clips ride an animation rig (`!Rig`) stays a
#                  rigid model, its part hierarchy in its `~PPx attach`
#                  helpers.
#   A mounted model  An ITEMS.DEF `addeweap`/`addeweapC <userpoint> <item>`
#                  child (the M1A1's turret) sits on its parent's user point:
#                  the name matches the parent's USRP table whole, without
#                  case, first match; a missing name leaves the child on the
#                  parent root (engine/runtime/mission/seat_spec_extract.cpp).
#                  Its frame is the user point's direction look-at through
#                  the owning part [orig: build_bone_attachment_matrix @
#                  0x56C630; build_direction_look_at_matrix @ 0x612C90].
# Both bind with every rig of the models at rest, so a pose a clip holds when
# they are set is not baked into the pairing.

from contextlib import contextmanager

import bpy
from mathutils import Matrix, Vector

from . import export

DRIVE = "O3D drive"
MOUNT = "O3D mount"
SOCKET = "!drive"  # export ignores "!" names
# ArmsG's bone heads sit within 0.5 mm of 357_1st's part pivots, but each weapon
# places the hands itself: against REVVY's AKM_1st the same arms are 1.2 cm out
# on a finger joint, and that is still the pair retail draws.
PIVOT_TOLERANCE = 0.025


def lod_root(model, index=0):
    return next((c for c in model.children if export.is_lod_root(c) and int(c["_lod_index"]) == index), None)


def armatures(model):
    """Every LOD's BN## armature of a skinned model. An armature named `!...`
    (a rigid model's animation rig) is none, as export reads it."""
    out = []
    for root in (c for c in model.children if export.is_lod_root(c)):
        out += [ob for ob in export.descendants(root)
                if ob.type == "ARMATURE" and not export.clean_name(ob.name).startswith("!")]
    return out


def rig_parts(model):
    """A rigid model's LOD 0 parts by index (its PN## empties)."""
    root = lod_root(model)
    parts = {}
    if root is None:
        return parts
    for ob in export.descendants(root):
        m = export.PART_RE.match(export.clean_name(ob.name))
        if m and ob.type == "EMPTY":
            parts[int(m.group(1)) - 1] = ob
    return parts


def bone_index(bone):
    m = export.BONE_RE.match(export.clean_name(bone.name))
    return int(m.group(1)) - 1 if m else None


def part_parent(ob):
    """A PN## empty's parent part, as export reads it: its first `~PPx attach`
    helper's (the one a part keeps once a `!Rig` bone is its Blender parent),
    else the PN## above it, else the root."""
    index = int(export.PART_RE.match(export.clean_name(ob.name)).group(1)) - 1
    helpers = sorted((export.dup_rank(m.group(2)), export.clean_name(c.name), int(m.group(1)) - 1)
                     for c in ob.children_recursive
                     for m in [export.ATTACH_RE.match(export.clean_name(c.name))]
                     if m and export.Exporter.helper_part(c) == index)
    if helpers:
        return helpers[0][2]
    above = export.Exporter.owning_part(ob)
    return above if above is not None else 0


@contextmanager
def at_rest(*models):
    """Every armature of the models (a skinned model's rig, a gun's `!Rig`)
    in its rest pose while the block runs."""
    rigs = [ob for m in models if m is not None
            for root in (c for c in m.children if export.is_lod_root(c))
            for ob in export.descendants(root) if ob.type == "ARMATURE"]
    kept = [(ob.data, ob.data.pose_position) for ob in rigs]
    for data, _ in kept:
        data.pose_position = "REST"
    bpy.context.view_layer.update()
    try:
        yield
    finally:
        for data, position in kept:
            data.pose_position = position
        bpy.context.view_layer.update()


def rig_fit(skin, rig):
    """How far the skinned model's LOD 0 bones sit from the rigid model's
    parts of the same index (None when they do not pair: a bone without its
    part, or another parent)."""
    arms = [a for a in armatures(skin) if a.parent == lod_root(skin)]
    parts = rig_parts(rig)
    if not arms or not parts:
        return None
    worst = 0.0
    for bone in arms[0].data.bones:
        if export.clean_name(bone.name).startswith("!"):
            continue
        i = bone_index(bone)
        if i is None or i not in parts:
            return None
        parent = export.bone_parent_part(bone)
        if i > 0 and (parent if parent is not None else 0) != part_parent(parts[i]):
            return None
        head = arms[0].matrix_world @ bone.head_local
        worst = max(worst, (head - parts[i].matrix_world.translation).length)
    return worst


def best_rig(skin, candidates):
    fits = []
    for rig in candidates:
        if rig == skin:
            continue
        with at_rest(skin, rig):
            fit = rig_fit(skin, rig)
        if fit is not None and fit <= PIVOT_TOLERANCE:
            fits.append((fit, rig.name, rig))
    return min(fits)[2] if fits else None


def sockets(skin):
    return [c for c in skin.children if export.clean_name(c.name).startswith(SOCKET)]


def drive(skin, rig):
    """Pose the skinned model's bones from the rig model's parts (None: free
    them again). Each bone copies a socket: an Empty holding the bone's rest
    frame that rides its part (a Child Of bound at rest), so the bone moves by
    exactly its part's motion and never twice through its parent bone."""
    for arm in armatures(skin):
        for pb in arm.pose.bones:
            for con in [c for c in pb.constraints if c.name == DRIVE]:
                pb.constraints.remove(con)
    for ob in sockets(skin):
        bpy.data.objects.remove(ob)
    parts = rig_parts(rig) if rig is not None else {}
    if not parts:
        return
    with at_rest(skin, rig):
        bind(skin, parts)


def bind(skin, parts):
    """Each bone's socket, bound to its part (drive; the models at rest)."""
    collection = skin.users_collection[0] if skin.users_collection else bpy.context.scene.collection
    for arm in armatures(skin):
        for pb in arm.pose.bones:
            target = parts.get(bone_index(pb.bone))
            if target is None:
                continue
            socket = bpy.data.objects.new(f"{SOCKET} {arm.name} {pb.name}", None)
            socket.empty_display_size = 0.02
            collection.objects.link(socket)
            socket.parent = skin
            socket.matrix_world = arm.matrix_world @ pb.bone.matrix_local
            ride = socket.constraints.new("CHILD_OF")
            ride.name = DRIVE
            ride.target = target
            ride.inverse_matrix = target.matrix_world.inverted_safe()
            try:
                socket.hide_set(True)
            except RuntimeError:
                pass  # not in the view layer: it still evaluates
            con = pb.constraints.new("COPY_TRANSFORMS")
            con.name = DRIVE
            con.target = socket


def user_points(model):
    """(label, empty) of the model's LOD 0 user points, in USRP order."""
    root = lod_root(model)
    if root is None:
        return []
    pts = []
    for ob in export.descendants(root):
        m = export.POINT_RE.match(export.clean_name(ob.name))
        if m and ob.type == "EMPTY":
            pts.append((m.group(3) if m.group(3) is not None else "Noname", ob))
    return sorted(pts, key=lambda e: export.order_key(e[1]))


def find_point(model, label):
    """The user point an addeweap row names: matched whole and without case,
    both names trimmed (retail labels carry trailing blanks: "ground ",
    "EXHAUST "), first match (engine/runtime/mission/seat_spec_extract.cpp)."""
    for name, ob in user_points(model):
        if name.strip().lower() == label.strip().lower():
            return ob
    return None


def look_at(direction):
    """[orig: build_direction_look_at_matrix @ 0x612C90] (the engine's
    renderer/direction_look_at.h): forward = the direction, right = (f.z, 0,
    -f.x) normalized, up = forward x right; a vertical direction takes the x
    axis as right, as the engine substitutes. Retail stores right, up and
    forward as the COLUMNS of a row-vector matrix (@ 0x612e18..0x612e6d);
    this returns that matrix."""
    f = direction.normalized() if direction.length > 1e-9 else Vector((0.0, 0.0, 1.0))
    h = (f.x * f.x + f.z * f.z) ** 0.5
    r = Vector((f.z / h, 0.0, -f.x / h)) if h > 0 else Vector((1.0, 0.0, 0.0))
    u = f.cross(r)
    u = u.normalized() if u.length > 1e-9 else Vector((0.0, 1.0, 0.0))
    return Matrix((r, u, f)).transposed()


# Mission axes (x forward, y left, z up) from the loader's model axes (x side,
# y up, z forward, the frame retail renders in): mission = LOADER @ model, and
# model = (-y, z, x) of a mission vector.
LOADER = Matrix(((0, 0, 1), (-1, 0, 0), (0, 1, 0)))
# The x mirror between the two swizzles retail reads a user point with.
MIRROR_X = Matrix(((-1, 0, 0), (0, 1, 0), (0, 0, 1)))


def mount_frame(parent, point, forward):
    """The world matrix a child mounted at `point` of `parent` takes, as
    retail builds it [orig: build_bone_attachment_matrix @ 0x56C630]: the
    position is the point's (-y, z, x), the model axes (@ 0x56c733), but the
    look-at direction is (y, z, x), unmirrored (@ 0x56c769); the look-at
    matrix then multiplies the part's as orient * bone (@ 0x56c786,
    Math_MultiplyMatrix4x4_Float @ 0x611750, row vectors), so the child's axes
    are its ROWS: in model axes (column form) the child's frame is
    look_at(X d)^T with X the x mirror, which engine/runtime/world/
    mounted_pose.cpp writes as X * frame^T * X in its mirrored model world."""
    basis = export.axis_basis(forward)
    local = parent.matrix_world.inverted_safe() @ point.matrix_world
    d = (local.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized()
    d_model = LOADER.transposed() @ (basis.transposed() @ d)
    child = look_at(MIRROR_X @ d_model).transposed()
    rot = basis @ (LOADER @ child @ LOADER.transposed()) @ basis.transposed()
    return parent.matrix_world @ Matrix.Translation(local.translation) @ rot.to_4x4()


def mount(child, scene):
    """Place a model on its `mount_parent` at `mount_point` (a Child Of
    constraint on the model root), or free it when the parent is cleared."""
    for con in [c for c in child.constraints if c.name == MOUNT]:
        child.constraints.remove(con)
    parent = child.o3d.mount_parent
    if parent is None or parent == child:
        return
    point = find_point(parent, child.o3d.mount_point) if child.o3d.mount_point else None
    target = point if point is not None else parent
    with at_rest(parent):
        desired = mount_frame(parent, point, scene.o3d.forward) if point is not None else parent.matrix_world.copy()
        inverse = target.matrix_world.inverted_safe() @ desired
    child.matrix_basis = Matrix.Identity(4)
    con = child.constraints.new("CHILD_OF")
    con.name = MOUNT
    con.target = target
    con.inverse_matrix = inverse


def pair_imported(context, models):
    """Pair each newly imported skinned model with the imported rigid model
    whose parts carry its bones (a gun and its arms); returns the pairs."""
    context.view_layer.update()
    rigs = [m for m in models if not armatures(m)]
    pairs = []
    for skin in (m for m in models if armatures(m)):
        rig = best_rig(skin, rigs)
        if rig is not None:
            skin.o3d.drive_rig = rig
            pairs.append((skin, rig))
    return pairs
