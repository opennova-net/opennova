# Scene -> .o3d text -> opennova-3di.
#
# The scene is read by the NovaLogic ASE/OED object-naming convention (the
# retired importer/exporter's, `classify_name` in the retired
# engine/formats/oed/convert_internal.cpp, a port of [orig: ConvertToInternal
# @ 0x4268B3]; docs/threedi/scene-naming-contract.md keeps the table):
#
#   LOD root      Empty with an integer `_lod_index` custom property (0 = the
#                 primary LOD). The scene's `poly_collision_lod` (the .3dp
#                 setting) picks which render LOD's meshes also become the
#                 bullet faces; it defaults to 0, the most detailed.
#   PN##          Empty: part (subobject) ## (1-based). Its origin is the
#                 pivot unless a `_## center` helper gives one.
#   ## Mesh<n>    Mesh: render geometry of part ##.
#   _## center    helper: part ##'s pivot.
#   ~PPx attach   helper under a child part: that child's parent is part PP.
#   UP<c>## <lbl> helper: user point, type letter c (G gameplay, S effect),
#                 part ## (00 = no part, -1), label = the USRP name; it faces
#                 along its local +Z.
#   <code>##[a..]-colonly  mesh on LOD0: a collision volume of type <code>
#                 (CB CS CC CL CV CA VC BB CD CT CM VK CF LP DH DM DL CP) on
#                 part ##, the convex hull of its vertices.
#   Material_<i>_<SHADER>  material: export order i, shader tag SHADER.
#   !name         ignored.
# Blender's own `.001` duplicate suffixes are stripped before classification
# (object names are unique per .blend, so LOD1's PN01 is "PN01.001"); two
# objects that classify to the same identity inside one LOD are an error.

import os
import re
import struct
import subprocess

import bpy
from mathutils import Vector


class ExportError(Exception):
    pass


ALPHA_SHADERS = {"FF_ST_AB", "FF_ST_AD", "FF_ST_AB_LUM", "FF_ST_AD_LUM", "FF_MT_AB", "FF_MT_AD", "FF_MT_AB_LUM",
                 "FF_MT_AD_LUM"}
BLENDER_SUFFIX = re.compile(r"\.\d{3,}$")
PART_RE = re.compile(r"^PN(\d{2})$")
MESH_RE = re.compile(r"^(\d{2}) Mesh(\d+)$")
CENTER_RE = re.compile(r"^_(\d{2}) center$")
ATTACH_RE = re.compile(r"^~(\d{2})([a-z]*) attach$")
POINT_RE = re.compile(r"^UP([A-Za-z])(\d{2})(?: (.*))?$")
MATERIAL_RE = re.compile(r"^Material_(\d+)_(\S+)$")
OCCLUSION_RE = re.compile(r"-occ?only$", re.IGNORECASE)

# The collidable-type codes (classify_name; runtime meanings in
# docs/world/world-wac-ai-re.md §15).
VOLUME_CODES = {"CB": 1, "CS": 2, "CC": 3, "CL": 4, "CV": 5, "CA": 6, "VC": 7, "BB": 8, "CD": 9, "CT": 10,
                "CM": 11, "VK": 12, "CF": 13, "LP": 14, "DH": 16, "DM": 17, "DL": 18, "CP": 19}
VOLUME_RE = re.compile(r"^([A-Z]{2})([VSWLO]*)(\d{2})([a-z]*)-colonly$")
BLINK_LETTER_BITS = {"V": 0x2, "S": 0x4, "W": 0x8, "L": 0x10, "O": 0x20}


def clean_name(name):
    return BLENDER_SUFFIX.sub("", name)


def axis_map(forward):
    # Mission axes: x forward, y left, z up (the frame the .o3d carries).
    if forward == "-Y":
        return lambda v: (-v.y, v.x, v.z)
    return lambda v: (v.x, v.y, v.z)


def fmt(*values):
    out = []
    for v in values:
        if isinstance(v, float):
            s = f"{v:.6f}".rstrip("0").rstrip(".")
            out.append("0" if s in ("-0", "") else s)
        else:
            out.append(str(v))
    return " ".join(out)


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


class Lod:
    """One LOD root, classified."""

    def __init__(self, root):
        self.root = root
        self.parts = {}     # part index -> PN## empty
        self.meshes = {}    # part index -> [(ordinal, object)]
        self.centers = {}   # part index -> helper
        self.attach = {}    # child part index -> parent part index
        self.points = []    # (type letter, part index or -1, label, object)
        self.volumes = []   # (type, flags, part index, object)


class Exporter:
    def __init__(self, context):
        self.context = context
        self.scene = context.scene
        self.props = self.scene.o3d
        self.to_mission = axis_map(self.props.forward)
        self.depsgraph = context.evaluated_depsgraph_get()
        self.registers = []
        self.materials = []
        self.material_index = {}
        self.textures = {}

    # --- helpers ------------------------------------------------------------
    def register(self, name):
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
                    label = m.group(3) or "Noname"
                    if len(label) > 15:
                        raise ExportError(f"{ob.name}: user point label '{label}' exceeds 15 characters")
                    lod.points.append((m.group(1), int(m.group(2)) - 1, label, ob))
                continue
            if OCCLUSION_RE.search(raw):
                raise ExportError(f"{ob.name}: occlusion volumes are not supported by this exporter yet")
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
            raise ExportError(f"{root.name}: no PN## part empties under the LOD root")
        count = max(lod.parts) + 1
        missing = [i + 1 for i in range(count) if i not in lod.parts]
        if missing:
            raise ExportError(f"{root.name}: parts are not contiguous from PN01 (missing PN{missing[0]:02d})")
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
        if index == 0:
            return 0
        if index in lod.attach:
            return lod.attach[index]
        above = self.owning_part(lod.parts[index])
        return above if above is not None else 0

    def part_pivot(self, lod, index):
        ob = lod.centers.get(index, lod.parts[index])
        return self.mission(ob.matrix_world.translation)

    # --- geometry -----------------------------------------------------------
    def mesh_strips(self, ob, strips):
        ev = ob.evaluated_get(self.depsgraph)
        mesh = ev.to_mesh()
        try:
            mesh.calc_loop_triangles()
            mw = ob.matrix_world
            nmat = mw.to_3x3().inverted_safe().transposed()
            normals = mesh.corner_normals if hasattr(mesh, "corner_normals") else None
            uv_layer = mesh.uv_layers.active
            for tri in mesh.loop_triangles:
                slot = tri.material_index
                mat = ob.material_slots[slot].material if slot < len(ob.material_slots) else None
                s = strips.setdefault(self.material_for(mat), {"verts": [], "index": {}, "tris": []})
                corners = []
                for li in tri.loops:
                    loop = mesh.loops[li]
                    p = mw @ mesh.vertices[loop.vertex_index].co
                    n = normals[li].vector if normals is not None else loop.normal
                    n = (nmat @ n).normalized()
                    uv = uv_layer.data[li].uv if uv_layer is not None else (0.0, 0.0)
                    pm, nm = self.mission(p), self.mission(n)
                    # D3D texture space: v runs down.
                    vert = (pm[0], pm[1], pm[2], nm[0], nm[1], nm[2], uv[0], 1.0 - uv[1])
                    key = tuple(round(x, 5) for x in vert)
                    if key not in s["index"]:
                        s["index"][key] = len(s["verts"])
                        s["verts"].append(vert)
                    corners.append(s["index"][key])
                s["tris"].append(corners)
        finally:
            ev.to_mesh_clear()

    def emit_lod(self, lod, lines):
        threshold = lod.root.o3d.lod_threshold
        lines.append(f"lod {threshold} gnrc  # {lod.root.name}")
        count = len(lod.parts)
        for i in range(count):
            lines.append(f"part {self.part_parent(lod, i)} {fmt(*self.part_pivot(lod, i))}  # PN{i + 1:02d}")
            strips = {}
            for _, ob in sorted(lod.meshes.get(i, []), key=lambda e: e[0]):
                self.mesh_strips(ob, strips)
            for mi, s in sorted(strips.items()):
                mat = self.materials[mi]
                alpha = 1 if self.shader_of(mat) in ALPHA_SHADERS else 0
                if len(s["verts"]) > 65535:
                    raise ExportError(f"PN{i + 1:02d}: one material has more than 65535 vertices")
                lines.append(f"strip {mi} {alpha}")
                for v in s["verts"]:
                    lines.append("v " + fmt(*v))
                for t in s["tris"]:
                    lines.append(f"t {t[0]} {t[1]} {t[2]}")
        for i in range(count):
            lines.append(f"panm {i} {self.part_parent(lod, i)}")
            for t in lod.parts[i].o3d.tracks:
                lines.append(self.track_line(t))

    def track_line(self, t):
        style = int(t.style)
        scale = 16384.0 / 360.0 if t.target.startswith("rot") else 256.0
        reg = t.register if style == 113 else "-"
        if style == 113:
            self.register(t.register)
        line = f"track {t.target} {style} {reg} {round(t.rate * 256)} {round(t.start * scale)} {round(t.end * scale)}"
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

    def emit_materials(self, lines):
        for mat in self.materials:
            if mat is None:
                lines.append("material FF_ST_OP  # (no material)")
                continue
            p = mat.o3d
            shader = self.shader_of(mat)
            lines.append(f"material {shader}  # {mat.name}")
            flags = (1 if p.alpha_test else 0) | (4 if p.two_sided else 0)
            if flags:
                lines.append(f"matflags {flags}")
            if p.alpha_test:
                lines.append("alphatest 128")
            if shader == "FFP_GLASS":
                lines.append("glass 1")
            else:
                image = material_image(mat)
                name = p.texture_name.strip()
                if not name and image is not None:
                    name = os.path.splitext(clean_name(image.name))[0][:11] + ".tga"
                if name:
                    if len(name) > 15:
                        raise ExportError(f"{mat.name}: texture name '{name}' exceeds 15 characters")
                    lines.append(f"texture {name}")
                    if image is not None:
                        self.textures[name] = image
            if p.rgb_style != "0":
                reg = self.register(p.rgb_register) if p.rgb_style == "113" else -1
                s = [round(c * 255) for c in p.rgb_start]
                e = [round(c * 255) for c in p.rgb_end]
                lines.append(f"rgbgen {p.rgb_style} {reg} {fmt(float(p.rgb_rate))} {s[0]} {s[1]} {s[2]} {e[0]} {e[1]} {e[2]}")
            if p.alpha_style != "0":
                reg = self.register(p.alpha_register) if p.alpha_style == "113" else -1
                lines.append(f"alphagen {p.alpha_style} {reg} {fmt(float(p.alpha_rate))} {p.alpha_start} {p.alpha_end}")
            if p.u_style != "0":
                lines.append(f"ugen {p.u_style} -1 {fmt(float(p.u_rate))} 0 1")
            if p.v_style != "0":
                lines.append(f"vgen {p.v_style} -1 {fmt(float(p.v_rate))} 0 1")

    # --- user points ----------------------------------------------------------
    def emit_points(self, lod, lines):
        # USRP order is the scene order of the helpers (the ASE object order).
        for letter, part, label, ob in sorted(lod.points, key=lambda e: e[3].name):
            pos = self.mission(ob.matrix_world.translation)
            d = self.mission((ob.matrix_world.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized())
            lines.append(f"userpoint {label} {fmt(*pos)} {fmt(*d)} {part} {ord(letter.upper())}")

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
        # Bullet faces: the collision LOD's part meshes, section = part.
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
                        s["faces"].append((corners, int(mat.o3d.surface) if mat is not None else 14))
                finally:
                    ev.to_mesh_clear()
        for vtype, flags, part, ob in sorted(lod0.volumes, key=lambda e: clean_name(e[3].name)):
            if not 0 <= part < count:
                raise ExportError(f"{ob.name}: part {part + 1:02d} does not exist")
            mn, mx, planes = self.hull_planes(ob)
            sections[part]["volumes"].append((vtype, flags, mn, mx, planes))
        for i, s in enumerate(sections):
            lines.append(f"cobj {self.part_parent(lod0, i)}")
            for v in s["verts"]:
                lines.append("cv " + fmt(*v))
            for (a, b, c), poly in s["faces"]:
                lines.append(f"cf {a} {b} {c} {poly}")
            # Retail flags a plane 1 where it is a seam: another volume of the
            # section carries the same plane facing the other way.
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
        out_dir = os.path.dirname(out_path)
        os.makedirs(out_dir, exist_ok=True)
        name = self.props.model_name.strip() or os.path.splitext(os.path.basename(out_path))[0]
        if len(name) > 15:
            raise ExportError("the model name exceeds 15 characters")
        self.context.view_layer.update()
        roots = self.lod_roots()
        lods = [self.classify(r, i == 0) for i, r in enumerate(roots)]
        bullet_index = self.props.poly_collision_lod
        if bullet_index >= len(lods):
            raise ExportError(f"poly_collision_lod {bullet_index} names no LOD (there are {len(lods)})")
        render = lods

        lod_lines, tail_lines, material_lines = [], [], []
        for lod in render:
            self.emit_lod(lod, lod_lines)
        self.emit_points(lods[0], tail_lines)
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

        text = ["o3d 1", f"model {name}"] + [f"register {r}" for r in self.registers] + material_lines + \
            lod_lines + tail_lines
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
