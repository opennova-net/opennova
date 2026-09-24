# Scene -> .o3d text -> opennova-3di.
#
# The scene is read by the NovaLogic ASE/OED object-naming convention (the
# retired importer/exporter's, `classify_name` in the retired
# engine/formats/oed/convert_internal.cpp, a port of [orig: ConvertToInternal
# @ 0x4268B3]; docs/threedi/scene-naming-contract.md keeps the table):
#
#   LOD root      Empty with an integer `_lod_index` custom property (0 = the
#                 primary LOD) and its threshold and RMDL type (gnrc, bldg,
#                 door, veh0). The scene's `poly_collision_lod` (the .3dp
#                 setting) picks which render LOD's meshes also become the
#                 bullet faces; it defaults to 0, the most detailed. A LOD root
#                 with no parts is legal (retail ships them).
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
#                 (CB CS CC CL CV CA VC BB CD CT CM VK CF LP DH DM DL CP) on
#                 part ##, the convex hull of its vertices.
#   OB##/OS##/OP##[-MM]/OH## (+ [a..], -occonly)  mesh on LOD0: an occlusion
#                 record in section ## (OB occluder, OS open, OP a window to
#                 the exterior or, with -MM, a portal to section MM, OH).
#   Material_<i>_<SHADER>  material: export order i, shader tag SHADER.
#   Armature      a skinned model: one Armature under the LOD root whose
#                 bones are named BN## (part ##, 1-based; the bone head is the
#                 pivot, the bone parent the part parent). Its "01 Mesh<n>"
#                 meshes (the root owns skinned strips) are weighted by BN##
#                 vertex groups (at most three influences a vertex); strips
#                 split so no bone table exceeds 16 parts. Each skinned mesh
#                 is appended as its own part after the bones (parent 0,
#                 pivot = its origin), as retail's exporter wrote bones then
#                 mesh objects. Collision: each bone's section carries a hit
#                 sphere around the vertices it dominates, the mesh part's
#                 section the bullet faces (the retail person layout).
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
from mathutils import Matrix, Vector


class ExportError(Exception):
    pass


ALPHA_SHADERS = {"FF_ST_AB", "FF_ST_AD", "FF_ST_AB_LUM", "FF_ST_AD_LUM", "FF_MT_AB", "FF_MT_AD", "FF_MT_AB_LUM",
                 "FF_MT_AD_LUM"}
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


def descendants(ob):
    for child in ob.children:
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
        self.volumes = []   # (type, flags, part index, object)
        self.armature = None  # skinned: the Armature; parts are its bones
        self.bone_count = 0   # skinned: parts past the bones are the meshes


class Exporter:
    def __init__(self, context):
        self.context = context
        self.scene = context.scene
        self.props = self.scene.o3d
        self.to_mission = axis_map(self.props.forward)
        self.basis = axis_basis(self.props.forward)
        self.depsgraph = context.evaluated_depsgraph_get()
        self.registers = []
        self.materials = []
        self.material_index = {}
        self.textures = {}
        self.frames = []
        self.skinned = False
        self.uv1 = False
        self.bone_points = {}  # skinned LOD0: part -> rest positions it dominates

    # --- helpers ------------------------------------------------------------
    def register(self, name):
        if not name:
            raise ExportError("a register-driven style (above 112) needs a register name")
        if name not in self.registers:
            self.registers.append(name)
        return self.registers.index(name)

    def mission(self, v):
        return self.to_mission(v)

    def material_for(self, mat):
        key = mat.name if mat is not None else None
        if key not in self.material_index:
            self.material_index[key] = len(self.materials)
            self.materials.append(mat)
        return self.material_index[key]

    def frame_index(self, ob):
        """The MTRX row a rotated PN## selects: its world rotation as a mission
        frame R (row-major, p' = p R); 0 for an unrotated part."""
        q = self.basis.transposed() @ ob.matrix_world.to_3x3().normalized() @ self.basis
        if all(abs(q[i][j] - (1.0 if i == j else 0.0)) < 1e-6 for i in range(3) for j in range(3)):
            return 0
        r = tuple(round(q[j][i], 6) for i in range(3) for j in range(3))
        if r not in self.frames:
            self.frames.append(r)
        return self.frames.index(r) + 1

    # --- classification -----------------------------------------------------
    def lod_roots(self):
        roots = [o for o in self.scene.objects if o.type == "EMPTY" and "_lod_index" in o]
        by_index = {}
        for o in roots:
            i = int(o["_lod_index"])
            if i in by_index:
                raise ExportError(f"two LOD roots carry _lod_index {i}: {by_index[i].name}, {o.name}")
            by_index[i] = o
        if 0 not in by_index:
            raise ExportError("no LOD root: add an Empty with the custom property _lod_index = 0 above PN01")
        order = sorted(by_index)
        if order != list(range(len(order))):
            raise ExportError(f"_lod_index values are not contiguous from 0: {order}")
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
                    lod.volumes.append((VOLUME_CODES[code], flags, int(nn) - 1, ob))
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
            # A skinned mesh is "01 Mesh<n>" (the root owns its strips). Each
            # one becomes its own part after the bones, as the retail
            # exporter wrote bones first and mesh objects after them: parent
            # 0, pivot = the mesh object's origin, holding the mesh bounds
            # and bullet faces (ArmsG: 37 bones + part 37; FSldr03: 19 + 19).
            if any(index != 0 for index in lod.meshes):
                raise ExportError(f"{root.name}: a skinned mesh is named '01 Mesh<n>' (the root part)")
            lod.bone_count = len(lod.parts)
            skins = sorted(lod.meshes.get(0, []), key=lambda e: e[0])
            if not skins:
                raise ExportError(f"{root.name}: no '01 Mesh<n>' skinned mesh")
            lod.meshes = {}
            for k, (ordinal, ob) in enumerate(skins):
                lod.parts[lod.bone_count + k] = ob
                lod.meshes[lod.bone_count + k] = [(ordinal, ob)]
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
                return self.mission(lod.parts[index].matrix_world.translation)
            return self.mission(lod.armature.matrix_world @ lod.parts[index].head_local)
        ob = lod.centers.get(index, lod.parts[index])
        return self.mission(ob.matrix_world.translation)

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
            mw = ob.matrix_world
            nmat = mw.to_3x3().inverted_safe().transposed()
            normals = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
            uv0, uv1 = self.uv_layers(mesh)
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = ob.material_slots[slot].material if slot < len(ob.material_slots) else None
                s = strips.setdefault(self.material_for(mat), {"verts": [], "index": {}, "tris": []})
                corners = []
                for li in tri.loops:
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
            mw = ob.matrix_world
            nmat = mw.to_3x3().inverted_safe().transposed()
            normals = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
            uv0, uv1 = self.uv_layers(mesh)
            influences = []
            for v in mesh.vertices:
                w = sorted(((g.weight, groups[g.group]) for g in v.groups if g.group in groups and g.weight > 1e-4),
                           reverse=True)[:3]
                if not w:
                    raise ExportError(f"{ob.name}: vertex {v.index} has no BN## weight")
                total = sum(x for x, _ in w)
                influences.append([(b, x / total) for x, b in w])
                if collect_bones:
                    self.bone_points.setdefault(w[0][1], []).append(self.mission(mw @ v.co))
            per_material = {}
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = ob.material_slots[slot].material if slot < len(ob.material_slots) else None
                corners = []
                for li in tri.loops:
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
        return 1 if self.shader_of(mat) in ALPHA_SHADERS or (mat is not None and mat.o3d.alpha_strips) else 0

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
            tracks = [(t.target, t.style, int(t.axis) if t.target == "trans" else 0) for t in part.o3d.tracks]
            flags = part.o3d.panm_flags if part.o3d.panm_flags >= 0 else derived_panm_flags(tracks)
            frame = self.frame_index(part)
            line = f"panm {i} {self.part_parent(lod, i)}"
            if part.o3d.panm_flags >= 0 or frame:
                line += f" 0x{flags:08x}" + (f" {frame}" if frame else "")
            lines.append(line)
            for t in part.o3d.tracks:
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
        for i in range(count):
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
    @staticmethod
    def shader_of(mat):
        if mat is None:
            return "FF_ST_OP"
        m = MATERIAL_RE.match(clean_name(mat.name))
        return m.group(2) if m else mat.o3d.shader

    def generator_register(self, style, name):
        return self.register(name) if style > CTRL_REFERENCE_THRESHOLD else -1

    def emit_materials(self, lines):
        for mat in self.materials:
            if mat is None:
                lines.append("material FF_ST_OP  # (no material)")
                continue
            p = mat.o3d
            shader = self.shader_of(mat)
            lines.append(f"material {quoted(shader)}  # {mat.name}")
            flags = (1 if p.alpha_test else 0) | (4 if p.two_sided else 0) | (p.other_flags & ~5 & 0xFF)
            if flags:
                lines.append(f"matflags {flags}")
            if p.alpha_test:
                lines.append(f"alphatest {p.alpha_test_value}")
            if p.glass or shader == "FFP_GLASS":
                lines.append("glass 1")
            if p.emissive:
                lines.append(f"emissive {p.emissive}")
            if any(c > 0 for c in p.reflect):
                lines.append("reflect " + " ".join(str(round(c * 255)) for c in p.reflect))
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
            elif shader != "FFP_GLASS":
                image = material_image(mat)
                if image is not None:
                    name = os.path.splitext(clean_name(image.name))[0][:12] + ".tga"
                    lines.append(f"texture {quoted(name)}")
                    self.textures[name] = image
            if p.anim_frames or p.anim_type or p.anim_time:
                lines.append(f"texanim {p.anim_frames} {p.anim_type} {p.anim_time}")
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
            pos = self.mission(ob.matrix_world.translation)
            d = self.mission((ob.matrix_world.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
            lines.append(f"userpoint {quoted(label)} {fmt(*pos)} {fmt(*d)} {part} {ord(letter.upper())}")

    def emit_lights(self, lod, count, lines):
        for part, ob in sorted(lod.lights, key=lambda e: order_key(e[1])):
            if part >= max(count, 1):
                raise ExportError(f"{ob.name}: part {part + 1:02d} does not exist")
            data, p = ob.data, ob.data.o3d
            spot = data.type == "SPOT"
            if data.type not in ("POINT", "SPOT"):
                raise ExportError(f"{ob.name}: a light is a point or spot light")
            pos = self.mission(ob.matrix_world.translation)
            phase = str(self.register(p.register)) if p.style > CTRL_REFERENCE_THRESHOLD else fmt(float(p.phase))
            s = [round(c * 255) for c in data.color]
            e = [round(c * 255) for c in p.color_end]
            flags = (1 if p.disable_corona else 0) | (2 if p.disable_terrain else 0) | \
                (4 if p.disable_objects else 0) | (8 if spot else 0) | (p.other_flags & ~0x0F & 0xFF)
            line = (f"light {part} {fmt(*pos)} {fmt(float(p.atten_start), float(p.atten_end))} {p.style} "
                    f"{fmt(float(p.rate))} {phase} {s[0]} {s[1]} {s[2]} {e[0]} {e[1]} {e[2]} 0x{flags:02x}")
            # The light's local +Z is its stored axis; an omni light pointing
            # straight down is the retail default the builder writes itself.
            d = self.mission((ob.matrix_world.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
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
                mw = ob.matrix_world
                for v in mesh.vertices:
                    lines.append("ov " + fmt(*self.mission(mw @ v.co)))
                # Counter-clockwise about the outward normal, as retail stores
                # them; the builder picks each face's plane (the OED rule).
                for poly in mesh.polygons:
                    vs = list(poly.vertices)
                    for k in range(1, len(vs) - 1):
                        lines.append(f"of {vs[0]} {vs[k]} {vs[k + 1]}")
            finally:
                ev.to_mesh_clear()

    # --- collision --------------------------------------------------------------
    def hull_planes(self, ob):
        """The convex hull of the volume mesh as retail lays a BVOL out: its six
        AABB planes first (+x -x +y -y +z -z), then every other hull face plane.
        Outward normals, n . p + d == 0 on the plane."""
        import bmesh
        ev = ob.evaluated_get(self.depsgraph)
        mesh = ev.to_mesh()
        try:
            mw = ob.matrix_world
            points = [self.mission(mw @ v.co) for v in mesh.vertices]
        finally:
            ev.to_mesh_clear()
        if len(points) < 4:
            raise ExportError(f"collision volume {ob.name} needs at least 4 vertices")
        bm = bmesh.new()
        for p in points:
            bm.verts.new(p)
        bmesh.ops.convex_hull(bm, input=bm.verts)
        mn = [min(p[k] for p in points) for k in range(3)]
        mx = [max(p[k] for p in points) for k in range(3)]
        planes = [((1, 0, 0), -mx[0]), ((-1, 0, 0), mn[0]), ((0, 1, 0), -mx[1]), ((0, -1, 0), mn[1]),
                  ((0, 0, 1), -mx[2]), ((0, 0, -1), mn[2])]
        # The vertex centroid is inside any convex hull (the AABB centre of a
        # wedge need not be): the reference that orients every normal outward.
        centre = Vector([sum(p[k] for p in points) / len(points) for k in range(3)])
        for f in bm.faces:
            if f.calc_area() < 1e-8:
                continue
            n = f.normal.normalized()
            p0 = Vector(f.verts[0].co)
            if n.dot(p0 - centre) < 0:
                n = -n
            d = -n.dot(p0)
            # The hull triangulates every face: fold near-coplanar triangles
            # (within ~2 degrees and 2 cm) into one plane.
            if any(n.x * q[0][0] + n.y * q[0][1] + n.z * q[0][2] > 0.9994 and abs(d - q[1]) < 0.02 for q in planes):
                continue
            planes.append(((n.x, n.y, n.z), d))
        bm.free()
        return mn, mx, planes

    def emit_collision(self, lod0, bullet, lines):
        count = len(lod0.parts)
        sections = [{"verts": [], "index": {}, "faces": [], "volumes": []} for _ in range(count)]
        # Bullet faces: the collision LOD's part meshes, section = part,
        # counter-clockwise about their outward normal (the retail order).
        for index, meshes in bullet.meshes.items():
            if index >= count:
                raise ExportError(f"the collision LOD has part PN{index + 1:02d}, which LOD0 lacks")
            s = sections[index]
            for _, ob in sorted(meshes, key=lambda e: e[0]):
                ev = ob.evaluated_get(self.depsgraph)
                mesh = ev.to_mesh()
                try:
                    mesh.calc_loop_triangles()
                    mw = ob.matrix_world
                    for tri in mesh.loop_triangles:
                        corners = []
                        for vi in tri.vertices:
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
                        s["faces"].append((corners, mat.o3d.surface if mat is not None else 14))
                finally:
                    ev.to_mesh_clear()
        for vtype, flags, part, ob in sorted(lod0.volumes, key=lambda e: clean_name(e[3].name)):
            if not 0 <= part < count:
                raise ExportError(f"{ob.name}: part {part + 1:02d} does not exist")
            mn, mx, planes = self.hull_planes(ob)
            sections[part]["volumes"].append((vtype, flags, mn, mx, planes))
        for i, s in enumerate(sections):
            # Every section sits at its part's pivot (the COBJ offset / CXLT
            # translation retail carries: Dtruck2's wheels, Dblkhwk1's rotors).
            lines.append(f"cobj {self.part_parent(lod0, i)} " + fmt(*self.part_pivot(lod0, i)))
            if self.skinned and not s["verts"]:
                # The retail person layout: a bone section carries a hit sphere
                # around the vertices the bone dominates (none: a zero sphere),
                # the mesh part's section the bullet faces.
                pts = self.bone_points.get(i, [])
                if pts:
                    c = [(min(p[k] for p in pts) + max(p[k] for p in pts)) * 0.5 for k in range(3)]
                    r = max(sum((p[k] - c[k]) ** 2 for k in range(3)) ** 0.5 for p in pts)
                else:
                    c, r = [0.0, 0.0, 0.0], 0.0
                lines.append("csphere " + fmt(*(float(x) for x in c), float(r)))
            for v in s["verts"]:
                lines.append("cv " + fmt(*v))
            for (a, b, c), poly in s["faces"]:
                lines.append(f"cf {a} {b} {c} {poly}")
            # Seam flags (BPLN 1; the line-of-sight sweep keeps a flagged
            # plane's radius non-negative). Retail's rule is not witnessed:
            # neither this one ("another volume carries the plane facing the
            # other way") nor "the face lies inside another volume" reproduces
            # the corpus's flags (83% and 80% agreement over 132,856 planes,
            # mostly unflagged ones).
            for vi, (vtype, flags, mn, mx, planes) in enumerate(s["volumes"]):
                lines.append(f"cvolume {vtype} {flags} " + fmt(*mn, *mx))
                for n, d in planes:
                    seam = any(oi != vi and any(abs(n[0] + m[0]) < 1e-3 and abs(n[1] + m[1]) < 1e-3
                                                and abs(n[2] + m[2]) < 1e-3 and abs(d + e) < 1e-2 for m, e in op)
                               for oi, (_, _, _, _, op) in enumerate(s["volumes"]))
                    lines.append("cp " + fmt(*n, d) + (" 1" if seam else " 0"))

    # --- driver -------------------------------------------------------------
    def run(self):
        out_path = bpy.path.abspath(self.props.output_path)
        if not out_path.lower().endswith(".3di"):
            raise ExportError("the output path must end in .3di")
        if not os.path.isabs(out_path):
            raise ExportError("save the .blend first or give an absolute output path (a '//' path is relative "
                              "to the saved file)")
        out_dir = os.path.dirname(out_path)
        os.makedirs(out_dir, exist_ok=True)
        name = self.props.model_name.strip() or os.path.splitext(os.path.basename(out_path))[0]
        if len(name) > 15:
            raise ExportError("the model name exceeds 15 characters")
        self.context.view_layer.update()
        roots = self.lod_roots()
        lods = [self.classify(r, i == 0) for i, r in enumerate(roots)]
        self.lod0 = lods[0]
        if not self.lod0.parts:
            raise ExportError(f"{roots[0].name}: LOD 0 has no parts")
        self.skinned = lods[0].armature is not None
        if any(l.parts and (l.armature is not None) != self.skinned for l in lods):
            raise ExportError("every LOD of a skinned model needs its BN## armature")
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

        if self.props.write_textures:
            for tex_name, image in self.textures.items():
                write_tga(image, os.path.join(out_dir, tex_name))

        cli = bpy.path.abspath(self.props.cli_path) if self.props.cli_path else os.path.join(
            os.path.dirname(__file__), "bin", "opennova-3di.exe")
        if not os.path.isfile(cli):
            raise ExportError(f"opennova-3di not found at {cli}")
        result = subprocess.run([cli, "build", o3d_path, "-o", out_path], capture_output=True, text=True)
        if result.returncode != 0:
            raise ExportError((result.stderr or result.stdout).strip()[:2000])
        tris = sum(1 for line in lod_lines if line.startswith("t "))
        return f"{result.stdout.strip()} ({len(render)} LODs, {tris} triangles total, {len(self.textures)} textures)"


def export_scene(context):
    return Exporter(context).run()
