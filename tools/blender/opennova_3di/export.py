# Scene -> .o3d text -> opennova-3di.
#
# A model is read by the NovaLogic ASE/OED object-naming convention (the
# retired importer/exporter's, `classify_name` in the retired
# engine/formats/oed/convert_internal.cpp, a port of [orig: ConvertToInternal
# @ 0x4268B3]; docs/threedi/scene-naming-contract.md keeps the table):
#
#   model root    Empty whose children are the model's LOD roots: one .3di.
#                 It carries the model name, output path and the collision
#                 LOD (`poly_collision_lod`, the .3dp setting: the render LOD
#                 whose meshes also become the bullet faces; 0 = the most
#                 detailed). A scene holds any number of models (a first-
#                 person gun and its arms, a hull and its turret); each exports
#                 in its model root's own frame, so placing or mounting a model
#                 never changes its bytes.
#   LOD root      Empty with an integer `_lod_index` custom property (0 = the
#                 primary LOD) and its threshold and RMDL type (gnrc, bldg,
#                 door, veh0). A LOD root with no parts is legal (retail ships
#                 them).
#   PN##          Empty: part (subobject) ## (1-based). Its origin is the
#                 pivot unless a `_## center` helper gives one; a rotated PN##
#                 is a PANM rotation frame (an MTRX row: its tracks turn about
#                 the empty's axes, like Dblkhwk1's canted tail rotor).
#   ## Mesh<n>    Mesh: render geometry of part ##.
#   _## center    helper: part ##'s pivot (its origin). A mesh one's first
#                 vertex seeds a rigid part that draws nothing: the point
#                 its bounds sit on, and its section's collision vertex.
#   ~PPx attach   helper under a child part: that child's parent is part PP
#                 (00 = -1, a part's own number = itself); the first in x
#                 order counts. In the collision LOD it is also the part's
#                 CXLT attach point: with any helper there, every section the
#                 count rule gives a row (a rigid model's after the root, a
#                 skinned model's all) takes its helper's, else its pivot;
#                 without any, the builder derives the rows at the pivots.
#                 A skinned model's helper sits under its part's bone (or its
#                 mesh part's mesh); the bone hierarchy stays its parent.
#   UP<c>## <lbl> helper: user point, type letter c (G gameplay, S effect),
#                 part ## (00 = no part, -1), label = the USRP name; it faces
#                 along its local +Z. Its `order` property keeps the USRP order.
#   LP##[a..]     Light (on LOD0): a LGHT light owned by part ## (00 = part 0);
#                 a spot light is a cone about its local -Z, the way Blender
#                 draws one.
#   <code>##[a..]-colonly  mesh on LOD0: a collision volume of type <code>
#                 (CB CS CC CL CV CA VC BB CD CT CM VK CF LP DH DM DL CP;
#                 any other C, D, L or V code is type 0, import's CX) in
#                 section ##; the builder takes its planes from its faces by
#                 the OED rule (a ladder, CL, faces its last face's plane).
#   OB##/OS##/OP##[-MM]/OH## (+ [a..], -occonly)  mesh on LOD0: an occlusion
#                 record in section ## (OB occluder, OS open, OP a window to
#                 the exterior or, with -MM, a portal to section MM, OH).
#   Material_<i>_<SHADER>  material: export order i, shader tag SHADER (any
#                 tag in the engine's table; without one, the default for its
#                 texture count).
#   Armature      a skinned model: one Armature under the LOD root whose
#                 bones are named BN## (part ##, 1-based; the bone head is the
#                 pivot, the bone parent the part parent). Its "## Mesh<n>"
#                 meshes are weighted by BN## vertex groups (at most three
#                 influences a vertex); strips split so no bone table exceeds
#                 16 parts. Retail keeps every skinned strip on the root ROBJ
#                 and gives each part the bounds of the geometry authored on
#                 it; ## names that part: a bone (dM1A1's hull is "01 Mesh0"
#                 on BN01) or a mesh part numbered after the bones (parent 0,
#                 pivot = the mesh origin), as retail's exporter wrote bones
#                 then mesh objects (ArmsG: 37 bones, then "38 Mesh0").
#                 Collision: each bone's section carries a hit sphere around
#                 every vertex it moves, a meshed part's section the bullet
#                 faces (the retail person layout). A bone named `Root` (any
#                 case) is no part: it is the ground under the character, the
#                 rig's top-level bone with the hips (BN01) below it
#                 (animation.py). Nor is a bone named `!...`, where the control
#                 bones an author rigs with live (a BN## bone below either takes
#                 the nearest BN## above it as its parent, the root when there is
#                 none).
#   !name         ignored.
# Blender's own `.001` duplicate suffixes are stripped before classification
# (object names are unique per .blend, so LOD1's PN01 is "PN01.001"); two
# objects that classify to the same identity inside one LOD are an error. Any
# other object is reported and left out.

import math
import os
import re
import struct
from array import array

import bpy
import numpy as np
from mathutils import Euler, Vector

from .o3dtext import (CTRL_REFERENCE_THRESHOLD, ExportError, ModelSpace, Notes, at_world_origin, cli_notes,
                      export_text, fmt, quoted)


# Shader capability bits (runtime/renderer/material_descriptor.h), read per
# tag from `opennova-3di catalog`.
FLAG_EMISSIVE, FLAG_DIFFUSE, FLAG_SECONDARY = 0x1, 0x4, 0x8
FLAG_BLENDING, FLAG_GLASS, FLAG_SKINNED, FLAG_TANGENT = 0x1000, 0x2000, 0x4000, 0x8000
BLENDER_SUFFIX = re.compile(r"\.\d{3,}$")
PART_RE = re.compile(r"^PN(\d{2})$")
BONE_RE = re.compile(r"^BN(\d{2})(?: .*)?$")
# A single mesh may omit its ordinal: "01 Mesh" is "01 Mesh0".
MESH_RE = re.compile(r"^(\d{2}) Mesh(\d*)$")
CENTER_RE = re.compile(r"^_(\d{2}) center$")
ATTACH_RE = re.compile(r"^~(\d{2})([a-z]*) attach$")
POINT_RE = re.compile(r"^UP([A-Za-z])(\d{2})(?: (.*))?$")
LIGHT_RE = re.compile(r"^LP(\d{2})([a-z]*)$")
MATERIAL_RE = re.compile(r"^Material_(\d+)_(\S+)$")
OCCLUSION_RE = re.compile(r"-occ?only$", re.IGNORECASE)
OCC_RE = re.compile(r"^(OB|OS|OP|OH)(\d{2})([a-z]*)(?:-(\d{2}))?-occonly$")
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
VOLUME_RE = re.compile(r"^([A-Z]{2})([VSWLO]*)(\d{2})([a-z]*)-colonly$")
BLINK_LETTER_BITS = {"V": 0x2, "S": 0x4, "W": 0x8, "L": 0x10, "O": 0x20}


def clean_name(name):
    return BLENDER_SUFFIX.sub("", name)


def is_root_bone(name):
    """A rig's `Root` bone, in any case: the ground under the character, no
    part (animation.py)."""
    return clean_name(name).lower() == "root"


def shader_table():
    """The engine's shader tags and capability words, in table order; an
    ExportError when `opennova-3di catalog` gave none (every shader flag export
    writes depends on it)."""
    from . import catalog, catalog_error
    table = catalog()[2]
    if not table:
        raise ExportError(f"no shader table: {catalog_error()}")
    return table


def shader_flags(tag):
    """A tag's capability word, matched without case as the runtime's effect
    lookup does; a tag outside the table reads row 0's, as OED's lookup did
    (lookup_material_info_flags, 5fc5b4f6a^ engine/formats/oed/
    material_utils.cpp)."""
    table = shader_table()
    for name, flags in table:
        if name.lower() == tag.lower():
            return flags
    return table[0][1]


def default_shader(map_count, skinned):
    """The shader of a material whose name carries none: the first table row
    of the model's kind (skinned or not) drawing that many texture maps
    (diffuse, detail), OED's find_material_index_by_flags (5fc5b4f6a^
    engine/formats/oed/convert_internal.cpp): FF_ST_OP for one map, FF_MT_OP
    for two, FFP_GLASS for none; VS_SKBASIC / VS_SKGLASS on a skinned model."""
    wanted = max(0, min(2, map_count))
    table = shader_table()
    for name, flags in table:
        if bool(flags & FLAG_SKINNED) != skinned:
            continue
        if (1 if flags & FLAG_DIFFUSE else 0) + (1 if flags & FLAG_SECONDARY else 0) == wanted:
            return name
    return table[0][0]


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


def fixed(value, scale, lo, hi, what):
    """`value` in the word the text carries it into (`value * scale` rounded,
    lo..hi); an ExportError naming `what` when it does not fit, which the CLI
    would refuse or the writer wrap or clamp."""
    raw = round(value * scale) if math.isfinite(value) else None
    if raw is None or not lo <= raw <= hi:
        raise ExportError(f"{what} is {value:g}; the file holds {lo / scale:g} to {hi / scale:g}")
    return raw


def write_tga(image, path):
    """Uncompressed 32-bit truecolor TGA, rows bottom-up (the retail shape)."""
    w, h = image.size
    if w == 0 or h == 0:
        raise ExportError(f"image {image.name} has no pixels")
    # Blender bundles NumPy. Bulk access avoids expanding a 4K image into
    # millions of Python floats; bounded chunks keep conversion memory small.
    px = np.empty(w * h * 4, dtype=np.float32)
    image.pixels.foreach_get(px)
    px = px.reshape(-1, 4)
    if not np.isfinite(px).all():
        raise ExportError(f"image {image.name} has non-finite pixels")
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 8)
    with open(path, "wb") as f:
        f.write(header)
        for start in range(0, len(px), 262144):
            values = px[start:start + 262144].astype(np.float64)
            # float64 and rint preserve Python round's ties-to-even result.
            values = np.clip(np.rint(values * 255.0), 0, 255).astype(np.uint8)
            f.write(values[:, [2, 1, 0, 3]].tobytes())


def material_image(mat):
    """The image a material without texture entries exports: the image
    texture wired (through any nodes) into its Principled BSDF's Base Color,
    else its first image texture node."""
    if mat is None or not mat.use_nodes or mat.node_tree is None:
        return None
    nodes = mat.node_tree.nodes
    bsdf = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is not None:
        seen, queue = set(), [bsdf.inputs["Base Color"]]
        while queue:
            for link in queue.pop(0).links:
                node = link.from_node
                if node.type == "TEX_IMAGE" and node.image is not None:
                    return node.image
                if node.name not in seen:
                    seen.add(node.name)
                    queue.extend(s for s in node.inputs if s.is_linked)
    return next((n.image for n in nodes if n.type == "TEX_IMAGE" and n.image is not None), None)


def slot_material(ev, slot):
    """The material of an evaluated object's slot (a Geometry Nodes or
    object-linked material included), as the original data-block: the
    evaluated copy does not carry the add-on's properties."""
    mat = ev.material_slots[slot].material if slot < len(ev.material_slots) else None
    return mat.original if mat is not None else None


def bone_parent_part(bone):
    """The part a BN## bone's parent is: the nearest BN## bone above it, past
    `Root` and any `!` control bones; None at the root."""
    above = bone.parent
    while above is not None:
        m = BONE_RE.match(clean_name(above.name))
        if m:
            return int(m.group(1)) - 1
        above = above.parent
    return None


def is_lod_root(ob):
    return ob.type == "EMPTY" and "_lod_index" in ob


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


def active_model(context):
    """The model the active object belongs to, else the scene's only model."""
    model = model_of(context.object) if context.object is not None else None
    if model is None:
        roots = model_roots(context.scene)
        model = roots[0] if len(roots) == 1 else None
    return model


def descendants(ob):
    for child in ob.children:
        if is_model_root(child):
            continue  # another model parented here is its own .3di
        yield child
        yield from descendants(child)


def order_key(ob):
    order = ob.o3d.order
    return (order if order >= 0 else 1 << 30, clean_name(ob.name))


def point_key(label, ob):
    """A user point's USRP place: its `order`, then (our own rule for points
    without one; retail's exporter kept the scene order) its label, so seats
    `sitex01`, `sitex02` stay in seat order whichever parts they sit on."""
    order = ob.o3d.order
    return (order if order >= 0 else 1 << 30, label.lower(), clean_name(ob.name))


class Lod:
    """One LOD root, classified."""

    def __init__(self, root):
        self.root = root
        self.parts = {}     # part index -> PN## empty
        self.meshes = {}    # part index -> [(ordinal, object)]
        self.centers = {}   # part index -> helper
        self.attach = {}    # child part index -> parent part index
        self.attach_points = []  # (part index, x rank, name, parent, helper)
        self.anchors = {}   # part index -> its first attach helper
        self.points = []    # (type letter, part index or -1, label, object)
        self.lights = []    # (part index, object)
        self.occluders = []  # (type, section, connecting, object)
        self.volumes = []   # (type, flags, part index, object, (code, dup rank))
        self.armature = None  # skinned: the Armature; parts are its bones
        self.bone_count = 0   # skinned: parts past the bones are the meshes


class Exporter(Notes):
    def __init__(self, context, model):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.props = model.o3d  # the model's name, output path, collision LOD
        self.settings = self.scene.o3d  # forward axis, textures, executable
        self.space = None  # set in run(): the model root's frame
        self.depsgraph = None  # set in run(), with every rig at rest
        self.registers = []
        self.materials = []
        self.material_index = {}
        self.textures = {}  # file name (lower case) -> (file name, image)
        self.frames = []
        self.skinned = False
        self.uv1 = False
        self.bone_points = {}  # skinned LOD0: part -> rest positions it moves
        self.notes = []

    # --- helpers ------------------------------------------------------------
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
        ev = ob.evaluated_get(self.depsgraph)
        if not ev.is_evaluated:
            shaped = ob.type == "MESH" and ob.data.shape_keys is not None
            if shaped or any(m.show_viewport and m.type != "ARMATURE" for m in ob.modifiers):
                raise ExportError(f"{ob.name}: Blender does not evaluate it (it or its collection is disabled in "
                                  "viewports, or its collection is excluded), so its modifiers or shape keys "
                                  "would be lost: enable it in viewports, or apply them")
        return ev

    def claim_texture(self, name, image, mat):
        """A texture file export writes: one image per file name (Windows
        names match without case)."""
        have = self.textures.get(name.lower())
        if have is not None and have[1] != image:
            raise ExportError(f"{mat.name}: the images '{have[1].name}' and '{image.name}' would both be written "
                              f"as {name}: give each its own file name (one derived from an image's name keeps "
                              "its first 12 characters)")
        self.textures[name.lower()] = (name, image)

    def material_for(self, mat):
        key = mat.name if mat is not None else None
        if key not in self.material_index:
            self.material_index[key] = len(self.materials)
            self.materials.append(mat)
        return self.material_index[key]

    def frame_index(self, ob):
        """The MTRX row a rotated PN## selects: its rotation in the model."""
        return self.frame_of(self.space.world(ob).to_3x3().normalized(), ob.name)

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

    # --- classification -----------------------------------------------------
    def lod_roots(self):
        roots = [o for o in self.model.children if is_lod_root(o)]
        by_index = {}
        for o in roots:
            i = int(o["_lod_index"])
            if i in by_index:
                raise ExportError(f"{self.model.name}: two LOD roots carry _lod_index {i}: {by_index[i].name}, "
                                  f"{o.name}")
            by_index[i] = o
        if 0 not in by_index:
            raise ExportError(f"{self.model.name}: no LOD root: add an Empty with the custom property "
                              "_lod_index = 0 under the model root, above PN01")
        order = sorted(by_index)
        if order != list(range(len(order))):
            raise ExportError(f"{self.model.name}: _lod_index values are not contiguous from 0: {order}")
        return [by_index[i] for i in order]

    def classify(self, root, primary):
        lod = Lod(root)
        seen = {}

        def claim(identity, ob):
            if identity in seen:
                raise ExportError(f"{root.name}: '{seen[identity].name}' and '{ob.name}' are the same {identity[0]}")
            seen[identity] = ob

        def lod0_only(ob, what):
            self.note(f"{ob.name}: {what} are read from LOD 0 only; not exported from {root.name}")

        for ob in descendants(root):
            raw = clean_name(ob.name)
            if raw.startswith("!"):
                # Ignored, and so is an armature named `!...`: it is not the
                # model's mesh rig but a rigid model's animation rig, whose
                # BN## bones mirror its parts so its clips can be authored
                # (animation.py). The model reads its parts from the PN##
                # empties as always.
                continue
            if ob.type == "ARMATURE":
                if lod.armature is not None:
                    raise ExportError(f"{root.name}: two armatures ({lod.armature.name}, {ob.name})")
                lod.armature = ob
                for bone in ob.data.bones:
                    # `Root` (the ground the clips stand on) and a bone whose
                    # name starts with `!` (a control bone an author rigs
                    # with) are not parts.
                    if clean_name(bone.name).startswith("!") or is_root_bone(bone.name):
                        continue
                    bm = BONE_RE.match(clean_name(bone.name))
                    if not bm:
                        raise ExportError(f"{ob.name}: bone '{bone.name}' is not named BN##")
                    index = int(bm.group(1)) - 1
                    if index < 0:
                        raise ExportError(f"{ob.name}: bones start at BN01")
                    claim(("part", index), bone)
                    lod.parts[index] = bone
                continue
            if ob.type == "LIGHT":
                m = LIGHT_RE.match(raw)
                if not m:
                    raise ExportError(f"{ob.name}: a light is named LP## (## = its owning part, 01 the root)")
                if primary:
                    claim(("light", m.group(1), m.group(2)), ob)
                    lod.lights.append((max(0, int(m.group(1)) - 1), ob))
                else:
                    lod0_only(ob, "lights")
                continue
            m = PART_RE.match(raw)
            if m and ob.type == "EMPTY":
                index = int(m.group(1)) - 1
                if index < 0:
                    raise ExportError(f"{ob.name}: parts start at PN01 (index 00 is silently dropped by OED)")
                claim(("part", index), ob)
                lod.parts[index] = ob
                continue
            m = MESH_RE.match(raw)
            if m and ob.type == "MESH":
                index, ordinal = int(m.group(1)) - 1, int(m.group(2) or "0")
                if index < 0:
                    raise ExportError(f"{ob.name}: a mesh names its part, and parts start at 01")
                claim(("mesh", index, ordinal), ob)
                lod.meshes.setdefault(index, []).append((ordinal, ob))
                continue
            m = CENTER_RE.match(raw)
            if m:
                index = int(m.group(1)) - 1
                claim(("center", index), ob)
                lod.centers[index] = ob
                continue
            m = ATTACH_RE.match(raw)
            if m:
                child = self.helper_part(ob)
                if child is None:
                    raise ExportError(f"{ob.name}: an attach helper sits under its part (a PN## empty, or a skinned "
                                      "model's BN## bone or mesh part)")
                lod.attach_points.append((child, dup_rank(m.group(2)), raw, int(m.group(1)) - 1, ob))
                continue
            m = POINT_RE.match(raw)
            if m:
                if primary:
                    label = m.group(3) if m.group(3) is not None else "Noname"
                    if len(label) > 15:
                        raise ExportError(f"{ob.name}: user point label '{label}' exceeds 15 characters")
                    lod.points.append((m.group(1), int(m.group(2)) - 1, label, ob))
                else:
                    lod0_only(ob, "user points")
                continue
            if OCCLUSION_RE.search(raw):
                m = OCC_RE.match(raw)
                if not m or ob.type != "MESH":
                    raise ExportError(f"{ob.name}: occlusion meshes are OB##, OS##, OP##[-MM] or OH##, then "
                                      "-occonly")
                prefix, nn, dup, mm = m.groups()
                if nn == "00" or mm == "00":
                    raise ExportError(f"{ob.name}: occlusion sections start at 01")
                claim(("occlusion", prefix, nn, dup, mm), ob)
                if primary:
                    kind = 3 if prefix == "OP" and mm else OCC_TYPES[prefix]
                    lod.occluders.append((kind, int(nn) - 1, int(mm) - 1 if mm else 0, ob))
                else:
                    lod0_only(ob, "occlusion meshes")
                continue
            m = VOLUME_RE.match(raw)
            if m and ob.type == "MESH":
                code, letters, nn, dup = m.groups()
                if code not in VOLUME_CODES and code[0] not in UNLISTED_TYPE_LETTERS:
                    raise ExportError(f"{ob.name}: unknown collision code '{code}'")
                if letters and code != "BB":
                    raise ExportError(f"{ob.name}: only blink boxes (BB) take flag letters")
                flags = 0
                if code == "BB":
                    flags = 0x3E
                    for letter in letters:
                        flags &= ~BLINK_LETTER_BITS[letter]
                claim(("volume", code + letters, nn, dup), ob)
                if primary:
                    lod.volumes.append((VOLUME_CODES.get(code, 0), flags, int(nn) - 1, ob,
                                        (code + letters, dup_rank(dup))))
                else:
                    lod0_only(ob, "collision volumes")
                continue
            if ob.type != "EMPTY" or not ob.children:
                # An empty that only groups objects is left alone: its
                # children are classified by their own names.
                self.note(f"{ob.name}: not a name of the naming contract (docs/threedi/scene-naming-contract.md) "
                          f"for a {ob.type.lower()}; not exported")
        if not lod.parts:
            if lod.meshes or lod.armature is not None:
                raise ExportError(f"{root.name}: meshes but no PN## part empties (or BN## armature bones)")
            return lod  # an empty LOD (retail ships them)
        if lod.armature is not None and any(getattr(p, "type", None) == "EMPTY" for p in lod.parts.values()):
            raise ExportError(f"{root.name}: a skinned LOD's parts are its BN## bones, not PN## empties")
        count = max(lod.parts) + 1
        missing = [i + 1 for i in range(count) if i not in lod.parts]
        if missing:
            raise ExportError(f"{root.name}: parts are not contiguous from PN01 (missing PN{missing[0]:02d})")
        if lod.armature is not None:
            # A skinned mesh "## Mesh<n>" authors its strips on part ##. The
            # builder moves every strip to the root ROBJ, as retail stores
            # them, and leaves each part the bounds of what is authored on it.
            # ## is a bone (dM1A1's hull: "01 Mesh0" on BN01, no mesh part) or
            # a mesh part after the bones: parent 0, pivot = the first mesh's
            # origin, as the retail exporter wrote bones first and mesh
            # objects after them (ArmsG: 37 bones + part 37; FSldr03: 19 + 19).
            lod.bone_count = len(lod.parts)
            if not lod.meshes:
                raise ExportError(f"{root.name}: no '## Mesh<n>' skinned mesh")
            extra = sorted(i for i in lod.meshes if i >= lod.bone_count)
            if extra != list(range(lod.bone_count, lod.bone_count + len(extra))):
                raise ExportError(f"{root.name}: a skinned mesh names a bone (BN01..BN{lod.bone_count:02d}) or a "
                                  f"mesh part numbered on from {lod.bone_count + 1:02d} without gaps")
            for index in extra:
                lod.parts[index] = min(lod.meshes[index], key=lambda e: e[0])[1]
            if lod.centers:
                self.note(f"{root.name}: a skinned part's pivot is its bone head (or its mesh part's origin); the "
                          "_## center helpers are not exported")
        else:
            for index, meshes in lod.meshes.items():
                if index not in lod.parts:
                    raise ExportError(f"{root.name}: mesh '{index + 1:02d} Mesh…' has no PN{index + 1:02d}")
                owner = [self.owning_part(ob) for _, ob in meshes]
                if any(o != index for o in owner):
                    raise ExportError(f"{root.name}: '{index + 1:02d} Mesh…' must sit under PN{index + 1:02d}")
            for index, ob in lod.centers.items():
                if index not in lod.parts:
                    self.note(f"{ob.name}: there is no PN{index + 1:02d}; not exported")
        # The attach helpers: each part's first (in x order) names its parent
        # and is its attach point; a skinned part's parent stays its bone's.
        lod.attach_points.sort(key=lambda e: (e[0], e[1], e[2], e[4].name))
        for child, _, name, parent, ob in lod.attach_points:
            if child not in lod.parts:
                raise ExportError(f"{ob.name}: its part {child + 1:02d} does not exist")
            if child in lod.anchors:
                self.note(f"{ob.name}: part {child + 1:02d} takes its parent and attach point from "
                          f"'{lod.anchors[child].name}'; this helper is not exported")
                continue
            lod.anchors[child] = ob
            if lod.armature is None:
                if parent >= count:
                    raise ExportError(f"{ob.name}: names PN{parent + 1:02d} its part's parent, which {root.name} "
                                      f"lacks ({count} parts)")
                lod.attach[child] = parent
            elif parent != self.part_parent(lod, child):
                self.note(f"{ob.name}: part {child + 1:02d}'s parent is its bone's parent "
                          f"({self.part_parent(lod, child) + 1:02d}) in a skinned model")
        return lod

    @staticmethod
    def owning_part(ob):
        p = ob.parent
        while p is not None:
            m = PART_RE.match(clean_name(p.name))
            if m and p.type == "EMPTY":
                return int(m.group(1)) - 1
            p = p.parent
        return None

    @staticmethod
    def helper_part(ob):
        """The part an attach helper sits under: the nearest PN## empty above
        it, or on a skinned model the BN## bone it is parented to or the
        `## Mesh<n>` mesh part it sits under."""
        p = ob
        while p.parent is not None:
            above = p.parent
            if above.type == "ARMATURE" and p.parent_type == "BONE":
                m = BONE_RE.match(clean_name(p.parent_bone))
                return int(m.group(1)) - 1 if m else None
            if above.type == "EMPTY":
                m = PART_RE.match(clean_name(above.name))
                if m:
                    return int(m.group(1)) - 1
            if above.type == "MESH" and above.parent is not None and above.parent.type == "ARMATURE":
                m = MESH_RE.match(clean_name(above.name))
                if m:
                    return int(m.group(1)) - 1
            p = above
        return None

    def part_parent(self, lod, index):
        if lod.armature is not None:
            if index >= lod.bone_count:
                return 0
            above = bone_parent_part(lod.parts[index])
            return above if above is not None else 0
        if index in lod.attach:
            return lod.attach[index]
        if index == 0:
            return 0
        above = self.owning_part(lod.parts[index])
        return above if above is not None else 0

    def part_pivot(self, lod, index):
        if lod.armature is not None:
            if index >= lod.bone_count:
                return self.space.mission(self.space.world(lod.parts[index]).translation)
            return self.space.mission(self.space.world(lod.armature) @ lod.parts[index].head_local)
        ob = lod.centers.get(index, lod.parts[index])
        return self.space.mission(self.space.world(ob).translation)

    def part_centre(self, lod, index):
        """The point a rigid part that draws nothing seeds its bounds with
        (radius 0): its `_## center` helper mesh's first vertex, as OED's
        placeholder injection took it (5fc5b4f6a^ engine/formats/oed/
        convert_internal.cpp); None without one, and the part's bounds sit at
        the origin."""
        ob = lod.centers.get(index)
        if ob is None or ob.type != "MESH" or len(ob.data.vertices) == 0:
            return None
        return self.space.mission(self.space.world(ob) @ ob.data.vertices[0].co)

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
        ev = self.evaluated(ob)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.space.world(ob)
            nmat = mw.to_3x3().inverted_safe().transposed()
            mirrored = mw.to_3x3().determinant() < 0
            normals = mesh.corner_normals
            uv0, uv1 = self.uv_layers(mesh)
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = slot_material(ev, slot)
                s = strips.setdefault(self.material_for(mat), {"verts": [], "index": {}, "tris": []})
                corners = []
                for li in reversed(tri.loops) if mirrored else tri.loops:
                    vert = self.corner(mesh, mesh.loops[li], mw, nmat, normals, uv0, uv1)
                    corners.append(shared_vertex(s, tuple(round(x, 5) for x in vert), vert, corners))
                s["tris"].append(corners)
        finally:
            ev.to_mesh_clear()

    def skinned_strips(self, ob, strips, collect_bones, count):
        """A skinned mesh's triangles grouped per material into strips whose
        bone tables stay within 16 parts. Each corner carries its rest-pose
        position and up to three (part, weight) influences from BN## groups,
        each naming one of the LOD's `count` parts."""
        groups = {}
        for g in ob.vertex_groups:
            m = BONE_RE.match(clean_name(g.name))
            if m:
                groups[g.index] = int(m.group(1)) - 1
        ev = self.evaluated(ob)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.space.world(ob)
            nmat = mw.to_3x3().inverted_safe().transposed()
            mirrored = mw.to_3x3().determinant() < 0
            normals = mesh.corner_normals
            uv0, uv1 = self.uv_layers(mesh)
            influences = []
            for v in mesh.vertices:
                w = sorted(((g.weight, groups[g.group]) for g in v.groups if g.group in groups and g.weight > 1e-4),
                           reverse=True)[:3]
                if not w:
                    member = [groups[g.group] for g in v.groups if g.group in groups]
                    if not member:
                        raise ExportError(f"{ob.name}: vertex {v.index} has no BN## weight")
                    # Only weight-0 memberships: every weight stays zero, as
                    # retail stores some (dM1A1's LOD 3); the vertex moves
                    # with no bone of its own.
                    influences.append([(member[0], 0.0)])
                    continue
                total = sum(x for x, _ in w)
                influences.append([(b, x / total) for x, b in w])
                if collect_bones:
                    # Every LOD 0 vertex a bone moves bounds that bone's
                    # section (WriteCOBJ's skinned rule).
                    for _, b in w:
                        self.bone_points.setdefault(b, []).append(self.space.mission(mw @ v.co))
            missing = sorted({b for inf in influences for b, _ in inf if b >= count})
            if missing:
                raise ExportError(f"{ob.name}: its vertex group BN{missing[0] + 1:02d} names no part (the LOD has "
                                  f"{count}), so no strip's bone table can hold it")
            per_material = {}
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = slot_material(ev, slot)
                corners = []
                for li in reversed(tri.loops) if mirrored else tri.loops:
                    loop = mesh.loops[li]
                    corners.append((self.corner(mesh, loop, mw, nmat, normals, uv0, uv1),
                                    influences[loop.vertex_index]))
                per_material.setdefault(self.material_for(mat), []).append(corners)
            for mi, tris in per_material.items():
                current = None
                for corners in tris:
                    need = {b for _, inf in corners for b, _ in inf}
                    if (current is None or len(current["table"] | need) > 16 or
                            len(current["tris"]) >= 65535 // 3):
                        current = {"table": set(), "order": [], "verts": [], "index": {}, "tris": []}
                        strips.setdefault(mi, []).append(current)
                    for b in sorted(need - current["table"]):
                        current["table"].add(b)
                        current["order"].append(b)
                    local = {b: i for i, b in enumerate(current["order"])}
                    ids = []
                    for vert, inf in corners:
                        inf3 = (inf + [(inf[0][0], 0.0)] * 3)[:3]
                        full = vert + tuple(local[b] for b, _ in inf3) + tuple(w for _, w in inf3)
                        ids.append(shared_vertex(current, tuple(round(x, 5) for x in full), full, ids))
                    current["tris"].append(ids)
        finally:
            ev.to_mesh_clear()

    def strip_alpha(self, mat):
        """Strips of a blending shader draw in the alpha pass (OED's
        material_alpha: the BLENDING capability bit; FFP_GLASS is one)."""
        blending = shader_flags(self.shader_of(mat)) & FLAG_BLENDING
        return 1 if blending or (mat is not None and mat.o3d.alpha_strips) else 0

    def emit_lod(self, lod, lines):
        if lod.armature is not None:
            return self.emit_skinned_lod(lod, lines)
        p = lod.root.o3d
        lines.append(f"lod {p.lod_threshold} {quoted(p.lod_type or 'gnrc')}  # {lod.root.name}")
        count = len(lod.parts)
        for i in range(count):
            strips = {}
            for _, ob in sorted(lod.meshes.get(i, []), key=lambda e: e[0]):
                self.mesh_strips(ob, strips)
            line = f"part {self.part_parent(lod, i)} {fmt(*self.part_pivot(lod, i))}"
            centre = self.part_centre(lod, i) if not strips else None
            if centre is not None:
                line += " " + fmt(*centre)
            lines.append(line + f"  # PN{i + 1:02d}")
            for mi, s in sorted(strips.items()):
                if len(s["verts"]) > 65535:
                    raise ExportError(f"PN{i + 1:02d}: one material has more than 65535 vertices")
                if len(s["tris"]) > 65535 // 3:
                    raise ExportError(f"PN{i + 1:02d}: one material has more than {65535 // 3} triangles (a strip "
                                      "holds 65535 indices)")
                lines.append(f"strip {mi} {self.strip_alpha(self.materials[mi])}")
                for v in s["verts"]:
                    lines.append("v " + fmt(*v))
                for t in s["tris"]:
                    lines.append(f"t {t[0]} {t[1]} {t[2]}")
        for i in range(count):
            part = lod.parts[i]
            self.emit_panm(lod, i, part.o3d, self.frame_index(part), part.name, lines)

    def emit_panm(self, lod, i, p, frame, what, lines):
        """A part's PANM row: its flags (derived from the tracks unless set),
        MTRX frame and tracks, from an empty's or a bone's part animation. A
        part has one track per target (PANM stores one slot each)."""
        tracks = [(t.target, t.style, int(t.axis) if t.target == "trans" else 0) for t in p.tracks]
        targets = [t.target for t in p.tracks]
        twice = sorted({t for t in targets if targets.count(t) > 1})
        if twice:
            raise ExportError(f"{what}: {targets.count(twice[0])} {twice[0]} tracks; a part has one track per target")
        flags = p.panm_flags if p.panm_flags >= 0 else derived_panm_flags(tracks)
        line = f"panm {i} {self.part_parent(lod, i)}"
        if p.panm_flags >= 0 or frame:
            line += f" 0x{flags:08x}" + (f" {frame}" if frame else "")
        lines.append(line)
        for t in p.tracks:
            lines.append(self.track_line(t, what))

    def emit_skinned_lod(self, lod, lines):
        p = lod.root.o3d
        lines.append(f"lod {p.lod_threshold} {quoted(p.lod_type or 'gnrc')}  # {lod.root.name}")
        count = len(lod.parts)
        primary = lod is self.lod0
        for i in range(count):
            label = f"BN{i + 1:02d}" if i < lod.bone_count else lod.parts[i].name
            lines.append(f"part {self.part_parent(lod, i)} {fmt(*self.part_pivot(lod, i))}  # {label}")
            strips = {}
            for _, ob in sorted(lod.meshes.get(i, []), key=lambda e: e[0]):
                self.skinned_strips(ob, strips, primary, count)
            for mi in sorted(strips):
                for s in strips[mi]:
                    if len(s["verts"]) > 65535:
                        raise ExportError(f"{label}: one strip has more than 65535 vertices")
                    if len(s["tris"]) > 65535 // 3:
                        raise ExportError(f"{label}: one strip has more than {65535 // 3} triangles (a strip "
                                          "holds 65535 indices)")
                    lines.append(f"strip {mi} {self.strip_alpha(self.materials[mi])}")
                    lines.append("bones " + " ".join(str(b) for b in s["order"]))
                    uv_end = 10 if self.uv1 else 8
                    for v in s["verts"]:
                        lines.append("v " + fmt(*v[:uv_end]) + " " + " ".join(str(int(x)) for x in v[uv_end:uv_end + 3]) +
                                     " " + fmt(*(float(x) for x in v[uv_end + 3:uv_end + 6])))
                    for t in s["tris"]:
                        lines.append(f"t {t[0]} {t[1]} {t[2]}")
        # A bone's part animation lives on the bone (its tracks turn about
        # its track frame, a rotation of the model's axes: dM1A1's turret
        # ring and wheels); a mesh part has none.
        arm = self.space.world(lod.armature).to_3x3().normalized()
        for i in range(count):
            if i < lod.bone_count:
                p = lod.parts[i].o3d
                what = f"{lod.armature.name} bone {lod.parts[i].name}"
                self.emit_panm(lod, i, p, self.frame_of(arm @ Euler(p.frame).to_matrix(), what), what, lines)
            else:
                lines.append(f"panm {i} {self.part_parent(lod, i)}")

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

    # --- materials ------------------------------------------------------------
    def shader_of(self, mat):
        """The name's tag (Material_<i>_<SHADER>), else the default for the
        material's texture maps (default_shader)."""
        m = MATERIAL_RE.match(clean_name(mat.name)) if mat is not None else None
        if m:
            return m.group(2)
        if mat is None:
            maps = 0
        elif len(mat.o3d.textures) > 0:
            maps = len({t.slot for t in mat.o3d.textures if t.slot in (1, 2)})
        else:
            maps = 1 if material_image(mat) is not None else 0
        return default_shader(maps, self.skinned)

    def generator_register(self, style, name, what):
        return self.register(name, what) if style > CTRL_REFERENCE_THRESHOLD else -1

    def emit_materials(self, lines):
        for mat in self.materials:
            # A mesh without a material draws with the shader a material
            # without textures takes (shader_of), its strips' pass included.
            shader = self.shader_of(mat)
            if len(shader) > 32:
                raise ExportError(f"{mat.name}: the shader tag '{shader}' exceeds 32 characters")
            caps = shader_flags(shader)
            lines.append(f"material {quoted(shader)}  # {mat.name if mat is not None else '(no material)'}")
            p = mat.o3d if mat is not None else None
            if p is not None:
                flags = (1 if p.alpha_test else 0) | (4 if p.two_sided else 0) | (p.other_flags & ~5 & 0xFF)
                if flags:
                    lines.append(f"matflags {flags}")
                if p.alpha_test:
                    lines.append(f"alphatest {p.alpha_test_value}")
            # The OED material rule (5fc5b4f6a^ export_3di.cpp, WriteMTRL): a
            # GLASS shader reflects 0x80 grey unless another colour is set, and
            # is glass while it reflects; an EMISSIVE one (*_LUM) is emissive
            # type 2. It holds for every material of the 958 JO models.
            reflect = [round(c * 255) for c in p.reflect] if p is not None else [0, 0, 0, 0]
            if caps & FLAG_GLASS and not any(reflect):
                reflect = [128, 128, 128, 0]
            if caps & FLAG_GLASS and any(reflect[:3]):
                lines.append("glass 1")
            if caps & FLAG_EMISSIVE:
                lines.append("emissive 2")
            if any(reflect):
                lines.append("reflect " + " ".join(str(c) for c in reflect))
            if p is None:
                continue
            if len(p.textures) > 0:
                for t in p.textures:
                    name = t.name.strip()
                    if not name and t.image is not None:
                        name = os.path.splitext(clean_name(t.image.name))[0][:12] + ".tga"
                    if not name:
                        raise ExportError(f"{mat.name}: a texture entry has neither a file name nor an image")
                    if len(name) > 16:
                        raise ExportError(f"{mat.name}: texture name '{name}' exceeds 16 characters")
                    lines.append(f"texture {quoted(name)} {t.slot} {t.type} {t.flags} {t.frame}")
                    if t.image is not None and t.write:
                        if name.lower().endswith(".tga"):
                            self.claim_texture(name, t.image, mat)
                        else:
                            self.note(f"{mat.name}: Write TGA writes .tga files only; {name} is not written")
            elif caps & FLAG_DIFFUSE:
                image = material_image(mat)
                if image is not None:
                    name = os.path.splitext(clean_name(image.name))[0][:12] + ".tga"
                    lines.append(f"texture {quoted(name)}")
                    self.claim_texture(name, image, mat)
            if p.anim_frames or p.anim_type or p.anim_time:
                if p.anim_type not in (0, 1):
                    raise ExportError(f"{mat.name}: the flipbook's anim type is {p.anim_type}; it is 0 (time) or 1 "
                                      "(register)")
                if p.anim_type == 1:
                    time_or_register = self.register(p.anim_register, f"{mat.name} texture flipbook")
                else:
                    time_or_register = fixed(p.anim_time, 1, -0x8000, 0x7FFF, f"{mat.name}: the flipbook frame time")
                lines.append(f"texanim {p.anim_frames} {p.anim_type} {time_or_register}")
            # A generator's words: an RGB rate a u16 of 1/256 steps, the other
            # rates and the U/V start and end int16 8.8, an alpha start and end
            # int16, a phase (styles up to 112) a byte of 1/256 turns.
            if p.rgb_style:
                what = f"{mat.name}: the RGB gen"
                reg = self.generator_register(p.rgb_style, p.rgb_register, what)
                fixed(p.rgb_rate, 256.0, 0, 0xFFFF, what + " rate")
                self.generator_phase(p.rgb_style, p.rgb_phase, what)
                s = [round(c * 255) for c in p.rgb_start]
                e = [round(c * 255) for c in p.rgb_end]
                lines.append(f"rgbgen {p.rgb_style} {reg} {fmt(float(p.rgb_rate))} {s[0]} {s[1]} {s[2]} "
                             f"{e[0]} {e[1]} {e[2]} {fmt(float(p.rgb_phase))}")
            if p.alpha_style:
                what = f"{mat.name}: the alpha gen"
                reg = self.generator_register(p.alpha_style, p.alpha_register, what)
                fixed(p.alpha_rate, 256.0, -0x8000, 0x7FFF, what + " rate")
                start = fixed(p.alpha_start, 1, -0x8000, 0x7FFF, what + " start")
                end = fixed(p.alpha_end, 1, -0x8000, 0x7FFF, what + " end")
                self.generator_phase(p.alpha_style, p.alpha_phase, what)
                lines.append(f"alphagen {p.alpha_style} {reg} {fmt(float(p.alpha_rate))} {start} {end} "
                             f"{fmt(float(p.alpha_phase))}")
            for axis in ("u", "v"):
                style = getattr(p, axis + "_style")
                if style:
                    what = f"{mat.name}: the {axis.upper()} gen"
                    reg = self.generator_register(style, getattr(p, axis + "_register"), what)
                    values = [float(getattr(p, f"{axis}_{field}")) for field in ("rate", "start", "end")]
                    for value, field in zip(values, ("rate", "start", "end")):
                        fixed(value, 256.0, -0x8000, 0x7FFF, f"{what} {field}")
                    phase = float(getattr(p, axis + "_phase"))
                    self.generator_phase(style, phase, what)
                    lines.append(f"{axis}gen {style} {reg} {fmt(*values)} {fmt(phase)}")

    @staticmethod
    def generator_phase(style, phase, what):
        """A generator's phase byte (styles up to 112; above, that byte is the
        register index): the writer would clamp one outside it."""
        if style <= CTRL_REFERENCE_THRESHOLD:
            fixed(phase, 256.0, 0, 0xFF, what + " phase")

    # --- user points, lights, occlusion -------------------------------------
    def emit_points(self, lod, lines):
        # USRP order: each helper's `order` (the imported index), then label.
        # The seat scan reads `sitex` without case and stops at 8 [orig:
        # Entity_GetBoneSlotType @ 0x434ED0; the scan end @ 0x43A5AF].
        seats = [ob.name for _, _, label, ob in lod.points if label.lower().startswith("sitex")]
        if len(seats) > 8:
            raise ExportError(f"{len(seats)} sitex seats ({', '.join(sorted(seats))}); the game reads 8")
        for letter, part, label, ob in sorted(lod.points, key=lambda e: point_key(e[2], e[3])):
            pos = self.space.mission(self.space.world(ob).translation)
            d = self.space.mission((self.space.world(ob).to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
            lines.append(f"userpoint {quoted(label)} {fmt(*pos)} {fmt(*d)} {part} {ord(letter.upper())}")

    def emit_lights(self, lod, count, lines):
        for part, ob in sorted(lod.lights, key=lambda e: order_key(e[1])):
            if part >= max(count, 1):
                raise ExportError(f"{ob.name}: part {part + 1:02d} does not exist")
            data, p = ob.data, ob.data.o3d
            spot = data.type == "SPOT"
            if data.type not in ("POINT", "SPOT"):
                raise ExportError(f"{ob.name}: a light is a point or spot light")
            pos = self.space.mission(self.space.world(ob).translation)
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
            d = self.space.mission((self.space.world(ob).to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized())
            falloff = math.degrees(data.spot_size) / 2.0 if spot else 0.0
            if spot or abs(d[0]) > 1e-6 or abs(d[1]) > 1e-6 or d[2] > -1.0 + 1e-9:
                line += " " + fmt(*d, float(falloff))
            lines.append(line)

    def emit_occlusion(self, lod, lines):
        for kind, section, connecting, ob in sorted(lod.occluders, key=lambda e: (order_key(e[3]), e[1])):
            ev = self.evaluated(ob)
            mesh = ev.to_mesh()
            try:
                if len(mesh.vertices) > 128:
                    raise ExportError(f"{ob.name}: an occlusion mesh holds at most 128 vertices")
                lines.append(f"occ {kind} {section} {connecting}  # {ob.name}")
                mw = self.space.world(ob)
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
            mw = self.space.world(ob)
            mirrored = mw.to_3x3().determinant() < 0
            verts = [self.space.mission(mw @ v.co) for v in mesh.vertices]
            tris = [tuple(reversed(t.vertices)) if mirrored else tuple(t.vertices) for t in mesh.loop_triangles]
        finally:
            ev.to_mesh_clear()
        if len(verts) < 4 or not tris:
            raise ExportError(f"collision volume {ob.name} needs at least 4 vertices and a face")
        return verts, tris

    def face_flags(self, mat):
        """A material's bullet-face flags: 1 (both sides) follows Two sided,
        as OED took both from one render attribute (export_3di.cpp
        material_flags); the others are the material's face settings."""
        if mat is None:
            return 0
        p = mat.o3d
        return ((1 if p.two_sided else 0) | (0x100 if p.face_never_hit else 0) |
                (0x800 if p.face_front_only else 0) | (p.face_other_flags & ~0x901))

    def emit_collision(self, lod0, bullet, lines):
        # One section per part of the collision LOD (WriteCOBJ walks that
        # LOD's subobjects: Dtruck2's collision LOD has 7 parts to LOD 0's 8,
        # CNet01's none), placed at that part's pivot with its parent.
        count = len(bullet.parts)
        sections = [{"verts": [], "index": {}, "faces": [], "volumes": []} for _ in range(count)]
        # Bullet faces: the collision LOD's part meshes, section = part,
        # counter-clockwise about their outward normal (the retail order).
        # Generate bullet faces off (first-person arms) keeps the volumes only.
        for index, meshes in (bullet.meshes.items() if self.props.export_bullet_faces else ()):
            s = sections[index]
            for _, ob in sorted(meshes, key=lambda e: e[0]):
                ev = self.evaluated(ob)
                mesh = ev.to_mesh()
                try:
                    mesh.calc_loop_triangles()
                    mw = self.space.world(ob)
                    mirrored = mw.to_3x3().determinant() < 0
                    for tri in mesh.loop_triangles:
                        corners = []
                        for vi in reversed(tri.vertices) if mirrored else tri.vertices:
                            p = self.space.mission(mw @ mesh.vertices[vi].co)
                            key = tuple(round(x, 4) for x in p)
                            if key not in s["index"]:
                                if not all(abs(x) < 128.0 for x in p):
                                    raise ExportError(f"{ob.name}: a vertex of the collision LOD lies at "
                                                      f"({fmt(*p)}); a collision vertex stays under 128 from the "
                                                      "model origin on each axis (CVRT stores 8.8 in an int16)")
                                s["index"][key] = len(s["verts"])
                                s["verts"].append(p)
                            corners.append(s["index"][key])
                        if len(set(corners)) != 3:
                            continue
                        slot = tri.material_index
                        mat = slot_material(ev, slot)
                        s["faces"].append((corners, mat.o3d.surface if mat is not None else 14, self.face_flags(mat)))
                finally:
                    ev.to_mesh_clear()
        # A part that draws nothing keeps the vertex OED seeded it with (its
        # `_## center` helper's first), and WriteCVRT wrote a section's part
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
            if not 0 <= part < count:
                raise ExportError(f"{ob.name}: its section {part + 1:02d} is not a part of the collision LOD "
                                  f"(LOD {self.props.poly_collision_lod} has {count})")
            sections[part]["volumes"].append((key, vtype, flags, ob))
        # Every section sits at its part's pivot (the COBJ offset retail
        # carries: Dtruck2's wheels, Dblkhwk1's rotors).
        pivots = [self.part_pivot(bullet, i) for i in range(count)]
        for i, s in enumerate(sections):
            lines.append(f"cobj {self.part_parent(bullet, i)} " + fmt(*pivots[i]))
            if self.skinned and not s["verts"]:
                # The retail person layout: a bone's section is bounded by
                # every LOD 0 vertex the bone moves, its radius the farthest of
                # them from their box's middle (WriteCOBJ's skinned rule); a
                # bone that moves none keeps an empty section.
                pts = self.bone_points.get(i, [])
                if pts:
                    mn = [min(p[k] for p in pts) for k in range(3)]
                    mx = [max(p[k] for p in pts) for k in range(3)]
                    c = [(mn[k] + mx[k]) * 0.5 for k in range(3)]
                    r = max(sum((p[k] - c[k]) ** 2 for k in range(3)) ** 0.5 for p in pts)
                    lines.append("csphere " + fmt(*(float(x) for x in c), float(r), *(float(x) for x in mn + mx)))
            for v in s["verts"]:
                lines.append("cv " + fmt(*v))
            for (a, b, c), poly, face_flags in s["faces"]:
                lines.append(f"cf {a} {b} {c} {poly} {face_flags}")
            # Volumes in name order within the section: code, then the
            # duplicate suffix (CB01, CB01a .. CB01z, CB01aa); OED kept its
            # scene order, which a Blender scene does not have.
            for key, vtype, flags, ob in sorted(s["volumes"], key=lambda e: e[0]):
                verts, tris = self.volume_mesh(ob)
                lines.append(f"cvmesh {vtype} {flags} {quoted(clean_name(ob.name))}")
                for v in verts:
                    lines.append("vv " + fmt(*v))
                for t in tris:
                    lines.append(f"vf {t[0]} {t[1]} {t[2]}")
        # CXLT: the collision LOD's attach points, which OED's WriteCXLT wrote
        # (5fc5b4f6a^ export_3di.cpp, "CXLT: attach points"). A scene holds
        # them for only some parts (the importer gives one to a part that
        # names itself or none, a rigid rig's parts get one each), so with
        # any helper in the LOD every row the retail count gives is written
        # (one per section after the root on a rigid model, one per section
        # on a skinned one, as the corpus stores them and the builder derives
        # them), each at its part's first helper, else at its pivot through
        # the very values its cobj line carries, so the CLI reads a row the
        # builder would derive as that row (our rule: OED wrote a row per
        # helper). With no helper the builder derives every row.
        if not bullet.anchors:
            return
        first = 0 if self.skinned else 1
        if first and 0 in bullet.anchors:
            self.note(f"{bullet.anchors[0].name}: a rigid model stores no attach point for its root section; "
                      "this one is not exported")
        for i in range(first, count):
            ob = bullet.anchors.get(i)
            if ob is None:
                lines.append("cxlt " + fmt(*pivots[i]) + f"  # section {i}: its pivot")
            else:
                lines.append("cxlt " + fmt(*self.space.mission(self.space.world(ob).translation)) + f"  # {ob.name}")

    # --- driver -------------------------------------------------------------
    def run(self):
        model = self.model.name
        out_path = bpy.path.abspath(self.props.output_path)
        if not out_path.lower().endswith(".3di"):
            raise ExportError(f"{model}: the output path must end in .3di")
        if not os.path.isabs(out_path):
            raise ExportError(f"{model}: save the .blend first or give an absolute output path (a '//' path is "
                              "relative to the saved file)")
        out_dir = os.path.dirname(out_path)
        os.makedirs(out_dir, exist_ok=True)
        name = self.props.model_name.strip() or os.path.splitext(os.path.basename(out_path))[0]
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
            with at_world_origin(self.context, self.model):
                return self.run_in_object_mode(model, name, out_path, out_dir)
        finally:
            if mode != "OBJECT":
                try:
                    bpy.ops.object.mode_set(mode=mode)
                except RuntimeError:
                    pass

    def run_in_object_mode(self, model, name, out_path, out_dir):
        self.context.view_layer.update()
        self.space = ModelSpace(self.model, self.settings.forward)
        for ob in self.model.children:
            if not (is_lod_root(ob) or is_model_root(ob) or clean_name(ob.name).startswith("!")):
                self.note(f"{ob.name}: not under a LOD root; not exported")
        roots = self.lod_roots()
        lods = [self.classify(r, i == 0) for i, r in enumerate(roots)]
        self.lod0 = lods[0]
        if not self.lod0.parts:
            raise ExportError(f"{roots[0].name}: LOD 0 has no parts")
        self.skinned = lods[0].armature is not None
        if any(l.parts and (l.armature is not None) != self.skinned for l in lods):
            raise ExportError(f"{model}: every LOD of a skinned model needs its BN## armature")
        self.uv1 = any(len(ob.data.uv_layers) > 1 for l in lods for meshes in l.meshes.values() for _, ob in meshes)
        # Skinned meshes are read in the armature's rest pose (the bind pose
        # the vertices are stored in) with their Armature modifier off: at
        # rest it moves nothing, and evaluating it anyway leaves float noise in
        # the normals (US01 and CIndo01: 1628 at the sixth decimal once their
        # rest bones are turned onto a clip's bind). Every rig reads at rest,
        # the mesh rig and a rigid model's animation rig alike (its Rest
        # Position mutes each part's `O3D follow`, anim_import.py): a model is
        # its authored layout, not a clip's pose. The scene's pose and
        # modifiers are restored after.
        rigs = [ob for r in roots for ob in descendants(r) if ob.type == "ARMATURE"]
        rest = [(ob.data, ob.data.pose_position) for ob in rigs]
        deforms = [m for r in roots for ob in descendants(r) if ob.type == "MESH"
                   for m in ob.modifiers if m.type == "ARMATURE" and m.show_viewport]
        for data, _ in rest:
            data.pose_position = "REST"
        for m in deforms:
            m.show_viewport = False
        if rest or deforms:
            self.context.view_layer.update()
        self.depsgraph = self.context.evaluated_depsgraph_get()
        try:
            return self.emit(name, out_path, out_dir, lods)
        finally:
            for data, position in rest:
                data.pose_position = position
            for m in deforms:
                m.show_viewport = True
            if rest or deforms:
                self.context.view_layer.update()

    def emit(self, name, out_path, out_dir, lods):
        bullet_index = self.props.poly_collision_lod
        if bullet_index >= len(lods):
            raise ExportError(f"poly_collision_lod {bullet_index} names no LOD (there are {len(lods)})")

        lod_lines, tail_lines, material_lines = [], [], []
        for lod in lods:
            self.emit_lod(lod, lod_lines)
        self.emit_points(lods[0], tail_lines)
        self.emit_lights(lods[0], len(lods[0].parts), tail_lines)
        self.emit_occlusion(lods[0], tail_lines)
        self.emit_collision(lods[0], lods[bullet_index], tail_lines)
        # Material order: the Material_<i> index, then first use.
        self.materials.sort(key=lambda m: (int(MATERIAL_RE.match(clean_name(m.name)).group(1))
                                           if m is not None and MATERIAL_RE.match(clean_name(m.name)) else 1 << 30))
        remap = {}
        for new, mat in enumerate(self.materials):
            remap[self.material_index[mat.name if mat is not None else None]] = new
        lod_lines = [f"strip {remap[int(l.split()[1])]} {l.split()[2]}" if l.startswith("strip ") else l
                     for l in lod_lines]
        self.emit_materials(material_lines)

        frame_lines = ["mtrx " + fmt(*(float(x) for x in r)) for r in self.frames]
        text = ["o3d 1", f"model {quoted(name)}"] + (["skinned 1"] if self.skinned else []) + \
            (["uv1 1"] if self.uv1 else []) + [f"register {quoted(r)}" for r in self.registers] + frame_lines + \
            material_lines + lod_lines + tail_lines

        if self.settings.write_textures:
            for tex_name, image in self.textures.values():
                write_tga(image, os.path.join(out_dir, tex_name))
        # The scene text is the CLI's input only.
        result = export_text(self.context, ["build"], text, "scene.o3d", "scene text", out_path)
        tris = sum(1 for line in lod_lines if line.startswith("t "))
        # The builder's notes (a non-convex volume, collinear faces, ...),
        # without the scene-file prefix.
        notes = cli_notes(result, "note: ")
        message = f"{result.stdout.strip()} ({len(lods)} LODs, {tris} triangles total, {len(self.textures)} textures)"
        return message, self.notes + notes


def export_model(context, model):
    """Export one model root; returns the summary line and the builder's notes."""
    return Exporter(context, model).run()
