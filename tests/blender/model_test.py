"""The add-on's model export and import in the one-armature scene shape, from
scratch and hermetic (every file lands in a fresh temporary directory):

    blender -b --factory-startup --python-exit-code 1 --python tests/blender/model_test.py -- <opennova-3di.exe>
    blender -b --factory-startup --python-exit-code 1 --python tests/blender/model_test.py -- --installed

Each case authors a model (a static prop on PN## empties, a first-person gun
on its rig with arms sharing it, a skinned model), exports it through
opennova-3di, reads the .3di back with `opennova-3di scene` and, where a case
round-trips, imports it again and has `opennova-3di compare` call the second
export the same model.
"""
import math
import os
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addon_harness  # noqa: E402
import clip_scene  # noqa: E402

import bpy  # noqa: E402
from mathutils import Vector  # noqa: E402

addon, CLI = addon_harness.load()
animation, export, importer, rig, o3dtext = (addon.animation, addon.export, addon.importer, addon.rig,
                                             addon.o3dtext)
OUT = addon_harness.scratch("opennova_model_test_")
FAILURES = []


def case(fn):
    """Run one case on an empty scene; a failure is reported, not raised."""
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    for arm in list(bpy.data.armatures):
        bpy.data.armatures.remove(arm)
    try:
        fn()
        print("PASS", fn.__name__)
    except Exception:  # noqa: BLE001
        FAILURES.append(fn.__name__)
        print("FAIL", fn.__name__)
        traceback.print_exc()
    return fn


# --- building ---------------------------------------------------------------

def link(ob, parent=None):
    bpy.context.collection.objects.link(ob)
    ob.parent = parent
    return ob


def empty(name, parent=None, at=(0.0, 0.0, 0.0)):
    ob = link(bpy.data.objects.new(name, None), parent)
    ob.location = at
    return ob


def model(name):
    """A model root at the origin with its LOD 0 root, writing <name>.3di into
    a folder of its own; returns (root, LOD 0 root)."""
    root = empty(name)
    root.o3d.output_path = os.path.join(OUT, name, name + ".3di").replace("\\", "/")
    lod = empty(name + "_LOD0", root)
    lod["_lod_index"] = 0
    return root, lod


def box(name, parent=None, at=(0.0, 0.0, 0.0), size=0.05):
    """A closed box mesh centred at `at` (world), with a UV map."""
    s = size
    me = bpy.data.meshes.new(name)
    me.from_pydata([(-s, -s, -s), (s, -s, -s), (s, s, -s), (-s, s, -s), (-s, -s, s), (s, -s, s), (s, s, s),
                    (-s, s, s)], [], [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6),
                                      (3, 0, 4, 7)])
    me.uv_layers.new(name="UVMap")
    ob = link(bpy.data.objects.new(name, me), parent)
    ob.location = at
    return ob


def armature(name, parent, bones):
    """An armature under `parent` with bones {name: (head, parent name or
    None)}, heads in world space (the parent at the origin)."""
    data = bpy.data.armatures.new(name)
    arm = link(bpy.data.objects.new(name, data), parent)
    names = {n: n for n in bones}
    heads = {n: Vector(h) for n, (h, _) in bones.items()}
    parents = {n: p for n, (_, p) in bones.items()}
    with rig.editing(bpy.context, arm) as edit_bones:
        rig.lay_bones(edit_bones, names, heads, parents)
    return arm


def hang(ob, arm, bone):
    """Parent to a bone where the object stands (Ctrl+P > Bone)."""
    bpy.context.view_layer.update()
    rig.hang(ob, arm, arm.data.bones[bone])


def skin(ob, arm, weights):
    """An Armature modifier on `arm` and weights {vertex: {bone: weight}}."""
    ob.modifiers.new("Armature", "ARMATURE").object = arm
    for vi, influences in weights.items():
        for bone, w in influences.items():
            g = ob.vertex_groups.get(bone) or ob.vertex_groups.new(name=bone)
            g.add([vi], w, "REPLACE")


def folder(root):
    return os.path.dirname(export.output_path(root))


def export_model(root):
    """(notes, the scene text lines `opennova-3di scene` writes back)."""
    bpy.context.view_layer.update()
    os.makedirs(folder(root), exist_ok=True)
    _, notes = export.export_model(bpy.context, root)
    return notes, scene_lines(export.output_path(root))


def scene_lines(path):
    out = path[:-4] + ".o3d"
    subprocess.run([CLI, "scene", path, "-o", out], check=True, capture_output=True)
    with open(out, encoding="utf-8") as f:
        return [line.split("  #")[0].rstrip() for line in f.read().splitlines() if line and not line.startswith("#")]


def records(lines, key):
    return [line.split()[1:] for line in lines if line.split()[:1] == [key]]


def spheres(lines):
    """{collision section: its csphere values}."""
    out, at = {}, -1
    for line in lines:
        key = line.split()[:1]
        if key == ["cobj"]:
            at += 1
        elif key == ["csphere"]:
            out[at] = [float(x) for x in line.split()[1:]]
    return out


def same_spheres(a, b):
    """The same sections carry the same spheres and boxes, on the file's 16.16
    grid (one step of float noise)."""
    assert a.keys() == b.keys(), (a, b)
    for k in a:
        assert len(a[k]) == len(b[k]) and all(abs(x - y) <= 1.0 / 65536.0 for x, y in zip(a[k], b[k])), \
            (k, a[k], b[k])


def hit_helpers():
    """The `_## hit` and `_## bounds` empties in the scene, by name."""
    names = (rig.clean_name(ob.name) for ob in bpy.data.objects)
    return sorted(n for n in names if (m := export.HELPER_RE.match(n)) and m.group(2) in ("hit", "bounds"))


def refused(root, *fragments):
    """The ExportError the export raises, which must name every fragment."""
    bpy.context.view_layer.update()
    os.makedirs(folder(root), exist_ok=True)
    try:
        export.export_model(bpy.context, root)
    except o3dtext.ExportError as e:
        for fragment in fragments:
            assert fragment in str(e), (fragment, str(e))
        return str(e)
    raise AssertionError(f"{root.name} exported; expected an error naming {fragments}")


def compare(a, b):
    """opennova-3di compare: the same model (drift allowed)."""
    r = subprocess.run([CLI, "compare", a, b], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def import_again(paths):
    """Import the files together into an emptied scene; their model roots."""
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    out = []
    for path, model_root, notes in importer.import_files(bpy.context, paths):
        assert model_root is not None, (path, notes)
        out.append(model_root)
    return out


# --- a static prop --------------------------------------------------------------

@case
def static_prop_reads_parts_from_where_things_sit():
    root, lod = model("crate")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    group = empty("hinges", pn1)           # an empty that only groups
    pn2 = empty("PN02", group, (0.0, 0.0, 0.1))
    box("lid any name", pn2, (0.0, 0.0, 0.12), 0.04)
    empty("UPG handle", pn2, (0.0, 0.0, 0.2))
    empty("UPS00 spark", lod, (0.3, 0.0, 0.0))  # 00: on no part
    box("CB-colonly", pn1, size=0.06)
    lamp = link(bpy.data.objects.new("LP", bpy.data.lights.new("LP", "POINT")), pn2)
    lamp.location = (0.0, 0.0, 0.3)
    notes, lines = export_model(root)
    parts = records(lines, "part")
    assert len(parts) == 2 and parts[1][0] == "0", parts
    assert [p[0] for p in records(lines, "strip")].count("0") == 2  # a strip on each part
    points = {p[0]: p[7] for p in records(lines, "userpoint")}
    assert points == {"handle": "1", "spark": "-1"}, points
    assert records(lines, "light")[0][0] == "1"
    assert records(lines, "cvolume") or records(lines, "cvol"), lines
    # The only notes are the materials' (the meshes have none).
    assert all(n.startswith("(no material)") for n in notes), notes


@case
def a_name_that_says_another_part_is_refused():
    root, lod = model("names")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    pn2 = empty("PN02", pn1, (0.0, 0.0, 0.1))
    empty("UPG01 handle", pn2)
    refused(root, "UPG01 handle", "says part 01", "part 02")
    bpy.data.objects["UPG01 handle"].name = "UPG02 handle"
    stray = box("Stray", lod)
    refused(root, "Stray", "sits on no part")
    bpy.data.objects.remove(stray)
    pn1.location = (0.2, 0.0, 0.0)
    refused(root, "PN01", "root part's pivot is the model origin")


@case
def a_parent_numbered_after_its_part_is_noted():
    root, lod = model("order")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    pn3 = empty("PN03", pn1, (0.0, 0.1, 0.0))
    pn2 = empty("PN02", pn3, (0.0, 0.2, 0.0))
    box("Tip", pn2, (0.0, 0.2, 0.0), 0.01)
    notes, lines = export_model(root)
    assert records(lines, "part")[1][0] == "2", records(lines, "part")
    assert any("PN02" in n and "numbered after it" in n for n in notes), notes


@case
def rigid_strips_split_at_the_index_limit():
    root, lod = model("grid")
    pn1 = empty("PN01", lod)
    me = bpy.data.meshes.new("grid")
    n = 106  # 106 x 106 quads: 22,472 triangles, past one strip's 21,845
    me.from_pydata([(x * 0.01, y * 0.01, 0.0) for y in range(n + 1) for x in range(n + 1)], [],
                   [(y * (n + 1) + x, y * (n + 1) + x + 1, (y + 1) * (n + 1) + x + 1, (y + 1) * (n + 1) + x)
                    for y in range(n) for x in range(n)])
    me.uv_layers.new(name="UVMap")
    link(bpy.data.objects.new("grid", me), pn1)
    root.o3d.export_bullet_faces = False
    _, lines = export_model(root)
    strips = records(lines, "strip")
    assert len(strips) == 2, strips
    assert sum(1 for line in lines if line.startswith("t ")) == 2 * n * n


@case
def unrealized_instances_refused():
    root, lod = model("gn")
    pn1 = empty("PN01", lod)
    ob = box("Instancer", pn1)
    tree = bpy.data.node_groups.new("scatter", "GeometryNodeTree")
    tree.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    inp, outp = tree.nodes.new("NodeGroupInput"), tree.nodes.new("NodeGroupOutput")
    points = tree.nodes.new("GeometryNodeMeshToPoints")
    inst = tree.nodes.new("GeometryNodeInstanceOnPoints")
    cube = tree.nodes.new("GeometryNodeMeshCube")
    tree.links.new(inp.outputs[0], points.inputs["Mesh"])
    tree.links.new(points.outputs["Points"], inst.inputs["Points"])
    tree.links.new(cube.outputs["Mesh"], inst.inputs["Instance"])
    tree.links.new(inst.outputs["Instances"], outp.inputs[0])
    ob.modifiers.new("scatter", "NODES").node_group = tree
    refused(root, "Instancer", "instances")


@case
def a_cli_refusal_names_its_object():
    # An occluder whose faces give more planes than a record holds (32): only
    # opennova-3di checks it, and its message names the occlusion mesh.
    root, lod = model("occl")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    sides = 40
    me = bpy.data.meshes.new("OB-occonly")
    ring = [(0.5 * math.cos(2 * math.pi * k / sides), 0.5 * math.sin(2 * math.pi * k / sides)) for k in range(sides)]
    me.from_pydata([(x, y, 0.0) for x, y in ring] + [(x, y, 1.0) for x, y in ring], [],
                   [(k, (k + 1) % sides, sides + (k + 1) % sides, sides + k) for k in range(sides)])
    link(bpy.data.objects.new("OB-occonly", me), pn1)
    refused(root, "OB-occonly")
    # A refusal naming a collision section alone is told its part and meshes.
    said = export.name_sections("occl.3di: collision section 1 (part 1) has 40,000 distinct bullet-face normals",
                                {1: "PN02: Hull, Turret"})
    assert "collision section 1 (PN02: Hull, Turret)" in said, said


# --- a first-person gun and its arms on one rig -----------------------------------

GUN_BONES = {  # the arm bones 01-03, then the gun body and its bolt
    "BN01": ((0.0, 0.0, 0.0), None),
    "BN02 L Hand": ((0.2, 0.0, 0.5), "BN01"),
    "BN03 R Hand": ((-0.2, 0.0, 0.5), "BN01"),
    "BN04 Body": ((-0.2, -0.3, 0.5), "BN03 R Hand"),
    "BN05 Bolt": ((-0.2, -0.35, 0.55), "BN04 Body"),
}


def gun_and_arms(name="fp"):
    """A gun model on its own rig (meshes hung from bones) and an arms model
    whose skinned mesh deforms with that rig, its weights on bones 01-03 (a
    vertex on four bones of which three are parts... all parts here)."""
    gun, glod = model(f"{name}gun")
    arm = armature(f"{name}gun Rig", glod, GUN_BONES)
    body = box("Receiver", None, (-0.2, -0.45, 0.5), 0.05)
    hang(body, arm, "BN04 Body")
    bolt = box("Bolt", None, (-0.2, -0.4, 0.56), 0.01)
    hang(bolt, arm, "BN05 Bolt")
    flash = empty("UPS MFLASH01", None, (-0.2, -0.6, 0.5))
    hang(flash, arm, "BN04 Body")
    arms, alod = model(f"{name}arms")
    arms.parent = gun
    hands = box("Hands", alod, (0.0, 0.0, 0.5), 0.2)
    skin(hands, arm, {0: {"BN01": 0.4, "BN02 L Hand": 0.3, "BN03 R Hand": 0.2, "BN04 Body": 0.1},
                      1: {"BN02 L Hand": 1.0}, 2: {"BN02 L Hand": 1.0}, 3: {"BN03 R Hand": 1.0},
                      4: {"BN01": 1.0}, 5: {"BN02 L Hand": 0.5, "BN03 R Hand": 0.5}, 6: {"BN03 R Hand": 1.0},
                      7: {"BN02 L Hand": 0.25, "BN03 R Hand": 0.75}})
    arms.o3d.export_bullet_faces = False
    return gun, arms, arm


@case
def first_person_gun_and_arms_share_one_rig():
    gun, arms, arm = gun_and_arms()
    _, gun_lines = export_model(gun)
    parts = records(gun_lines, "part")
    assert [p[0] for p in parts] == ["0", "0", "0", "2", "3"], parts
    assert not any(line.startswith("skinned") for line in gun_lines)
    assert any(p[7] == "3" for p in records(gun_lines, "userpoint"))
    notes, arm_lines = export_model(arms)
    assert any(line == "skinned 1" for line in arm_lines)
    # The arms' parts are the rig's bones up to the highest one weighted
    # (BN04, the body, carries a tenth of vertex 0).
    assert len(records(arm_lines, "part")) == 4, records(arm_lines, "part")
    # Contract C1: vertex 0 keeps its four influences, dominant first, the
    # fourth on slot 3 with the remainder of the three stored weights.
    four = [v for v in records(arm_lines, "v") if abs(float(v[-3]) - 0.4) < 1e-6]
    assert four, records(arm_lines, "v")[:4]
    w = [float(x) for x in four[0][-3:]]
    assert abs(w[0] - 0.4) < 1e-6 and abs(w[1] - 0.3) < 1e-6 and abs(w[2] - 0.2) < 1e-6, w
    table = records(arm_lines, "bones")[0]
    slots = [int(x) for x in four[0][-7:-3]]
    assert [int(table[s]) for s in slots] == [0, 1, 2, 3], (table, slots)


@case
def first_person_gun_and_arms_round_trip():
    gun, arms, arm = gun_and_arms("rt")
    export_model(gun)
    export_model(arms)
    first = [export.output_path(gun), export.output_path(arms)]
    again = import_again(first)
    names = {m.name: m for m in again}
    g, a = names["rtgun"], names["rtarms"]
    # Imported together, the arms deform with the gun's rig again; their bone
    # sections' spheres are the derived ones, so no helper stands for them.
    assert rig.rig_of(a) == rig.rig_of(g) and rig.model_of(rig.rig_of(g)) is g
    assert not hit_helpers(), hit_helpers()
    for m in (g, a):
        m.o3d.output_path = os.path.join(OUT, "again", m.name + ".3di").replace("\\", "/")
        export_model(m)
    compare(first[0], export.output_path(g))
    compare(first[1], export.output_path(a))


@case
def number_parts_follows_the_hierarchy():
    root, lod = model("numbered")
    arm = armature("numbered Rig", lod, {"Hips": ((0, 0, 0), None), "Spine": ((0, 0, 0.3), "Hips"),
                                         "Arm.L": ((0.2, 0, 0.5), "Spine"), "Arm.R": ((-0.2, 0, 0.5), "Spine")})
    with rig.editing(bpy.context, arm) as edit_bones:
        ground = edit_bones.new("Root")
        ground.head, ground.tail = (0, 0, -1), (0, 0.2, -1)
        edit_bones["Hips"].parent = ground
        ctrl = edit_bones.new("ik_hand")
        ctrl.head, ctrl.tail, ctrl.use_deform = (0.3, 0, 0.5), (0.3, 0.1, 0.5), False
    mesh = box("Body", arm, (0, 0, 0.3), 0.2)
    skin(mesh, arm, {v: {"Spine": 1.0} for v in range(8)})
    # Two clips keyed on Spine: one the rig plays, whose channels Blender
    # renames with the bone, and one only the table holds.
    arm.animation_data_create()
    clips = []
    for name in ("Held", "Playing"):
        clips.append(bpy.data.actions.new(name))
        arm.animation_data.action = clips[-1]
        arm.pose.bones["Spine"].keyframe_insert("location", frame=0)
    row = root.o3d.rows.add()
    row.key = "anim_reset"
    row.variants.add().action = clips[0]
    assert rig.number_parts(arm) == 4
    names = sorted(b.name for b in arm.data.bones)
    assert names == ["BN01 Hips", "BN02 Spine", "BN03 Arm.L", "BN04 Arm.R", "Root", "ik_hand"], names
    assert mesh.vertex_groups[0].name == "BN02 Spine"
    for action in clips:
        bags = [bag for layer in action.layers for strip in layer.strips for bag in strip.channelbags]
        keyed = ({fc.data_path for bag in bags for fc in bag.fcurves}, {g.name for bag in bags for g in bag.groups})
        assert keyed == ({'pose.bones["BN02 Spine"].location'}, {"BN02 Spine"}), (action.name, keyed)
    # Symmetrize-style duplicates renumber too.
    arm.data.bones["BN04 Arm.R"].name = "BN03 Arm.R"
    refused(root, "both part 03")
    rig.number_parts(arm)
    assert sorted(b.name for b in arm.data.bones)[:4] == ["BN01 Hips", "BN02 Spine", "BN03 Arm.L", "BN04 Arm.R"]


@case
def make_rig_exports_the_same_model():
    root, lod = model("rigged")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    pn2 = empty("PN02", pn1, (0.0, 0.1, 0.2))
    pn2.rotation_euler = (0.0, 0.0, 0.5)
    box("Door", pn2, (0.0, 0.15, 0.2), 0.03)
    empty("_attach", pn2, (0.0, 0.12, 0.25))
    track = pn2.o3d.tracks.add()
    track.target, track.style, track.register = "rotx", 113, "DOOR_OPEN"
    export_model(root)
    before = export.output_path(root)
    rig.make_rig(bpy.context, root)
    root.o3d.output_path = os.path.join(OUT, "rigged2", "rigged.3di").replace("\\", "/")
    export_model(root)
    compare(before, export.output_path(root))


@case
def share_rig_refuses_arms_that_do_not_fit():
    gun, glod = model("fitgun")
    armature("fitgun Rig", glod, {"BN01": ((0, 0, 0), None), "BN02": ((0.2, 0, 0.5), "BN01")})
    arms, alod = model("fitarms")
    own = armature("fitarms Rig", alod, {"BN01": ((0, 0, 0), None), "BN02": ((0.3, 0, 0.5), "BN01")})
    hands = box("Hands", own, (0.2, 0, 0.5), 0.05)
    skin(hands, own, {v: {"BN02": 1.0} for v in range(8)})
    try:
        rig.share_rig(bpy.context, arms, gun)
        raise AssertionError("arms 10 cm off the gun's bones shared its rig")
    except o3dtext.ExportError as e:
        assert "BN02" in str(e) and "cm" in str(e), e
    assert rig.rig_of(arms) == own


# --- skinned geometry -----------------------------------------------------------

def person(name):
    """A skinned model with its own rig: a hips bone and two limbs."""
    root, lod = model(name)
    arm = armature(f"{name} Rig", lod, {"BN01 Hips": ((0, 0, 0), None), "BN02 Leg": ((0, 0, -0.5), "BN01 Hips"),
                                        "BN03 Arm": ((0.3, 0, 0.4), "BN01 Hips")})
    body = box("Body", arm, (0, 0, 0), 0.3)
    return root, arm, body


@case
def skin_weights_follow_contract_c1():
    root, arm, body = person("weights")
    with rig.editing(bpy.context, arm) as edit_bones:
        for n, name in enumerate(("BN04 A", "BN05 B")):
            b = edit_bones.new(name)
            b.head, b.tail, b.parent = (0.1 * n, 0.2, 0), (0.1 * n, 0.3, 0), edit_bones["BN01 Hips"]
        root_bone = edit_bones.new("Root")
        root_bone.head, root_bone.tail = (0, 0, -1), (0, 0.2, -1)
    all_five = {"BN01 Hips": 0.2, "BN02 Leg": 0.2, "BN03 Arm": 0.2, "BN04 A": 0.2, "BN05 B": 0.2}
    skin(body, arm, {0: all_five, **{v: {"BN01 Hips": 1.0} for v in range(1, 8)}})
    refused(root, "Body", "vertex 0", "5 bones")
    body.vertex_groups["BN05 B"].remove([0])
    body.vertex_groups["BN01 Hips"].remove([1])
    refused(root, "Body", "vertex 1", "no weight")
    body.vertex_groups["BN01 Hips"].add([1], 1.0, "REPLACE")
    body.vertex_groups.new(name="Root").add([2], 0.5, "REPLACE")
    refused(root, "Body", "vertex 2", "Root", "no part")
    body.vertex_groups.remove(body.vertex_groups["Root"])
    arm.data.bones["BN04 A"].use_deform = False
    refused(root, "Body", "BN04 A", "Deform is off")
    arm.data.bones["BN04 A"].use_deform = True
    _, lines = export_model(root)
    first = records(lines, "v")[0]
    assert len(first) == 15, first  # x y z nx ny nz u v i0 i1 i2 i3 w0 w1 w2


@case
def mesh_part_holds_the_skinned_geometry():
    root, arm, body = person("meshpart")
    body.location = (0, 0, -1.0)  # the mesh part's pivot: the mesh origin, on the ground
    skin(body, arm, {v: {"BN02 Leg" if v < 4 else "BN03 Arm": 1.0} for v in range(8)})
    root.o3d.mesh_part = True
    _, lines = export_model(root)
    parts = records(lines, "part")
    assert len(parts) == 4 and parts[3][0] == "0", parts
    assert abs(float(parts[3][3]) + 1.0) < 1e-6, parts[3]
    # The bullet faces sit in the mesh part's section; each bone that moves
    # vertices carries a derived hit sphere.
    faces, at = {}, -1
    for line in lines:
        key = line.split()[:1]
        if key == ["cobj"]:
            at += 1
        elif key == ["cf"]:
            faces[at] = faces.get(at, 0) + 1
    assert list(faces) == [3], faces
    assert set(spheres(lines)) == {1, 2}, spheres(lines)


@case
def skinned_strips_split_by_the_oed_palette_rule():
    # Fifteen triangles, each wholly on a bone of its own: a table counts a
    # triangle's missing bones corner by corner (OED's rule), so the 14-bone
    # table takes no triangle bringing a 15th on three corners (14 + 3 > 16),
    # though the bones alone would fit.
    root, lod = model("palette")
    bones = {"BN01": ((0.0, 0.0, 0.0), None)}
    bones.update({f"BN{i:02d}": ((0.1 * i, 0.0, 0.0), "BN01") for i in range(2, 16)})
    arm = armature("palette Rig", lod, bones)
    me = bpy.data.meshes.new("Fifteen")
    me.from_pydata([(0.1 * k + dx, dy, 0.0) for k in range(15) for dx, dy in ((0, 0), (0.05, 0), (0, 0.05))], [],
                   [(3 * k, 3 * k + 1, 3 * k + 2) for k in range(15)])
    me.uv_layers.new(name="UVMap")
    body = link(bpy.data.objects.new("Fifteen", me), arm)
    skin(body, arm, {3 * k + c: {f"BN{k + 1:02d}": 1.0} for k in range(15) for c in range(3)})
    _, lines = export_model(root)
    tables = [[int(b) for b in t] for t in records(lines, "bones")]
    assert tables == [list(range(14)), [14]], tables


@case
def derived_hit_spheres_import_without_helpers():
    root, arm, body = person("derived")
    skin(body, arm, {v: {"BN02 Leg" if v < 4 else "BN03 Arm": 1.0} for v in range(8)})
    arm.data.bones["BN03 Arm"].o3d.hit_sphere = False
    stray = empty("_03 hit", None, (0.3, 0.0, 0.4))
    hang(stray, arm, "BN03 Arm")
    refused(root, "_03 hit", "Hit sphere is off")
    bpy.data.objects.remove(stray)
    _, lines = export_model(root)
    # The leg's sphere is the one its vertices give (the body's bottom face,
    # a 0.6 m square: its middle, reaching the corners); the arm's Hit sphere
    # is off and the hips move no vertex, so those sections store none.
    one = spheres(lines)
    assert set(one) == {1}, one
    assert abs(one[1][2] + 0.3) < 1e-4 and abs(one[1][3] - 0.3 * math.sqrt(2.0)) < 1e-4, one
    first = export.output_path(root)
    again = import_again([first])[0]
    # An own export imports with no helper: its spheres are the derived ones,
    # and the arm's section, which stores none, turns its Hit sphere off.
    assert not hit_helpers(), hit_helpers()
    bones = rig.rig_of(again).data.bones
    assert [bones[f"BN0{i}"].o3d.hit_sphere for i in (1, 2, 3)] == [True, True, False]
    again.o3d.output_path = os.path.join(OUT, "derived2", "derived.3di").replace("\\", "/")
    _, lines = export_model(again)
    same_spheres(one, spheres(lines))
    compare(first, export.output_path(again))


@case
def an_authored_hit_sphere_keeps_its_helpers():
    root, arm, body = person("hits")
    skin(body, arm, {v: {"BN02 Leg" if v < 4 else "BN03 Arm": 1.0} for v in range(8)})
    hit = empty("_hit", None, (0.0, 0.0, -0.4))
    hit.empty_display_type = "SPHERE"
    hit.empty_display_size = 0.25
    hang(hit, arm, "BN02 Leg")
    bounds = empty("_02 bounds", None, (0.0, 0.0, -0.4))
    bounds.empty_display_type = "CUBE"
    bounds.scale = (0.1, 0.2, 0.3)
    hang(bounds, arm, "BN02 Leg")
    _, lines = export_model(root)
    # The leg's sphere and box are its helpers'; the arm's, which has none,
    # the ones its vertices give.
    one = spheres(lines)
    assert set(one) == {1, 2}, one
    c = one[1]
    assert abs(c[2] + 0.4) < 1e-4 and abs(c[3] - 0.25) < 1e-4, c
    # Mission axes: x forward (Blender -Y), y left (Blender X).
    assert abs(c[4] + 0.2) < 1e-4 and abs(c[5] + 0.1) < 1e-4 and abs(c[6] + 0.7) < 1e-4, c
    first = export.output_path(root)
    again = import_again([first])[0]
    # Only the leg's sphere and box are not the derived ones.
    assert hit_helpers() == ["_02 bounds", "_02 hit"], hit_helpers()
    again.o3d.output_path = os.path.join(OUT, "hits2", "hits.3di").replace("\\", "/")
    _, lines = export_model(again)
    same_spheres(one, spheres(lines))
    compare(first, export.output_path(again))


@case
def a_sheet_stored_in_both_windings_keeps_its_normals():
    # A strip whose triangles come in both windings over the same vertices
    # (retail's two-sided wire, Baricd02), their normals leaning off the
    # faces: import puts each side on vertices of its own, so Blender holds
    # every corner's normal and export writes the strip back.
    folder_path = os.path.join(OUT, "twins")
    os.makedirs(folder_path, exist_ok=True)
    scene = os.path.join(folder_path, "twins.o3d")
    with open(scene, "w", encoding="utf-8") as f:
        f.write("o3d 1\nmodel TWINS\nmaterial FF_ST_OP\ntexture wire.tga\nmatflags 4\nlod 0 gnrc\npart 0 0 0 0\nstrip 0\n"
                "v 0 0 0 0.6 0 0.8 0 0\nv 1 0 0 0 0.6 0.8 1 0\nv 0 1 0 0 0 1 0 1\nv 1 1 0.2 0.28 0.96 0 1 1\n"
                "t 0 1 2\nt 2 1 0\nt 1 3 2\nt 2 3 1\npanm 0 0\n"
                "cobj 0\ncv 0 0 0\ncv 1 0 0\ncv 0 1 0\ncv 1 1 0.2\ncf 0 1 2 1 1\ncf 2 1 0 1 1\ncf 1 3 2 1 1\ncf 2 3 1 1 1\n")
    first = os.path.join(folder_path, "twins.3di")
    subprocess.run([CLI, "build", scene, "-o", first], check=True, capture_output=True)
    again = import_again([first])[0]
    again.o3d.output_path = os.path.join(OUT, "twins2", "twins.3di").replace("\\", "/")
    _, lines = export_model(again)
    assert len(records(lines, "t")) == 4, lines
    compare(first, export.output_path(again))


@case
def a_panm_flags_word_the_tracks_do_not_imply_is_noted_not_kept():
    # A stored PANM flags word that is not the one the row's tracks imply
    # (here a rotation mode with no rotation track) is what the scene cannot
    # express: import reports it and leaves the part's word derived (ADR
    # 0047: nothing stashed for a round trip), so the model comes back with
    # the word its tracks imply.
    folder_path = os.path.join(OUT, "panmword")
    os.makedirs(folder_path, exist_ok=True)
    scene = os.path.join(folder_path, "src.o3d")
    with open(scene, "w", encoding="utf-8") as f:
        f.write("o3d 1\nmodel PANMWORD\nmaterial FF_ST_OP\ntexture wire.tga\nlod 0 gnrc\npart 0 0 0 0\nstrip 0\n"
                "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\npanm 0 0 0x00000200\ncobj 0\n")
    first = os.path.join(folder_path, "panmword.3di")
    subprocess.run([CLI, "build", scene, "-o", first], check=True, capture_output=True)
    stored = records(scene_lines(first), "panm")
    assert len(stored) == 1 and int(stored[0][2], 0) == 0x200, stored
    ((_, imported, notes),) = importer.import_files(bpy.context, [first])
    assert imported is not None, notes
    assert any("part 01" in n and "0x00000200" in n and "0x00000000" in n for n in notes), notes
    pn1 = next(o for o in imported.children_recursive if rig.clean_name(o.name) == "PN01")
    assert pn1.o3d.panm_flags == -1 and len(pn1.o3d.tracks) == 0, (pn1.o3d.panm_flags, len(pn1.o3d.tracks))
    imported.o3d.output_path = os.path.join(OUT, "panmword2", "panmword.3di").replace("\\", "/")
    _, lines = export_model(imported)
    assert records(lines, "panm") == [["0", "0"]], records(lines, "panm")


@case
def registers_are_declared_in_oed_order():
    # OED collected the materials' registers, then the tracks', then the
    # lights' (every JOTAC model that declares registers keeps that order),
    # whatever order the records name them in.
    root, lod = model("regs")
    pn1 = empty("PN01", lod)
    body = box("Body", pn1)
    lamp_mat = bpy.data.materials.new("Lamp")
    lamp_mat.o3d.rgb_style, lamp_mat.o3d.rgb_register = 113, "LIGHTSWITCH0"
    body.data.materials.append(lamp_mat)
    pn2 = empty("PN02", pn1, (0.0, 0.1, 0.0))
    box("Door", pn2, (0.0, 0.1, 0.0), 0.03)
    track = pn2.o3d.tracks.add()
    track.target, track.style, track.register = "roty", 113, "DOOR_00"
    light = link(bpy.data.objects.new("LP", bpy.data.lights.new("LP", "POINT")), pn2)
    light.data.o3d.style, light.data.o3d.register = 113, "FLICKER"
    _, lines = export_model(root)
    assert records(lines, "register") == [["LIGHTSWITCH0"], ["DOOR_00"], ["FLICKER"]], records(lines, "register")
    # light: part x y z atten_start atten_end style rate register ...
    assert records(lines, "rgbgen")[0][1] == "0" and records(lines, "light")[0][8] == "2", lines


@case
def attach_helpers_make_the_tables_one_per_part_cannot():
    root, lod = model("attach")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    pn2 = empty("PN02", pn1, (0.0, 0.1, 0.0))
    box("Lid", pn2, (0.0, 0.1, 0.0), 0.03)
    pn3 = empty("PN03", pn1, (0.0, -0.1, 0.0))
    box("Door", pn3, (0.0, -0.1, 0.0), 0.03)
    # The attach helpers, in export order: two on the root (one per part
    # stores none there) and one on part 03.
    root.o3d.attach_points = "HELPERS"
    for name, parent, at, order in (("_attach", pn3, (0.0, 0.0, 0.05), 0), ("_01 attach", pn1, (0.0, 0.0, 0.0), 1),
                                    ("_attach", pn1, (0.0, 0.0, 0.2), 2)):
        empty(name, parent, at).o3d.order = order
    _, lines = export_model(root)
    # Mission axes: x forward (Blender -Y), z up; CXLT truncates to 16.16.
    rows = [[float(x) for x in r] for r in records(lines, "cxlt")]
    step = 1.0 / 65536.0
    assert len(rows) == 3 and abs(rows[0][0] - 0.1) < step and abs(rows[0][2] - 0.05) < step and \
        rows[1] == [0.0, 0.0, 0.0] and abs(rows[2][2] - 0.2) < step, rows
    first = export.output_path(root)
    again = import_again([first])[0]
    assert again.o3d.attach_points == "HELPERS"
    again.o3d.output_path = os.path.join(OUT, "attach2", "attach.3di").replace("\\", "/")
    export_model(again)
    compare(first, export.output_path(again))
    # No helper: an empty table, which also comes back.
    for ob in [o for o in again.children_recursive if export.HELPER_RE.match(rig.clean_name(o.name))]:
        bpy.data.objects.remove(ob)
    _, lines = export_model(again)
    assert records(lines, "cxlt") == [[]], records(lines, "cxlt")
    empty_table = export.output_path(again)
    back = import_again([empty_table])[0]
    assert back.o3d.attach_points == "HELPERS"
    back.o3d.output_path = os.path.join(OUT, "attach3", "attach.3di").replace("\\", "/")
    export_model(back)
    compare(empty_table, export.output_path(back))
    # One per part takes one attach point a part.
    back.o3d.attach_points = "PARTS"
    pn = next(o for o in back.children_recursive if rig.clean_name(o.name) == "PN02")
    empty("_attach", pn)
    empty("_02 attach", pn, (0.0, 0.0, 0.1))
    refused(back, "both the attach point of")
    # A retail table at section offsets that are not the parts' pivots
    # (MWalA2X pivots every part on the origin): import gives each row an
    # Empty, since export writes a part without one at its pivot.
    folder_path = os.path.join(OUT, "offsets")
    os.makedirs(folder_path, exist_ok=True)
    scene = os.path.join(folder_path, "offsets.o3d")
    with open(scene, "w", encoding="utf-8") as f:
        f.write("o3d 1\nmodel OFFSETS\nmaterial FF_ST_OP\ntexture wall.tga\nlod 0 bldg\n"
                "part 0 0 0 0\nstrip 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"
                "part 0 0 0 0\nstrip 0\nv 2 0 0 0 0 1 0 0\nv 3 0 0 0 0 1 1 0\nv 2 1 0 0 0 1 0 1\nt 0 1 2\n"
                "panm 0 0\npanm 1 0\ncobj 0\ncobj 0 2.5 0.25 0\ncxlt 2.5 0.25 0\n")
    first = os.path.join(folder_path, "offsets.3di")
    subprocess.run([CLI, "build", scene, "-o", first], check=True, capture_output=True)
    offsets = import_again([first])[0]
    offsets.o3d.output_path = os.path.join(OUT, "offsets2", "offsets.3di").replace("\\", "/")
    _, lines = export_model(offsets)
    assert records(lines, "cxlt") == [["2.5", "0.25", "0"]], records(lines, "cxlt")


@case
def an_occlusion_sphere_keeps_what_the_file_stores():
    root, lod = model("occsphere")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    box("OB-occonly", pn1, (0.5, 0.0, 0.0), 0.2)
    _, lines = export_model(root)
    # Without a `_sphere` the builder derives the record's sphere: none written.
    assert records(lines, "occ") == [["0", "0", "0"]], records(lines, "occ")
    again = import_again([export.output_path(root)])[0]
    assert not [o for o in again.children_recursive if rig.clean_name(o.name) == "_sphere"]
    # A retail-style record: its centre mirrored across the model's y (Blender
    # X), the radius the box's own.
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    root, lod = model("occmirror")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    occ = box("OB-occonly", pn1, (0.5, 0.0, 0.0), 0.2)
    sphere = empty("_sphere", occ, (-1.0, 0.0, 0.0))  # world (-0.5, 0, 0)
    sphere.empty_display_type = "SPHERE"
    sphere.empty_display_size = 0.2 * math.sqrt(3.0)
    _, lines = export_model(root)
    (occ_line,) = records(lines, "occ")
    # Mission axes: y left (Blender X).
    assert len(occ_line) == 7 and abs(float(occ_line[4]) + 0.5) < 1e-6, occ_line
    first = export.output_path(root)
    again = import_again([first])[0]
    helpers = [o for o in again.children_recursive if rig.clean_name(o.name) == "_sphere"]
    assert len(helpers) == 1 and helpers[0].parent.name.startswith("OB"), helpers
    again.o3d.output_path = os.path.join(OUT, "occmirror2", "occmirror.3di").replace("\\", "/")
    export_model(again)
    compare(first, export.output_path(again))
    # A sphere sits on an occlusion mesh.
    stray = empty("_sphere", helpers[0].parent.parent)
    refused(again, "_sphere", "occlusion mesh")
    bpy.data.objects.remove(stray)


@case
def first_person_gun_over_64_parts_refused():
    gun, glod = model("biggun")
    bones = {"BN01": ((0, 0, 0), None)}
    bones.update({f"BN{i:02d}": ((0.01 * i, 0, 0.5), "BN01") for i in range(2, 66)})
    arm = armature("biggun Rig", glod, bones)
    body = box("Body", None, (0.3, 0, 0.5))
    hang(body, arm, "BN10")
    arms, alod = model("bigarms")
    arms.parent = gun
    hands = box("Hands", alod, (0, 0, 0.5), 0.1)
    skin(hands, arm, {v: {"BN02": 1.0} for v in range(8)})
    refused(gun, "65 parts", "64")


@case
def stock_arm_rig_checked_on_a_first_person_gun():
    to_blender = o3dtext.blender_axes()
    bones = {f"BN{i + 1:02d}": (to_blender(p), f"BN{parent + 1:02d}" if i else None)
             for i, (parent, p) in enumerate(addon.assembly.STOCK_ARMS)}
    bones["BN38 Body"] = ((0.0, -0.3, 0.6), "BN06")
    gun, glod = model("stockgun")
    arm = armature("stockgun Rig", glod, bones)
    body = box("Body", None, (0.0, -0.3, 0.6))
    hang(body, arm, "BN38 Body")
    gun.o3d.rows.add()  # a clip table: a first-person gun
    notes, _ = export_model(gun)
    assert not any("stock first-person arms" in n for n in notes), notes
    arm.data.bones["BN10"].name = "BN10 moved"
    with rig.editing(bpy.context, arm) as edit_bones:
        edit_bones["BN10 moved"].head.x += 0.05
    notes, _ = export_model(gun)
    assert any("stock first-person arms" in n and "part 10" in n for n in notes), notes


@case
def stock_arms_table_is_armsg():
    # assembly.STOCK_ARMS transcribes ArmsG.3di's parts 0-36 (each part's
    # parent and pivot): the reference model, read by the add-on's own reader
    # (opennova-3di scene, read_o3d), must give the table to the bit. The
    # retail leg: without the reference file it returns (addon_harness).
    path = addon_harness.retail_asset("ArmsG.3di")
    if path is None:
        return
    out = os.path.join(OUT, "armsg", "ArmsG.o3d")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([CLI, "scene", path, "-o", out], check=True, capture_output=True)
    parts = importer.read_o3d(out)["lods"][0]["parts"]
    table = addon.assembly.STOCK_ARMS
    assert len(parts) >= len(table), len(parts)
    got = tuple((p["parent"], tuple(p["pivot"])) for p in parts[:len(table)])
    assert got == table, [(f"part {i + 1:02d}", a, b) for i, (a, b) in enumerate(zip(got, table)) if a != b]


@case
def a_part_on_another_armatures_bone_is_refused():
    # A `!` armature is left out, and so is its pose: a mesh or a part hung
    # from one of its bones would be read as that armature stands posed, on a
    # bone the model does not have.
    root, lod = model("foreign")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    helper = armature("!Helper", pn1, {"BN01": ((0.0, 0.0, 0.0), None), "BN02": ((0.0, 0.2, 0.3), "BN01")})
    trim = box("Trim", None, (0.0, 0.1, 0.3), 0.02)
    hang(trim, helper, "BN01")
    refused(root, "Trim", "!Helper", "BN01")
    bpy.data.objects.remove(trim)
    pn2 = empty("PN02", None, (0.0, 0.2, 0.3))
    hang(pn2, helper, "BN02")
    box("Lid", pn2, (0.0, 0.0, 0.0), 0.02)
    refused(root, "PN02", "!Helper", "BN02")


@case
def a_geometry_nodes_uv_map_is_the_second():
    # UV1 is read from the meshes as export evaluates them: a second UV map a
    # Geometry Nodes modifier adds is the detail stage's.
    root, lod = model("gnuv")
    ob = box("Body", empty("PN01", lod))
    tree = bpy.data.node_groups.new("detail uv", "GeometryNodeTree")
    tree.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    inp, outp = tree.nodes.new("NodeGroupInput"), tree.nodes.new("NodeGroupOutput")
    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.data_type, store.domain = "FLOAT2", "CORNER"
    store.inputs["Name"].default_value = "Detail"
    tree.links.new(inp.outputs[0], store.inputs["Geometry"])
    tree.links.new(store.outputs["Geometry"], outp.inputs[0])
    ob.modifiers.new("detail uv", "NODES").node_group = tree
    _, lines = export_model(root)
    assert "uv1 1" in lines, lines[:6]
    # x y z nx ny nz u v u1 v1: the stored (0, 0), D3D's v running down.
    first = records(lines, "v")[0]
    assert len(first) == 10 and first[8:] == ["0", "1"], first


@case
def a_mesh_hung_from_the_last_arm_bone_shares_the_rig():
    # A mesh hung from an arms bone is skinned wholly on it once the arms
    # deform with the gun's rig, so its bone is one the arms' weights reach:
    # a watch on the last bone is no reason to refuse.
    gun, glod = model("watchgun")
    theirs = armature("watchgun Rig", glod, GUN_BONES)
    arms, alod = model("watcharms")
    own = armature("watcharms Rig", alod, {n: GUN_BONES[n] for n in ("BN01", "BN02 L Hand", "BN03 R Hand")})
    hands = box("Hands", alod, (0.0, 0.0, 0.5), 0.2)
    skin(hands, own, {v: {"BN01" if v < 4 else "BN02 L Hand": 1.0} for v in range(8)})
    watch = box("Watch", None, (-0.2, 0.0, 0.5), 0.02)
    hang(watch, own, "BN03 R Hand")
    rig.share_rig(bpy.context, arms, gun)
    assert rig.rig_of(arms) == theirs and rig.skin_rig(watch) == theirs
    arms.o3d.export_bullet_faces = False
    _, lines = export_model(arms)
    assert len(records(lines, "part")) == 3, records(lines, "part")


@case
def a_builder_note_names_its_object():
    # opennova-3di notes a volume that is not convex on its scene line, which
    # export tells by the object it came from.
    root, lod = model("dented")
    pn1 = empty("PN01", lod)
    box("Body", pn1)
    dent = box("CB-colonly", pn1, size=0.1)
    dent.data.vertices[6].co = (0.0, 0.0, 0.0)
    notes, _ = export_model(root)
    assert any(n.startswith("CB-colonly: ") and "not convex" in n for n in notes), notes


@case
def export_all_passes_by_what_blender_does_not_evaluate():
    # A model in an excluded collection keeps the matrices Blender last
    # evaluated: Export All passes it by, and exporting it alone is refused.
    shown, slod = model("shown")
    box("Body", empty("PN01", slod))
    parked, plod = model("parked")
    box("Body", empty("PN01", plod))
    collection = bpy.data.collections.new("Parked")
    bpy.context.scene.collection.children.link(collection)
    for ob in [parked] + list(parked.children_recursive):
        for holder in list(ob.users_collection):
            holder.objects.unlink(ob)
        collection.objects.link(ob)
    bpy.context.view_layer.layer_collection.children["Parked"].exclude = True
    try:
        for root in (shown, parked):
            os.makedirs(folder(root), exist_ok=True)
        assert bpy.ops.opennova_3di.export_all() == {"FINISHED"}
        assert os.path.isfile(export.output_path(shown)) and not os.path.exists(export.output_path(parked))
        refused(parked, "parked", "does not evaluate")
    finally:
        bpy.data.collections.remove(collection)


@case
def export_all_animations_passes_by_what_blender_does_not_evaluate():
    # A rig in an excluded collection keeps the pose Blender last evaluated,
    # so its clips would sample that pose: Export All Animations passes the
    # model by, and exporting its clips alone is refused.
    # Table names of at most 7 characters: <table>_rst.bad must fit the 15 a
    # retail archive holds.
    shown, shown_rig = clip_scene.gun(OUT, "shownc")
    parked, parked_rig = clip_scene.gun(OUT, "parkedc")
    for root, arm in ((shown, shown_rig), (parked, parked_rig)):
        idle = clip_scene.clip(arm, f"{root.name} Idle", clip_scene.bolt_turns())
        clip_scene.set_rows(root, [("anim_wpn_idle", [idle])])
    collection = bpy.data.collections.new("Parked")
    bpy.context.scene.collection.children.link(collection)
    for ob in [parked] + list(parked.children_recursive):
        for holder in list(ob.users_collection):
            holder.objects.unlink(ob)
        collection.objects.link(ob)
    bpy.context.view_layer.layer_collection.children["Parked"].exclude = True
    try:
        assert bpy.ops.opennova_3di.export_all_anim() == {"FINISHED"}
        assert os.path.isfile(shown.o3d.adm_path) and not os.path.exists(parked.o3d.adm_path)
        try:
            animation.export_animations(bpy.context, parked)
        except o3dtext.ExportError as e:
            assert "does not evaluate" in str(e) and "parkedc" in str(e), str(e)
        else:
            raise AssertionError("parkedc exported its clips although Blender does not evaluate it")
    finally:
        bpy.data.collections.remove(collection)


@case
def a_loose_vertex_bounds_no_bone():
    # The file keeps the vertices triangles draw, and import measures a bone's
    # section from those: a loose vertex the bone moves bounds nothing, so
    # the model comes back without helpers.
    root, arm, body = person("loose")
    body.data.vertices.add(1)
    body.data.vertices[8].co = (0.0, 0.0, 2.0)
    skin(body, arm, {v: {"BN02 Leg" if v < 4 else "BN03 Arm": 1.0} for v in range(9)})
    export_model(root)
    first = export.output_path(root)
    again = import_again([first])[0]
    assert not hit_helpers(), hit_helpers()
    again.o3d.output_path = os.path.join(OUT, "loose2", "loose.3di").replace("\\", "/")
    export_model(again)
    compare(first, export.output_path(again))


@case
def records_on_the_mesh_part_sit_on_the_root():
    # A skinned model's mesh part holds its skinned geometry alone and hangs
    # nothing: a user point and a light the file keeps there import on the
    # root and a hit sphere there is left out, each noted, and the model
    # exports.
    folder_path = os.path.join(OUT, "meshpart")
    os.makedirs(folder_path, exist_ok=True)
    scene = os.path.join(folder_path, "meshpart.o3d")
    with open(scene, "w", encoding="utf-8") as f:
        f.write("o3d 1\nmodel MESHPART\nskinned 1\nmaterial VS_SKBASIC\ntexture skin.tga\nlod 0 gnrc\n"
                "part 0 0 0 0\npart 0 0 0 0.5\npart 0 0 0 -1\nstrip 0\nbones 0 1\n"
                "v 0 0 0 0 0 1 0 0 0 0 0 0 1 0 0\nv 0.1 0 0 0 0 1 1 0 0 0 0 0 1 0 0\n"
                "v 0 0.1 0.5 0 0 1 0 1 1 1 1 1 1 0 0\nt 0 1 2\npanm 0 0\npanm 1 0\npanm 2 0\n"
                "userpoint grip 0 0 -1 0 0 1 2 71\nlight 2 0 0 -1 0 4 24 0 0 255 255 255 0 0 0 0x00\n"
                "cobj 0\ncobj 0 0 0 0.5\ncobj 0 0 0 -1\ncsphere 0 0 -1 0.3\n")
    first = os.path.join(folder_path, "meshpart.3di")
    subprocess.run([CLI, "build", scene, "-o", first], check=True, capture_output=True)
    ((_, imported, notes),) = importer.import_files(bpy.context, [first])
    assert imported is not None, notes
    assert imported.o3d.mesh_part
    for fragment in ("user point grip: on the mesh part", "light 0: on the mesh part", "collision section 2"):
        assert any(fragment in n for n in notes), (fragment, notes)
    arm = rig.rig_of(imported)
    point, light = (next(o for o in imported.children_recursive if rig.clean_name(o.name) == n)
                    for n in ("UPG01 grip", "LP01"))
    assert all(o.parent == arm and o.parent_bone == "BN01" for o in (point, light))
    assert not hit_helpers(), hit_helpers()
    imported.o3d.output_path = os.path.join(OUT, "meshpart2", "meshpart.3di").replace("\\", "/")
    _, lines = export_model(imported)
    assert records(lines, "userpoint")[0][7] == "0" and records(lines, "light")[0][0] == "0", lines


@case
def a_failed_import_leaves_nothing():
    # A file whose build fails leaves nothing of itself: no object, mesh,
    # light, material, armature, image or collection.
    root, lod = model("leftover")
    pn1 = empty("PN01", lod)
    box("Body", pn1).data.materials.append(bpy.data.materials.new("Paint"))  # a swatch the import loads
    link(bpy.data.objects.new("LP", bpy.data.lights.new("LP", "POINT")), pn1)
    box("CB-colonly", pn1, size=0.06)
    export_model(root)
    first = export.output_path(root)
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    kinds = (bpy.data.objects, bpy.data.meshes, bpy.data.lights, bpy.data.materials, bpy.data.armatures,
             bpy.data.images, bpy.data.collections)

    def names():
        return [sorted(block.name for block in kind) for kind in kinds]
    before = names()
    held = importer.Builder.attach_points

    def refuse(self, lod_objects):
        raise RuntimeError("refused")
    importer.Builder.attach_points = refuse
    try:
        ((_, imported, error),) = importer.import_files(bpy.context, [first])
    finally:
        importer.Builder.attach_points = held
    assert imported is None and "refused" in str(error), error
    assert names() == before, (before, names())


if FAILURES:
    raise SystemExit(f"model_test: {len(FAILURES)} failed: {', '.join(FAILURES)}")
print("MODEL_TEST_OK")
