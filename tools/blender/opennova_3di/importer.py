# .3di -> opennova-3di scene -> .o3d text -> a Blender scene.
#
# The engine decodes the model (`opennova-3di scene`, the inverse of the
# exporter's `build`); this module only lays the .o3d out in the scene shape
# export.py reads (rig.py, docs/threedi/scene-naming-contract.md), so an
# imported model exports again. Each file comes into the current scene under a
# model root of its own (in a collection named after the model), with LOD 1
# and up hidden:
#
#   a static model     its parts as PN## empties, each part's geometry and
#                      helpers under its empty;
#   a skinned model    one Armature under LOD 0's root whose BN## bones are its
#                      parts, its skinned geometry one mesh per LOD deforming
#                      with it (on the mesh part, whose pivot is the mesh's
#                      origin, when the file keeps one), its helpers hung from
#                      the bones;
#   a first-person gun imported with its arms: the gun's parts as the bones of
#                      a rig (its geometry and helpers hung from them), and the
#                      arms, whose bones are the gun's first parts, deform with
#                      that rig: their model root sits under the gun's, and
#                      their helpers under their own LOD root, naming their
#                      part (`_05 hit`).
#
# Textures load from the files `opennova-3di` resolved beside the .3di by the
# runtime's candidate order (`texfile` records). What the scene cannot carry
# is reported as a note, never stashed; a file that fails leaves nothing of
# itself behind.

import math
import os

import bpy
from mathutils import Matrix, Vector

from . import export, materials, rig
from .o3dtext import (CTRL_REFERENCE_THRESHOLD, ExportError, ImportFailed, Notes, axis_basis, blender_axes,
                      import_text, num, strip_comment, tokens)


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
                    # Contract C1: four bone-table slots, three stored weights;
                    # slot 3 takes 1 - (w0 + w1 + w2).
                    vert["bi"] = [int(x) for x in rest[0:4]]
                    vert["bw"] = rest[4:7]
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
                occ = {"type": int(a[0]), "a": int(a[1]), "b": int(a[2]), "verts": [], "faces": [],
                       "sphere": tuple(num(x) for x in a[3:7]) if len(a) >= 7 else None}
                sc["occ"].append(occ)
            elif k == "ov":
                occ["verts"].append(tuple(num(x) for x in a[:3]))
            elif k == "of":
                occ["faces"].append(tuple(int(x) for x in a[:3]))
            elif k == "cobj":
                cobj = {"parent": int(a[0]), "offset": tuple(num(x) for x in a[1:4]) if len(a) >= 4 else (0, 0, 0),
                        "sphere": None, "box": None, "verts": [], "faces": [], "volumes": []}
                sc["cobjs"].append(cobj)
            elif k == "csphere":
                cobj["sphere"] = tuple(num(x) for x in a[:4])
                cobj["box"] = tuple(num(x) for x in a[4:10]) if len(a) >= 10 else None
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


def top_parent(parts, pi):
    """A part's parent as the scene holds it: none (a top part) for the root,
    for a part naming itself or -1, or one the LOD lacks."""
    parent = parts[pi]["parent"]
    return None if pi == 0 or parent == pi or not 0 <= parent < len(parts) else parent


def skin_layout(sc):
    """A skinned model's parts as the scene holds them: (the number of bones,
    the mesh part or None). The mesh part is the part `scene` writes the
    skinned strips on that no bone table names (ArmsG part 37, US01 part 19);
    without one the strips are the root's (Delta04, ArmGlovD)."""
    parts = sc["lods"][0]["parts"] if sc["lods"] else []
    named = {b for lod in sc["lods"] for p in lod["parts"] for s in p["strips"] for b in s["bones"]}
    mesh = [pi for pi, p in enumerate(parts) if pi > 0 and p["strips"] and pi not in named]
    if mesh and mesh[0] == len(parts) - 1:
        return len(parts) - 1, mesh[0]
    return len(parts), None


def weighted_top(sc):
    """One past the highest part any skinned vertex of the model weights."""
    top = 0
    for lod in sc["lods"]:
        for p in lod["parts"]:
            for s in p["strips"]:
                for v in s["verts"]:
                    for part, w in influences(s, v):
                        if w >= rig.WEIGHT_EPS:
                            top = max(top, part + 1)
    return top


def influences(s, v):
    """A skinned vertex's (part, weight) pairs, the implicit fourth included
    (contract C1: slot 3 takes 1 - (w0 + w1 + w2), so a vertex whose stored
    weights are all zero lies wholly on it), summed per part; slots past the
    strip's bone table are left out."""
    table = s["bones"]
    out = {}
    weights = list(v["bw"]) + [max(0.0, 1.0 - sum(v["bw"]))]
    for slot, w in zip(v["bi"], weights):
        if w > 0.0 and slot < len(table):
            out[table[slot]] = out.get(table[slot], 0.0) + w
    return sorted(out.items())


def same_turn(face, ref):
    """Whether a triangle winds as `ref` does (a cyclic turn of its corners)."""
    return face in (ref, (ref[1], ref[2], ref[0]), (ref[2], ref[0], ref[1]))


def back_sides(tris, normal):
    """Which of a mesh's triangles, [(strip corners, strip, material slot,
    merged vertices)], lie on the back of a two-sided sheet: of the triangles
    over one set of vertices in both windings, those wound against their
    corners' stored normals (a scene triangle winds counter-clockwise about
    its outward normal in mission axes); where the normals give no side, the
    first triangle's winding is the front."""
    by_corners = {}
    for i, (_, _, _, merged) in enumerate(tris):
        if len(set(merged)) == 3:
            by_corners.setdefault(frozenset(merged), []).append(i)
    back = set()
    for group in by_corners.values():
        ref = tris[group[0]][3]
        turns = [same_turn(tris[i][3], ref) for i in group]
        if all(turns):
            continue
        corners, s, _, _ = tris[group[0]]
        p = [s["verts"][x]["p"] for x in corners]
        n = [normal(s["verts"][x]) for x in corners]
        e1 = [p[1][k] - p[0][k] for k in range(3)]
        e2 = [p[2][k] - p[0][k] for k in range(3)]
        cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        facing = sum(cross[k] * (n[0][k] + n[1][k] + n[2][k]) for k in range(3)) >= 0.0
        back.update(i for i, same in zip(group, turns) if same != facing)
    return back


# A section's bounds are stored on the 16.16 grid: the builder rounds a hit
# sphere's centre to it and truncates its radius and box
# (formats/threedi/threedi_build.cpp).
Q16 = 65536.0


def on_grid(stored, derived, rounded=False):
    """Whether stored 16.16 values (read back exactly) are the ones the
    builder writes for `derived` values (rounded, else truncated), within one
    step: the float noise of the vertices they come from can move a
    truncation a whole step."""
    q = round if rounded else int
    return all(abs(round(a * Q16) - q(b * Q16)) <= 1 for a, b in zip(stored, derived))


def pair(read):
    """Which skinned model of an import deforms with which rigid model's rig:
    {skinned index: (gun index, worst pivot distance)}. A skinned model pairs
    with the rigid model whose first parts are its bones (the same parents,
    pivots within rig.PAIR_TOLERANCE, best fit first), as retail draws a gun's
    arms with the gun's part matrices; only when its weights reach its last
    bone (a shared rig gives the arms exactly those parts) and the gun has at
    least as many parts as the arms."""
    out = {}
    guns = [(i, sc) for i, (_, sc, _, _) in enumerate(read) if sc is not None and not sc["skinned"] and sc["lods"]]
    for i, (_, sc, _, _) in enumerate(read):
        if sc is None or not sc["skinned"] or not sc["lods"]:
            continue
        bones, mesh_part = skin_layout(sc)
        if bones == 0 or weighted_top(sc) != bones:
            continue
        arms = sc["lods"][0]["parts"]
        need = bones + (1 if mesh_part is not None else 0)
        fits = []
        for g, gun in guns:
            parts = gun["lods"][0]["parts"]
            if len(parts) < need:
                continue
            if any(top_parent(arms, k) != top_parent(parts, k) for k in range(bones)):
                continue
            worst = max(math.dist(arms[k]["pivot"], parts[k]["pivot"]) for k in range(bones))
            if worst <= rig.PAIR_TOLERANCE:
                fits.append((worst, g))
        if fits:
            worst, g = min(fits)
            out[i] = (g, worst)
    return out


# --- scene building ------------------------------------------------------------

class Builder(Notes):
    """One file's model, built into the scene. `rigged`: a first-person gun
    imported with its arms, whose LOD 0 parts become a rig."""

    def __init__(self, context, sc, source_path, op, rigged=False):
        self.context = context
        self.sc = sc
        self.stem = os.path.splitext(os.path.basename(source_path))[0]
        self.op = op
        self.rigged = rigged
        self.notes = []
        self.images = {}
        self.collection = None
        self.made = []           # data-blocks a failed build removes
        self.made_materials = []
        self.world = {}          # object name -> its matrix as Blender holds it
        self.blender = blender_axes()  # mission -> Blender, the export's axis map inverted
        self.model = None
        self.roots = []          # LOD roots
        self.rig = None          # the model's rig
        self.part_empties = {}   # LOD -> {part: its PN## empty}
        self.mesh_part = None    # a skinned model's mesh part

    def discard(self):
        """Remove what a failed build made."""
        if self.collection is not None:
            for ob in list(self.collection.objects):
                bpy.data.objects.remove(ob)
            bpy.data.collections.remove(self.collection)
        for mat in self.made_materials:
            bpy.data.materials.remove(mat)
        for block in self.made:
            if isinstance(block, bpy.types.Armature):
                bpy.data.armatures.remove(block)

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

    def put(self, ob, li, part, world=None):
        """Put `ob` on part `part` of LOD li at `world`: under the part's PN##
        empty, hung from its bone, or under the LOD root when the LOD holds no
        bones of its own (a later LOD of a skinned model, arms on a gun's rig;
        the object's name then names its part); part -1 is no part, under the
        LOD root."""
        world = world if world is not None else Matrix.Identity(4)
        empties = self.part_empties.get(li)
        if part >= 0 and empties is not None:
            return self.link(ob, empties[part], world)
        if part >= 0 and li == 0 and self.rig is not None:
            self.collection.objects.link(ob)
            rig.hang(ob, self.rig, self.rig.data.bones[f"BN{part + 1:02d}"], world)
            self.world[ob.name] = world
            return ob
        return self.link(ob, self.roots[li], world)

    # --- meshes ---------------------------------------------------------------
    def mesh(self, name, strips, origin, mats, skinned):
        """One mesh from strips, its vertices relative to `origin` (Blender
        axes): vertices merged by position, normal (and weights), loops
        carrying UVMap/UV1 and the stored normals. A triangle whose corners
        another takes in the opposite winding (a two-sided sheet stored as
        both windings over one set of vertices: Baricd02's wire) goes on
        vertices of the side it faces, so that each side is a sheet of its
        own: Blender holds a custom normal relative to the smooth fan around
        its corner, and a fan holding two faces back to back has no
        direction to hold it in. Returns the mesh and each vertex's (part,
        weight) influences."""
        index, verts, loops, faces, face_mat, slots = {}, [], [], [], [], []
        weights = []
        dropped = 0

        def normal(v):
            # Zero-length and not-finite (J_bsh1's NaN) normals alike: Blender
            # calculates the corner's own.
            n = v["n"]
            if not finite(n) or sum(x * x for x in n) < 1e-12:
                self.note("zero-length or not-finite vertex normals are replaced by Blender's calculated normals")
                return (0.0, 0.0, 0.0)
            return n

        tris = []  # (the strip's corners, the strip, material slot, the merged vertices)
        for s in strips:
            mi = s["material"]
            if mi not in slots:
                slots.append(mi)
            ids = []
            for v in s["verts"]:
                key = (tuple(round(x, 6) for x in v["p"]), tuple(round(x, 4) for x in normal(v)))
                infl = ()
                if skinned:
                    infl = tuple((b, round(w, 6)) for b, w in influences(s, v))
                    table = len(s["bones"])
                    dropped += sum(1 for slot, w in zip(v["bi"], list(v["bw"]) + [1.0 - sum(v["bw"])])
                                   if w > 1e-6 and slot >= table)
                    key += (infl,)
                if key not in index:
                    index[key] = len(verts)
                    verts.append(self.blender(v["p"]) - origin)
                    weights.append(infl)
                ids.append(index[key])
            for a, b, c in s["tris"]:
                tris.append(((a, b, c), s, slots.index(mi), (ids[a], ids[b], ids[c])))
        back = back_sides(tris, normal)
        second = {}  # a vertex -> its copy on the back side
        for i, (corners, s, slot, merged) in enumerate(tris):
            face = list(merged)
            if len(set(face)) < 3:
                # Corners the merge collapsed (a sliver): keep the face on
                # vertices of its own so its triangle survives.
                for k, x in enumerate(corners):
                    face[k] = len(verts)
                    verts.append(self.blender(s["verts"][x]["p"]) - origin)
                    weights.append(weights[merged[k]])
            elif i in back:
                for k, vi in enumerate(face):
                    if vi not in second:
                        second[vi] = len(verts)
                        verts.append(verts[vi].copy())
                        weights.append(weights[vi])
                    face[k] = second[vi]
            faces.append(tuple(face))
            face_mat.append(slot)
            loops.extend(s["verts"][x] for x in corners)
        if dropped:
            self.note(f"{dropped} weights on bone-table slots past their strip's table (retail FSldr03 ships them) are "
                      "not kept")
        me = bpy.data.meshes.new(name)
        me.from_pydata([tuple(v) for v in verts], [], faces)
        for mi in slots:
            me.materials.append(mats[mi] if 0 <= mi < len(mats) else None)
        me.polygons.foreach_set("material_index", face_mat)
        me.polygons.foreach_set("use_smooth", [True] * len(faces))
        # D3D texture space runs v down; Blender's runs up. One bulk write per
        # map: a write per loop through the legacy accessor is quadratic.
        me.uv_layers.new(name="UVMap").data.foreach_set(
            "uv", [x for v in loops for x in (v["uv"][0], 1.0 - v["uv"][1])])
        if self.sc["uv1"]:
            me.uv_layers.new(name="UV1").data.foreach_set(
                "uv", [x for v in loops for x in (v["uv1"][0], 1.0 - v["uv1"][1])])
        me.normals_split_custom_set([tuple(self.blender(normal(v)).normalized()) for v in loops])
        me.update()
        return me, weights

    def groups(self, ob, weights):
        """The influences as vertex groups named after the rig's bones, one
        call per group and weight."""
        by = {}
        for vi, infl in enumerate(weights):
            for part, w in infl:
                by.setdefault((part, w), []).append(vi)
        made = {}
        for (part, w), verts in sorted(by.items()):
            g = made.get(part)
            if g is None:
                g = made[part] = ob.vertex_groups.new(name=self.rig.data.bones[f"BN{part + 1:02d}"].name)
            g.add(verts, w, "REPLACE")

    # --- the model --------------------------------------------------------------
    def build(self, scene):
        sc = self.sc
        self.scene = scene
        self.collection = bpy.data.collections.new(sc["name"])
        scene.collection.children.link(self.collection)
        mats = materials.import_materials(self)
        # The model root: one .3di, its LOD roots below it. It holds the
        # model's own settings, so several models share a scene.
        self.model = self.empty(sc["name"], size=1.0, display="CUBE")
        self.model.o3d.model_name = sc["name"]
        self.model.o3d.output_path = self.output_path(scene)
        self.model.o3d.export_bullet_faces = any(c["faces"] for c in sc["cobjs"])
        for li, lod in enumerate(sc["lods"]):
            root = self.empty(f"{sc['name']}_LOD{li}", self.model, size=0.5, display="ARROWS")
            root["_lod_index"] = li
            root.o3d.lod_threshold = lod["threshold"]
            root.o3d.lod_type = lod["type"]
            self.roots.append(root)
        lod_objects = [[r] for r in self.roots]
        for li, lod in enumerate(sc["lods"]):
            if not lod["parts"]:
                self.note(f"LOD {li} has no parts")
            elif sc["skinned"]:
                lod_objects[li] += self.skinned_lod(li, lod, mats)
            elif li == 0 and self.rigged:
                lod_objects[li] += self.rig_lod(lod, mats)
            else:
                lod_objects[li] += self.rigid_lod(li, lod, mats)
        materials.keep_unused(self, mats, lod_objects)
        self.points(lod_objects)
        if self.op is None or self.op.import_lights:
            self.lights(lod_objects)
        if self.op is None or self.op.import_occlusion:
            self.occlusion(lod_objects)
        if self.op is None or self.op.import_collision:
            self.collision(lod_objects)
        self.hit_spheres(lod_objects)
        self.bullet_lod(mats)
        self.attach_points(lod_objects)
        vl = self.context.view_layer
        for li, objs in enumerate(lod_objects):
            if li > 0:
                for ob in objs:
                    ob.hide_set(True, view_layer=vl)

    def output_path(self, scene):
        """The file the model writes: the imported file's name beside the
        .blend (Excavatr.3di's GHDR name is OrngFlag), a number added when
        another model already writes it."""
        taken = {export.output_path(m).lower() for m in rig.model_roots(scene)}
        output = f"//{self.stem}.3di"
        n = 2
        while bpy.path.abspath(output).lower() in taken:
            output = f"//{self.stem}_{n}.3di"
            n += 1
        return output

    def frame_rotation(self, index):
        """A PANM row's MTRX frame (mission axes, row-major, p' = p R) as a
        Blender rotation: the part empty's orientation (column form R^T)."""
        if index <= 0 or index > len(self.sc["frames"]):
            return Matrix.Identity(3)
        r = self.sc["frames"][index - 1]
        q = Matrix(((r[0], r[3], r[6]), (r[1], r[4], r[7]), (r[2], r[5], r[8])))
        b = axis_basis()
        return b @ q @ b.transposed()

    def parent_notes(self, li, parts):
        for pi, part in enumerate(parts):
            if part["parent"] >= len(parts):
                self.note(f"LOD {li} part {pi + 1:02d} names part {part['parent'] + 1:02d} as its parent, which the "
                          "LOD lacks; it exports as a top part (parent 01)")

    @staticmethod
    def store_parent(holder, parts, pi):
        """A part that names itself or no part as its parent, which a
        hierarchy cannot say: its Parent setting (rig.stored_parent)."""
        parent = parts[pi]["parent"]
        if pi > 0 and parent == pi:
            holder.part_parent = "SELF"
        elif parent < 0:
            holder.part_parent = "NONE"

    def rigid_lod(self, li, lod, mats):
        objs = []
        parts = []
        rows = {p["part"]: p for p in lod["panm"]}
        self.parent_notes(li, lod["parts"])
        root = self.roots[li]
        for pi, part in enumerate(lod["parts"]):
            pivot = self.blender(part["pivot"])
            row = rows.get(pi)
            rot = self.frame_rotation(row["matrix"]) if row else Matrix.Identity(3)
            world = Matrix.Translation(pivot) @ rot.to_4x4()
            parent = top_parent(lod["parts"], pi)
            parent_ob = parts[parent] if parent is not None and parent < len(parts) else root
            ob = self.empty(f"PN{pi + 1:02d}", parent_ob, world, part=True)
            self.store_parent(ob.o3d, lod["parts"], pi)
            parts.append(ob)
            objs.append(ob)
            if row is not None:
                self.tracks(ob.o3d, row)
        for pi, part in enumerate(lod["parts"]):
            parent = top_parent(lod["parts"], pi)
            if parent is not None and parent > pi:
                # A parent listed after its child (17 retail models).
                ob = parts[pi]
                above = self.world[parts[parent].name]
                ob.parent = parts[parent]
                ob.matrix_parent_inverse = Matrix.Identity(4)
                ob.matrix_basis = above.inverted() @ self.world[ob.name]
                self.world[ob.name] = above @ ob.matrix_basis
        self.part_empties[li] = dict(enumerate(parts))
        objs += self.part_geometry(li, lod, mats)
        return objs

    def part_geometry(self, li, lod, mats):
        """A rigid LOD's part meshes and the centre helpers of the parts that
        draw nothing, on their parts."""
        objs = []
        for pi, part in enumerate(lod["parts"]):
            pivot = self.blender(part["pivot"])
            if part["strips"]:
                me, _ = self.mesh(f"{pi + 1:02d} Mesh", part["strips"], pivot, mats, False)
                objs.append(self.put(bpy.data.objects.new(me.name, me), li, pi, Matrix.Translation(pivot)))
            elif part["centre"] is not None:
                objs.append(self.centre_helper(li, pi, part["centre"]))
        return objs

    def centre_helper(self, li, pi, centre):
        """Part pi's `_## center` helper mesh, for a part that draws nothing:
        OED seeded such a part with its helper's first vertex, the point its
        bounds sit on at radius 0 (5fc5b4f6a^ engine/formats/oed/
        convert_internal.cpp, the placeholder injection), which retail keeps
        near the pivot. It stands on the part's pivot, its one vertex that
        point."""
        pivot = self.blender(self.sc["lods"][li]["parts"][pi]["pivot"])
        me = bpy.data.meshes.new(f"_{pi + 1:02d} center")
        me.from_pydata([tuple(self.blender(centre) - pivot)], [], [])
        return self.put(bpy.data.objects.new(me.name, me), li, pi, Matrix.Translation(pivot))

    def armature(self, parts, count):
        """The model's rig under LOD 0's root: a BN## bone per part up to
        `count`, its head at the part's pivot, in the part hierarchy."""
        data = bpy.data.armatures.new(f"{self.sc['name']} Rig")
        self.made.append(data)
        arm = bpy.data.objects.new(data.name, data)
        self.link(arm, self.roots[0])
        names = {pi: f"BN{pi + 1:02d}" for pi in range(count)}
        heads = {pi: self.blender(parts[pi]["pivot"]) for pi in range(count)}
        parents = {pi: top_parent(parts, pi) for pi in range(count)}
        parents = {pi: (p if p is not None and p < count else None) for pi, p in parents.items()}
        with rig.editing(self.context, arm) as edit_bones:
            rig.lay_bones(edit_bones, names, heads, parents)
        for pi in range(count):
            self.store_parent(data.bones[names[pi]].o3d, parts, pi)
        self.rig = arm
        return arm

    def bone_rows(self, lod, count):
        """Each bone's part animation: its tracks, flags and track frame."""
        rows = {p["part"]: p for p in lod["panm"]}
        for pi in range(count):
            row = rows.get(pi)
            if row is None:
                continue
            p = self.rig.data.bones[f"BN{pi + 1:02d}"].o3d
            p.frame = self.frame_rotation(row["matrix"])
            self.tracks(p, row)

    def rig_lod(self, lod, mats):
        """A first-person gun's LOD 0 as a rig: a bone per part, the parts'
        meshes and helpers hung from their bones."""
        count = len(lod["parts"])
        self.parent_notes(0, lod["parts"])
        arm = self.armature(lod["parts"], count)
        self.bone_rows(lod, count)
        return [arm] + self.part_geometry(0, lod, mats)

    def skinned_lod(self, li, lod, mats):
        """A skinned model's LOD: its rig (LOD 0 of a model that owns one), and
        its skinned strips as one mesh deforming with the rig, on the mesh part
        (the mesh's origin its pivot) when the file keeps one."""
        objs = []
        bones, mesh_part = skin_layout(self.sc)
        if li == 0:
            if mesh_part is not None:
                self.mesh_part = mesh_part
                self.model.o3d.mesh_part = True
            self.parent_notes(0, lod["parts"][:bones])
            objs.append(self.armature(lod["parts"], bones))
            self.bone_rows(lod, bones)
        elif len(lod["parts"]) != len(self.sc["lods"][0]["parts"]):
            self.note(f"LOD {li} has {len(lod['parts'])} parts, LOD 0 {len(self.sc['lods'][0]['parts'])}; every LOD "
                      "of a skinned model reads its parts from the one rig")
        strips = [s for p in lod["parts"] for s in p["strips"]]
        on = [pi for pi, p in enumerate(lod["parts"]) if p["strips"]]
        authored = self.mesh_part if self.mesh_part is not None else 0
        if any(pi != authored for pi in on):
            self.note(f"LOD {li}: skinned geometry authored on its bones (dM1A1, DT801, Ftruck1X) is authored on one "
                      "part in a scene; each part's own bounds are not kept")
        if not strips:
            return objs
        pivot = self.blender(lod["parts"][authored]["pivot"]) if authored < len(lod["parts"]) else Vector()
        name = f"{self.sc['name']} Skin" + (f" LOD{li}" if li else "")
        me, weights = self.mesh(name, strips, pivot, mats, True)
        ob = bpy.data.objects.new(name, me)
        world = Matrix.Translation(pivot)
        if li == 0:
            self.link(ob, self.rig, world)
        else:
            self.link(ob, self.roots[li], world)
        self.groups(ob, weights)
        ob.modifiers.new("Armature", "ARMATURE").object = self.rig
        objs.append(ob)
        return objs

    def tracks(self, p, row):
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

    # --- points, lights, occlusion, collision ---------------------------------
    def section_part(self, index):
        """The LOD 0 part a record of part `index` sits on (the root when the
        LOD lacks it)."""
        count = len(self.sc["lods"][0]["parts"]) if self.sc["lods"] else 0
        return index if 0 <= index < count else 0

    def points(self, lod_objects):
        for order, u in enumerate(self.sc["points"]):
            letter = chr(u["type"]) if 65 <= u["type"] <= 90 else "G"
            if letter != chr(u["type"]):
                self.note(f"user point {u['name']}: type {u['type']} has no letter; written as G")
            label = u["name"] if u["name"] else "Noname"
            part = u["part"] if u["part"] >= 0 else -1
            if part >= 0 and part != self.section_part(part):
                self.note(f"user point {u['name']}: its part {part} is past the model's parts; it sits on the root")
                part = 0
            name = f"UP{letter}{part + 1:02d} {label}"
            d = self.blender(u["d"])
            rot = d.to_track_quat("Z", "Y").to_matrix() if d.length > 1e-9 else Matrix.Identity(3)
            world = Matrix.Translation(self.blender(u["p"])) @ rot.to_4x4()
            ob = bpy.data.objects.new(name, None)
            ob.empty_display_type = "SINGLE_ARROW"
            ob.empty_display_size = 0.15
            ob.o3d.order = order
            lod_objects[0].append(self.put(ob, 0, part, world))

    def lights(self, lod_objects):
        dups = {}
        for i, l in enumerate(self.sc["lights"]):
            spot = bool(l["flags"] & 0x08)
            part = self.section_part(max(0, l["part"]))
            data = bpy.data.lights.new(f"LP{part + 1:02d}", "SPOT" if spot else "POINT")
            data.color = [c / 255.0 for c in l["rgb0"]]
            data.energy = 50.0
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
            ob = bpy.data.objects.new(f"LP{part + 1:02d}" + dup_suffix(dups, part), data)
            ob.o3d.order = i
            world = Matrix.Translation(self.blender(l["p"])) @ rot.to_4x4()
            lod_objects[0].append(self.put(ob, 0, part, world))

    def occlusion(self, lod_objects):
        dups = {}
        for i, o in enumerate(self.sc["occ"]):
            prefix = OCC_PREFIX.get(o["type"])
            if prefix is None:
                self.note(f"occlusion record {i}: type {o['type']} has no name form")
                continue
            section = self.section_part(o["a"])
            key = (prefix, section, o["type"] == 3 and o["b"])
            name = f"{prefix}{section + 1:02d}" + dup_suffix(dups, key)
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
            ob.o3d.order = i
            lod_objects[0].append(self.put(ob, 0, section))
            if o["sphere"] is not None and not finite(o["sphere"]):
                self.note(f"occlusion record {i}: its stored sphere is not finite; it exports with the one its mesh "
                          "gives")
            elif o["sphere"] is not None:
                # The record's sphere as the file stores it, which is not the
                # one export derives from the mesh (206 retail models mirror
                # its centre across y): a `_sphere` Empty on the mesh keeps it.
                centre, radius = self.blender(o["sphere"][:3]), o["sphere"][3]
                sphere = self.empty("_sphere", ob, Matrix.Translation(centre), size=radius, display="SPHERE")
                sphere.hide_set(True)
                lod_objects[0].append(sphere)

    def collision(self, lod_objects):
        dups = {}
        unbuilt = 0
        for si, c in enumerate(self.sc["cobjs"]):
            part = self.section_part(si)
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
                name = f"{code}{letters}{part + 1:02d}" + dup_suffix(dups, (code + letters, part)) + "-colonly"
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
                lod_objects[0].append(self.put(ob, 0, part))
        if unbuilt:
            self.note(f"{unbuilt} collision volume planes touch their volume in no face of 1 cm2 or more (or not at "
                      "all); export derives planes from faces, so it does not rebuild them")

    def moved_points(self):
        """The LOD 0 vertices each part's bone moves (mission axes), as export
        gathers them from the mesh import makes of them: every part a
        vertex's weights give rig.WEIGHT_EPS or more."""
        points = {}
        for p in self.sc["lods"][0]["parts"] if self.sc["lods"] else ():
            for s in p["strips"]:
                for v in s["verts"]:
                    for part, w in influences(s, v):
                        if round(w, 6) >= rig.WEIGHT_EPS:
                            points.setdefault(part, []).append(v["p"])
        return points

    def hit_spheres(self, lod_objects):
        """A skinned model's bone sections (those without collision geometry)
        as export writes them back: a section whose stored sphere is not the
        one export derives from the LOD 0 vertices its bone moves
        (export.bone_bounds, compared on the file's 16.16 grid) gets a
        `_## hit` sphere Empty on the bone (its origin the centre, its scale
        the radius), and one whose box is not the derived one (with no
        vertices, the sphere's cube) a `_## bounds` box Empty (its origin the
        middle, its scale the half extents), both hidden; a section that
        stores none although its bone moves vertices turns the bone's Hit
        sphere off. A model this export wrote imports with none of them."""
        if not self.sc["skinned"]:
            return
        points = self.moved_points()
        for si, c in enumerate(self.sc["cobjs"]):
            if c["verts"]:
                continue
            derived = export.bone_bounds(points[si]) if si in points else None
            if c["sphere"] is None:
                bone = self.rig.data.bones.get(f"BN{si + 1:02d}") if self.rig is not None else None
                if derived is not None and bone is not None:
                    bone.o3d.hit_sphere = False
                continue
            part = self.section_part(si)
            centre, radius = c["sphere"][:3], c["sphere"][3]
            if derived is None or not on_grid(centre, derived[0], True) or not on_grid([radius], [derived[1]]):
                ob = bpy.data.objects.new(f"_{si + 1:02d} hit", None)
                ob.empty_display_type = "SPHERE"
                ob.empty_display_size = 1.0
                world = Matrix.Translation(self.blender(centre)) @ Matrix.Scale(radius, 4)
                lod_objects[0].append(self.put(ob, 0, part, world))
                ob.hide_set(True)
            cube = [x - radius for x in centre] + [x + radius for x in centre]
            if c["box"] is not None and not on_grid(c["box"], derived[2] if derived is not None else cube):
                lo, hi = self.blender(c["box"][:3]), self.blender(c["box"][3:6])
                box = bpy.data.objects.new(f"_{si + 1:02d} bounds", None)
                box.empty_display_type = "CUBE"
                box.empty_display_size = 1.0
                half = [abs(hi[k] - lo[k]) * 0.5 for k in range(3)]
                world = Matrix.Translation((lo + hi) * 0.5) @ Matrix.Diagonal((*half, 1.0))
                lod_objects[0].append(self.put(box, 0, part, world))
                box.hide_set(True)

    def attach_points(self, lod_objects):
        """The CXLT attach points: retail stores one per part after the root
        on a rigid model and one per part on a skinned one (the builder's
        derivation, formats/threedi/threedi_build.cpp), OED's WriteCXLT source
        (5fc5b4f6a^ engine/formats/oed/export_3di.cpp). A row that is its
        section's own offset, the row the builder derives, needs no helper;
        any other is an `_## attach` Empty on its part in the collision LOD.
        A table of another count or none (156 JOTAC models: M24_1st's 42 rows
        for 42 sections, Chair03X's none for seven) sets the model's Attach
        points to the attach helpers: an Empty per row, in its order, on the
        part whose section offset it is (else the root)."""
        rows = self.sc["cxlt"]
        li = self.model.o3d.poly_collision_lod
        parts = self.sc["lods"][li]["parts"] if li < len(self.sc["lods"]) else []
        first = 0 if self.sc["skinned"] else 1
        cobjs = self.sc["cobjs"]

        def attach(i, pi, row):
            ob = bpy.data.objects.new(f"_{pi + 1:02d} attach", None)
            ob.empty_display_type = "PLAIN_AXES"
            ob.empty_display_size = 0.05
            ob.o3d.order = i
            lod_objects[li].append(self.put(ob, li, pi, Matrix.Translation(self.blender(row))))

        def at_offset(row, offset):
            return all(round(a * 65536.0) == round(b * 65536.0) for a, b in zip(row, offset))

        if not rows and not self.sc["cxlt_given"]:
            return
        if rows and len(rows) == len(parts) - first:
            for i, row in enumerate(rows):
                pi = i + first
                if pi < len(cobjs) and at_offset(row, cobjs[pi]["offset"]):
                    continue  # the same 16.16 row as the section's offset
                attach(i, pi, row)
            return
        self.model.o3d.attach_points = "HELPERS"
        for i, row in enumerate(rows):
            pi = next((si for si, c in enumerate(cobjs) if si < len(parts) and at_offset(row, c["offset"])), 0)
            attach(i, pi, row)

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
        chosen = None
        for li, lod in enumerate(self.sc["lods"]):
            if len(lod["parts"]) == len(counts) and \
                    [sum(len(s["tris"]) for s in p["strips"]) for p in lod["parts"]] == counts:
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
            if bool(flags & 1) != materials.two_sided(mats[mi]):
                # Its faces' "both sides" is not its drawing's (154 JOTAC
                # models: Baricd02's two-sided wire stores its faces one-sided).
                p.face_both_sides = "YES" if flags & 1 else "NO"
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
                for tri in s["tris"]:
                    section_tris.setdefault(pi, []).append((s["material"], [s["verts"][x]["p"] for x in tri]))
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


def import_files(context, paths, op=None):
    """Import .3di files into the current scene, each under a model root of
    its own; a first-person gun and the arms imported with it share the gun's
    rig (pair, rig.share_rig). Returns [(path, model or None, notes or the
    error)], in the order given. A file that fails leaves nothing of itself
    behind and does not stop the others."""
    read = []
    for path in paths:
        try:
            sc, notes = import_text(context, ["scene"], path, "scene.o3d", read_o3d)
            read.append((path, sc, notes, None))
        except Exception as e:  # noqa: BLE001 (reported per file)
            read.append((path, None, None, e))
    pairs = pair(read)
    guns = {g for g, _ in pairs.values()}
    built, results = {}, [None] * len(read)
    for i, (path, sc, notes, error) in enumerate(read):
        if error is not None:
            results[i] = (path, None, error)
            continue
        builder = Builder(context, sc, path, op, rigged=i in guns)
        try:
            builder.build(context.scene)
        except Exception as e:  # noqa: BLE001 (reported per file)
            builder.discard()
            results[i] = (path, None, ImportFailed(str(e)) if isinstance(e, ExportError) else e)
            continue
        built[i] = builder
        results[i] = (path, builder.model, ["not carried: " + n for n in notes] + builder.notes)
    for i, (g, _) in sorted(pairs.items()):
        if i in built and g in built:
            try:
                results[i][2].extend(rig.share_rig(context, built[i].model, built[g].model))
            except ExportError as e:
                results[i][2].append(f"it keeps its own rig: {e}")
    return results


def import_file(context, path, op=None):
    """Import one .3di into the current scene under a model root of its own;
    returns the model root and the notes. A file that fails leaves nothing of
    itself in the scene."""
    _, model, notes = import_files(context, [path], op)[0]
    if model is None:
        raise notes if isinstance(notes, ImportFailed) else ImportFailed(str(notes))
    return model, notes
