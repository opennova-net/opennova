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
#   _## center    helper: part ##'s pivot.
#   ~PPx attach   helper under a child part: that child's parent is part PP.
#   UP<c>## <lbl> helper: user point, type letter c (G gameplay, S effect),
#                 part ## (00 = no part, -1), label = the USRP name; it faces
#                 along its local +Z. Its `order` property keeps the USRP order.
#   LP##[a..]     Light (on LOD0): a LGHT light owned by part ## (00 = part 0);
#                 a spot light is a cone about its local +Z.
#   <code>##[a..]-colonly  mesh on LOD0: a collision volume of type <code>
#                 (CB CS CC CL CV CA VC BB CD CT CM VK CF LP DH DM DL CP) in
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
#                 faces (the retail person layout). A bone named `!...` is no
#                 part: `!RM` carries a clip's root track (animation.py), and a
#                 control bone an author rigs with is ignored the same way.
#   !name         ignored.
# Blender's own `.001` duplicate suffixes are stripped before classification
# (object names are unique per .blend, so LOD1's PN01 is "PN01.001"); two
# objects that classify to the same identity inside one LOD are an error.

import math
import os
import re
import struct
import subprocess

import bpy
from mathutils import Euler, Matrix, Vector


class ExportError(Exception):
    pass


# Shader capability bits (runtime/renderer/material_descriptor.h), read per
# tag from `opennova-3di catalog`.
FLAG_EMISSIVE, FLAG_DIFFUSE, FLAG_SECONDARY = 0x1, 0x4, 0x8
FLAG_BLENDING, FLAG_GLASS, FLAG_SKINNED = 0x1000, 0x2000, 0x4000
BLENDER_SUFFIX = re.compile(r"\.\d{3,}$")
PART_RE = re.compile(r"^PN(\d{2})$")
BONE_RE = re.compile(r"^BN(\d{2})(?: .*)?$")
MESH_RE = re.compile(r"^(\d{2}) Mesh(\d+)$")
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
VOLUME_RE = re.compile(r"^([A-Z]{2})([VSWLO]*)(\d{2})([a-z]*)-colonly$")
BLINK_LETTER_BITS = {"V": 0x2, "S": 0x4, "W": 0x8, "L": 0x10, "O": 0x20}
CTRL_REFERENCE_THRESHOLD = 0x70


def clean_name(name):
    return BLENDER_SUFFIX.sub("", name)


def shader_table():
    """The engine's shader tags and capability words, in table order."""
    from . import catalog
    return catalog()[2]


def shader_flags(tag):
    """A tag's capability word, matched without case as the runtime's effect
    lookup does; a tag outside the table reads row 0's, as OED's lookup did
    (lookup_material_info_flags)."""
    table = shader_table()
    for name, flags in table:
        if name.lower() == tag.lower():
            return flags
    return table[0][1] if table else FLAG_DIFFUSE


def default_shader(map_count, skinned):
    """The shader of a material whose name carries none: the first table row
    of the model's kind (skinned or not) drawing that many texture maps
    (diffuse, detail), OED's find_material_index_by_flags: FF_ST_OP for one
    map, FF_MT_OP for two, FFP_GLASS for none; VS_SKBASIC / VS_SKGLASS on a
    skinned model."""
    wanted = max(0, min(2, map_count))
    table = shader_table()
    for name, flags in table:
        if bool(flags & FLAG_SKINNED) != skinned:
            continue
        if (1 if flags & FLAG_DIFFUSE else 0) + (1 if flags & FLAG_SECONDARY else 0) == wanted:
            return name
    return table[0][0] if table else "FF_ST_OP"


def dup_rank(letters):
    """A duplicate suffix's place: '' 0, 'a' 1 .. 'z' 26, 'aa' 27, ..."""
    rank = 0
    for c in letters:
        rank = rank * 26 + (ord(c) - ord("a") + 1)
    return rank


def axis_basis(forward):
    """Mission -> Blender as a column matrix B (b = B m); a proper rotation."""
    if forward == "-Y":
        return Matrix(((0, 1, 0), (-1, 0, 0), (0, 0, 1)))
    return Matrix.Identity(3)


def axis_map(forward):
    # Mission axes: x forward, y left, z up (the frame the .o3d carries).
    if forward == "-Y":
        return lambda v: (-v.y, v.x, v.z)
    return lambda v: (v.x, v.y, v.z)


def derived_panm_flags(tracks):
    """The PANM flags word build derives from a part's tracks (target, style,
    axis): rotation type 2 for any live rotation track, scale type 2 for any
    live scale track, the translate axis for a live translation."""
    live = [(target, axis) for target, style, axis in tracks if style != 0]
    rot = any(t.startswith("rot") for t, _ in live)
    scale = any(t.startswith("scale") for t, _ in live)
    axis = next((a for t, a in live if t == "trans"), 0)
    return (2 if scale else 0) | ((2 if rot else 0) << 8) | (axis << 24)


def fmt(*values):
    out = []
    for v in values:
        if isinstance(v, float):
            s = f"{v:.6f}".rstrip("0").rstrip(".")
            out.append("0" if s in ("-0", "") else s)
        else:
            out.append(str(v))
    return " ".join(out)


def quoted(name):
    """A .o3d name field: bare when it is one plain token, else "quoted"."""
    if name and not any(c.isspace() or c == '"' for c in name) and not name.startswith("#"):
        return name
    if '"' in name:
        raise ExportError(f"the name '{name}' holds a double quote")
    return f'"{name}"'


def write_tga(image, path):
    """Uncompressed 32-bit truecolor TGA, rows bottom-up (the retail shape)."""
    w, h = image.size
    if w == 0 or h == 0:
        raise ExportError(f"image {image.name} has no pixels")
    px = list(image.pixels[:])
    data = bytearray(w * h * 4)
    for i in range(w * h):
        r, g, b, a = px[i * 4:i * 4 + 4]
        data[i * 4 + 0] = max(0, min(255, round(b * 255)))
        data[i * 4 + 1] = max(0, min(255, round(g * 255)))
        data[i * 4 + 2] = max(0, min(255, round(r * 255)))
        data[i * 4 + 3] = max(0, min(255, round(a * 255)))
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 8)
    with open(path, "wb") as f:
        f.write(header)
        f.write(data)


def material_image(mat):
    if mat is None or not mat.use_nodes or mat.node_tree is None:
        return None
    for node in mat.node_tree.nodes:
        if node.type == "TEX_IMAGE" and node.image is not None:
            return node.image
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


def descendants(ob):
    for child in ob.children:
        if is_model_root(child):
            continue  # another model parented here is its own .3di
        yield child
        yield from descendants(child)


def order_key(ob):
    order = ob.o3d.order
    return (order if order >= 0 else 1 << 30, clean_name(ob.name))


class Lod:
    """One LOD root, classified."""

    def __init__(self, root):
        self.root = root
        self.parts = {}     # part index -> PN## empty
        self.meshes = {}    # part index -> [(ordinal, object)]
        self.centers = {}   # part index -> helper
        self.attach = {}    # child part index -> parent part index
        self.points = []    # (type letter, part index or -1, label, object)
        self.lights = []    # (part index, object)
        self.occluders = []  # (type, section, connecting, object)
        self.volumes = []   # (type, flags, part index, object, (code, dup rank))
        self.armature = None  # skinned: the Armature; parts are its bones
        self.bone_count = 0   # skinned: parts past the bones are the meshes


class Exporter:
    def __init__(self, context, model):
        self.context = context
        self.scene = context.scene
        self.model = model
        self.props = model.o3d  # the model's name, output path, collision LOD
        self.settings = self.scene.o3d  # forward axis, textures, executable
        self.to_mission = axis_map(self.settings.forward)
        self.basis = axis_basis(self.settings.forward)
        self.space = None  # set in run(): the inverse of a moved model root
        self.depsgraph = context.evaluated_depsgraph_get()
        self.registers = []
        self.materials = []
        self.material_index = {}
        self.textures = {}
        self.frames = []
        self.skinned = False
        self.uv1 = False
        self.bone_points = {}  # skinned LOD0: part -> rest positions it moves

    # --- helpers ------------------------------------------------------------
    def register(self, name):
        if not name:
            raise ExportError("a register-driven style (above 112) needs a register name")
        if name not in self.registers:
            self.registers.append(name)
        return self.registers.index(name)

    def mission(self, v):
        return self.to_mission(v)

    def world(self, ob):
        """An object's matrix in the model root's frame, so a model placed,
        parented or mounted anywhere in the scene exports the same model (a
        root at the origin reads Blender's world matrices untouched)."""
        return ob.matrix_world if self.space is None else self.space @ ob.matrix_world

    def material_for(self, mat):
        key = mat.name if mat is not None else None
        if key not in self.material_index:
            self.material_index[key] = len(self.materials)
            self.materials.append(mat)
        return self.material_index[key]

    def frame_index(self, ob):
        """The MTRX row a rotated PN## selects: its rotation in the model."""
        return self.frame_of(self.world(ob).to_3x3().normalized())

    def frame_of(self, rotation):
        """The MTRX row of a part frame given as a Blender rotation in the
        model: that rotation as a mission frame R (row-major, p' = p R); 0 for
        the identity."""
        q = self.basis.transposed() @ rotation @ self.basis
        if all(abs(q[i][j] - (1.0 if i == j else 0.0)) < 1e-6 for i in range(3) for j in range(3)):
            return 0
        r = tuple(round(q[j][i], 6) for i in range(3) for j in range(3))
        if r not in self.frames:
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

        for ob in descendants(root):
            raw = clean_name(ob.name)
            if raw.startswith("!"):
                continue
            if ob.type == "ARMATURE":
                if lod.armature is not None:
                    raise ExportError(f"{root.name}: two armatures ({lod.armature.name}, {ob.name})")
                lod.armature = ob
                for bone in ob.data.bones:
                    # A bone whose name starts with `!` is not a part: the
                    # animation root track (`!RM`) and any control bone an
                    # author rigs with live there.
                    if clean_name(bone.name).startswith("!"):
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
                index, ordinal = int(m.group(1)) - 1, int(m.group(2))
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
                child = self.owning_part(ob)
                if child is None:
                    raise ExportError(f"{ob.name}: an attach helper must sit under its child part's PN##")
                lod.attach[child] = int(m.group(1)) - 1
                continue
            m = POINT_RE.match(raw)
            if m:
                if primary:
                    label = m.group(3) if m.group(3) is not None else "Noname"
                    if len(label) > 15:
                        raise ExportError(f"{ob.name}: user point label '{label}' exceeds 15 characters")
                    lod.points.append((m.group(1), int(m.group(2)) - 1, label, ob))
                continue
            if OCCLUSION_RE.search(raw):
                m = OCC_RE.match(raw)
                if not m or ob.type != "MESH":
                    raise ExportError(f"{ob.name}: occlusion meshes are OB##, OS##, OP##[-MM] or OH##, then "
                                      "-occonly")
                prefix, nn, dup, mm = m.groups()
                claim(("occlusion", prefix, nn, dup, mm), ob)
                if primary:
                    kind = 3 if prefix == "OP" and mm else OCC_TYPES[prefix]
                    lod.occluders.append((kind, int(nn) - 1, int(mm) - 1 if mm else 0, ob))
                continue
            m = VOLUME_RE.match(raw)
            if m and ob.type == "MESH":
                code, letters, nn, dup = m.groups()
                if code not in VOLUME_CODES:
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
                    lod.volumes.append((VOLUME_CODES[code], flags, int(nn) - 1, ob, (code + letters, dup_rank(dup))))
                continue
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
            return lod
        for index, meshes in lod.meshes.items():
            if index not in lod.parts:
                raise ExportError(f"{root.name}: mesh '{index + 1:02d} Mesh…' has no PN{index + 1:02d}")
            owner = [self.owning_part(ob) for _, ob in meshes]
            if any(o != index for o in owner):
                raise ExportError(f"{root.name}: '{index + 1:02d} Mesh…' must sit under PN{index + 1:02d}")
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

    def part_parent(self, lod, index):
        if lod.armature is not None:
            if index >= lod.bone_count:
                return 0
            bone = lod.parts[index]
            if bone.parent is None:
                return 0
            return int(BONE_RE.match(clean_name(bone.parent.name)).group(1)) - 1
        if index == 0:
            return 0
        if index in lod.attach:
            return lod.attach[index]
        above = self.owning_part(lod.parts[index])
        return above if above is not None else 0

    def part_pivot(self, lod, index):
        if lod.armature is not None:
            if index >= lod.bone_count:
                return self.mission(self.world(lod.parts[index]).translation)
            return self.mission(self.world(lod.armature) @ lod.parts[index].head_local)
        ob = lod.centers.get(index, lod.parts[index])
        return self.mission(self.world(ob).translation)

    # --- geometry -----------------------------------------------------------
    def uv_layers(self, mesh):
        layers = mesh.uv_layers
        if len(layers) == 0:
            return None, None
        return layers[0], (layers[1] if len(layers) > 1 else layers[0])

    def corner(self, mesh, loop, mw, nmat, normals, uv0, uv1):
        p = mw @ mesh.vertices[loop.vertex_index].co
        n = normals[loop.index].vector if normals is not None else loop.normal
        n = (nmat @ n).normalized()
        a = uv0.data[loop.index].uv if uv0 is not None else (0.0, 0.0)
        b = uv1.data[loop.index].uv if uv1 is not None else a
        pm, nm = self.mission(p), self.mission(n)
        # D3D texture space: v runs down.
        vert = (pm[0], pm[1], pm[2], nm[0], nm[1], nm[2], a[0], 1.0 - a[1])
        if self.uv1:
            vert += (b[0], 1.0 - b[1])
        return vert

    def mesh_strips(self, ob, strips):
        ev = ob.evaluated_get(self.depsgraph)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.world(ob)
            nmat = mw.to_3x3().inverted_safe().transposed()
            mirrored = mw.to_3x3().determinant() < 0
            normals = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
            uv0, uv1 = self.uv_layers(mesh)
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = ob.material_slots[slot].material if slot < len(ob.material_slots) else None
                s = strips.setdefault(self.material_for(mat), {"verts": [], "index": {}, "tris": []})
                corners = []
                for li in reversed(tri.loops) if mirrored else tri.loops:
                    vert = self.corner(mesh, mesh.loops[li], mw, nmat, normals, uv0, uv1)
                    key = tuple(round(x, 5) for x in vert)
                    if key not in s["index"]:
                        s["index"][key] = len(s["verts"])
                        s["verts"].append(vert)
                    corners.append(s["index"][key])
                s["tris"].append(corners)
        finally:
            ev.to_mesh_clear()

    def skinned_strips(self, ob, strips, collect_bones):
        """A skinned mesh's triangles grouped per material into strips whose
        bone tables stay within 16 parts. Each corner carries its rest-pose
        position and up to three (part, weight) influences from BN## groups."""
        groups = {}
        for g in ob.vertex_groups:
            m = BONE_RE.match(clean_name(g.name))
            if m:
                groups[g.index] = int(m.group(1)) - 1
        ev = ob.evaluated_get(self.depsgraph)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.world(ob)
            nmat = mw.to_3x3().inverted_safe().transposed()
            mirrored = mw.to_3x3().determinant() < 0
            normals = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
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
                        self.bone_points.setdefault(b, []).append(self.mission(mw @ v.co))
            per_material = {}
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = ob.material_slots[slot].material if slot < len(ob.material_slots) else None
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
                    if current is None or len(current["table"] | need) > 16:
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
                        key = tuple(round(x, 5) for x in full)
                        if key not in current["index"]:
                            current["index"][key] = len(current["verts"])
                            current["verts"].append(full)
                        ids.append(current["index"][key])
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
            lines.append(f"part {self.part_parent(lod, i)} {fmt(*self.part_pivot(lod, i))}  # PN{i + 1:02d}")
            strips = {}
            for _, ob in sorted(lod.meshes.get(i, []), key=lambda e: e[0]):
                self.mesh_strips(ob, strips)
            for mi, s in sorted(strips.items()):
                if len(s["verts"]) > 65535:
                    raise ExportError(f"PN{i + 1:02d}: one material has more than 65535 vertices")
                lines.append(f"strip {mi} {self.strip_alpha(self.materials[mi])}")
                for v in s["verts"]:
                    lines.append("v " + fmt(*v))
                for t in s["tris"]:
                    lines.append(f"t {t[0]} {t[1]} {t[2]}")
        for i in range(count):
            part = lod.parts[i]
            self.emit_panm(lod, i, part.o3d, self.frame_index(part), lines)

    def emit_panm(self, lod, i, p, frame, lines):
        """A part's PANM row: its flags (derived from the tracks unless set),
        MTRX frame and tracks, from an empty's or a bone's part animation."""
        tracks = [(t.target, t.style, int(t.axis) if t.target == "trans" else 0) for t in p.tracks]
        flags = p.panm_flags if p.panm_flags >= 0 else derived_panm_flags(tracks)
        line = f"panm {i} {self.part_parent(lod, i)}"
        if p.panm_flags >= 0 or frame:
            line += f" 0x{flags:08x}" + (f" {frame}" if frame else "")
        lines.append(line)
        for t in p.tracks:
            lines.append(self.track_line(t))

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
                self.skinned_strips(ob, strips, primary)
            for mi in sorted(strips):
                for s in strips[mi]:
                    if len(s["verts"]) > 65535:
                        raise ExportError(f"BN{i + 1:02d}: one strip has more than 65535 vertices")
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
        arm = self.world(lod.armature).to_3x3().normalized()
        for i in range(count):
            if i < lod.bone_count:
                p = lod.parts[i].o3d
                self.emit_panm(lod, i, p, self.frame_of(arm @ Euler(p.frame).to_matrix()), lines)
            else:
                lines.append(f"panm {i} {self.part_parent(lod, i)}")

    def track_line(self, t):
        style = t.style
        rotation = t.target.startswith("rot")
        scale = 16384.0 / 360.0 if rotation else 256.0
        if style > CTRL_REFERENCE_THRESHOLD:
            self.register(t.register)
            field = quoted(t.register)
        else:
            field = str(t.param) if t.param else "-"
        line = f"track {t.target} {style} {field} {round(t.rate * 256)} {round(t.start * scale)} {round(t.end * scale)}"
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

    def generator_register(self, style, name):
        return self.register(name) if style > CTRL_REFERENCE_THRESHOLD else -1

    def emit_materials(self, lines):
        for mat in self.materials:
            if mat is None:
                lines.append("material FF_ST_OP  # (no material)")
                continue
            p = mat.o3d
            shader = self.shader_of(mat)
            caps = shader_flags(shader)
            lines.append(f"material {quoted(shader)}  # {mat.name}")
            flags = (1 if p.alpha_test else 0) | (4 if p.two_sided else 0) | (p.other_flags & ~5 & 0xFF)
            if flags:
                lines.append(f"matflags {flags}")
            if p.alpha_test:
                lines.append(f"alphatest {p.alpha_test_value}")
            # The OED material rule (5fc5b4f6a^ export_3di.cpp, WriteMTRL): a
            # GLASS shader reflects 0x80 grey unless another colour is set, and
            # is glass while it reflects; an EMISSIVE one (*_LUM) is emissive
            # type 2. It holds for every material of the 958 JO models.
            reflect = [round(c * 255) for c in p.reflect]
            if caps & FLAG_GLASS and not any(reflect):
                reflect = [128, 128, 128, 0]
            if caps & FLAG_GLASS and any(reflect[:3]):
                lines.append("glass 1")
            if caps & FLAG_EMISSIVE:
                lines.append("emissive 2")
            if any(reflect):
                lines.append("reflect " + " ".join(str(c) for c in reflect))
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
                        self.textures[name] = t.image
            elif caps & FLAG_DIFFUSE:
                image = material_image(mat)
                if image is not None:
                    name = os.path.splitext(clean_name(image.name))[0][:12] + ".tga"
                    lines.append(f"texture {quoted(name)}")
                    self.textures[name] = image
            if p.anim_frames or p.anim_type or p.anim_time:
                time_or_register = self.register(p.anim_register) if p.anim_type == 1 else p.anim_time
                lines.append(f"texanim {p.anim_frames} {p.anim_type} {time_or_register}")
            if p.rgb_style:
                reg = self.generator_register(p.rgb_style, p.rgb_register)
                s = [round(c * 255) for c in p.rgb_start]
                e = [round(c * 255) for c in p.rgb_end]
                lines.append(f"rgbgen {p.rgb_style} {reg} {fmt(float(p.rgb_rate))} {s[0]} {s[1]} {s[2]} "
                             f"{e[0]} {e[1]} {e[2]} {fmt(float(p.rgb_phase))}")
            if p.alpha_style:
                reg = self.generator_register(p.alpha_style, p.alpha_register)
                lines.append(f"alphagen {p.alpha_style} {reg} {fmt(float(p.alpha_rate))} {p.alpha_start} {p.alpha_end} "
                             f"{fmt(float(p.alpha_phase))}")
            for axis in ("u", "v"):
                style = getattr(p, axis + "_style")
                if style:
                    reg = self.generator_register(style, getattr(p, axis + "_register"))
                    lines.append(f"{axis}gen {style} {reg} {fmt(float(getattr(p, axis + '_rate')))} "
                                 f"{fmt(float(getattr(p, axis + '_start')))} {fmt(float(getattr(p, axis + '_end')))} "
                                 f"{fmt(float(getattr(p, axis + '_phase')))}")

    # --- user points, lights, occlusion -------------------------------------
    def emit_points(self, lod, lines):
        # USRP order: each helper's `order` (the imported index), then name.
        for letter, part, label, ob in sorted(lod.points, key=lambda e: order_key(e[3])):
            pos = self.mission(self.world(ob).translation)
            d = self.mission((self.world(ob).to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
            lines.append(f"userpoint {quoted(label)} {fmt(*pos)} {fmt(*d)} {part} {ord(letter.upper())}")

    def emit_lights(self, lod, count, lines):
        for part, ob in sorted(lod.lights, key=lambda e: order_key(e[1])):
            if part >= max(count, 1):
                raise ExportError(f"{ob.name}: part {part + 1:02d} does not exist")
            data, p = ob.data, ob.data.o3d
            spot = data.type == "SPOT"
            if data.type not in ("POINT", "SPOT"):
                raise ExportError(f"{ob.name}: a light is a point or spot light")
            pos = self.mission(self.world(ob).translation)
            phase = str(self.register(p.register)) if p.style > CTRL_REFERENCE_THRESHOLD else fmt(float(p.phase))
            s = [round(c * 255) for c in data.color]
            e = [round(c * 255) for c in p.color_end]
            flags = (1 if p.disable_corona else 0) | (2 if p.disable_terrain else 0) | \
                (4 if p.disable_objects else 0) | (8 if spot else 0) | (p.other_flags & ~0x0F & 0xFF)
            line = (f"light {part} {fmt(*pos)} {fmt(float(p.atten_start), float(p.atten_end))} {p.style} "
                    f"{fmt(float(p.rate))} {phase} {s[0]} {s[1]} {s[2]} {e[0]} {e[1]} {e[2]} 0x{flags:02x}")
            # The light's local +Z is its stored axis; an omni light pointing
            # straight down is the retail default the builder writes itself.
            d = self.mission((self.world(ob).to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
            falloff = math.degrees(data.spot_size) / 2.0 if spot else 0.0
            if spot or abs(d[0]) > 1e-6 or abs(d[1]) > 1e-6 or d[2] > -1.0 + 1e-9:
                line += " " + fmt(*d, float(falloff))
            lines.append(line)

    def emit_occlusion(self, lod, lines):
        for kind, section, connecting, ob in sorted(lod.occluders, key=lambda e: (order_key(e[3]), e[1])):
            ev = ob.evaluated_get(self.depsgraph)
            mesh = ev.to_mesh()
            try:
                if len(mesh.vertices) > 128:
                    raise ExportError(f"{ob.name}: an occlusion mesh holds at most 128 vertices")
                lines.append(f"occ {kind} {section} {connecting}  # {ob.name}")
                mw = self.world(ob)
                mirrored = mw.to_3x3().determinant() < 0
                for v in mesh.vertices:
                    lines.append("ov " + fmt(*self.mission(mw @ v.co)))
                # Counter-clockwise about the outward normal, as retail stores
                # them; the builder picks each face's plane (the OED rule).
                for poly in mesh.polygons:
                    vs = list(poly.vertices)
                    if mirrored:
                        vs.reverse()
                    for k in range(1, len(vs) - 1):
                        lines.append(f"of {vs[0]} {vs[k]} {vs[k + 1]}")
            finally:
                ev.to_mesh_clear()

    # --- collision --------------------------------------------------------------
    def volume_mesh(self, ob):
        """A volume's vertices (mission axes) and triangles, wound
        counter-clockwise about the outward normal as authored; the builder
        derives the planes, box and seam flags by the OED rule."""
        ev = ob.evaluated_get(self.depsgraph)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = self.world(ob)
            mirrored = mw.to_3x3().determinant() < 0
            verts = [self.mission(mw @ v.co) for v in mesh.vertices]
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
                (0x800 if p.face_double_sided else 0) | (p.face_other_flags & ~0x901))

    def emit_collision(self, lod0, bullet, lines):
        # One section per part of the collision LOD (WriteCOBJ walks that
        # LOD's subobjects: Dtruck2's collision LOD has 7 parts to LOD 0's 8,
        # CNet01's none), placed at that part's pivot with its parent.
        count = len(bullet.parts)
        sections = [{"verts": [], "index": {}, "faces": [], "volumes": []} for _ in range(count)]
        # Bullet faces: the collision LOD's part meshes, section = part,
        # counter-clockwise about their outward normal (the retail order).
        for index, meshes in bullet.meshes.items():
            s = sections[index]
            for _, ob in sorted(meshes, key=lambda e: e[0]):
                ev = ob.evaluated_get(self.depsgraph)
                mesh = ev.to_mesh()
                try:
                    mesh.calc_loop_triangles()
                    mw = self.world(ob)
                    mirrored = mw.to_3x3().determinant() < 0
                    for tri in mesh.loop_triangles:
                        corners = []
                        for vi in reversed(tri.vertices) if mirrored else tri.vertices:
                            p = self.mission(mw @ mesh.vertices[vi].co)
                            key = tuple(round(x, 4) for x in p)
                            if key not in s["index"]:
                                s["index"][key] = len(s["verts"])
                                s["verts"].append(p)
                            corners.append(s["index"][key])
                        if len(set(corners)) != 3:
                            continue
                        slot = tri.material_index
                        mat = ob.material_slots[slot].material if slot < len(ob.material_slots) else None
                        s["faces"].append((corners, mat.o3d.surface if mat is not None else 14, self.face_flags(mat)))
                finally:
                    ev.to_mesh_clear()
        for vtype, flags, part, ob, key in lod0.volumes:
            if not 0 <= part < count:
                raise ExportError(f"{ob.name}: its section {part + 1:02d} is not a part of the collision LOD "
                                  f"(LOD {self.props.poly_collision_lod} has {count})")
            sections[part]["volumes"].append((key, vtype, flags, ob))
        for i, s in enumerate(sections):
            # Every section sits at its part's pivot (the COBJ offset / CXLT
            # translation retail carries: Dtruck2's wheels, Dblkhwk1's rotors).
            lines.append(f"cobj {self.part_parent(bullet, i)} " + fmt(*self.part_pivot(bullet, i)))
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
        self.context.view_layer.update()
        root = self.model.matrix_world
        if root != Matrix.Identity(4):
            self.space = root.inverted_safe()
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
        # the vertices are stored in); the scene's pose is restored after.
        rest = [(l.armature.data, l.armature.data.pose_position) for l in lods if l.armature is not None]
        for data, _ in rest:
            data.pose_position = "REST"
        if rest:
            self.context.view_layer.update()
            self.depsgraph = self.context.evaluated_depsgraph_get()
        try:
            return self.emit(name, out_path, out_dir, lods)
        finally:
            for data, position in rest:
                data.pose_position = position
            if rest:
                self.context.view_layer.update()

    def emit(self, name, out_path, out_dir, lods):
        bullet_index = self.props.poly_collision_lod
        if bullet_index >= len(lods):
            raise ExportError(f"poly_collision_lod {bullet_index} names no LOD (there are {len(lods)})")
        render = lods

        lod_lines, tail_lines, material_lines = [], [], []
        for lod in render:
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
        o3d_path = os.path.splitext(out_path)[0] + ".o3d"
        with open(o3d_path, "w", newline="\n") as f:
            f.write("\n".join(text) + "\n")

        if self.settings.write_textures:
            for tex_name, image in self.textures.items():
                write_tga(image, os.path.join(out_dir, tex_name))

        cli = bpy.path.abspath(self.settings.cli_path) if self.settings.cli_path else os.path.join(
            os.path.dirname(__file__), "bin", "opennova-3di.exe")
        if not os.path.isfile(cli):
            raise ExportError(f"opennova-3di not found at {cli}")
        result = subprocess.run([cli, "build", o3d_path, "-o", out_path], capture_output=True, text=True)
        if result.returncode != 0:
            raise ExportError((result.stderr or result.stdout).strip()[:2000])
        tris = sum(1 for line in lod_lines if line.startswith("t "))
        # The builder's notes (a non-convex volume, collinear faces, ...),
        # without the scene-file prefix.
        notes = [line.split("note: ", 1)[1] for line in result.stderr.splitlines() if "note: " in line]
        message = f"{result.stdout.strip()} ({len(render)} LODs, {tris} triangles total, {len(self.textures)} textures)"
        return message, notes


def export_model(context, model):
    """Export one model root; returns the summary line and the builder's notes."""
    return Exporter(context, model).run()
