# Scene -> .o3d text -> opennova-3di.
#
# A model is read by the NovaLogic ASE/OED object-naming convention (the
# retired importer/exporter's, `classify_name` in the retired
# engine/formats/oed/convert_internal.cpp, a port of [orig: ConvertToInternal
# @ 0x4268B3]), over the part model rig.py reads; docs/threedi/
# scene-naming-contract.md keeps the table:
#
#   model root    Empty whose children are the model's LOD roots: one .3di.
#                 It carries the model name, output path (empty: the root's
#                 name beside the .blend) and the collision LOD. Each model
#                 exports in its root's own frame, facing its -Y, so placing
#                 or turning a model never changes its bytes.
#   LOD root      Empty with an integer `_lod_index` custom property (0 = the
#                 primary LOD), its threshold and RMDL type. A LOD root with no
#                 parts is legal (retail ships them).
#   parts         PN## empties (a static model) or the BN## bones of the
#                 model's one rig (an animated or skinned model): rig.py. A
#                 rotated PN## is a PANM rotation frame (an MTRX row); a bone
#                 carries its track frame as a property.
#   a mesh        render geometry of the part it sits on (rig.part_of): any
#                 mesh under a PN##, or hung from a part bone. A mesh whose
#                 Armature modifier deforms it with the rig is skinned
#                 geometry, authored on the root part (or on the model's mesh
#                 part); in a skinned model a mesh hung from a bone is skinned
#                 too, wholly on its bone.
#   _## center    mesh helper of a rigid part that draws nothing: its first
#                 vertex is the point the part's bounds sit on, and in the
#                 collision LOD its section's collision vertex.
#   _## attach    Empty on a part of the collision LOD: that section's CXLT
#                 attach point.
#   _## hit       Empty on a bone of a skinned model: that bone's hit sphere
#                 (its origin the centre, its display size times its scale the
#                 radius); `_## bounds`, a box Empty, the section's bounds box.
#                 Without them the LOD 0 vertices the bone moves give both
#                 (bone_bounds); a bone whose Hit sphere is off stores none.
#   UP<c>## <lbl> Empty: user point, type letter c (G gameplay, S effect), on
#                 its part (00: on none, -1), label = the USRP name; it faces
#                 along its local +Z. Its `order` property keeps the USRP order.
#   LP##[a..]     Light (on LOD 0): a LGHT light owned by its part; a spot
#                 light is a cone about its local -Z, the way Blender draws one.
#   <code>##[a..]-colonly  mesh on LOD 0: a collision volume of type <code>
#                 (CB CS CC CL CV CA VC BB CD CT CM VK CF LP DH DM DL CP; any
#                 other C, D, L or V code is type 0, import's CX) in its
#                 part's section; the builder takes its planes from its faces
#                 by the OED rule (a ladder, CL, faces its last face's plane).
#   OB##/OS##/OP##[-MM]/OH## (+ [a..], -occonly)  mesh on LOD 0: an occlusion
#                 record in its part's section (OB occluder, OS open, OP a
#                 window to the exterior or, with -MM, a portal to section MM).
#   _sphere       Empty on an occlusion mesh: that record's sphere as a file
#                 stores it (its origin the centre, its display size times its
#                 scale the radius); without it the mesh's vertices give it.
#   Material      any name: its Shader and Export order properties and
#                 Blender's own settings (materials.py).
#   !name         ignored.
# A helper sits on a part like everything else; the `##` in its name is
# optional, and one that names another part than the one it sits on is an
# error. Blender's own `.001` duplicate suffixes are stripped before a name is
# read. Any other object is reported and left out.

import math
import os
import re
from array import array

import bpy
from mathutils import Matrix, Vector

from . import assembly, materials
from .o3dtext import (CTRL_REFERENCE_THRESHOLD, ExportError, ModelSpace, Notes, at_world_origin, cli_notes,
                      export_text, fmt, quoted)
from .rig import (PART_RE, WEIGHT_EPS, clean_name, descendants, ignored, is_lod_root, is_model_root,
                  lod_of, lod_parts, lod_roots, model_of, model_roots, part_bones, part_of, rig_of)


POINT_RE = re.compile(r"^UP([A-Za-z])(\d{2})?(?: (.*))?$")
LIGHT_RE = re.compile(r"^LP(\d{2})?([a-z]*)$")
HELPER_RE = re.compile(r"^_(?:(\d{2}) )?(center|attach|hit|bounds|sphere)$")
OCCLUSION_RE = re.compile(r"-occ?only$", re.IGNORECASE)
OCC_RE = re.compile(r"^(OB|OS|OP|OH)(\d{2})?([a-z]*)(?:-(\d{2}))?-occonly$")
OCC_TYPES = {"OB": 0, "OS": 1, "OP": 2, "OH": 4}  # OP with -MM is a portal, type 3

# The collidable-type codes (classify_name; runtime meanings in
# docs/world/world-wac-ai-re.md §15).
VOLUME_CODES = {"CB": 1, "CS": 2, "CC": 3, "CL": 4, "CV": 5, "CA": 6, "VC": 7, "BB": 8, "CD": 9, "CT": 10,
                "CM": 11, "VK": 12, "CF": 13, "LP": 14, "DH": 16, "DM": 17, "DL": 18, "CP": 19}
# classify_name leaves any other pair it reads as a volume at type 0; of the
# collision letters, a C, D, L or V pair is one here (B and O pairs are blink
# boxes and occlusion). 36 of the 48 retail first-person weapons carry a type
# 0 box per section, which import names CX.
UNLISTED_TYPE_LETTERS = "CDLV"
UNLISTED_TYPE_CODE = "CX"
VOLUME_RE = re.compile(r"^([A-Z]{2})([VSWLO]*)(\d{2})?([a-z]*)-colonly$")
BLINK_LETTER_BITS = {"V": 0x2, "S": 0x4, "W": 0x8, "L": 0x10, "O": 0x20}

# A PANM row's tracks in their record order (rotation x, y, z, scale x, y,
# z, translation), the order OED collected their registers in.
TRACK_ORDER = ("rotx", "roty", "rotz", "scalex", "scaley", "scalez", "trans")

# A strip holds at most 65535 indices (u16), so 21,845 triangles; export
# starts another strip there, and a skinned one's bone table holds at most 16
# parts (the skinned palette, MAX_SKIN_MATRICES), filled by OED's rule
# (skinned_strips).
STRIP_TRIANGLES = 65535 // 3
SKIN_TABLE = 16
# Every first-person bone buffer is a 64-entry array: a gun of more parts
# overruns it [orig: Player_RenderFirstPersonViewModel @ 0x4DED60, its
# 64-matrix locals; the part-count test @ 0x4DEF8B], and the arms draw with
# the gun's array by index [the arms submit @ 0x4DF088].
FIRST_PERSON_PARTS = 64
# A file retail packs (.3di, .adm, .bad, textures) is named in 15 bytes (a
# PFF entry's name field, contract C3).
FILE_NAME_BYTES = 15
SCENE_LINE = re.compile(r"^scene text:(\d+): (.*)$")
SECTION_WORDS = re.compile(r"collision section (\d+)")


def dup_rank(letters):
    """A duplicate suffix's place: '' 0, 'a' 1 .. 'z' 26, 'aa' 27, ..."""
    rank = 0
    for c in letters:
        rank = rank * 26 + (ord(c) - ord("a") + 1)
    return rank


def derived_panm_flags(tracks):
    """The PANM flags word build derives from a part's tracks (target, style,
    axis): rotation type 2 for any live rotation track, scale type 2 for any
    live scale track, the translate axis for a live translation."""
    live = [(target, axis) for target, style, axis in tracks if style != 0]
    rot = any(t.startswith("rot") for t, _ in live)
    scale = any(t.startswith("scale") for t, _ in live)
    axis = next((a for t, a in live if t == "trans"), 0)
    return (2 if scale else 0) | ((2 if rot else 0) << 8) | (axis << 24)


def bone_bounds(points):
    """A bone section's hit sphere and box from the LOD 0 vertices the bone
    moves (mission axes): the box around them, and the sphere about its
    middle reaching the farthest of them, (centre, radius, [min x y z, max x
    y z]) (OED's WriteCOBJ skinned rule, 5fc5b4f6a^
    engine/formats/oed/export_3di.cpp). Export writes it for a bone without
    `_hit` / `_bounds` empties, and import makes them only where the file's
    sphere or box is not this."""
    mn = [min(p[k] for p in points) for k in range(3)]
    mx = [max(p[k] for p in points) for k in range(3)]
    centre = [(mn[k] + mx[k]) * 0.5 for k in range(3)]
    radius = max(math.dist(p, centre) for p in points)
    return centre, radius, mn + mx


def shared_vertex(strip, key, vert, corners):
    """A triangle corner's vertex in its strip: the one it shares with every
    corner of the same key, but never one another corner of its triangle
    already took. Two corners of a Blender triangle are always two vertices,
    and a sliver whose corners coincide (retail ships them) keeps all three,
    as the CLI takes a triangle only on three vertices."""
    at = strip["index"].get(key)
    if at is None or at in corners:
        at = len(strip["verts"])
        strip["verts"].append(vert)
        strip["index"].setdefault(key, at)
    return at


def new_strip(ob):
    return {"verts": [], "index": {}, "tris": [], "meshes": [ob.name], "table": [], "slots": {}}


def fixed(value, scale, lo, hi, what):
    """`value` in the word the text carries it into (`value * scale` rounded,
    lo..hi); an ExportError naming `what` when it does not fit, which the CLI
    would refuse or the writer wrap or clamp."""
    raw = round(value * scale) if math.isfinite(value) else None
    if raw is None or not lo <= raw <= hi:
        raise ExportError(f"{what} is {value:g}; the file holds {lo / scale:g} to {hi / scale:g}")
    return raw


def slot_material(ev, slot):
    """The material of an evaluated object's slot (a Geometry Nodes or
    object-linked material included), as the original data-block: the
    evaluated copy does not carry the add-on's properties."""
    mat = ev.material_slots[slot].material if slot < len(ev.material_slots) else None
    return mat.original if mat is not None else None


def order_key(ob):
    order = ob.o3d.order
    return (order if order >= 0 else 1 << 30, clean_name(ob.name), ob.name)


def point_key(label, ob):
    """A user point's USRP place: its `order`, then (our own rule for points
    without one; retail's exporter kept the scene order) its label, so seats
    `sitex01`, `sitex02` stay in seat order whichever parts they sit on."""
    order = ob.o3d.order
    return (order if order >= 0 else 1 << 30, label.lower(), clean_name(ob.name), ob.name)


def output_path(model):
    """The .3di a model writes: its output path, else //<model root name>.3di
    beside the .blend; absolute."""
    return bpy.path.abspath(model.o3d.output_path or f"//{clean_name(model.name)}.3di")


def is_first_person(scene, model, lp):
    """A first-person gun: a rigid model on a rig of its own that an arms
    model of the scene deforms with, or that carries a clip table."""
    if lp.rig is None or lp.shared or lp.skinned:
        return False
    if len(model.o3d.rows) > 0:
        return True
    return any(m is not model and rig_of(m) == lp.rig for m in model_roots(scene))


class ExportRun:
    """One export of one or more models (Export Model, Export All Models): the
    output files its models write, so two never write one file, and the
    texture names they give (materials.TextureRun)."""

    def __init__(self):
        self.paths = {}  # normalized output path -> the model writing it
        self.textures = materials.TextureRun()

    def claim(self, path, model):
        key = os.path.normcase(os.path.normpath(path))
        other = self.paths.get(key)
        if other is not None and other != model.name:
            raise ExportError(f"{model.name}: {path} is also {other}'s output; give each model its own output path")
        self.paths[key] = model.name


class Lod:
    """One LOD root, classified: its parts (rig.LodParts) and what sits on
    them."""

    def __init__(self, lp):
        self.lp = lp
        self.root = lp.root
        self.parts = lp.parts
        self.skinned = lp.skinned  # meshes deforming with the rig
        self.meshes = {}     # part index -> [mesh]: geometry on the part
        self.centers = {}    # part index -> `_center` mesh
        self.anchors = []    # (part index, `_attach` empty)
        self.spheres = {}    # part index -> `_hit` empty
        self.boxes = {}      # part index -> `_bounds` empty
        self.points = []     # (type letter, part index or -1, label, object)
        self.lights = []     # (part index, object)
        self.occluders = []  # (type, section, connecting, object)
        self.occ_spheres = {}  # occlusion mesh name -> its `_sphere` empty
        self.volumes = []    # (type, flags, section, object, sort key)

    @property
    def authored(self):
        """The part a skinned model's skinned geometry is authored on: its
        mesh part, else the root."""
        return self.lp.mesh_part if self.lp.mesh_part is not None else 0


class Exporter(Notes):
    def __init__(self, context, model, run):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.export_run = run
        self.props = model.o3d  # the model's name, output path, collision LOD
        self.settings = self.scene.o3d  # Write textures
        self.space = None  # set in run(): the model root's frame
        self.depsgraph = None  # set in run(), with every rig at rest
        self.instancers = set()  # objects that make instances (set in run())
        self.registers = []
        self.materials = materials.ModelMaterials(self, run)  # its materials and the texture files they name
        self.frames = []
        self.skinned = False
        self.uv1 = False
        self.bone_points = {}  # skinned LOD 0: part -> rest positions it moves
        self.sections = {}  # collision section -> its part and the meshes giving its faces
        self.notes = []

    # --- helpers ------------------------------------------------------------
    def declare_registers(self, lods):
        """The CTRL table in OED's collection order (the retired port's
        collect_control_registers, 5fc5b4f6a^ engine/formats/oed/
        export_3di.cpp): the materials' registers in export order, each one's
        U, V, alpha and RGB generators above style 112 and then a register-
        driven flipbook; then every LOD's tracks above 0x70, part by part,
        rotations x, y, z, the scales, the translation; then LOD 0's lights.
        Each name comes once, at its first use. Every JOTAC model whose
        registers anything references keeps this order (256 models; 30 more
        declare only names nothing references). Called once the materials are
        in order; the records then take their indices from it."""
        self.registers = []
        for mat in self.materials.used:
            if mat is None:
                continue
            p = mat.o3d
            for style, name, what in ((p.u_style, p.u_register, "the U gen"), (p.v_style, p.v_register, "the V gen"),
                                      (p.alpha_style, p.alpha_register, "the alpha gen"),
                                      (p.rgb_style, p.rgb_register, "the RGB gen")):
                if style > CTRL_REFERENCE_THRESHOLD:
                    self.register(name, f"{mat.name}: {what}")
            if p.anim_type == 1:
                self.register(p.anim_register, f"{mat.name} texture flipbook")
        for lod in lods:
            for part in lod.parts:
                holder = part.empty if part.empty is not None else part.bone
                if holder is None:
                    continue
                what = part.name if part.empty is not None else f"{part.rig.name} bone {part.name}"
                for t in sorted(holder.o3d.tracks, key=lambda t: TRACK_ORDER.index(t.target)):
                    if t.style > CTRL_REFERENCE_THRESHOLD:
                        self.register(t.register, f"{what} track {t.target}")
        for _, ob in sorted(lods[0].lights, key=lambda e: order_key(e[1])):
            if ob.data.o3d.style > CTRL_REFERENCE_THRESHOLD:
                self.register(ob.data.o3d.register, f"{ob.name} colour")

    def register(self, name, what):
        """A CTRL register's index, declaring it on first use. An empty name is
        a register too (retail's IBlock02 declares one and drives a door by
        it), so it exports, with a note in case the name was forgotten."""
        if not name:
            self.note(f"{what}: a register-driven style (above 112) with no register name reads the unnamed "
                      "register")
        if len(name) > 24:
            raise ExportError(f"{what}: the register name '{name}' exceeds 24 characters (its CTRL field)")
        if name not in self.registers:
            self.registers.append(name)
        return self.registers.index(name)

    def evaluated(self, ob):
        """The object as the depsgraph evaluates it (modifiers applied). An
        object Blender does not evaluate (disabled in viewports, or in a hidden
        or excluded collection) reads as its base mesh, which is right unless a
        modifier or a shape key would change it."""
        if ob.name in self.instancers:
            raise ExportError(f"{ob.name}: it makes instances (Geometry Nodes, or instancing), which export cannot "
                              "read: realize them (a Realize Instances node, or Make Instances Real)")
        ev = ob.evaluated_get(self.depsgraph)
        if not ev.is_evaluated:
            shaped = ob.type == "MESH" and ob.data.shape_keys is not None
            if shaped or any(m.show_viewport and m.type != "ARMATURE" for m in ob.modifiers):
                raise ExportError(f"{ob.name}: Blender does not evaluate it (it or its collection is disabled in "
                                  "viewports, or its collection is excluded), so its modifiers or shape keys "
                                  "would be lost: enable it in viewports, or apply them")
        return ev

    def frame_of(self, rotation, what):
        """The MTRX row of a part frame given as a Blender rotation in the
        model: that rotation as a mission frame R (row-major, p' = p R); 0 for
        the identity. A mirrored frame (a negative scale) is written as the
        reflection it is: retail stores five (dKA501x, dtaxi1, Dtaxi1X, MWalG,
        PKM_1st), and a track turns the other way about it."""
        if rotation.determinant() < 0:
            self.note(f"{what}: its frame is mirrored (a negative scale), so its tracks turn the other way, as in "
                      "the retail models that store one")
        q = self.space.mission_rotation(rotation)
        if all(abs(q[i][j] - (1.0 if i == j else 0.0)) < 1e-6 for i in range(3) for j in range(3)):
            return 0
        r = tuple(round(q[j][i], 6) for i in range(3) for j in range(3))
        if r not in self.frames:
            if len(self.frames) == 255:
                raise ExportError(f"{what}: a 256th part frame; a PANM row selects one by a byte")
            self.frames.append(r)
        return self.frames.index(r) + 1

    def part_frame(self, part):
        """A part's MTRX row: a PN##'s own turn in the model, a bone's track
        frame (a turn of the rig's axes); a mesh part has none."""
        if part.empty is not None:
            return self.frame_of(self.space.local(part.empty.matrix_world).to_3x3().normalized(), part.name)
        if part.bone is not None:
            arm = self.space.local(part.rig.matrix_world).to_3x3().normalized()
            what = f"{part.rig.name} bone {part.name}"
            return self.frame_of(arm @ Matrix(part.bone.o3d.frame), what)
        return 0

    def pivot(self, part):
        """A part's pivot, mission axes."""
        return self.space.mission(self.space.local(part.matrix_world()).translation)

    def world(self, ob):
        return self.space.world(ob)

    # --- classification -----------------------------------------------------
    def on_part(self, ob, part, number, lod, none_ok=False, root_ok=False):
        """The part an object sits on, checked against the `##` its name may
        give: an error when they disagree, when it sits on no part, or on a
        part the LOD lacks. `00` names no part where none_ok (-1) and the root
        where root_ok (a light's, as OED clamped it). A LOD whose bones another
        LOD holds (a later LOD of a skinned model, arms on a gun's rig) has
        nothing of its own to hang a helper from: there a helper sits under
        the LOD root and its `##` names its part."""
        if number is not None:
            named = int(number) - 1
            if named < 0 and none_ok:
                if part is not None:
                    raise ExportError(f"{ob.name}: its name says no part (00), but it sits on part {part + 1:02d}")
                return -1
            if named < 0 and root_ok:
                named = 0
            if named < 0:
                raise ExportError(f"{ob.name}: parts start at 01")
            if part is not None and named != part:
                raise ExportError(f"{ob.name}: its name says part {named + 1:02d}, but it sits on part "
                                  f"{part + 1:02d} ({self.part_name(lod, part)})")
            if part is None and lod.lp.rig is not None and lod_of(lod.lp.rig) is not lod.root:
                part = named
        if part is None:
            raise ExportError(self.unplaced(ob, lod))
        if part >= len(lod.parts):
            raise ExportError(f"{ob.name}: it sits on part {part + 1:02d}, which {lod.root.name} lacks (it has "
                              f"{len(lod.parts)})")
        return part

    @staticmethod
    def part_name(lod, index):
        return lod.parts[index].name if 0 <= index < len(lod.parts) else f"part {index + 1:02d}"

    @staticmethod
    def unplaced(ob, lod):
        """Why an object sits on no part, and what to do."""
        rig = lod.lp.rig
        if ob.parent is not None and ob.parent_type == "BONE":
            return (f"{ob.name}: it hangs from bone '{ob.parent_bone}' of {ob.parent.name}, which is no part "
                    "(a part bone is named BN##; Number Parts names them)")
        if rig is not None:
            return (f"{ob.name}: it sits on no part: parent it to a bone of {rig.name} (Ctrl+P > Bone), or give it "
                    f"an Armature modifier on {rig.name}")
        return f"{ob.name}: it sits on no part: put it under a PN## empty (Add Part from Selection)"

    def classify(self, lp):
        lod = Lod(lp)
        root = lp.root
        primary = lp.index == 0
        skip = set(lp.skinned) | ({lp.rig} if lp.rig is not None else set())

        def lod0_only(ob, what):
            self.note(f"{ob.name}: {what} are read from LOD 0 only; not exported from {root.name}")

        def once(table, part, ob, what):
            if part in table:
                raise ExportError(f"{root.name}: '{table[part].name}' and '{ob.name}' are both the {what} of "
                                  f"{self.part_name(lod, part)}")
            table[part] = ob

        for ob in descendants(root):
            if ob in skip or ignored(ob):
                continue
            raw = clean_name(ob.name)
            if ob.type == "ARMATURE" or (ob.type == "EMPTY" and PART_RE.match(raw)):
                continue
            part = part_of(ob)
            if ob.type == "LIGHT":
                m = LIGHT_RE.match(raw)
                if not m:
                    raise ExportError(f"{ob.name}: a model's light is named LP (LP## names its part); a light named "
                                      "!... is left out")
                if primary:
                    lod.lights.append((self.on_part(ob, part, m.group(1), lod, root_ok=True), ob))
                else:
                    lod0_only(ob, "lights")
                continue
            if ob.type == "EMPTY":
                m = POINT_RE.match(raw)
                if m:
                    if not primary:
                        lod0_only(ob, "user points")
                        continue
                    label = m.group(3) if m.group(3) is not None else "Noname"
                    if len(label) > 15:
                        raise ExportError(f"{ob.name}: user point label '{label}' exceeds 15 characters")
                    lod.points.append((m.group(1), self.on_part(ob, part, m.group(2), lod, none_ok=True), label, ob))
                    continue
                m = HELPER_RE.match(raw)
                if m and m.group(2) == "attach":
                    lod.anchors.append((self.on_part(ob, part, m.group(1), lod), ob))
                    continue
                if m and m.group(2) == "sphere":
                    mesh = ob.parent
                    if mesh is None or mesh.type != "MESH" or not OCCLUSION_RE.search(clean_name(mesh.name)):
                        raise ExportError(f"{ob.name}: an occlusion sphere sits on its occlusion mesh (a -occonly "
                                          "mesh)")
                    if primary:
                        self.on_part(ob, part, m.group(1), lod)
                        if mesh.name in lod.occ_spheres:
                            raise ExportError(f"'{lod.occ_spheres[mesh.name].name}' and '{ob.name}' are both the "
                                              f"sphere of {mesh.name}")
                        lod.occ_spheres[mesh.name] = ob
                    continue
                if m and m.group(2) in ("hit", "bounds"):
                    if not lp.skinned:
                        raise ExportError(f"{ob.name}: a hit sphere and its bounds belong to a bone of a skinned "
                                          "model")
                    if not primary:
                        lod0_only(ob, "hit spheres")
                        continue
                    table, what = (lod.spheres, "hit sphere") if m.group(2) == "hit" else (lod.boxes, "bounds box")
                    once(table, self.on_part(ob, part, m.group(1), lod), ob, what)
                    continue
                if not ob.children:
                    # An empty that only groups objects is left alone: its
                    # children are classified on their own.
                    self.note(f"{ob.name}: an empty that is no part of the naming contract "
                              "(docs/threedi/scene-naming-contract.md); not exported")
                continue
            if ob.type != "MESH":
                self.note(f"{ob.name}: a {ob.type.lower()} is not exported (only meshes, empties and lights are "
                          "read)")
                continue
            if OCCLUSION_RE.search(raw):
                m = OCC_RE.match(raw)
                if not m:
                    raise ExportError(f"{ob.name}: occlusion meshes are OB##, OS##, OP##[-MM] or OH##, then -occonly")
                prefix, number, _, mm = m.groups()
                if mm == "00":
                    raise ExportError(f"{ob.name}: occlusion sections start at 01")
                if not primary:
                    lod0_only(ob, "occlusion meshes")
                    continue
                kind = 3 if prefix == "OP" and mm else OCC_TYPES[prefix]
                lod.occluders.append((kind, self.on_part(ob, part, number, lod), int(mm) - 1 if mm else 0, ob))
                continue
            m = VOLUME_RE.match(raw)
            if m:
                code, letters, number, dup = m.groups()
                if code not in VOLUME_CODES and code[0] not in UNLISTED_TYPE_LETTERS:
                    raise ExportError(f"{ob.name}: unknown collision code '{code}'")
                if letters and code != "BB":
                    raise ExportError(f"{ob.name}: only blink boxes (BB) take flag letters")
                flags = 0
                if code == "BB":
                    flags = 0x3E
                    for letter in letters:
                        flags &= ~BLINK_LETTER_BITS[letter]
                if not primary:
                    lod0_only(ob, "collision volumes")
                    continue
                lod.volumes.append((VOLUME_CODES.get(code, 0), flags, self.on_part(ob, part, number, lod), ob,
                                    (code + letters, dup_rank(dup), clean_name(ob.name), ob.name)))
                continue
            m = HELPER_RE.match(raw)
            if m and m.group(2) == "center":
                index = self.on_part(ob, part, m.group(1), lod)
                if lp.skinned:
                    self.note(f"{ob.name}: a skinned part's bounds are its geometry's; the centre helper is not "
                              "exported")
                    continue
                once(lod.centers, index, ob, "centre helper")
                continue
            if m:
                raise ExportError(f"{ob.name}: an {m.group(2)} helper is an Empty")
            index = self.on_part(ob, part, None, lod)
            if lod.lp.mesh_part is not None and index == lod.lp.mesh_part:
                raise ExportError(f"{ob.name}: the mesh part holds the skinned geometry alone")
            lod.meshes.setdefault(index, []).append(ob)
        for meshes in lod.meshes.values():
            meshes.sort(key=lambda o: o.name)
        return lod

    # --- geometry -----------------------------------------------------------
    def uv_layers(self, mesh):
        """UV0 is the UV map Blender renders with, UV1 (the detail stage's)
        the first other one; one map serves both."""
        layers = mesh.uv_layers
        if len(layers) == 0:
            return None, None
        uv0 = next((l for l in layers if l.active_render), layers[0])
        uv1 = next((l for l in layers if l.name != uv0.name), uv0)
        # Blender's legacy UV .data accessor materializes its compatibility
        # view. Reading it for every triangle corner becomes quadratic on
        # large meshes. Snapshot each layer once, including both UV channels.
        def coordinates(layer):
            data = layer.data
            values = array("f", [0.0]) * (len(data) * 2)
            data.foreach_get("uv", values)
            return values
        first = coordinates(uv0)
        return first, first if uv1 == uv0 else coordinates(uv1)

    def corner(self, mesh, loop, mw, nmat, normals, uv0, uv1):
        p = mw @ mesh.vertices[loop.vertex_index].co
        n = (nmat @ normals[loop.index].vector).normalized()
        at = loop.index * 2
        a = (uv0[at], uv0[at + 1]) if uv0 is not None else (0.0, 0.0)
        b = (uv1[at], uv1[at + 1]) if uv1 is not None else a
        pm, nm = self.space.mission(p), self.space.mission(n)
        # D3D texture space: v runs down.
        vert = (pm[0], pm[1], pm[2], nm[0], nm[1], nm[2], a[0], 1.0 - a[1])
        if self.uv1:
            vert += (b[0], 1.0 - b[1])
        return vert

    def mesh_strips(self, ob, strips):
        """A rigid part's mesh into its strips (material index -> [strip]),
        each at most STRIP_TRIANGLES triangles: a full strip starts another,
        as a skinned one does."""
        ev = self.evaluated(ob)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.world(ob)
            nmat = mw.to_3x3().inverted_safe().transposed()
            mirrored = mw.to_3x3().determinant() < 0
            normals = mesh.corner_normals
            uv0, uv1 = self.uv_layers(mesh)
            self.materials.record_mesh(ob, ev, mesh, uv0)
            for tri in mesh.loop_triangles:
                mi = self.materials.index_of(slot_material(ev, tri.material_index))
                runs = strips.setdefault(mi, [])
                if not runs or len(runs[-1]["tris"]) >= STRIP_TRIANGLES:
                    runs.append(new_strip(ob))
                s = runs[-1]
                if s["meshes"][-1] != ob.name:
                    s["meshes"].append(ob.name)
                corners = []
                for li in reversed(tri.loops) if mirrored else tri.loops:
                    vert = self.corner(mesh, mesh.loops[li], mw, nmat, normals, uv0, uv1)
                    corners.append(shared_vertex(s, tuple(round(x, 5) for x in vert), vert, corners))
                s["tris"].append(corners)
        finally:
            ev.to_mesh_clear()

    def weights(self, ob, ev, mesh, lod):
        """Each vertex's influences, [(part, weight)] dominant first (contract
        C1: up to four, each a part bone of the LOD, normalized to one as
        Blender's Armature modifier blends them), from the vertex groups named
        after the rig's deforming bones; an error for a vertex with more than
        four, with none, or with a weight on a deforming bone that is no part
        (Root, a control bone) or past the LOD's parts."""
        rig = lod.lp.rig
        parts = {b.name: i for i, b in part_bones(rig).items()}
        groups = {}      # vertex group index -> part index
        foreign = {}     # vertex group index -> a deforming bone that is no part
        for g in ev.vertex_groups:
            bone = rig.data.bones.get(g.name)
            if bone is None:
                continue  # not a bone's group: Blender deforms nothing by it
            if not bone.use_deform:
                if g.name in parts:
                    foreign[g.index] = f"{g.name}, a part bone whose Deform is off"
                continue
            if g.name in parts and parts[g.name] < len(lod.parts) and parts[g.name] != lod.lp.mesh_part:
                groups[g.index] = parts[g.name]
            elif g.name in parts:
                foreign[g.index] = f"{g.name}, past the {len(lod.parts)} parts the model reaches"
            else:
                foreign[g.index] = f"{g.name}, which is no part (a part bone is named BN##)"
        out = []
        for v in mesh.vertices:
            infl = {}
            for g in v.groups:
                if g.weight < WEIGHT_EPS:
                    continue
                if g.group in foreign:
                    raise ExportError(f"{ob.name}: vertex {v.index} is weighted to bone {foreign[g.group]}: the game "
                                      "moves a vertex by part bones alone")
                if g.group in groups:
                    infl[groups[g.group]] = infl.get(groups[g.group], 0.0) + g.weight
            if not infl:
                raise ExportError(f"{ob.name}: vertex {v.index} has no weight of {WEIGHT_EPS:g} or more on a part "
                                  "bone")
            if len(infl) > 4:
                raise ExportError(f"{ob.name}: vertex {v.index} is weighted to {len(infl)} bones; the game blends "
                                  "four (Weights > Limit Total)")
            total = sum(infl.values())
            out.append(sorted(((b, w / total) for b, w in infl.items()), key=lambda e: (-e[1], e[0])))
        return out

    def skinned_strips(self, ob, strips, lod, bone=None):
        """A mesh's triangles on a skinned model, grouped per material into
        strips whose bone tables stay within SKIN_TABLE parts and whose
        triangles within STRIP_TRIANGLES, by OED's palette rule (the retired
        port's rdta.cpp, WriteRDTA_Skinned's grouping): a triangle joins the
        first strip of its material whose table, counting each corner's
        bones anew where the table lacks them, stays within SKIN_TABLE, and
        else starts one; a table lists its bones in the order they come. So a
        table of 15 bones takes no triangle bringing a 16th on more than one
        corner: 56 retail splits (JNTOPSB2's 15 and 2 bones, CIndo01's 15 and
        1) keep a union a plain count would have fitted in one strip. A
        corner carries its rest position and its influences (weights), or
        wholly `bone` for a mesh hung from it. LOD 0's vertices bound the
        bones that move them (bone_points)."""
        ev = self.evaluated(ob)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.world(ob)
            nmat = mw.to_3x3().inverted_safe().transposed()
            mirrored = mw.to_3x3().determinant() < 0
            normals = mesh.corner_normals
            uv0, uv1 = self.uv_layers(mesh)
            self.materials.record_mesh(ob, ev, mesh, uv0)
            if bone is None:
                influences = self.weights(ob, ev, mesh, lod)
            else:
                influences = [[(bone, 1.0)]] * len(mesh.vertices)
            if lod.lp.index == 0:
                # Every LOD 0 vertex a bone moves bounds that bone's section
                # (WriteCOBJ's skinned rule).
                for v, infl in zip(mesh.vertices, influences):
                    at = self.space.mission(mw @ v.co)
                    for b, _ in infl:
                        self.bone_points.setdefault(b, []).append(at)
            for tri in mesh.loop_triangles:
                mi = self.materials.index_of(slot_material(ev, tri.material_index))
                corners = []
                for li in reversed(tri.loops) if mirrored else tri.loops:
                    loop = mesh.loops[li]
                    corners.append((self.corner(mesh, loop, mw, nmat, normals, uv0, uv1),
                                    influences[loop.vertex_index]))
                bones = [b for _, infl in corners for b, _ in infl]
                runs = strips.setdefault(mi, [])
                s = next((run for run in runs if len(run["tris"]) < STRIP_TRIANGLES and
                          len(run["table"]) + sum(1 for b in bones if b not in run["slots"]) <= SKIN_TABLE), None)
                if s is None:
                    s = new_strip(ob)
                    runs.append(s)
                if s["meshes"][-1] != ob.name:
                    s["meshes"].append(ob.name)
                for b in bones:
                    if b not in s["slots"]:
                        s["slots"][b] = len(s["table"])
                        s["table"].append(b)
                ids = []
                for vert, infl in corners:
                    slots = [s["slots"][b] for b, _ in infl]
                    # Four bone-table slots and three stored weights: the
                    # fourth slot takes 1 - (w0 + w1 + w2) (contract C1, the
                    # retail blend); an unused slot repeats the first, as
                    # retail pads them.
                    slots += [slots[0]] * (4 - len(slots))
                    stored = [w for _, w in infl[:3]] + [0.0] * (3 - min(3, len(infl)))
                    full = vert + tuple(slots) + tuple(stored)
                    ids.append(shared_vertex(s, tuple(round(x, 5) for x in full), full, ids))
                s["tris"].append(ids)
        finally:
            ev.to_mesh_clear()

    def emit_strip(self, s, mi, lines, skinned):
        lines.append(f"strip {mi} {self.materials.strip_alpha(mi)}  # {', '.join(s['meshes'])}")
        if skinned:
            lines.append("bones " + " ".join(str(b) for b in s["table"]))
        uv_end = 10 if self.uv1 else 8
        for v in s["verts"]:
            if skinned:
                lines.append("v " + fmt(*v[:uv_end]) + " " + " ".join(str(int(x)) for x in v[uv_end:uv_end + 4]) +
                             " " + fmt(*(float(x) for x in v[uv_end + 4:uv_end + 7])))
            else:
                lines.append("v " + fmt(*v))
        for t in s["tris"]:
            lines.append(f"t {t[0]} {t[1]} {t[2]}")

    def part_line(self, lod, part, centre=None):
        line = f"part {part.parent} {fmt(*self.pivot(part))}"
        if centre is not None:
            line += " " + fmt(*centre)
        return line + f"  # {part.name}"

    def part_centre(self, lod, index):
        """The point a rigid part that draws nothing seeds its bounds with
        (radius 0): its `_center` helper mesh's first vertex, as OED's
        placeholder injection took it (5fc5b4f6a^ engine/formats/oed/
        convert_internal.cpp); None without one, and the part's bounds sit at
        the origin."""
        ob = lod.centers.get(index)
        if ob is None or len(ob.data.vertices) == 0:
            return None
        return self.space.mission(self.world(ob) @ ob.data.vertices[0].co)

    def emit_lod(self, lod, lines):
        p = lod.root.o3d
        lines.append(f"lod {p.lod_threshold} {quoted(p.lod_type or 'gnrc')}  # {lod.root.name}")
        for part in lod.parts:
            strips = {}
            if self.skinned:
                if part.index == lod.authored:
                    for ob in lod.skinned:
                        self.skinned_strips(ob, strips, lod)
                for ob in lod.meshes.get(part.index, []):
                    self.skinned_strips(ob, strips, lod, part.index)
            else:
                for ob in lod.meshes.get(part.index, []):
                    self.mesh_strips(ob, strips)
            centre = self.part_centre(lod, part.index) if not strips and not self.skinned else None
            lines.append(self.part_line(lod, part, centre))
            for mi in sorted(strips):
                for s in strips[mi]:
                    self.emit_strip(s, mi, lines, self.skinned)
        for part in lod.parts:
            self.emit_panm(lod, part, lines)

    def emit_panm(self, lod, part, lines):
        """A part's PANM row: its flags (derived from the tracks unless set),
        MTRX frame and tracks, from a PN##'s or a bone's part animation. A part
        has one track per target (PANM stores one slot each); a mesh part has
        none."""
        holder = part.empty if part.empty is not None else part.bone
        if holder is None:
            lines.append(f"panm {part.index} {part.parent}  # {part.name}")
            return
        p = holder.o3d
        what = part.name if part.empty is not None else f"{part.rig.name} bone {part.name}"
        tracks = [(t.target, t.style, int(t.axis) if t.target == "trans" else 0) for t in p.tracks]
        targets = [t.target for t in p.tracks]
        twice = sorted({t for t in targets if targets.count(t) > 1})
        if twice:
            raise ExportError(f"{what}: {targets.count(twice[0])} {twice[0]} tracks; a part has one track per target")
        frame = self.part_frame(part)
        flags = p.panm_flags if p.panm_flags >= 0 else derived_panm_flags(tracks)
        line = f"panm {part.index} {part.parent}"
        if p.panm_flags >= 0 or frame:
            line += f" 0x{flags:08x}" + (f" {frame}" if frame else "")
        lines.append(line + f"  # {part.name}")
        for t in p.tracks:
            lines.append(self.track_line(t, what))

    def track_line(self, t, what):
        style = t.style
        rotation = t.target.startswith("rot")
        scale = 16384.0 / 360.0 if rotation else 256.0
        if style > CTRL_REFERENCE_THRESHOLD:
            self.register(t.register, f"{what} track {t.target}")
            field = quoted(t.register)
        else:
            field = str(t.param) if t.param else "-"
        # Rotations in 1/16384 turn, the others and every rate 8.8, int16.
        words = [fixed(value, s, -0x8000, 0x7FFF, f"{what} track {t.target} {name}")
                 for value, s, name in ((t.rate, 256.0, "rate"), (t.start, scale, "start"), (t.end, scale, "end"))]
        line = f"track {t.target} {style} {field} {words[0]} {words[1]} {words[2]}"
        if t.target == "trans":
            line += f" {t.axis}"
        return line

    # --- user points, lights, occlusion -------------------------------------
    def emit_points(self, lod, lines):
        # USRP order: each helper's `order` (the imported index), then label.
        # The seat scan reads `sitex` without case and stops at 8 [orig:
        # Entity_GetBoneSlotType @ 0x434ED0; the scan end @ 0x43A5AF].
        seats = [ob.name for _, _, label, ob in lod.points if label.lower().startswith("sitex")]
        if len(seats) > 8:
            raise ExportError(f"{len(seats)} sitex seats ({', '.join(sorted(seats))}); the game reads 8")
        for letter, part, label, ob in sorted(lod.points, key=lambda e: point_key(e[2], e[3])):
            pos = self.space.mission(self.world(ob).translation)
            d = self.space.mission((self.world(ob).to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
            lines.append(f"userpoint {quoted(label)} {fmt(*pos)} {fmt(*d)} {part} {ord(letter.upper())}  # {ob.name}")

    def emit_lights(self, lod, lines):
        for part, ob in sorted(lod.lights, key=lambda e: order_key(e[1])):
            data, p = ob.data, ob.data.o3d
            spot = data.type == "SPOT"
            if data.type not in ("POINT", "SPOT"):
                raise ExportError(f"{ob.name}: a light is a point or spot light")
            pos = self.space.mission(self.world(ob).translation)
            phase = str(self.register(p.register, f"{ob.name} colour")) if p.style > CTRL_REFERENCE_THRESHOLD \
                else fmt(float(p.phase))
            # WriteLGHT packs the rate times 256, truncated, into a u16 and each
            # colour channel into a byte (threedi_build_light_rate).
            if not 0.0 <= p.rate < 256.0:
                raise ExportError(f"{ob.name}: its rate is {p.rate:g}; the file holds 0 up to 256")
            s = [fixed(c, 255.0, 0, 255, f"{ob.name}: its colour") for c in data.color]
            e = [round(c * 255) for c in p.color_end]
            flags = (1 if p.disable_corona else 0) | (2 if p.disable_terrain else 0) | \
                (4 if p.disable_objects else 0) | (8 if spot else 0) | (p.other_flags & ~0x0F & 0xFF)
            line = (f"light {part} {fmt(*pos)} {fmt(float(p.atten_start), float(p.atten_end))} {p.style} "
                    f"{fmt(float(p.rate))} {phase} {s[0]} {s[1]} {s[2]} {e[0]} {e[1]} {e[2]} 0x{flags:02x}")
            # The light's local -Z is its stored axis (Blender draws a spot
            # light's cone down it); an omni light pointing straight down, an
            # unrotated one, is the retail default the builder writes itself.
            d = self.space.mission((self.world(ob).to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized())
            falloff = math.degrees(data.spot_size) / 2.0 if spot else 0.0
            if spot or abs(d[0]) > 1e-6 or abs(d[1]) > 1e-6 or d[2] > -1.0 + 1e-9:
                line += " " + fmt(*d, float(falloff))
            lines.append(line + f"  # {ob.name}")

    def emit_occlusion(self, lod, lines):
        for kind, section, connecting, ob in sorted(lod.occluders, key=lambda e: (order_key(e[3]), e[1])):
            ev = self.evaluated(ob)
            mesh = ev.to_mesh()
            try:
                if len(mesh.vertices) > 128:
                    raise ExportError(f"{ob.name}: an occlusion mesh holds at most 128 vertices")
                # The record's sphere: its `_sphere` empty's, else the builder
                # derives it from the vertices (docs/threedi/o3d-scene-format.md).
                sphere = lod.occ_spheres.get(ob.name)
                given = ""
                if sphere is not None:
                    centre, radius = self.hit_sphere(sphere)
                    given = " " + fmt(*centre, radius)
                lines.append(f"occ {kind} {section} {connecting}{given}  # {ob.name}")
                mw = self.world(ob)
                mirrored = mw.to_3x3().determinant() < 0
                for v in mesh.vertices:
                    lines.append("ov " + fmt(*self.space.mission(mw @ v.co)))
                # Counter-clockwise about the outward normal, as retail stores
                # them; the builder picks each face's plane (the OED rule).
                mesh.calc_loop_triangles()
                for tri in mesh.loop_triangles:
                    a, b, c = reversed(tri.vertices) if mirrored else tri.vertices
                    lines.append(f"of {a} {b} {c}")
            finally:
                ev.to_mesh_clear()

    # --- collision --------------------------------------------------------------
    def volume_mesh(self, ob):
        """A volume's vertices (mission axes) and triangles, wound
        counter-clockwise about the outward normal as authored; the builder
        derives the planes, box and seam flags by the OED rule."""
        ev = self.evaluated(ob)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.world(ob)
            mirrored = mw.to_3x3().determinant() < 0
            verts = [self.space.mission(mw @ v.co) for v in mesh.vertices]
            tris = [tuple(reversed(t.vertices)) if mirrored else tuple(t.vertices) for t in mesh.loop_triangles]
        finally:
            ev.to_mesh_clear()
        if len(verts) < 4 or not tris:
            raise ExportError(f"collision volume {ob.name} needs at least 4 vertices and a face")
        return verts, tris

    def bullet_faces(self, section, meshes):
        """The meshes' triangles as a section's bullet faces, on the 8.8 grid
        the section stores, counter-clockwise about their outward normal (the
        retail order)."""
        for ob in meshes:
            ev = self.evaluated(ob)
            mesh = ev.to_mesh()
            try:
                mesh.calc_loop_triangles()
                mw = self.world(ob)
                mirrored = mw.to_3x3().determinant() < 0
                for tri in mesh.loop_triangles:
                    corners = []
                    for vi in reversed(tri.vertices) if mirrored else tri.vertices:
                        p = self.space.mission(mw @ mesh.vertices[vi].co)
                        key = tuple(round(x, 4) for x in p)
                        if key not in section["index"]:
                            if not all(abs(x) < 128.0 for x in p):
                                raise ExportError(f"{ob.name}: a vertex of the collision LOD lies at ({fmt(*p)}); a "
                                                  "collision vertex stays under 128 from the model origin on each "
                                                  "axis (CVRT stores 8.8 in an int16)")
                            section["index"][key] = len(section["verts"])
                            section["verts"].append(p)
                        corners.append(section["index"][key])
                    if len(set(corners)) != 3:
                        continue
                    mat = slot_material(ev, tri.material_index)
                    section["faces"].append((corners, mat.o3d.surface if mat is not None else 14,
                                             materials.face_flags(mat)))
            finally:
                ev.to_mesh_clear()

    def hit_sphere(self, ob):
        """A `_hit` empty's sphere: its origin, and its display size times its
        scale as the radius (mission axes)."""
        m = self.world(ob)
        scales = [m.col[i].xyz.length for i in range(3)]
        if max(scales) - min(scales) > 1e-4 * max(scales):
            raise ExportError(f"{ob.name}: a hit sphere is scaled evenly on its three axes")
        return self.space.mission(m.translation), ob.empty_display_size * scales[0]

    def hit_box(self, ob):
        """A `_bounds` empty's box, [min x y z, max x y z] in mission axes: its
        origin the middle, its display size times its scale on each axis the
        half extents; it stays square with the model's axes."""
        m = self.world(ob)
        axes = m.to_3x3()
        scales = [axes.col[i].length for i in range(3)]
        if any(abs(axes[i][j]) > 1e-6 * max(max(scales), 1e-9) for i in range(3) for j in range(3) if i != j):
            raise ExportError(f"{ob.name}: a bounds box is not turned (it stays square with the model's axes)")
        c = self.space.mission(m.translation)
        h = self.space.mission(Vector(tuple(ob.empty_display_size * s for s in scales)))
        h = [abs(x) for x in h]
        return [c[k] - h[k] for k in range(3)] + [c[k] + h[k] for k in range(3)]

    def bone_sphere(self, lod0, part, section):
        """A skinned model's section `csphere`: [centre x y z, radius, and the
        box's min x y z, max x y z unless the sphere's cube bounds it] in
        mission axes, or None for the empty section's sentinels. A bone
        section (one without collision geometry of its own) takes its bone's
        `_hit` sphere and `_bounds` box, and what the empty lacks from the
        LOD 0 vertices the bone moves (bone_bounds); a bone that moves none
        and has no `_hit`, or whose Hit sphere is off, keeps the sentinels."""
        i = part.index
        hit, bounds = lod0.spheres.get(i), lod0.boxes.get(i)
        helper = hit if hit is not None else bounds
        if helper is not None and section["verts"]:
            raise ExportError(f"{helper.name}: section {i + 1:02d} holds bullet faces, which bound it; a hit sphere "
                              "and its box are a bone section's")
        if part.bone is not None and not part.bone.o3d.hit_sphere:
            if helper is not None:
                raise ExportError(f"{helper.name}: bone {part.bone.name}'s Hit sphere is off (its section stores "
                                  "none)")
            return None
        points = None if section["verts"] else self.bone_points.get(i)
        derived = bone_bounds(points) if points else None
        if hit is not None:
            centre, radius = self.hit_sphere(hit)
        elif derived is not None:
            centre, radius = derived[0], derived[1]
        elif bounds is not None:
            raise ExportError(f"{bounds.name}: a bounds box goes with a hit sphere, and bone {i + 1:02d} moves no "
                              f"vertex to derive one from: add its `_{i + 1:02d} hit`")
        else:
            return None
        box = self.hit_box(bounds) if bounds is not None else (derived[2] if derived is not None else [])
        return [*centre, radius, *box]

    def emit_collision(self, lod0, bullet, lines):
        # One section per part of the collision LOD (WriteCOBJ walks that
        # LOD's subobjects: Dtruck2's collision LOD has 7 parts to LOD 0's 8,
        # CNet01's none), placed at that part's pivot with its parent.
        count = len(bullet.parts)
        sections = [{"verts": [], "index": {}, "faces": [], "volumes": []} for _ in range(count)]
        # Bullet faces: the collision LOD's geometry, each part's in its
        # section and a skinned model's skinned meshes in the section of the
        # part they are authored on (the retail person layout: the mesh part's
        # or the root's). Generate bullet faces off (first-person arms) keeps
        # the volumes only.
        if self.props.export_bullet_faces:
            for index in sorted(bullet.meshes):
                self.bullet_faces(sections[index], bullet.meshes[index])
            if bullet.skinned:
                self.bullet_faces(sections[bullet.authored], bullet.skinned)
        # What each section is, for a refusal that names a section alone.
        for i, part in enumerate(bullet.parts):
            meshes = list(bullet.meshes.get(i, [])) + (list(bullet.skinned) if i == bullet.authored else [])
            self.sections[i] = part.name + (f": {', '.join(ob.name for ob in meshes)}" if meshes else "")
        # A part that draws nothing keeps the vertex OED seeded it with (its
        # `_center` helper's first), and WriteCVRT wrote a section's part
        # vertices, faces or not: 1,779 of the 2,411 such retail parts carry
        # it as their section's only collision vertex (every first-person
        # weapon's empty parts).
        for i, s in enumerate(sections):
            centre = self.part_centre(bullet, i) if not self.skinned and not s["verts"] else None
            if centre is not None:
                if not all(abs(x) < 128.0 for x in centre):
                    raise ExportError(f"{bullet.centers[i].name}: its first vertex lies at ({fmt(*centre)}); a "
                                      "collision vertex stays under 128 from the model origin on each axis")
                s["verts"].append(centre)
        for vtype, flags, part, ob, key in lod0.volumes:
            if part >= count:
                raise ExportError(f"{ob.name}: its section {part + 1:02d} is not a part of the collision LOD "
                                  f"(LOD {self.props.poly_collision_lod} has {count})")
            sections[part]["volumes"].append((key, vtype, flags, ob))
        for part, ob in list(lod0.spheres.items()) + list(lod0.boxes.items()):
            if part >= count:
                raise ExportError(f"{ob.name}: its section {part + 1:02d} is not a part of the collision LOD "
                                  f"(LOD {self.props.poly_collision_lod} has {count})")
        # Every section sits at its part's pivot (the COBJ offset retail
        # carries: Dtruck2's wheels, Dblkhwk1's rotors).
        pivots = [self.pivot(p) for p in bullet.parts]
        for i, s in enumerate(sections):
            lines.append(f"cobj {bullet.parts[i].parent} " + fmt(*pivots[i]) + f"  # {bullet.parts[i].name}")
            if self.skinned:
                sphere = self.bone_sphere(lod0, bullet.parts[i], s)
                if sphere is not None:
                    lines.append("csphere " + fmt(*(float(x) for x in sphere)))
            for v in s["verts"]:
                lines.append("cv " + fmt(*v))
            for (a, b, c), poly, face_flags in s["faces"]:
                lines.append(f"cf {a} {b} {c} {poly} {face_flags}")
            # Volumes in name order within the section: code, then the
            # duplicate suffix (CB01, CB01a .. CB01z, CB01aa); OED kept its
            # scene order, which a Blender scene does not have.
            for key, vtype, flags, ob in sorted(s["volumes"], key=lambda e: e[0]):
                verts, tris = self.volume_mesh(ob)
                lines.append(f"cvmesh {vtype} {flags} {quoted(clean_name(ob.name))}  # {ob.name}")
                for v in verts:
                    lines.append("vv " + fmt(*v))
                for t in tris:
                    lines.append(f"vf {t[0]} {t[1]} {t[2]}")
        # CXLT: the collision LOD's attach points, which OED's WriteCXLT wrote,
        # a row per `~` attach helper sorted by name (5fc5b4f6a^
        # engine/formats/oed/convert_internal.cpp and export_3di.cpp, "CXLT:
        # attach points"). Attach points "The attach helpers" is that rule:
        # a row per `_attach`, in export order, none an empty table (import
        # sets it for the 156 JOTAC tables one per part cannot say: M24_1st's
        # 42 rows in the name order of its helpers, the parent order; dM1A1's
        # 33 for 25 sections; Chair03X's none).
        if self.props.attach_points == "HELPERS":
            for _, ob in sorted(bullet.anchors, key=lambda e: order_key(e[1])):
                lines.append("cxlt " + fmt(*self.space.mission(self.world(ob).translation)) + f"  # {ob.name}")
            if not bullet.anchors:
                lines.append("cxlt  # no attach helper: an empty table")
            return
        # "One per part": a scene holds attach points for only some parts
        # (import makes one where the row is not the section's own offset),
        # so with any `_attach` in the LOD every row the retail count gives is
        # written (one per section after the root on a rigid model, one per
        # section on a skinned one, as the corpus stores them and the builder
        # derives them), each at its part's attach point, else at its pivot
        # through the very values its cobj line carries, so the CLI reads a
        # row the builder would derive as that row. With none the builder
        # derives every row.
        by_part = {}
        for part, ob in bullet.anchors:
            if part in by_part:
                raise ExportError(f"{bullet.root.name}: '{by_part[part].name}' and '{ob.name}' are both the attach "
                                  f"point of {self.part_name(bullet, part)} (a part carries one while the model's "
                                  "Attach points are One per part)")
            by_part[part] = ob
        if not by_part:
            return
        first = 0 if self.skinned else 1
        if first and 0 in by_part:
            self.note(f"{by_part[0].name}: a rigid model stores no attach point for its root section while its "
                      "Attach points are One per part; this one is not exported")
        for i in range(first, count):
            ob = by_part.get(i)
            if ob is None:
                lines.append("cxlt " + fmt(*pivots[i]) + f"  # {bullet.parts[i].name}'s pivot")
            else:
                lines.append("cxlt " + fmt(*self.space.mission(self.world(ob).translation)) + f"  # {ob.name}")

    # --- checks -------------------------------------------------------------
    def check_parts(self, lods):
        """What the engine cannot pose: a root part off the model origin, a
        first-person gun over its 64 parts, arms with more parts than their
        gun; and a parent numbered after its part, or a first-person gun whose
        arm parts are not the stock arms'."""
        for lod in lods:
            if not lod.parts:
                continue
            at = self.pivot(lod.parts[0])
            if any(abs(x) > 1e-5 for x in at):
                raise ExportError(f"{lod.parts[0].name}: the root part's pivot is the model origin, but it lies at "
                                  f"({fmt(*at)}): move the model root there (the engine poses every part from it)")
            for part in lod.parts:
                if part.parent > part.index:
                    # The engine reads a parent's matrix from the array it is
                    # filling, so a parent numbered after its part is read
                    # before it is posed [orig: BoneAnim_BuildWorldMatrices @
                    # 0x40C400, the parent read @ 0x40C674;
                    # Model_TransformBoneMatrices @ 0x58E390, @ 0x58F09F]; 17
                    # retail models carry one.
                    self.note(f"{part.name}: its parent ({self.part_name(lod, part.parent)}) is numbered after it, "
                              "so the game reads the parent before posing it (Number Parts numbers parents first)")
        lp = lods[0].lp
        parts = len(lods[0].parts)
        if is_first_person(self.scene, self.model, lp):
            if parts > FIRST_PERSON_PARTS:
                raise ExportError(f"{self.model.name}: {parts} parts; a first-person gun has at most "
                                  f"{FIRST_PERSON_PARTS}, the game's bone arrays")
            fit = assembly.stock_arms_fit(self, lods[0])
            if fit is not None:
                self.note(fit)
        if lp.shared and lp.skinned:
            gun = model_of(lp.rig)
            owned = len(part_bones(lp.rig))
            if parts > owned:
                raise ExportError(f"{self.model.name}: {parts} parts, more than the {owned} of {gun.name}, whose rig "
                                  "it deforms with: the game draws the arms with the gun's part matrices, and a part "
                                  "past them has none")

    # --- the run --------------------------------------------------------------
    def run(self):
        model = self.model.name
        out_path = output_path(self.model)
        if not out_path.lower().endswith(".3di"):
            raise ExportError(f"{model}: the output path must end in .3di")
        if not os.path.isabs(out_path):
            raise ExportError(f"{model}: save the .blend first or give an absolute output path (a '//' path is "
                              "relative to the saved file)")
        base = os.path.basename(out_path)
        if not base.isascii() or len(base) > FILE_NAME_BYTES:
            raise ExportError(f"{model}: the file name {base} is not {FILE_NAME_BYTES} ASCII characters or fewer, "
                              "which a game archive's entry holds")
        self.export_run.claim(out_path, self.model)
        out_dir = os.path.dirname(out_path)
        name = self.props.model_name.strip() or os.path.splitext(base)[0]
        if len(name) > 15:
            raise ExportError(f"{model}: the model name exceeds 15 characters")
        # Edit Mode keeps its edits out of the data export reads (an
        # armature's new bones, a mesh's new faces): leave it for the export
        # and return to it after.
        active = self.context.view_layer.objects.active
        mode = active.mode if active is not None else "OBJECT"
        if mode != "OBJECT":
            try:
                bpy.ops.object.mode_set(mode="OBJECT")
            except RuntimeError as e:
                raise ExportError(f"{model}: leave {mode.lower().replace('_', ' ')} mode first ({e})")
        try:
            rig = rig_of(self.model)
            owner = model_of(rig) if rig is not None else None
            if owner is not None and owner is not self.model:
                # Arms on a gun's rig: the game draws them with the gun's part
                # matrices, so they stand where the gun stands.
                a, b = self.model.matrix_world, owner.matrix_world
                if any(abs(a[i][j] - b[i][j]) > 1e-5 for i in range(4) for j in range(4)):
                    raise ExportError(f"{model}: its root must stand where {owner.name}'s does, whose rig it deforms "
                                      "with (parent it to that root with no offset of its own)")
            with at_world_origin(self.context, self.model, owner):
                return self.run_in_object_mode(name, out_path, out_dir)
        finally:
            if mode != "OBJECT":
                try:
                    bpy.ops.object.mode_set(mode=mode)
                except RuntimeError:
                    pass

    def run_in_object_mode(self, name, out_path, out_dir):
        self.context.view_layer.update()
        self.space = ModelSpace(self.model)
        for ob in self.model.children:
            if not (is_lod_root(ob) or is_model_root(ob) or ignored(ob)):
                self.note(f"{ob.name}: not under a LOD root; not exported")
        parts = [lod_parts(root) for root in lod_roots(self.model)]
        if not parts[0].parts:
            raise ExportError(f"{parts[0].root.name}: LOD 0 has no parts")
        self.skinned = bool(parts[0].skinned)
        for lp in parts[1:]:
            if lp.parts and bool(lp.skinned) != self.skinned:
                raise ExportError(f"{lp.root.name}: every LOD of a skinned model deforms with its rig, and no LOD "
                                  "of a rigid one does")
        # Every model is read at rest (the bind pose skinned vertices are
        # stored in, the layout a rigid rig's parts have) with its Armature
        # modifiers off: at rest they move nothing, and evaluating them anyway
        # leaves float noise in the normals (US01 and CIndo01: 1628 at the
        # sixth decimal once their rest bones are turned onto a clip's bind).
        # The scene's pose and modifiers are restored after.
        rigs = {lp.rig for lp in parts if lp.rig is not None}
        rest = [(ob.data, ob.data.pose_position) for ob in rigs]
        deforms = [m for lp in parts for ob in lp.skinned for m in ob.modifiers
                   if m.type == "ARMATURE" and m.show_viewport]
        for data, _ in rest:
            data.pose_position = "REST"
        for m in deforms:
            m.show_viewport = False
        self.context.view_layer.update()
        self.depsgraph = self.context.evaluated_depsgraph_get()
        self.instancers = {inst.parent.original.name for inst in self.depsgraph.object_instances
                           if inst.is_instance and inst.parent is not None}
        try:
            lods = [self.classify(lp) for lp in parts]
            self.uv1 = any(len(ob.data.uv_layers) > 1 for l in lods
                           for ob in [o for meshes in l.meshes.values() for o in meshes] + list(l.skinned))
            self.check_parts(lods)
            return self.emit(name, out_path, out_dir, lods)
        finally:
            for data, position in rest:
                data.pose_position = position
            for m in deforms:
                m.show_viewport = True
            self.context.view_layer.update()

    def emit(self, name, out_path, out_dir, lods):
        bullet_index = self.props.poly_collision_lod
        if bullet_index >= len(lods):
            raise ExportError(f"poly_collision_lod {bullet_index} names no LOD (there are {len(lods)})")
        for lod in lods:
            if lod.anchors and lod is not lods[bullet_index]:
                self.note(f"{lod.anchors[0][1].name}: attach points are read from the collision LOD "
                          f"(LOD {bullet_index}) only; not exported from {lod.root.name}")
        thresholds = [lod.root.o3d.lod_threshold for lod in lods]
        if any(t == 0 for t in thresholds[:-1]) or any(a < b for a, b in zip(thresholds, thresholds[1:])):
            self.note(f"LOD thresholds {thresholds}: a LOD draws above its threshold (the projected radius in pixels), "
                      "so every LOD but the last needs one, falling from LOD 0 (Armry01's run 200, 60, 20, 0)")

        lod_lines, tail_lines, material_lines = [], [], []
        for lod in lods:
            self.emit_lod(lod, lod_lines)
        # The materials take their export order once the geometry has named
        # them all; the register table follows it, and the records written
        # after this take their register indices from it.
        remap = self.materials.order()
        self.declare_registers(lods)
        self.emit_points(lods[0], tail_lines)
        self.emit_lights(lods[0], tail_lines)
        self.emit_occlusion(lods[0], tail_lines)
        self.emit_collision(lods[0], lods[bullet_index], tail_lines)
        for i, line in enumerate(lod_lines):
            if line.startswith("strip "):
                record, _, comment = line.partition("  #")
                fields = record.split()
                lod_lines[i] = f"strip {remap[int(fields[1])]} {fields[2]}  #{comment}"
        self.materials.emit(material_lines, name)

        frame_lines = ["mtrx " + fmt(*(float(x) for x in r)) for r in self.frames]
        text = ["o3d 1", f"model {quoted(name)}"] + (["skinned 1"] if self.skinned else []) + \
            (["uv1 1"] if self.uv1 else []) + [f"register {quoted(r)}" for r in self.registers] + frame_lines + \
            material_lines + lod_lines + tail_lines
        os.makedirs(out_dir, exist_ok=True)
        # The scene text is the CLI's input only; a refusal names the object
        # its line came from (the `# name` comment above it).
        try:
            result = export_text(self.context, ["build"], text, "scene.o3d", "scene text", out_path)
        except ExportError as e:
            raise ExportError(name_sections(name_lines(str(e), text), self.sections)) from None
        # The textures only once the model is built: a refused model writes
        # nothing.
        self.materials.write_textures(out_dir)
        tris = sum(1 for line in lod_lines if line.startswith("t "))
        # The builder's notes (a non-convex volume, collinear faces, ...),
        # without the scene-file prefix.
        notes = [name_lines(n, text) for n in cli_notes(result, "note: ")]
        message = (f"{result.stdout.strip()} ({len(lods)} LODs, {tris} triangles total, "
                   f"{len(self.materials.textures)} textures)")
        return message, self.notes + notes


def name_lines(message, text):
    """A CLI message with each `scene text:N:` line told by the object that
    line came from: the `# name` comment on it or on the nearest line above
    it that carries one."""
    out = []
    for line in message.splitlines():
        m = SCENE_LINE.match(line.strip())
        if m is None:
            out.append(line)
            continue
        at = int(m.group(1)) - 1
        owner = None
        while 0 <= at < len(text) and owner is None:
            record, sep, comment = text[at].partition("  # ")
            if sep:
                owner = comment.strip()
            at -= 1
        out.append(f"{owner}: {m.group(2)} (scene text line {m.group(1)})" if owner else line)
    return "\n".join(out)


def name_sections(message, sections):
    """A CLI message with each `collision section N` told by its part and the
    meshes giving its bullet faces."""
    return SECTION_WORDS.sub(lambda m: f"{m.group(0)} ({sections[int(m.group(1))]})"
                             if int(m.group(1)) in sections else m.group(0), message)


def export_model(context, model, run=None):
    """Export one model root; returns the summary line and the builder's notes.
    Models exported together share one run (ExportRun), which refuses two of
    them writing one file."""
    return Exporter(context, model, run if run is not None else ExportRun()).run()
