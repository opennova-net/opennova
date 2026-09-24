# .3di -> opennova-3di scene -> .o3d text -> a Blender scene.
#
# The engine decodes the model (`opennova-3di scene`, the inverse of the
# exporter's `build`); this module only lays the .o3d out by the naming
# contract export.py reads (docs/threedi/scene-naming-contract.md), so an
# imported model exports again. Each file becomes its own Blender scene named
# after the model (the exporter reads a whole scene), with LOD 1 and up hidden.
# Textures load from the file `opennova-3di` resolved beside the .3di by the
# runtime's candidate order (`texfile` records).

import math
import os
import subprocess
import tempfile

import bmesh
import bpy
from mathutils import Matrix, Vector

from . import export


class ImportFailed(Exception):
    pass


VOLUME_NAMES = {v: k for k, v in export.VOLUME_CODES.items()}
OCC_PREFIX = {0: "OB", 1: "OS", 2: "OP", 3: "OP", 4: "OH"}
DUP_LETTERS = "abcdefghijklmnopqrstuvwxyz"


def dup_suffix(counter, key):
    n = counter.get(key, 0)
    counter[key] = n + 1
    if n == 0:
        return ""
    out = ""
    n -= 1
    while True:
        out = DUP_LETTERS[n % 26] + out
        n = n // 26 - 1
        if n < 0:
            return out


# --- .o3d reading ---------------------------------------------------------------

def tokens(line):
    """Whitespace tokens with "quoted names" kept whole (quotes dropped)."""
    out, i = [], 0
    while i < len(line):
        if line[i].isspace():
            i += 1
            continue
        if line[i] == '"':
            j = line.find('"', i + 1)
            j = len(line) if j < 0 else j
            out.append(line[i + 1:j])
            i = j + 1
            continue
        j = i
        while j < len(line) and not line[j].isspace():
            j += 1
        out.append(line[i:j])
        i = j
    return out


def strip_comment(line):
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == "#" and not quoted and (i == 0 or line[i - 1] in " \t"):
            return line[:i]
    return line


def read_o3d(path):
    sc = {"name": "MODEL", "skinned": False, "uv1": False, "registers": [], "frames": [], "materials": [],
          "texfiles": {}, "lods": [], "points": [], "lights": [], "occ": [], "cobjs": []}
    lod = part = strip = panm = mat = cobj = volume = occ = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            raw = raw.rstrip("\r\n")
            if raw.startswith("texfile "):
                t = tokens(raw[8:].split(" ", 1)[0] if not raw[8:].startswith('"') else raw[8:])
                name = t[0]
                rest = raw[8:][len(name) + (2 if raw[8:].startswith('"') else 0):].strip()
                sc["texfiles"][name] = None if rest == "-" else rest
                continue
            t = tokens(strip_comment(raw))
            if not t:
                continue
            k, a = t[0], t[1:]
            if k == "model":
                sc["name"] = a[0] if a else "MODEL"
            elif k == "skinned":
                sc["skinned"] = a[0] != "0"
            elif k == "uv1":
                sc["uv1"] = a[0] != "0"
            elif k == "register":
                sc["registers"].append(a[0] if a else "")
            elif k == "mtrx":
                sc["frames"].append([float(x) for x in a[:9]])
            elif k == "material":
                mat = {"shader": a[0], "textures": [], "texanim": None, "reflect": None, "matflags": 0,
                       "alphatest": 0, "glass": 0, "emissive": 0, "rgbgen": None, "alphagen": None, "ugen": None,
                       "vgen": None}
                sc["materials"].append(mat)
            elif k == "texture":
                mat["textures"].append((a[0],) + tuple(int(x) for x in (a[1:5] + ["1", "0", "0", "0"][len(a[1:5]):])))
            elif k == "texanim":
                mat["texanim"] = tuple(int(x) for x in a[:3])
            elif k == "reflect":
                mat["reflect"] = tuple(int(x) for x in a[:4])
            elif k in ("matflags", "alphatest", "glass", "emissive"):
                mat[k] = int(a[0])
            elif k == "rgbgen":
                mat["rgbgen"] = (int(a[0]), int(a[1]), float(a[2]), [int(x) for x in a[3:6]], [int(x) for x in a[6:9]],
                                 float(a[9]) if len(a) > 9 else 0.0)
            elif k in ("alphagen", "ugen", "vgen"):
                mat[k] = (int(a[0]), int(a[1]), float(a[2]), float(a[3]), float(a[4]), float(a[5]) if len(a) > 5 else 0.0)
            elif k == "lod":
                lod = {"threshold": int(a[0]) if a else 0, "type": a[1] if len(a) > 1 else "gnrc", "parts": [],
                       "panm": []}
                sc["lods"].append(lod)
            elif k == "part":
                part = {"parent": int(a[0]), "pivot": tuple(float(x) for x in a[1:4]), "strips": []}
                lod["parts"].append(part)
            elif k == "strip":
                strip = {"material": int(a[0]), "alpha": len(a) > 1 and a[1] != "0", "bones": [], "verts": [],
                         "tris": []}
                part["strips"].append(strip)
            elif k == "bones":
                strip["bones"] = [int(x) for x in a]
            elif k == "v":
                f = [float(x) for x in a]
                vert = {"p": tuple(f[0:3]), "n": tuple(f[3:6]), "uv": (f[6], f[7])}
                rest = f[8:]
                if sc["uv1"]:
                    vert["uv1"], rest = (rest[0], rest[1]), rest[2:]
                else:
                    vert["uv1"] = vert["uv"]
                if sc["skinned"]:
                    vert["bi"] = [int(x) for x in rest[0:3]]
                    vert["bw"] = rest[3:6]
                strip["verts"].append(vert)
            elif k == "t":
                strip["tris"].append(tuple(int(x) for x in a[:3]))
            elif k == "panm":
                panm = {"part": int(a[0]), "parent": int(a[1]), "flags": int(a[2], 0) if len(a) > 2 else None,
                        "matrix": int(a[3]) if len(a) > 3 else 0, "tracks": []}
                lod["panm"].append(panm)
            elif k == "track":
                panm["tracks"].append((a[0], int(a[1]), a[2], int(a[3]), int(a[4]), int(a[5]),
                                       int(a[6]) if len(a) > 6 else 0))
            elif k == "userpoint":
                sc["points"].append({"name": a[0], "p": tuple(float(x) for x in a[1:4]),
                                     "d": tuple(float(x) for x in a[4:7]), "part": int(a[7]),
                                     "type": int(a[8]) if len(a) > 8 else 71})
            elif k == "light":
                light = {"part": int(a[0]), "p": tuple(float(x) for x in a[1:4]), "atten": (float(a[4]), float(a[5])),
                         "style": int(a[6]), "rate": float(a[7]), "phase": float(a[8]),
                         "rgb0": [int(x) for x in a[9:12]], "rgb1": [int(x) for x in a[12:15]], "flags": int(a[15], 0),
                         "dir": None, "falloff": 0.0}
                if len(a) >= 20:
                    light["dir"] = tuple(float(x) for x in a[16:19])
                    light["falloff"] = float(a[19])
                sc["lights"].append(light)
            elif k == "occ":
                occ = {"type": int(a[0]), "a": int(a[1]), "b": int(a[2]), "verts": [], "faces": []}
                sc["occ"].append(occ)
            elif k == "ov":
                occ["verts"].append(tuple(float(x) for x in a[:3]))
            elif k == "of":
                occ["faces"].append(tuple(int(x) for x in a[:3]))
            elif k == "cobj":
                cobj = {"parent": int(a[0]), "offset": tuple(float(x) for x in a[1:4]) if len(a) >= 4 else (0, 0, 0),
                        "sphere": None, "verts": [], "faces": [], "volumes": []}
                sc["cobjs"].append(cobj)
            elif k == "csphere":
                cobj["sphere"] = tuple(float(x) for x in a[:4])
            elif k == "cv":
                cobj["verts"].append(tuple(float(x) for x in a[:3]))
            elif k == "cf":
                cobj["faces"].append((int(a[0]), int(a[1]), int(a[2]), int(a[3]) if len(a) > 3 else 1))
            elif k in ("cvolume", "cvol"):
                box = [float(x) for x in a[2:8]]
                volume = {"type": int(a[0]), "flags": int(a[1]), "planes": []}
                if k == "cvol":
                    volume["planes"] = [((1, 0, 0), -box[3]), ((-1, 0, 0), box[0]), ((0, 1, 0), -box[4]),
                                        ((0, -1, 0), box[1]), ((0, 0, 1), -box[5]), ((0, 0, -1), box[2])]
                cobj["volumes"].append(volume)
            elif k == "cp":
                volume["planes"].append((tuple(float(x) for x in a[:3]), float(a[3])))
    return sc


# --- scene building ------------------------------------------------------------

class Builder:
    def __init__(self, context, sc, source_path, op):
        self.context = context
        self.sc = sc
        self.dir = os.path.dirname(os.path.abspath(source_path))
        self.op = op
        self.notes = []
        self.images = {}

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    # mission -> Blender, the inverse of export.axis_map
    def blender(self, m):
        if self.forward == "-Y":
            return Vector((m[1], -m[0], m[2]))
        return Vector((m[0], m[1], m[2]))

    def link(self, ob, parent=None, world=None, parent_world=None):
        self.collection.objects.link(ob)
        if parent is not None:
            ob.parent = parent
            ob.matrix_parent_inverse = Matrix.Identity(4)
        world = world if world is not None else Matrix.Identity(4)
        ob.matrix_basis = (parent_world.inverted() if parent_world is not None else Matrix.Identity(4)) @ world
        self.world[ob.name] = world
        return ob

    def empty(self, name, parent=None, world=None, size=0.1, display="PLAIN_AXES"):
        ob = bpy.data.objects.new(name, None)
        ob.empty_display_type = display
        ob.empty_display_size = size
        return self.link(ob, parent, world, self.world.get(parent.name) if parent is not None else None)

    # --- materials ---------------------------------------------------------
    def image(self, name):
        if name in self.images:
            return self.images[name]
        path = self.sc["texfiles"].get(name)
        img = None
        if not path:
            self.note(f"texture {name} not found beside the model")
        else:
            try:
                img = bpy.data.images.load(path, check_existing=True)
                img.name = name
            except RuntimeError:
                self.note(f"texture {name}: Blender cannot read {os.path.basename(path)}")
        self.images[name] = img
        return img

    def materials(self):
        out = []
        reg = self.sc["registers"]
        alpha_by_material = {}
        for lod in self.sc["lods"]:
            for part in lod["parts"]:
                for s in part["strips"]:
                    alpha_by_material[s["material"]] = alpha_by_material.get(s["material"], False) or s["alpha"]

        def regname(style, index):
            if style > 112 and 0 <= index < len(reg):
                return reg[index]
            return ""

        from . import SHADERS
        shader_ids = {s[0] for s in SHADERS}
        for i, m in enumerate(self.sc["materials"]):
            mat = bpy.data.materials.new(f"Material_{i}_{m['shader']}")
            p = mat.o3d
            if m["shader"] in shader_ids:
                p.shader = m["shader"]
            flags = m["matflags"]
            p.alpha_test = bool(flags & 1)
            p.two_sided = bool(flags & 4)
            p.other_flags = flags & ~5 & 0xFF
            p.alpha_test_value = m["alphatest"]
            if m["alphatest"] and not p.alpha_test:
                self.note(f"material {i}: an alpha-test value without the alpha-test flag")
            p.glass = bool(m["glass"])
            p.emissive = m["emissive"]
            if m["reflect"]:
                p.reflect = [c / 255.0 for c in m["reflect"]]
            blended = m["shader"] in export.ALPHA_SHADERS
            p.alpha_strips = alpha_by_material.get(i, False) and not blended
            for name, slot, typ, tflags, frame in m["textures"]:
                t = p.textures.add()
                t.name = name
                t.slot, t.type, t.flags, t.frame = slot, typ, tflags, frame
                t.image = self.image(name)
                t.write = False  # the file beside the model already serves it
            if m["texanim"]:
                frames, typ, time_or_register = m["texanim"]
                p.anim_frames, p.anim_type = frames, typ
                if typ == 1:
                    p.anim_register = reg[time_or_register] if 0 <= time_or_register < len(reg) else ""
                else:
                    p.anim_time = time_or_register
            if m["rgbgen"]:
                style, r, rate, s0, s1, phase = m["rgbgen"]
                p.rgb_style, p.rgb_register, p.rgb_rate, p.rgb_phase = style, regname(style, r), rate, phase
                p.rgb_start = [c / 255.0 for c in s0]
                p.rgb_end = [c / 255.0 for c in s1]
            if m["alphagen"]:
                style, r, rate, start, end, phase = m["alphagen"]
                p.alpha_style, p.alpha_register, p.alpha_rate, p.alpha_phase = style, regname(style, r), rate, phase
                p.alpha_start, p.alpha_end = int(start), int(end)
            for axis in ("u", "v"):
                g = m[axis + "gen"]
                if g:
                    style, r, rate, start, end, phase = g
                    setattr(p, axis + "_style", style)
                    setattr(p, axis + "_register", regname(style, r))
                    setattr(p, axis + "_rate", rate)
                    setattr(p, axis + "_start", start)
                    setattr(p, axis + "_end", end)
                    setattr(p, axis + "_phase", phase)
            self.shade(mat, m, blended)
            out.append(mat)
        return out

    def shade(self, mat, m, blended):
        """A preview node tree: slot 1 as the base colour (times a slot-2 detail
        texture on UV1, Modulate2x, for FF_MT shaders), alpha for blended and
        alpha-tested shaders, emission for *_LUM, a tint for glass."""
        mat.use_nodes = True
        nodes, links = mat.node_tree.nodes, mat.node_tree.links
        bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
        mat.use_backface_culling = not mat.o3d.two_sided
        by_slot = {}
        for name, slot, *_ in m["textures"]:
            by_slot.setdefault(slot, name)
        color = None
        if 1 in by_slot and self.images.get(by_slot[1]) is not None:
            tex = nodes.new("ShaderNodeTexImage")
            tex.image = self.images[by_slot[1]]
            tex.location = (-600, 300)
            color = tex.outputs["Color"]
            if blended or mat.o3d.alpha_test:
                links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
                if hasattr(mat, "surface_render_method"):
                    mat.surface_render_method = "BLENDED" if blended else "DITHERED"
        if 2 in by_slot and self.images.get(by_slot[2]) is not None and color is not None:
            uv = nodes.new("ShaderNodeUVMap")
            uv.uv_map = "UV1"
            uv.location = (-900, -100)
            detail = nodes.new("ShaderNodeTexImage")
            detail.image = self.images[by_slot[2]]
            detail.location = (-600, -100)
            links.new(uv.outputs["UV"], detail.inputs["Vector"])
            mix = nodes.new("ShaderNodeMix")
            mix.data_type = "RGBA"
            mix.blend_type = "MULTIPLY"
            mix.inputs[0].default_value = 1.0
            mix.location = (-300, 200)
            links.new(color, mix.inputs[6])
            links.new(detail.outputs["Color"], mix.inputs[7])
            scale = nodes.new("ShaderNodeVectorMath")
            scale.operation = "SCALE"
            scale.inputs[3].default_value = 2.0
            scale.location = (-120, 200)
            links.new(mix.outputs[2], scale.inputs[0])
            color = scale.outputs[0]
        if color is not None:
            links.new(color, bsdf.inputs["Base Color"])
            if m["emissive"] or m["shader"].endswith("_LUM"):
                links.new(color, bsdf.inputs["Emission Color"])
                bsdf.inputs["Emission Strength"].default_value = 1.0
        if m["glass"] or "GLASS" in m["shader"]:
            bsdf.inputs["Base Color"].default_value = (0.6, 0.75, 0.85, 1.0)
            bsdf.inputs["Alpha"].default_value = 0.35
            if hasattr(mat, "surface_render_method"):
                mat.surface_render_method = "BLENDED"

    # --- meshes ---------------------------------------------------------------
    def mesh(self, name, strips, pivot, materials, skinned):
        """One mesh from a part's strips: vertices merged by position, normal
        (and weights), loops carrying UVMap/UV1 and the stored normals."""
        index, verts, loops, faces, face_mat, slots = {}, [], [], [], [], []
        weights = []
        for s in strips:
            mi = s["material"]
            if mi not in slots:
                slots.append(mi)
            ids = []
            for v in s["verts"]:
                if sum(x * x for x in v["n"]) < 1e-12:
                    self.note("zero-length vertex normals are replaced by Blender's calculated normals")
                key = (tuple(round(x, 6) for x in v["p"]), tuple(round(x, 4) for x in v["n"]))
                infl = ()
                if skinned:
                    infl = tuple(sorted((s["bones"][b], round(w, 5)) for b, w in zip(v["bi"], v["bw"])
                                        if w > 0 and b < len(s["bones"])))
                    key += (infl,)
                if key not in index:
                    index[key] = len(verts)
                    verts.append(self.blender(v["p"]) - pivot)
                    weights.append(infl)
                ids.append(index[key])
            for a, b, c in s["tris"]:
                face = [ids[a], ids[b], ids[c]]
                if len(set(face)) < 3:
                    # Corners the merge collapsed (a sliver): keep the face
                    # on vertices of its own so its triangle survives.
                    for k, x in enumerate((a, b, c)):
                        face[k] = len(verts)
                        verts.append(self.blender(s["verts"][x]["p"]) - pivot)
                        weights.append(weights[ids[x]])
                faces.append(tuple(face))
                face_mat.append(slots.index(mi))
                loops.extend((s["verts"][a], s["verts"][b], s["verts"][c]))
        me = bpy.data.meshes.new(name)
        me.from_pydata([tuple(v) for v in verts], [], faces)
        for mi in slots:
            me.materials.append(materials[mi] if 0 <= mi < len(materials) else None)
        me.polygons.foreach_set("material_index", face_mat)
        me.polygons.foreach_set("use_smooth", [True] * len(faces))
        uv0 = me.uv_layers.new(name="UVMap")
        uv1 = me.uv_layers.new(name="UV1") if self.sc["uv1"] else None
        for li, v in enumerate(loops):
            # D3D texture space runs v down; Blender's runs up.
            uv0.data[li].uv = (v["uv"][0], 1.0 - v["uv"][1])
            if uv1 is not None:
                uv1.data[li].uv = (v["uv1"][0], 1.0 - v["uv1"][1])
        me.normals_split_custom_set([tuple(self.blender(v["n"]).normalized()) for v in loops])
        me.update()
        return me, weights

    # --- the model --------------------------------------------------------------
    def build(self, scene):
        sc = self.sc
        self.scene = scene
        self.forward = scene.o3d.forward
        self.world = {}
        self.collection = bpy.data.collections.new(sc["name"])
        scene.collection.children.link(self.collection)
        mats = self.materials()
        lod_objects = []
        self.lod0_parts = {}
        for li, lod in enumerate(sc["lods"]):
            root = self.empty(f"{sc['name']}_LOD{li}", size=0.5, display="ARROWS")
            root["_lod_index"] = li
            root.o3d.lod_threshold = lod["threshold"]
            root.o3d.lod_type = lod["type"]
            objs = [root]
            if not lod["parts"]:
                self.note(f"LOD {li} has no parts")
            elif sc["skinned"]:
                objs += self.skinned_lod(li, lod, root, mats)
            else:
                objs += self.rigid_lod(li, lod, root, mats)
            lod_objects.append(objs)
        self.points(lod_objects)
        if self.op is None or self.op.import_lights:
            self.lights(lod_objects)
        if self.op is None or self.op.import_occlusion:
            self.occlusion(lod_objects)
        if self.op is None or self.op.import_collision:
            self.collision(lod_objects)
        self.bullet_lod(mats)
        if sc["skinned"] and sc["cobjs"]:
            self.note("skin weights are normalized on export and skinned hit spheres are regenerated from them")
        scene.o3d.model_name = sc["name"]
        vl = scene.view_layers[0]
        for li, objs in enumerate(lod_objects):
            if li > 0:
                for ob in objs:
                    ob.hide_set(True, view_layer=vl)

    def frame_rotation(self, index):
        """A PANM row's MTRX frame (mission axes, row-major, p' = p R) as a
        Blender rotation: the part empty's orientation (column form R^T)."""
        if index <= 0 or index > len(self.sc["frames"]):
            return Matrix.Identity(3)
        r = self.sc["frames"][index - 1]
        q = Matrix(((r[0], r[3], r[6]), (r[1], r[4], r[7]), (r[2], r[5], r[8])))
        if self.forward == "-Y":
            b = Matrix(((0, 1, 0), (-1, 0, 0), (0, 0, 1)))
            q = b @ q @ b.transposed()
        return q

    def rigid_lod(self, li, lod, root, mats):
        objs = []
        parts = []
        rows = {p["part"]: p for p in lod["panm"]}
        for pi, part in enumerate(lod["parts"]):
            pivot = self.blender(part["pivot"])
            row = rows.get(pi)
            rot = self.frame_rotation(row["matrix"]) if row else Matrix.Identity(3)
            world = Matrix.Translation(pivot) @ rot.to_4x4()
            parent = part["parent"]
            if pi == 0 or parent == pi or parent < 0:
                parent_ob = root
                if parent not in (pi, 0):
                    self.note(f"LOD {li} part {pi} names parent {parent}; it exports under the root")
            elif parent < len(parts):
                parent_ob = parts[parent]
            else:
                parent_ob = root  # re-parented below once the parent exists
            ob = self.empty(f"PN{pi + 1:02d}", parent_ob, world)
            parts.append(ob)
            objs.append(ob)
            if row is not None:
                self.tracks(ob, row, lod)
            if part["strips"]:
                me, _ = self.mesh(f"{pi + 1:02d} Mesh0", part["strips"], pivot, mats, False)
                mob = bpy.data.objects.new(f"{pi + 1:02d} Mesh0", me)
                objs.append(self.link(mob, ob, Matrix.Translation(pivot), world))
        for pi, part in enumerate(lod["parts"]):
            parent = part["parent"]
            if pi < parent < len(parts):
                # A parent listed after its child (retail ships some).
                ob = parts[pi]
                world = self.world[ob.name]
                ob.parent = parts[parent]
                ob.matrix_parent_inverse = Matrix.Identity(4)
                ob.matrix_basis = self.world[parts[parent].name].inverted() @ world
        if li == 0:
            self.lod0_parts = {i: ob for i, ob in enumerate(parts)}
        return objs

    def tracks(self, ob, row, lod):
        p = ob.o3d
        axis_default = (row["flags"] or 0) >> 24 & 0xFF
        derived = export.derived_panm_flags([(t[0], t[1], (t[6] or axis_default or 3) if t[0] == "trans" else 0)
                                             for t in row["tracks"]])
        if row["flags"] is not None and row["flags"] != derived:
            p.panm_flags = row["flags"]
        for target, style, reg, rate, start, end, axis in row["tracks"]:
            t = p.tracks.add()
            t.target = target
            t.style = style
            if style > 0x70:
                t.register = reg if reg != "-" else ""
            elif reg not in ("-", ""):
                try:
                    t.param = int(reg)
                except ValueError:
                    pass
            scale = 360.0 / 16384.0 if target.startswith("rot") else 1.0 / 256.0
            t.rate = rate / 256.0
            t.start = start * scale
            t.end = end * scale
            if target == "trans":
                t.axis = str(axis or axis_default or 3)

    def skinned_lod(self, li, lod, root, mats):
        objs = []
        bone_parts = [pi for pi, p in enumerate(lod["parts"]) if not p["strips"]]
        mesh_parts = [pi for pi, p in enumerate(lod["parts"]) if p["strips"]]
        if bone_parts != list(range(len(bone_parts))):
            self.note(f"LOD {li}: the mesh parts are not the last parts; export renumbers them after the bones")
        arm = bpy.data.armatures.new(f"{self.sc['name']}_Rig{li}")
        arm_ob = bpy.data.objects.new(f"{self.sc['name']}_Rig{li}", arm)
        self.link(arm_ob, root, Matrix.Identity(4), self.world[root.name])
        objs.append(arm_ob)
        vl = self.scene.view_layers[0]
        with self.context.temp_override(scene=self.scene, view_layer=vl, active_object=arm_ob, object=arm_ob,
                                        selected_objects=[arm_ob]):
            vl.objects.active = arm_ob
            bpy.ops.object.mode_set(mode="EDIT")
            bones = {}
            for pi in bone_parts:
                b = arm.edit_bones.new(f"BN{pi + 1:02d}")
                b.head = self.blender(lod["parts"][pi]["pivot"])
                b.use_connect = False
                bones[pi] = b
            for pi in bone_parts:
                b = bones[pi]
                kids = [c for c in bone_parts if lod["parts"][c]["parent"] == pi and c != pi]
                tail = None
                if kids:
                    t = self.blender(lod["parts"][kids[0]]["pivot"])
                    if (t - b.head).length > 1e-3:
                        tail = t
                b.tail = tail if tail is not None else b.head + Vector((0.0, 0.0, 0.05))
                parent = lod["parts"][pi]["parent"]
                if parent != pi and parent in bones:
                    b.parent = bones[parent]
            bpy.ops.object.mode_set(mode="OBJECT")
        for k, pi in enumerate(mesh_parts):
            part = lod["parts"][pi]
            pivot = self.blender(part["pivot"])
            me, weights = self.mesh(f"01 Mesh{k}", part["strips"], pivot, mats, True)
            mob = bpy.data.objects.new(f"01 Mesh{k}", me)
            self.link(mob, arm_ob, Matrix.Translation(pivot), self.world[arm_ob.name])
            groups = {}
            for vi, infl in enumerate(weights):
                for bone, w in infl:
                    g = groups.get(bone)
                    if g is None:
                        g = groups[bone] = mob.vertex_groups.new(name=f"BN{bone + 1:02d}")
                    g.add([vi], w, "REPLACE")
            mod = mob.modifiers.new("Armature", "ARMATURE")
            mod.object = arm_ob
            objs.append(mob)
        if li == 0:
            self.lod0_parts = {pi: arm_ob for pi in range(len(lod["parts"]))}
        return objs

    # --- points, lights, occlusion, collision ---------------------------------
    def owner(self, part, lod_objects):
        ob = self.lod0_parts.get(part)
        return ob if ob is not None else lod_objects[0][0]

    def points(self, lod_objects):
        for order, u in enumerate(self.sc["points"]):
            letter = chr(u["type"]) if 65 <= u["type"] <= 90 else "G"
            if letter != chr(u["type"]):
                self.note(f"user point {u['name']}: type {u['type']} has no letter; written as G")
            label = u["name"] if u["name"] else "Noname"
            name = f"UP{letter}{u['part'] + 1:02d} {label}" if u["part"] >= 0 else f"UP{letter}00 {label}"
            d = self.blender(u["d"])
            rot = d.to_track_quat("Z", "Y").to_matrix() if d.length > 1e-9 else Matrix.Identity(3)
            world = Matrix.Translation(self.blender(u["p"])) @ rot.to_4x4()
            parent = self.owner(u["part"], lod_objects)
            ob = self.empty(name, parent, world, size=0.15, display="SINGLE_ARROW")
            ob.o3d.order = order
            lod_objects[0].append(ob)

    def lights(self, lod_objects):
        dups = {}
        for i, l in enumerate(self.sc["lights"]):
            spot = bool(l["flags"] & 0x08)
            data = bpy.data.lights.new(f"LP{l['part'] + 1:02d}", "SPOT" if spot else "POINT")
            data.color = [c / 255.0 for c in l["rgb0"]]
            data.energy = 50.0
            if hasattr(data, "use_custom_distance"):
                data.use_custom_distance = True
                data.cutoff_distance = max(l["atten"][1], 0.01)
            p = data.o3d
            p.style = l["style"]
            if l["style"] > 0x70:
                idx = int(l["phase"])
                p.register = self.sc["registers"][idx] if 0 <= idx < len(self.sc["registers"]) else ""
            else:
                p.phase = l["phase"]
            p.rate = l["rate"]
            p.color_end = [c / 255.0 for c in l["rgb1"]]
            p.atten_start, p.atten_end = l["atten"]
            p.disable_corona = bool(l["flags"] & 1)
            p.disable_terrain = bool(l["flags"] & 2)
            p.disable_objects = bool(l["flags"] & 4)
            p.other_flags = l["flags"] & ~0x0F & 0xFF
            # The light's local Z is its stored axis (straight down for omni
            # lights), as the ASE light matrix row the retired exporter read.
            d = self.blender(l["dir"] if l["dir"] else (0.0, 0.0, -1.0))
            rot = d.to_track_quat("Z", "Y").to_matrix()
            if spot:
                data.spot_size = math.radians(max(1.0, 2.0 * l["falloff"]))
            name = f"LP{l['part'] + 1:02d}" + dup_suffix(dups, l["part"])
            ob = bpy.data.objects.new(name, data)
            world = Matrix.Translation(self.blender(l["p"])) @ rot.to_4x4()
            parent = self.owner(l["part"], lod_objects)
            self.link(ob, parent, world, self.world.get(parent.name))
            ob.o3d.order = i
            lod_objects[0].append(ob)

    def occlusion(self, lod_objects):
        dups = {}
        for i, o in enumerate(self.sc["occ"]):
            prefix = OCC_PREFIX.get(o["type"])
            if prefix is None:
                self.note(f"occlusion record {i}: type {o['type']} has no name form")
                continue
            key = (prefix, o["a"], o["type"] == 3 and o["b"])
            name = f"{prefix}{o['a'] + 1:02d}" + dup_suffix(dups, key)
            if o["type"] == 3:
                name += f"-{o['b'] + 1:02d}"
            elif o["type"] == 2 and o["b"] != 0:
                self.note(f"occlusion record {i}: a window into section {o['b']} (windows open to the exterior)")
            me = bpy.data.meshes.new(name + "-occonly")
            me.from_pydata([tuple(self.blender(v)) for v in o["verts"]], [], o["faces"])
            me.update()
            ob = bpy.data.objects.new(name + "-occonly", me)
            ob.display_type = "WIRE"
            parent = lod_objects[0][0]
            self.link(ob, parent, Matrix.Identity(4), self.world[parent.name])
            ob.o3d.order = i
            lod_objects[0].append(ob)

    def collision(self, lod_objects):
        dups = {}
        for si, c in enumerate(self.sc["cobjs"]):
            for v in c["volumes"]:
                code = VOLUME_NAMES.get(v["type"])
                if code is None:
                    self.note(f"collision section {si}: volume type {v['type']} has no name code")
                    continue
                letters = ""
                if code == "BB":
                    if v["flags"] & ~0x3E:
                        self.note(f"blink box flags 0x{v['flags']:x} carry bits outside 0x3e")
                    letters = "".join(L for L, bit in export.BLINK_LETTER_BITS.items() if not v["flags"] & bit)
                elif v["flags"]:
                    self.note(f"collision section {si}: a {code} volume with flags {v['flags']} (only BB takes flags)")
                corners = polytope(v["planes"])
                if len(corners) < 4:
                    self.note(f"collision section {si}: a {code} volume bounds no solid")
                    continue
                name = f"{code}{letters}{si + 1:02d}" + dup_suffix(dups, (code + letters, si)) + "-colonly"
                bm = bmesh.new()
                for p in corners:
                    bm.verts.new(self.blender(p))
                bmesh.ops.convex_hull(bm, input=bm.verts)
                me = bpy.data.meshes.new(name)
                bm.to_mesh(me)
                bm.free()
                ob = bpy.data.objects.new(name, me)
                ob.display_type = "WIRE"
                parent = self.owner(si, lod_objects)
                self.link(ob, parent, Matrix.Identity(4), self.world.get(parent.name))
                lod_objects[0].append(ob)

    def bullet_lod(self, mats):
        """The render LOD whose part triangles are the bullet faces (the OED
        poly_collision_lod), and each material's face surface voted from them."""
        counts = [len(c["faces"]) for c in self.sc["cobjs"]]
        if not any(counts):
            return
        self.note("bullet faces are rebuilt from the collision LOD; stored face normals and flags are not retained")
        chosen = None
        for li, lod in enumerate(self.sc["lods"]):
            tris = [sum(len(s["tris"]) for s in p["strips"]) for p in lod["parts"]]
            if self.sc["skinned"]:
                tris = [0] * len(counts)
                for pi, p in enumerate(lod["parts"]):
                    if pi < len(tris):
                        tris[pi] = sum(len(s["tris"]) for s in p["strips"])
            tris += [0] * (len(counts) - len(tris))
            if tris[:len(counts)] == counts:
                chosen = li
                break
        if chosen is None:
            self.note("the bullet faces match no render LOD; export derives them from LOD "
                      f"{self.scene.o3d.poly_collision_lod}")
            return
        self.scene.o3d.poly_collision_lod = chosen
        votes = {}
        lod = self.sc["lods"][chosen]
        for si, c in enumerate(self.sc["cobjs"]):
            if si >= len(lod["parts"]):
                break
            by_centre = {}
            for a, b, cc, poly in c["faces"]:
                p = [c["verts"][x] for x in (a, b, cc)]
                by_centre[tuple(round(sum(q[k] for q in p) / 3.0, 1) for k in range(3))] = poly
            for s in lod["parts"][si]["strips"]:
                for tri in s["tris"]:
                    p = [s["verts"][x]["p"] for x in tri]
                    key = tuple(round(sum(q[k] for q in p) / 3.0, 1) for k in range(3))
                    if key in by_centre:
                        votes.setdefault(s["material"], {}).setdefault(by_centre[key], 0)
                        votes[s["material"]][by_centre[key]] += 1
        for mi, v in votes.items():
            if 0 <= mi < len(mats):
                mats[mi].o3d.surface = max(v.items(), key=lambda kv: kv[1])[0]


def polytope(planes):
    """Corners of the convex solid the planes bound (n . p + d <= 0 inside)."""
    out = []
    n = len(planes)
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                a, b, c = (Vector(planes[x][0]) for x in (i, j, k))
                m = Matrix((a, b, c))
                if abs(m.determinant()) < 1e-9:
                    continue
                p = m.inverted() @ Vector((-planes[i][1], -planes[j][1], -planes[k][1]))
                if all(Vector(q[0]).dot(p) + q[1] <= 1e-4 for q in planes) and \
                        not any((p - o).length < 1e-5 for o in out):
                    out.append(p)
    return out


def run_scene(context, path):
    from . import cli_path
    cli = cli_path(context)
    if not os.path.isfile(cli):
        raise ImportFailed(f"opennova-3di not found at {cli}")
    tmp = tempfile.mkdtemp(prefix="opennova3di_")
    o3d = os.path.join(tmp, "scene.o3d")
    result = subprocess.run([cli, "scene", path, "-o", o3d], capture_output=True, text=True)
    if result.returncode != 0:
        raise ImportFailed((result.stderr or result.stdout).strip()[:2000])
    notes = [line.split("scene drops ", 1)[1] for line in result.stderr.splitlines() if "scene drops " in line]
    return o3d, notes


def import_file(context, path, op=None, new_scene=True):
    o3d, notes = run_scene(context, path)
    sc = read_o3d(o3d)
    if new_scene:
        scene = bpy.data.scenes.new(sc["name"])
        if context.window is not None:
            context.window.scene = scene
    else:
        scene = context.scene
    builder = Builder(context, sc, path, op)
    builder.build(scene)
    return scene, ["not carried: " + n for n in notes] + builder.notes
