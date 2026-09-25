# .3di -> opennova-3di scene -> .o3d text -> a Blender scene.
#
# The engine decodes the model (`opennova-3di scene`, the inverse of the
# exporter's `build`); this module only lays the .o3d out by the naming
# contract export.py reads (docs/threedi/scene-naming-contract.md), so an
# imported model exports again. Each file becomes its own Blender scene named
# after the model (the exporter reads a whole scene), with LOD 1 and up hidden.
# Textures load from the file `opennova-3di` resolved beside the .3di by the
# runtime's candidate order (`texfile` records). What the scene cannot carry is
# reported as a note; a file that fails leaves nothing of itself behind.

import math
import os

import bpy
from mathutils import Matrix, Vector

from . import export
from .o3dtext import (CTRL_REFERENCE_THRESHOLD, ExportError, ImportFailed, axis_basis, blender_axes, cli_notes, num,
                      run_cli, scratch, strip_comment, tokens)


VOLUME_NAMES = {v: k for k, v in export.VOLUME_CODES.items()}
VOLUME_NAMES[0] = export.UNLISTED_TYPE_CODE
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

def finite(values):
    return all(math.isfinite(x) for x in values)


def read_o3d(path):
    sc = {"name": "MODEL", "skinned": False, "uv1": False, "registers": [], "frames": [], "materials": [],
          "texfiles": {}, "lods": [], "points": [], "lights": [], "occ": [], "cobjs": [], "cxlt": [],
          "cxlt_given": False}
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
                sc["frames"].append([num(x) for x in a[:9]])
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
                mat["rgbgen"] = (int(a[0]), int(a[1]), num(a[2]), [int(x) for x in a[3:6]], [int(x) for x in a[6:9]],
                                 num(a[9]) if len(a) > 9 else 0.0)
            elif k in ("alphagen", "ugen", "vgen"):
                mat[k] = (int(a[0]), int(a[1]), num(a[2]), num(a[3]), num(a[4]), num(a[5]) if len(a) > 5 else 0.0)
            elif k == "lod":
                lod = {"threshold": int(a[0]) if a else 0, "type": a[1] if len(a) > 1 else "gnrc", "parts": [],
                       "panm": []}
                sc["lods"].append(lod)
            elif k == "part":
                # A part that draws nothing may carry the point its bounds sit
                # on after its pivot.
                part = {"parent": int(a[0]), "pivot": tuple(num(x) for x in a[1:4]),
                        "centre": tuple(num(x) for x in a[4:7]) if len(a) >= 7 else None, "strips": []}
                lod["parts"].append(part)
            elif k == "strip":
                strip = {"material": int(a[0]), "alpha": len(a) > 1 and a[1] != "0", "bones": [], "verts": [],
                         "tris": []}
                part["strips"].append(strip)
            elif k == "bones":
                strip["bones"] = [int(x) for x in a]
            elif k == "v":
                f = [num(x) for x in a]
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
                sc["points"].append({"name": a[0], "p": tuple(num(x) for x in a[1:4]),
                                     "d": tuple(num(x) for x in a[4:7]), "part": int(a[7]),
                                     "type": int(a[8]) if len(a) > 8 else 71})
            elif k == "light":
                light = {"part": int(a[0]), "p": tuple(num(x) for x in a[1:4]), "atten": (num(a[4]), num(a[5])),
                         "style": int(a[6]), "rate": num(a[7]), "phase": num(a[8]),
                         "rgb0": [int(x) for x in a[9:12]], "rgb1": [int(x) for x in a[12:15]], "flags": int(a[15], 0),
                         "dir": None, "falloff": 0.0}
                if len(a) >= 20:
                    light["dir"] = tuple(num(x) for x in a[16:19])
                    light["falloff"] = num(a[19])
                sc["lights"].append(light)
            elif k == "occ":
                occ = {"type": int(a[0]), "a": int(a[1]), "b": int(a[2]), "verts": [], "faces": []}
                sc["occ"].append(occ)
            elif k == "ov":
                occ["verts"].append(tuple(num(x) for x in a[:3]))
            elif k == "of":
                occ["faces"].append(tuple(int(x) for x in a[:3]))
            elif k == "cobj":
                cobj = {"parent": int(a[0]), "offset": tuple(num(x) for x in a[1:4]) if len(a) >= 4 else (0, 0, 0),
                        "sphere": None, "verts": [], "faces": [], "volumes": []}
                sc["cobjs"].append(cobj)
            elif k == "csphere":
                cobj["sphere"] = tuple(num(x) for x in a[:4])
            elif k == "cv":
                cobj["verts"].append(tuple(num(x) for x in a[:3]))
            elif k == "cf":
                cobj["faces"].append((int(a[0]), int(a[1]), int(a[2]), int(a[3]) if len(a) > 3 else 1,
                                      int(a[4]) if len(a) > 4 else 0))
            elif k in ("cvolume", "cvol"):
                box = [num(x) for x in a[2:8]]
                volume = {"type": int(a[0]), "flags": int(a[1]), "planes": []}
                if k == "cvol":
                    volume["planes"] = [((1, 0, 0), -box[3]), ((-1, 0, 0), box[0]), ((0, 1, 0), -box[4]),
                                        ((0, -1, 0), box[1]), ((0, 0, 1), -box[5]), ((0, 0, -1), box[2])]
                cobj["volumes"].append(volume)
            elif k == "cp":
                volume["planes"].append((tuple(num(x) for x in a[:3]), num(a[3])))
            elif k == "cxlt":
                # A bare `cxlt` declares the table empty.
                sc["cxlt_given"] = True
                if a:
                    sc["cxlt"].append(tuple(num(x) for x in a[:3]))
    return sc


# --- scene building ------------------------------------------------------------

class Builder:
    def __init__(self, context, sc, source_path, op):
        self.context = context
        self.sc = sc
        self.dir = os.path.dirname(os.path.abspath(source_path))
        self.stem = os.path.splitext(os.path.basename(source_path))[0]
        self.op = op
        self.notes = []
        self.images = {}
        self.collection = None
        self.made_materials = []
        self.part_objects = {}  # LOD -> {part: its PN## empty}
        self.skinned_parts = {}  # LOD -> (armature, {mesh part: its mesh})
        self.attach_helpers = {}  # (LOD, part) -> its `~PPx attach` helper

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    def discard(self):
        """Remove what a failed build made."""
        if self.collection is not None:
            for ob in list(self.collection.objects):
                bpy.data.objects.remove(ob)
            bpy.data.collections.remove(self.collection)
        for mat in self.made_materials:
            bpy.data.materials.remove(mat)

    def link(self, ob, parent=None, world=None, part=False):
        """Put `ob` at `world` under `parent`. Blender holds an object's own
        matrix as location, rotation and scale, which a retail part frame is
        not quite (Mp5b_1st's scales run 0.9995 to 1.0005), so every offset is
        taken from the frame the parent actually got (self.world keeps each
        object's matrix as Blender holds it). A part empty carries its frame
        in its own matrix, its offset from its parent part there too, so its
        pivot lands exactly and only its frame keeps what Blender can hold.
        Anything else is parented the way Blender's Keep Transform does it:
        the parent's inverse in the parent inverse, which Blender keeps whole,
        and `world` as its own matrix, so a part's mesh, helpers, points and
        volumes land where the file puts them however the part is turned."""
        self.collection.objects.link(ob)
        above = Matrix.Identity(4)
        world = world if world is not None else Matrix.Identity(4)
        if parent is not None:
            ob.parent = parent
            above = self.world[parent.name]
            ob.matrix_parent_inverse = Matrix.Identity(4) if part else above.inverted()
        ob.matrix_basis = above.inverted() @ world if part else world
        self.world[ob.name] = above @ ob.matrix_parent_inverse @ ob.matrix_basis
        return ob

    def empty(self, name, parent=None, world=None, size=0.1, display="PLAIN_AXES", part=False):
        ob = bpy.data.objects.new(name, None)
        ob.empty_display_type = display
        ob.empty_display_size = size
        return self.link(ob, parent, world, part)

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
                # Blender loads what it cannot decode (PCX, archive-compressed
                # files) as an image without pixels.
                if img.size[0] == 0:
                    self.note(f"texture {name}: Blender cannot read {os.path.basename(path)}")
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
            if style > CTRL_REFERENCE_THRESHOLD and 0 <= index < len(reg):
                return reg[index]
            return ""

        for i, m in enumerate(self.sc["materials"]):
            # The name carries the export index and the shader tag.
            mat = bpy.data.materials.new(f"Material_{i}_{m['shader']}")
            self.made_materials.append(mat)
            p = mat.o3d
            flags = m["matflags"]
            p.alpha_test = bool(flags & 1)
            p.two_sided = bool(flags & 4)
            p.other_flags = flags & ~5 & 0xFF
            p.alpha_test_value = m["alphatest"]
            if m["alphatest"] and not p.alpha_test:
                self.note(f"material {i}: an alpha-test value without the alpha-test flag")
            # Glass and emissive follow the shader on export (OED's rule).
            caps = export.shader_flags(m["shader"])
            if bool(m["glass"]) != bool(caps & export.FLAG_GLASS and m["reflect"] and any(m["reflect"][:3])):
                self.note(f"material {i}: glass {m['glass']} is not what its shader {m['shader']} gives")
            if m["emissive"] != (2 if caps & export.FLAG_EMISSIVE else 0):
                self.note(f"material {i}: emissive {m['emissive']} is not what its shader {m['shader']} gives")
            if m["reflect"]:
                p.reflect = [c / 255.0 for c in m["reflect"]]
            blended = bool(export.shader_flags(m["shader"]) & export.FLAG_BLENDING)
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
            if m["emissive"] or export.shader_flags(m["shader"]) & export.FLAG_EMISSIVE:
                links.new(color, bsdf.inputs["Emission Color"])
                bsdf.inputs["Emission Strength"].default_value = 1.0
        if m["glass"] or export.shader_flags(m["shader"]) & export.FLAG_GLASS:
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

        def normal(v):
            # Zero-length and not-finite (J_bsh1's NaN) normals alike: Blender
            # calculates the corner's own.
            n = v["n"]
            if not finite(n) or sum(x * x for x in n) < 1e-12:
                self.note("zero-length or not-finite vertex normals are replaced by Blender's calculated normals")
                return (0.0, 0.0, 0.0)
            return n

        for s in strips:
            mi = s["material"]
            if mi not in slots:
                slots.append(mi)
            ids = []
            for v in s["verts"]:
                key = (tuple(round(x, 6) for x in v["p"]), tuple(round(x, 4) for x in normal(v)))
                infl = ()
                if skinned:
                    infl = tuple(sorted((s["bones"][b], round(w, 5)) for b, w in zip(v["bi"], v["bw"])
                                        if w > 0 and b < len(s["bones"])))
                    if not infl and v["bi"] and v["bi"][0] < len(s["bones"]):
                        # Every weight zero (dM1A1's LOD 3; the renderer
                        # draws such a vertex wholly on its slot 0 bone,
                        # runtime/renderer/model_mesh_prepare.cpp): a weight 0
                        # membership of that bone, which export writes back.
                        infl = ((s["bones"][v["bi"][0]], 0.0),)
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
        me.normals_split_custom_set([tuple(self.blender(normal(v)).normalized()) for v in loops])
        me.update()
        return me, weights

    # --- the model --------------------------------------------------------------
    def build(self, scene):
        sc = self.sc
        self.scene = scene
        self.forward = scene.o3d.forward
        # mission -> Blender, the inverse of the export's axis map
        self.blender = blender_axes(self.forward)
        self.world = {}
        # The output path follows the imported file (Excavatr.3di's GHDR name
        # is OrngFlag), one per model root: a second import of one file writes
        # beside the first, not over it.
        taken = {m.o3d.output_path.lower() for m in export.model_roots(scene)}
        output = f"//{self.stem}.3di"
        n = 2
        while output.lower() in taken:
            output = f"//{self.stem}_{n}.3di"
            n += 1
        self.collection = bpy.data.collections.new(sc["name"])
        scene.collection.children.link(self.collection)
        mats = self.materials()
        # The model root: one .3di, its LOD roots below it. It holds the
        # model's own settings, so several models share a scene.
        self.model = self.empty(sc["name"], size=1.0, display="CUBE")
        self.model.o3d.model_name = sc["name"]
        self.model.o3d.output_path = output
        lod_objects = []
        self.lod0_parts = {}
        self.split = self.skinned_split() if sc["skinned"] else None
        for li, lod in enumerate(sc["lods"]):
            root = self.empty(f"{sc['name']}_LOD{li}", self.model, size=0.5, display="ARROWS")
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
        self.attach_points(lod_objects)
        if sc["skinned"] and sc["cobjs"]:
            self.note("skin weights are normalized on export and skinned hit spheres are regenerated from them")
        vl = self.context.view_layer
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
        b = axis_basis(self.forward)
        return b @ q @ b.transposed()

    def rigid_lod(self, li, lod, root, mats):
        objs = []
        parts = []
        rows = {p["part"]: p for p in lod["panm"]}
        count = len(lod["parts"])
        for pi, part in enumerate(lod["parts"]):
            pivot = self.blender(part["pivot"])
            row = rows.get(pi)
            rot = self.frame_rotation(row["matrix"]) if row else Matrix.Identity(3)
            world = Matrix.Translation(pivot) @ rot.to_4x4()
            parent = part["parent"]
            if pi == 0 or parent == pi or parent < 0 or parent >= count:
                parent_ob = root
                if pi == 0 and parent != 0 or parent >= count:
                    self.note(f"LOD {li} part {pi} names parent {parent}; it exports under the root")
            elif parent < len(parts):
                parent_ob = parts[parent]
            else:
                parent_ob = root  # re-parented below once the parent exists
            ob = self.empty(f"PN{pi + 1:02d}", parent_ob, world, part=True)
            parts.append(ob)
            objs.append(ob)
            if pi > 0 and (parent == pi or parent < 0):
                # A part that is its own parent (Eturret, APLFP1) or names -1
                # (Excavatr): the Blender hierarchy cannot say it, an attach
                # helper can (`~PP attach`, PP its own number or 00).
                objs.append(self.attach_helper(li, pi, ob, parent))
            if row is not None:
                self.tracks(ob.o3d, row, lod)
            if part["strips"]:
                me, _ = self.mesh(f"{pi + 1:02d} Mesh0", part["strips"], pivot, mats, False)
                mob = bpy.data.objects.new(f"{pi + 1:02d} Mesh0", me)
                objs.append(self.link(mob, ob, Matrix.Translation(pivot)))
            elif part["centre"] is not None:
                objs.append(self.centre_helper(pi, ob, part["centre"]))
        for pi, part in enumerate(lod["parts"]):
            parent = part["parent"]
            if pi < parent < len(parts):
                # A parent listed after its child (retail ships some).
                ob = parts[pi]
                above = self.world[parts[parent].name]
                ob.parent = parts[parent]
                ob.matrix_parent_inverse = Matrix.Identity(4)
                ob.matrix_basis = above.inverted() @ self.world[ob.name]
                self.world[ob.name] = above @ ob.matrix_basis
        self.part_objects[li] = dict(enumerate(parts))
        if li == 0:
            self.lod0_parts = {i: ob for i, ob in enumerate(parts)}
        return objs

    def centre_helper(self, pi, part_ob, centre):
        """Part pi's `_## center` helper mesh, for a part that draws nothing:
        OED seeded such a part with its helper's first vertex, the point its
        bounds sit on at radius 0 (5fc5b4f6a^ engine/formats/oed/
        convert_internal.cpp, the placeholder injection), which retail keeps
        near the pivot. The helper's origin is the part's pivot, as export
        reads it, so it sits on its PN## empty with no offset of its own and
        its one vertex is that point."""
        me = bpy.data.meshes.new(f"_{pi + 1:02d} center")
        me.from_pydata([tuple(self.world[part_ob.name].inverted() @ self.blender(centre))], [], [])
        ob = bpy.data.objects.new(f"_{pi + 1:02d} center", me)
        self.collection.objects.link(ob)
        ob.parent = part_ob
        ob.matrix_parent_inverse = Matrix.Identity(4)
        self.world[ob.name] = self.world[part_ob.name]
        return ob

    def attach_helper(self, li, pi, parent_ob, parent, world=None, bone=None):
        """Part pi's `~PP attach` helper in LOD li, naming its parent part
        (PP 1-based; 00 for -1), at `world`: under its PN## empty, or on a
        skinned model under its bone (or its mesh part's mesh). Without a
        `world` it sits on its PN## empty's origin, the part's pivot, with no
        offset of its own, so it reads back as the very pivot (its attach
        point is then the row the builder would derive)."""
        ob = bpy.data.objects.new(f"~{parent + 1 if parent >= 0 else 0:02d} attach", None)
        ob.empty_display_type = "PLAIN_AXES"
        ob.empty_display_size = 0.05
        if world is None:
            self.collection.objects.link(ob)
            ob.parent = parent_ob
            ob.matrix_parent_inverse = Matrix.Identity(4)
            self.world[ob.name] = self.world[parent_ob.name]
        elif bone is None:
            self.link(ob, parent_ob, world)
        else:
            # A bone child hangs off the bone's tail, at rest here.
            self.collection.objects.link(ob)
            ob.parent = parent_ob
            ob.parent_type = "BONE"
            ob.parent_bone = bone.name
            ob.matrix_parent_inverse = Matrix.Identity(4)
            tail = self.world[parent_ob.name] @ bone.matrix_local @ Matrix.Translation((0.0, bone.length, 0.0))
            ob.matrix_basis = tail.inverted() @ world
            self.world[ob.name] = world
        self.attach_helpers[(li, pi)] = ob
        return ob

    def attach_points(self, lod_objects):
        """The CXLT attach points: retail stores one per part after the root
        on a rigid model and one per part on a skinned one (the builder's
        derivation, formats/threedi/threedi_build.cpp), each at its part's
        `~PPx attach` helper in the collision LOD, OED's WriteCXLT source
        (5fc5b4f6a^ engine/formats/oed/export_3di.cpp). A row that is its
        section's own offset, the row the builder derives, needs no helper:
        export writes a part without one at its pivot. A row count that does
        not fit places none, and an empty table (Chair03X: seven sections, no
        row) has no scene form: an attach point is a helper, and a collision
        LOD without one exports the rows the builder derives."""
        rows = self.sc["cxlt"]
        if not rows:
            if self.sc["cxlt_given"]:
                self.note("the model stores no CXLT attach point, which a scene cannot say: export leaves the rows to "
                          "the builder, which derives one per collision section"
                          f"{'' if self.sc['skinned'] else ' after the root'} at its pivot")
            return
        li = self.model.o3d.poly_collision_lod
        parts = self.sc["lods"][li]["parts"] if li < len(self.sc["lods"]) else []
        first = 0 if self.sc["skinned"] else 1
        if len(rows) != len(parts) - first:
            self.note(f"{len(rows)} CXLT attach points do not fit the collision LOD's {len(parts)} parts (one per "
                      f"part{'' if first == 0 else ' after the root'}); export puts each at its part's pivot")
            return
        cobjs = self.sc["cobjs"]
        for i, row in enumerate(rows):
            pi = i + first
            if pi < len(cobjs) and all(round(a * 65536.0) == round(b * 65536.0)
                                       for a, b in zip(row, cobjs[pi]["offset"])):
                continue  # the same 16.16 row as the section's offset
            world = Matrix.Translation(self.blender(row))
            helper = self.attach_helpers.get((li, pi))
            if helper is not None:
                # A rigid part's own helper (it names itself or none): moved
                # off its pivot the way link() places an object.
                above = self.world[helper.parent.name]
                helper.matrix_parent_inverse = above.inverted()
                helper.matrix_basis = world
                self.world[helper.name] = above @ helper.matrix_parent_inverse @ helper.matrix_basis
                continue
            parent = parts[pi]["parent"]
            if self.sc["skinned"]:
                arm, meshes = self.skinned_parts[li]
                if pi in meshes:
                    helper = self.attach_helper(li, pi, meshes[pi], parent, world)
                else:
                    helper = self.attach_helper(li, pi, arm, parent, world, arm.data.bones[f"BN{pi + 1:02d}"])
            else:
                helper = self.attach_helper(li, pi, self.part_objects[li][pi], parent, world)
            lod_objects[li].append(helper)

    def tracks(self, p, row, lod):
        """A PANM row's flags and tracks onto a part's animation (an empty's
        or a bone's o3d)."""
        axis_default = (row["flags"] or 0) >> 24 & 0xFF
        derived = export.derived_panm_flags([(t[0], t[1], (t[6] or axis_default or 3) if t[0] == "trans" else 0)
                                             for t in row["tracks"]])
        if row["flags"] is not None and row["flags"] != derived:
            p.panm_flags = row["flags"]
        for target, style, reg, rate, start, end, axis in row["tracks"]:
            t = p.tracks.add()
            t.target = target
            t.style = style
            if style > CTRL_REFERENCE_THRESHOLD:
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
        # The mesh parts are the parts `scene` authored strips on that no
        # bone table names (ArmsG part 37); every other part is a bone, the
        # root among them, and strips `scene` kept on a bone stay on it
        # (dM1A1's hull, which has no mesh part, on BN01).
        named = {b for p in lod["parts"] for s in p["strips"] for b in s["bones"]}
        mesh_parts = [pi for pi, p in enumerate(lod["parts"]) if pi > 0 and p["strips"] and pi not in named]
        bone_parts = [pi for pi in range(len(lod["parts"])) if pi not in mesh_parts]
        if bone_parts != list(range(len(bone_parts))):
            self.note(f"LOD {li}: the mesh parts are not the last parts; export renumbers them after the bones")
        arm = bpy.data.armatures.new(f"{self.sc['name']}_Rig{li}")
        arm_ob = bpy.data.objects.new(f"{self.sc['name']}_Rig{li}", arm)
        self.link(arm_ob, root)
        objs.append(arm_ob)
        vl = self.context.view_layer
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
                elif pi > 0:
                    # DT801's LOD 3 names -1: a bone without a parent bone
                    # exports parent 0.
                    self.note(f"LOD {li} bone BN{pi + 1:02d} names parent {parent}; it exports under the root (0)")
            bpy.ops.object.mode_set(mode="OBJECT")
        # Each bone's part animation: its tracks, flags and track frame.
        rows = {p["part"]: p for p in lod["panm"]}
        for pi in bone_parts:
            row = rows.get(pi)
            if row is None:
                continue
            p = arm.bones[f"BN{pi + 1:02d}"].o3d
            p.frame = self.frame_rotation(row["matrix"]).to_euler()
            self.tracks(p, row, lod)
        authored = {}
        for pi, part in enumerate(lod["parts"]):
            for s in part["strips"]:
                if self.split is not None and self.split[0] == li:
                    # The collision LOD of a model without a mesh part: each
                    # triangle back on the part whose section holds it.
                    by_part = {}
                    for ti, tri in enumerate(s["tris"]):
                        by_part.setdefault(self.split[1][(id(s), ti)], []).append(tri)
                    for owner, tris in by_part.items():
                        authored.setdefault(owner, []).append(sub_strip(s, tris))
                else:
                    authored.setdefault(pi, []).append(s)
        meshes = {}
        for pi, strips in sorted(authored.items()):
            part = lod["parts"][pi]
            pivot = self.blender(part["pivot"])
            me, weights = self.mesh(f"{pi + 1:02d} Mesh0", strips, pivot, mats, True)
            mob = bpy.data.objects.new(f"{pi + 1:02d} Mesh0", me)
            self.link(mob, arm_ob, Matrix.Translation(pivot))
            if pi in mesh_parts:
                meshes[pi] = mob
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
        self.skinned_parts[li] = (arm_ob, meshes)
        seeded = sum(1 for p in lod["parts"] if p["centre"] is not None)
        if seeded:
            # dM1A1 and DT801 only: a skinned part's pivot is its bone head,
            # and no helper of a skinned model carries this point.
            self.note(f"LOD {li}: {seeded} bones that draw nothing keep the point their bounds sit on, which a "
                      "skinned model's scene does not carry; export leaves it at the origin")
        if li == 0:
            self.lod0_parts = {pi: arm_ob for pi in range(len(lod["parts"]))}
        return objs

    def skinned_split(self):
        """A skinned model with no mesh part keeps every strip on the root
        (dM1A1's hull), yet each part was authored with geometry of its own:
        the collision LOD's triangles are exactly the bullet faces of the
        sections, one section per part (dM1A1: LOD 1, 875 hull faces and 40
        a wheel). Returns (that LOD, {(strip id, triangle): part}) matched by
        centroid, or None when no LOD's triangles are the bullet faces."""
        cobjs = self.sc["cobjs"]
        total = sum(len(c["faces"]) for c in cobjs)
        if not total or len(cobjs) < 2:
            return None
        buckets = {}
        for si, c in enumerate(cobjs):
            for a, b, cc, _, _ in c["faces"]:
                centre = centroid([c["verts"][x] for x in (a, b, cc)])
                buckets.setdefault(tuple(round(x, 1) for x in centre), []).append((centre, si))
        for li, lod in enumerate(self.sc["lods"]):
            strips = [s for p in lod["parts"] for s in p["strips"]]
            if len(lod["parts"]) != len(cobjs) or any(p["strips"] for p in lod["parts"][1:]) or \
                    sum(len(s["tris"]) for s in strips) != total:
                continue
            owners = {}
            for s in strips:
                for ti, tri in enumerate(s["tris"]):
                    centre = centroid([s["verts"][x]["p"] for x in tri])
                    key = tuple(round(x, 1) for x in centre)
                    near = [e for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
                            for e in buckets.get((round(key[0] + dx / 10, 1), round(key[1] + dy / 10, 1),
                                                  round(key[2] + dz / 10, 1)), [])]
                    best = min(near, key=lambda e: sum((e[0][k] - centre[k]) ** 2 for k in range(3)), default=None)
                    if best is None or sum((best[0][k] - centre[k]) ** 2 for k in range(3)) > 0.05 ** 2:
                        break
                    owners[(id(s), ti)] = best[1]
                else:
                    continue
                break
            else:
                return li, owners
        return None

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
            if l["style"] > CTRL_REFERENCE_THRESHOLD:
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
            # The light's local -Z is its stored axis (straight down for omni
            # lights), the way Blender draws a spot light's cone.
            d = self.blender(l["dir"] if l["dir"] else (0.0, 0.0, -1.0))
            if abs(d.length - 1.0) > 1e-3:
                self.note(f"light {i}: its stored axis is {d.length:.4g} long (CmpFire1, the FireBrl barrels); export "
                          "writes the light's unit axis")
            rot = d.to_track_quat("-Z", "Y").to_matrix()
            if spot:
                data.spot_size = math.radians(max(1.0, 2.0 * l["falloff"]))
            name = f"LP{l['part'] + 1:02d}" + dup_suffix(dups, l["part"])
            ob = bpy.data.objects.new(name, data)
            world = Matrix.Translation(self.blender(l["p"])) @ rot.to_4x4()
            parent = self.owner(l["part"], lod_objects)
            self.link(ob, parent, world)
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
            if not o["verts"]:
                self.note(f"occlusion record {i} holds no polygon (ChmLFP1's stored planes are not finite); it "
                          "exports with none")
            me = bpy.data.meshes.new(name + "-occonly")
            me.from_pydata([tuple(self.blender(v)) for v in o["verts"]], [], o["faces"])
            me.update()
            ob = bpy.data.objects.new(name + "-occonly", me)
            ob.display_type = "WIRE"
            parent = lod_objects[0][0]
            self.link(ob, parent)
            ob.o3d.order = i
            lod_objects[0].append(ob)

    def collision(self, lod_objects):
        dups = {}
        unbuilt = 0
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
                facets, missing = volume_facets(v["planes"], ladder=v["type"] == 4)
                if len(facets) < 4:
                    # A ladder without thickness (94 of the 102 retail ones)
                    # is the one polygon on plane 0, its facing, which the OED
                    # rule reads back as the same planes (all 94). No polygon
                    # of a flat CB or CP volume (38 retail) rebuilds its
                    # planes, so it stays out.
                    flat = [f for pi, f in facets if pi == 0] if v["type"] == 4 else []
                    if not flat:
                        self.note(f"collision section {si}: a {code} volume bounds no solid")
                        continue
                    facets = [(0, flat[0])]
                unbuilt += missing
                name = f"{code}{letters}{si + 1:02d}" + dup_suffix(dups, (code + letters, si)) + "-colonly"
                verts, index, polys = [], {}, []
                for _, facet in facets:
                    poly = []
                    for p in facet:
                        key = tuple(round(x, 5) for x in p)
                        if key not in index:
                            index[key] = len(verts)
                            verts.append(tuple(self.blender(p)))
                        if not poly or poly[-1] != index[key]:
                            poly.append(index[key])
                    if len(poly) > 1 and poly[0] == poly[-1]:
                        poly.pop()
                    if len(set(poly)) >= 3:
                        polys.append(poly)
                me = bpy.data.meshes.new(name)
                me.from_pydata(verts, [], polys)
                me.update()
                ob = bpy.data.objects.new(name, me)
                ob.display_type = "WIRE"
                parent = self.owner(si, lod_objects)
                self.link(ob, parent)
                lod_objects[0].append(ob)
        if unbuilt:
            self.note(f"{unbuilt} collision volume planes touch their volume in no face of 1 cm2 or more (or not at "
                      "all); export derives planes from faces, so it does not rebuild them")

    def bullet_lod(self, mats):
        """The render LOD whose parts are the collision sections and whose
        part triangles are the bullet faces (the OED poly_collision_lod), and
        each material's bullet-face surface and flags voted from them. When no
        LOD holds them one for one (a first-person weapon's bullet faces come
        from a collision mesh of its own, which the file does not keep), the
        LOD whose triangles meet most of them, at least half, still votes."""
        counts = [len(c["faces"]) for c in self.sc["cobjs"]]
        if not any(counts):
            return
        self.note("bullet faces are rebuilt from the collision LOD's triangles on export; their stored normals are "
                  "not kept")
        chosen = self.split[0] if self.split is not None else None
        for li, lod in enumerate(self.sc["lods"]):
            if chosen is not None or len(lod["parts"]) != len(counts):
                continue
            if [sum(len(s["tris"]) for s in p["strips"]) for p in lod["parts"]] == counts:
                chosen = li
                break
        if chosen is not None:
            votes, _ = self.face_votes(chosen)
        else:
            best = max(((self.face_votes(li), li) for li, lod in enumerate(self.sc["lods"])
                        if len(lod["parts"]) == len(counts)), key=lambda e: e[0][1], default=None)
            if best is None or 2 * best[0][1] < sum(counts):
                self.note("the bullet faces match no render LOD; export derives them from LOD "
                          f"{self.model.o3d.poly_collision_lod}")
                return
            (votes, met), chosen = best
            self.note(f"the bullet faces match no render LOD one for one; export derives them from LOD {chosen}, "
                      f"whose triangles meet {met} of the {sum(counts)} (its materials' surfaces are voted from those)")
        self.model.o3d.poly_collision_lod = chosen
        outvoted = 0
        for mi, v in votes.items():
            if not 0 <= mi < len(mats):
                continue
            (surface, flags), won = max(v.items(), key=lambda kv: kv[1])
            outvoted += sum(v.values()) - won
            p = mats[mi].o3d
            p.surface = surface
            p.face_never_hit = bool(flags & 0x100)
            p.face_front_only = bool(flags & 0x800)
            p.face_other_flags = flags & ~0x901
            if bool(flags & 1) != p.two_sided:
                self.note(f"material {mi}: its bullet faces' both-sides flag differs from its two-sided flag; "
                          "export takes it from Two sided")
        if outvoted:
            self.note(f"{outvoted} bullet faces take their material's most common surface and flags, not their own "
                      "(a material carries one set)")

    def face_votes(self, li):
        """Each material's votes for the (surface, flags) of the bullet faces
        LOD li's triangles meet (by centroid, within its section), and how many
        they meet."""
        votes = {}
        met = 0
        lod = self.sc["lods"][li]
        section_tris = {}
        for pi, part in enumerate(lod["parts"]):
            for s in part["strips"]:
                for ti, tri in enumerate(s["tris"]):
                    si = self.split[1][(id(s), ti)] if self.split is not None and self.split[0] == li else pi
                    section_tris.setdefault(si, []).append((s["material"], [s["verts"][x]["p"] for x in tri]))
        for si, c in enumerate(self.sc["cobjs"]):
            by_centre = {}
            for a, b, cc, poly, flags in c["faces"]:
                p = [c["verts"][x] for x in (a, b, cc)]
                by_centre[tuple(round(sum(q[k] for q in p) / 3.0, 1) for k in range(3))] = (poly, flags)
            for material, p in section_tris.get(si, []):
                key = tuple(round(sum(q[k] for q in p) / 3.0, 1) for k in range(3))
                if key in by_centre:
                    votes.setdefault(material, {}).setdefault(by_centre[key], 0)
                    votes[material][by_centre[key]] += 1
                    met += 1
        return votes, met


def centroid(points):
    return tuple(sum(p[k] for p in points) / 3.0 for k in range(3))


def sub_strip(s, tris):
    """A strip holding only some of its triangles (and the vertices they use)."""
    used = sorted({x for t in tris for x in t})
    remap = {old: new for new, old in enumerate(used)}
    return dict(s, verts=[s["verts"][i] for i in used], tris=[tuple(remap[x] for x in t) for t in tris])


def solve_planes(a, b, c):
    """The point on three planes ((n, d), n . p + d == 0), or None when they
    do not meet in one point. Doubles: mathutils is single precision."""
    (na, da), (nb, db), (nc, dc) = a, b, c
    det = (na[0] * (nb[1] * nc[2] - nb[2] * nc[1]) - na[1] * (nb[0] * nc[2] - nb[2] * nc[0]) +
           na[2] * (nb[0] * nc[1] - nb[1] * nc[0]))
    if abs(det) < 1e-9:
        return None
    r = (-da, -db, -dc)
    x = (r[0] * (nb[1] * nc[2] - nb[2] * nc[1]) - na[1] * (r[1] * nc[2] - nb[2] * r[2]) +
         na[2] * (r[1] * nc[1] - nb[1] * r[2])) / det
    y = (na[0] * (r[1] * nc[2] - nb[2] * r[2]) - r[0] * (nb[0] * nc[2] - nb[2] * nc[0]) +
         na[2] * (nb[0] * r[2] - r[1] * nc[0])) / det
    z = (na[0] * (nb[1] * r[2] - r[1] * nc[1]) - na[1] * (nb[0] * r[2] - r[1] * nc[0]) +
         r[0] * (nb[0] * nc[1] - nb[1] * nc[0])) / det
    return (x, y, z)


def volume_facets(planes, ladder=False):
    """The convex solid a volume's planes bound (n . p + d <= 0 inside), as
    (plane index, polygon), one per plane it has a face on, the corners ON
    that plane and wound counter-clockwise about its outward normal, in plane
    order. The OED rule export applies (formats/threedi/threedi_build.h
    add_volume_mesh) then reads each plane back from its polygon's triangles.
    A ladder's facing (plane 0) goes last: the rule takes the ladder's facing
    from its last triangle. Also returns how many planes other than the six
    box planes got no polygon of 1 cm2 or more (the rule reads a smaller
    triangle as degenerate)."""
    n = len(planes)
    corners = []
    # Retail's stored planes (Q14 normals, 16.16 distances) meet a few tenths
    # of a millimetre off where four or more should meet, so a corner is kept
    # within 1 mm of the solid and each polygon's sub-millimetre clusters are
    # collapsed to the corner deepest inside: a sliver triangle's float
    # normal would drift past the rule's 0.005.
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                p = solve_planes(planes[i], planes[j], planes[k])
                if p is None:
                    continue
                excess = max(q[0][0] * p[0] + q[0][1] * p[1] + q[0][2] * p[2] + q[1] for q in planes)
                if excess <= 1e-3:
                    corners.append((p, (i, j, k), excess))
    facets, missing = [], 0
    for pi, (nrm, _) in enumerate(planes):
        on = []
        for p, trio, excess in corners:
            if pi in trio and not any(sum((p[x] - o[0][x]) ** 2 for x in range(3)) < 1e-12 for o in on):
                on.append((p, excess))
        axis = sum(1 for x in nrm if abs(x) > 1e-6) == 1
        polygon = None
        if len(on) >= 3:
            c = [sum(p[x] for p, _ in on) / len(on) for x in range(3)]
            u = max(((p[0] - c[0], p[1] - c[1], p[2] - c[2]) for p, _ in on), key=lambda e: sum(x * x for x in e))
            ul = math.sqrt(sum(x * x for x in u))
            if ul > 1e-9:
                u = tuple(x / ul for x in u)
                w = (nrm[1] * u[2] - nrm[2] * u[1], nrm[2] * u[0] - nrm[0] * u[2], nrm[0] * u[1] - nrm[1] * u[0])
                on.sort(key=lambda e: math.atan2(sum((e[0][x] - c[x]) * w[x] for x in range(3)),
                                                 sum((e[0][x] - c[x]) * u[x] for x in range(3))))
                clusters = []
                for p, excess in on:
                    if clusters and sum((p[x] - clusters[-1][-1][0][x]) ** 2 for x in range(3)) < 1e-6:
                        clusters[-1].append((p, excess))
                    else:
                        clusters.append([(p, excess)])
                if len(clusters) > 1 and \
                        sum((clusters[0][0][0][x] - clusters[-1][-1][0][x]) ** 2 for x in range(3)) < 1e-6:
                    clusters[0] = clusters.pop() + clusters[0]
                on = [min(cl, key=lambda e: e[1])[0] for cl in clusters]
                area = 0.0
                for a in range(1, len(on) - 1):
                    e1 = [on[a][x] - on[0][x] for x in range(3)]
                    e2 = [on[a + 1][x] - on[0][x] for x in range(3)]
                    cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                    area += 0.5 * math.sqrt(sum(x * x for x in cr))
                if area > 1e-8:
                    polygon = on
        if polygon is None:
            if not axis:
                missing += 1
            continue
        if not axis and area < 0.5e-4:
            missing += 1
        facets.append((pi, polygon))
    if ladder:
        facets.sort(key=lambda f: f[0] == 0)
    return facets, missing


def run_scene(context, path):
    """The model's scene text, read, and the CLI's notes."""
    with scratch() as tmp:
        o3d = os.path.join(tmp, "scene.o3d")
        result = run_cli(context, ["scene", path, "-o", o3d], ImportFailed)
        sc = read_o3d(o3d)
    return sc, cli_notes(result, "scene drops ")


def import_file(context, path, op=None):
    """Import one .3di into the current scene under a model root of its own;
    returns the model root and the notes. A file that fails leaves nothing of
    itself in the scene."""
    sc, notes = run_scene(context, path)
    builder = Builder(context, sc, path, op)
    try:
        builder.build(context.scene)
    except Exception as e:
        builder.discard()
        if isinstance(e, ExportError):
            raise ImportFailed(str(e)) from e
        raise
    return builder.model, ["not carried: " + n for n in notes] + builder.notes
