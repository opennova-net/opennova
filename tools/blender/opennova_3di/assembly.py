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

# The first-person arms every stock character draws (its Avatars.def `arms`:
# ArmsG, ArmsGb, ArmsR, ArmGlove, ArmGlovD/J/S, ArmsD/S, IndoArms) share one
# 37-bone rig: each bone's parent and pivot (mission axes), ArmsG.3di's parts
# 0-36 in JOTAC's base archive (the others' are the same to the bit). The game
# draws a character's arms with the first-person gun's part matrices, paired
# by index [orig: Player_RenderFirstPersonViewModel @ 0x4DED60, the arms
# submit @ 0x4DF088], so a gun's parts 01-37 are this rig.
STOCK_ARMS = (
    (0, (0, 0, 0)), (0, (-0.00510000018, -0.15640001, 0.587199926)),
    (0, (-0.000300000014, 0.15609999, 0.584999919)), (1, (-0.00579999993, -0.445299983, 0.599499941)),
    (2, (-0.00609999988, 0.445199996, 0.582700014)), (3, (-0.00650000013, -0.71509999, 0.61500001)),
    (4, (-0.0114000002, 0.715399981, 0.584299922)), (5, (-0.0335999988, -0.732200027, 0.620099902)),
    (6, (-0.0373999998, 0.732800007, 0.588199973)), (7, (-0.0742999986, -0.765300035, 0.634899974)),
    (8, (-0.0754000023, 0.763799965, 0.601499915)), (5, (0.0288999993, -0.828999996, 0.617900014)),
    (6, (-0.0494000018, 0.827899992, 0.58799994)), (11, (0.0287999995, -0.857800007, 0.619699955)),
    (12, (-0.0502999984, 0.869099975, 0.58859992)), (13, (0.0286999997, -0.881900012, 0.621099949)),
    (14, (-0.050999999, 0.900699973, 0.58889997)), (5, (0.00520000001, -0.835600019, 0.618699908)),
    (6, (-0.0259000007, 0.834699988, 0.587499976)), (17, (0.00510000018, -0.869599998, 0.620700002)),
    (18, (-0.0284000002, 0.879399955, 0.588099957)), (19, (0.00499999989, -0.903400004, 0.622699976)),
    (20, (-0.0306000002, 0.917599976, 0.58859992)), (5, (-0.0184000004, -0.845600009, 0.619799972)),
    (6, (-0.00209999993, 0.83099997, 0.586799979)), (23, (-0.0185000002, -0.882300019, 0.621999979)),
    (24, (-0.00499999989, 0.870799959, 0.587000012)), (25, (-0.0186000001, -0.91930002, 0.624199986)),
    (26, (-0.00749999983, 0.903899968, 0.58709991)), (5, (-0.0421999991, -0.835400045, 0.618999958)),
    (6, (0.0218000002, 0.823300004, 0.586099982)), (29, (-0.0417000018, -0.871200025, 0.621099949)),
    (30, (0.0186000001, 0.857800007, 0.586300015)), (31, (-0.0412999988, -0.901800036, 0.622900009)),
    (32, (0.0163000003, 0.88349998, 0.586199999)), (9, (-0.0983999968, -0.78490001, 0.643599987)),
    (10, (-0.108999997, 0.791099966, 0.613299966)),
)


def stock_arms_fit(exporter, lod):
    """A note when a first-person gun's parts 01-37 are not the stock arms' rig
    (their parents, and pivots within rig.PAIR_TOLERANCE), else None: the
    character's own arms then draw wrong on the gun, and arms made for it draw
    wrong on every stock gun."""
    parts = lod.parts
    why = None
    if len(parts) < len(STOCK_ARMS):
        why = f"it has {len(parts)} parts"
    else:
        worst, at = 0.0, 0
        for i, (parent, pivot) in enumerate(STOCK_ARMS):
            if parts[i].parent != parent:
                why = f"part {i + 1:02d}'s parent is {parts[i].parent + 1:02d}, the arms' {parent + 1:02d}"
                break
            d = (Vector(exporter.pivot(parts[i])) - Vector(pivot)).length
            if d > worst:
                worst, at = d, i
        if why is None and worst > rig.PAIR_TOLERANCE:
            why = f"part {at + 1:02d} is {worst * 100:.1f} cm from the arms' joint"
    if why is None:
        return None
    return (f"{exporter.model.name}: its parts 01-37 are not the stock first-person arms' rig ({why}): the game "
            "draws the character's arms (Avatars.def) with a gun's first 37 part matrices, so stock arms draw wrong "
            "on this gun, and arms made for it draw wrong on every stock gun")


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
