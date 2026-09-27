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
import tempfile
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addon_harness  # noqa: E402

import bpy  # noqa: E402
from mathutils import Vector  # noqa: E402

addon, CLI = addon_harness.load()
export, importer, rig, o3dtext = addon.export, addon.importer, addon.rig, addon.o3dtext
OUT = tempfile.mkdtemp(prefix="opennova_model_test_")
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
    assert rig.number_parts(arm) == 4
    names = sorted(b.name for b in arm.data.bones)
    assert names == ["BN01 Hips", "BN02 Spine", "BN03 Arm.L", "BN04 Arm.R", "Root", "ik_hand"], names
    assert mesh.vertex_groups[0].name == "BN02 Spine"
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


if FAILURES:
    raise SystemExit(f"model_test: {len(FAILURES)} failed: {', '.join(FAILURES)}")
print("MODEL_TEST_OK")
