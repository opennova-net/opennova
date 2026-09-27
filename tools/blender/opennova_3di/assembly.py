# Several models seen together, as the game assembles them. Display only:
# every model still exports alone, in its own model root's frame, and export
# reads every rig at rest, so nothing here reaches a .3di.
#
#   A mounted model  An ITEMS.DEF `addeweap`/`addeweapC <userpoint> <item>`
#                  child (the M1A1's turret) sits on its parent's user point:
#                  the name matches the parent's USRP table whole, without
#                  case, first match; a missing name leaves the child on the
#                  parent root (engine/runtime/mission/seat_spec_extract.cpp).
#                  Its frame is the user point's direction look-at through
#                  the owning part [orig: Bone_BuildAttachmentMatrix @
#                  0x56C630; Math_BuildDirectionLookAtMatrix @ 0x612C90].
# A first-person gun and its arms need no assembly: they share the gun's rig
# (rig.py), so posing the gun poses the arms.

from contextlib import contextmanager

import bpy
from mathutils import Matrix, Vector

from . import export, rig
from .o3dtext import axis_basis

MOUNT = "O3D mount"


@contextmanager
def at_rest(*models):
    """Every rig of the models in its rest pose while the block runs."""
    rigs = {r for m in models if m is not None for r in [rig.rig_of(m)] if r is not None}
    kept = [(r.data, r.data.pose_position) for r in rigs]
    for data, _ in kept:
        data.pose_position = "REST"
    bpy.context.view_layer.update()
    try:
        yield
    finally:
        for data, position in kept:
            data.pose_position = position
        bpy.context.view_layer.update()


def user_points(model):
    """(label, empty) of the model's LOD 0 user points, in USRP order."""
    root = next((c for c in model.children if rig.is_lod_root(c) and rig.lod_index(c) == 0), None)
    if root is None:
        return []
    pts = []
    for ob in rig.descendants(root):
        m = export.POINT_RE.match(rig.clean_name(ob.name))
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
    """[orig: Math_BuildDirectionLookAtMatrix @ 0x612C90] (the engine's
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


def mount_frame(parent, point):
    """The world matrix a child mounted at `point` of `parent` takes, as
    retail builds it [orig: Bone_BuildAttachmentMatrix @ 0x56C630]: the
    position is the point's (-y, z, x), the model axes (@ 0x56c733), but the
    look-at direction is (y, z, x), unmirrored (@ 0x56c769); the look-at
    matrix then multiplies the part's as orient * bone (@ 0x56c786,
    Math_MultiplyMatrix4x4_Float @ 0x611750, row vectors), so the child's axes
    are its ROWS: in model axes (column form) the child's frame is
    look_at(X d)^T with X the x mirror, which engine/runtime/world/
    mounted_pose.cpp writes as X * frame^T * X in its mirrored model world."""
    basis = axis_basis()
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
        desired = mount_frame(parent, point) if point is not None else parent.matrix_world.copy()
        inverse = target.matrix_world.inverted_safe() @ desired
    child.matrix_basis = Matrix.Identity(4)
    con = child.constraints.new("CHILD_OF")
    con.name = MOUNT
    con.target = target
    con.inverse_matrix = inverse
