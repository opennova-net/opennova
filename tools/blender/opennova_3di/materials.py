# Materials and textures: the `material` records a model's Blender materials
# export as, the texture files written beside its .3di, and the Blender
# material an imported record becomes (docs/threedi/o3d-scene-format.md). The
# shader tags and their capability words are the engine's own table, read from
# `opennova-3di catalog`; the OED rules decide what a shader implies
# (docs/adr/0047-blender-3di-exporter.md, decision 7).

import itertools
import os
import re
import struct
from collections import namedtuple

import bpy
import numpy as np

from . import export
from .o3dtext import CTRL_REFERENCE_THRESHOLD, ExportError, fmt, quoted


# Shader capability bits (runtime/renderer/material_descriptor.h), read per
# tag from `opennova-3di catalog`.
FLAG_EMISSIVE, FLAG_DIFFUSE, FLAG_SECONDARY = 0x1, 0x4, 0x8
FLAG_BLENDING, FLAG_GLASS, FLAG_SKINNED, FLAG_TANGENT = 0x1000, 0x2000, 0x4000, 0x8000
MATERIAL_RE = re.compile(r"^Material_(\d+)_(\S+)$")


# --- the shader table -----------------------------------------------------------

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


def face_flags(mat):
    """A material's bullet-face flags: 1 (both sides) follows Two sided, as
    OED took both from one render attribute (export_3di.cpp material_flags);
    the others are the material's face settings."""
    if mat is None:
        return 0
    p = mat.o3d
    return ((1 if p.two_sided else 0) | (0x100 if p.face_never_hit else 0) |
            (0x800 if p.face_front_only else 0) | (p.face_other_flags & ~0x901))


# --- the node tree -----------------------------------------------------------

def socket(sockets, identifier):
    """A node's socket by its identifier (a Mix node has several inputs named A)."""
    return next(s for s in sockets if s.identifier == identifier)


def links_into(sock, groups=()):
    """The (node, output socket, groups) at the far end of each live link into
    the input `sock`, seen past reroutes, muted nodes and node groups: a group
    node's output continues inside the group at its Group Output's input, a
    Group Input's output outside it at the group node's input. `groups` is the
    stack of group nodes the socket sits in."""
    for link in sock.links:
        if link.is_muted or not link.is_valid:
            continue
        node, out = link.from_node, link.from_socket
        if node.type == "REROUTE":
            yield from links_into(node.inputs[0], groups)
        elif node.mute:
            for internal in node.internal_links:
                if internal.to_socket == out:
                    yield from links_into(internal.from_socket, groups)
        elif node.type == "GROUP":
            tree = node.node_tree
            inner = next((n for n in tree.nodes if n.type == "GROUP_OUTPUT" and n.is_active_output),
                         None) if tree is not None else None
            if inner is not None:
                yield from links_into(socket(inner.inputs, out.identifier), groups + (node,))
        elif node.type == "GROUP_INPUT":
            if groups:
                yield from links_into(socket(groups[-1].inputs, out.identifier), groups[:-1])
        else:
            yield node, out, groups


def principled(mat):
    """The Principled BSDF that draws a material: the one its active Material
    Output's Surface takes, else the tree's first; None without one."""
    tree = mat.node_tree if mat is not None else None
    if tree is None:
        return None
    output = next((n for n in tree.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    if output is not None:
        for node, _, groups in links_into(output.inputs["Surface"]):
            if node.type == "BSDF_PRINCIPLED" and not groups:
                return node
    return next((n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"), None)


def base_color(mat):
    """What draws a material's Base Color: (images, others), the image nodes
    whose Color feeds its Principled BSDF's Base Color, through reroutes,
    groups, Mix nodes and any other colour node, each as (node, groups); and
    the other nodes on the way (colour adjustments, procedural textures,
    attributes), which the game does not draw. Only a Mix node's colours are
    followed, not its factor, and not the colour a Mix's constant factor
    leaves out (0 takes A, 1 takes B)."""
    bsdf = principled(mat)
    images, others = [], []
    if bsdf is None:
        return images, others
    stack = list(links_into(bsdf.inputs["Base Color"]))
    seen = set()
    while stack:
        node, out, groups = stack.pop()
        key = (node.as_pointer(), tuple(g.as_pointer() for g in groups))
        if key in seen:
            continue
        seen.add(key)
        if node.type == "TEX_IMAGE":
            if out.identifier == "Color" and node.image is not None:
                images.append((node, groups))
            else:
                others.append(node)
            continue
        if node.type == "MIX" and node.data_type == "RGBA":
            factor = socket(node.inputs, "Factor_Float")
            follow = [socket(node.inputs, "A_Color"), socket(node.inputs, "B_Color")]
            if node.blend_type == "MIX" and not factor.is_linked and factor.default_value in (0.0, 1.0):
                follow = [follow[int(factor.default_value)]]
        elif node.type == "MIX_RGB":
            follow = [node.inputs["Color1"], node.inputs["Color2"]]
        elif node.type == "VECT_MATH" and node.operation == "SCALE":
            follow = [node.inputs[0]]  # the imported detail stage doubles the product
        else:
            follow = [i for i in node.inputs if i.enabled and i.type in ("RGBA", "VECTOR")]
            others.append(node)
        for i in follow:
            stack.extend(links_into(i, groups))
    return images, others


def image_uv(node, groups):
    """The UV map an image node samples: ("render", None) for the one Blender
    renders with (its Vector unlinked, a UV Map node naming none, a Texture
    Coordinate node's UV), ("named", name) for a UV Map node's map, or
    ("other", node) for any other coordinates, which the game cannot draw."""
    for src, out, _ in links_into(node.inputs["Vector"], groups):
        if src.type == "UVMAP":
            return ("named", src.uv_map) if src.uv_map else ("render", None)
        if src.type == "TEX_COORD" and out.identifier == "UV":
            return ("render", None)
        return ("other", src)
    return ("render", None)


def node_label(node):
    return node.label or node.name


# --- images -----------------------------------------------------------------

def check_image(image, what):
    """An ExportError when the game cannot draw the image as one texture: a
    UDIM (tiled) image, an image sequence, a movie; only a still image loaded
    from a file or made in Blender (a bake, a painting) can be written."""
    if image.source == "TILED":
        raise ExportError(f"{what}: the image {image.name} is a UDIM (tiled) image; the game draws one image per "
                          "texture: bake its tiles into one image")
    if image.source not in ("FILE", "GENERATED"):
        raise ExportError(f"{what}: the image {image.name} is a {image.source.lower()} image; the game draws one "
                          "still image per texture")


def srgb_encode(linear):
    """The sRGB transfer (IEC 61966-2-1): linear light to the encoded value."""
    linear = np.clip(linear, 0.0, None)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def image_rows(image):
    """The image's RGBA pixels as the game's texture holds them, rows
    bottom-up, in chunks of float64 values 0..1 (clipped when quantized). A
    byte image (an 8-bit PNG, TGA, JPEG) holds its file's values, straight: as
    they are. A float image (a 16-bit PNG or TIFF, an EXR, a float bake)
    holds scene-linear light, premultiplied unless its alpha is channel
    packed or unused (Blender's imb_handle_alpha): its colour is made straight
    and sRGB-encoded, as an 8-bit file holds it, unless it is non-colour data
    (a normal map, a mask), whose values are what they are."""
    w, h = image.size
    if w == 0 or h == 0:
        raise ExportError(f"image {image.name} has no pixels")
    channels = image.channels
    if channels not in (1, 3, 4):
        raise ExportError(f"image {image.name} has {channels} channels")
    colour = image.is_float and not image.colorspace_settings.is_data
    if colour and bpy.data.colorspace.working_space != "Linear Rec.709":
        raise ExportError(f"image {image.name}: the blend file's working colour space is "
                          f"{bpy.data.colorspace.working_space}; export encodes float images as sRGB from Linear "
                          "Rec.709 only (Color Management > Working Space)")
    premultiplied = colour and image.alpha_mode in ("STRAIGHT", "PREMUL")
    # Blender bundles NumPy. Bulk access avoids expanding a 4K image into
    # millions of Python floats; bounded chunks keep conversion memory small.
    px = np.empty(w * h * channels, dtype=np.float32)
    image.pixels.foreach_get(px)
    px = px.reshape(-1, channels)
    if not np.isfinite(px).all():
        raise ExportError(f"image {image.name} has non-finite pixels")
    for start in range(0, len(px), 262144):
        values = px[start:start + 262144].astype(np.float64)
        if channels == 1:
            values = np.repeat(values, 3, axis=1)
        if values.shape[1] == 3:
            values = np.concatenate([values, np.ones((len(values), 1))], axis=1)
        if premultiplied:
            alpha = values[:, 3:4]
            values[:, :3] = np.divide(values[:, :3], alpha, out=np.zeros_like(values[:, :3]), where=alpha > 0.0)
        if colour:
            values[:, :3] = srgb_encode(values[:, :3])
        yield values


def write_tga(image, path):
    """Uncompressed 32-bit truecolor TGA, rows bottom-up (the retail shape),
    of the image as the game draws it (image_rows)."""
    w, h = image.size
    rows = image_rows(image)
    first = next(rows)  # the checks above run before the file is opened
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 8)
    with open(path, "wb") as f:
        f.write(header)
        for values in itertools.chain([first], rows):
            # float64 and rint preserve Python round's ties-to-even result.
            values = np.clip(np.rint(values * 255.0), 0, 255).astype(np.uint8)
            f.write(values[:, [2, 1, 0, 3]].tobytes())


def check_pixels(image, what):
    """An ExportError when the image cannot be written: no pixels, pixels
    that are not finite, a float image under another working colour space
    (image_rows' checks, run before anything is written)."""
    try:
        for _ in image_rows(image):
            pass
    except ExportError as e:
        raise ExportError(f"{what}: {e}") from e


# --- texture names ------------------------------------------------------------

# A texture row's name field holds 16 characters and a NUL (the MTRL row,
# formats/threedi/threedi_3di3.h ThreediMaterialTexture); a file the export
# writes is named in at most 15 bytes, as every texture retail packs is (a
# PFF entry's 16-byte name field, formats/pff/pff.h). The game asks for a
# row's name cut three characters past its first dot ("x.dds.tga" asks for
# "x.dds") [orig: Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B4FA;
# renderer::material_texture_query], so a written file's name has one dot.
ROW_NAME_BYTES, FILE_NAME_BYTES = 16, 15
WRITTEN_EXTENSIONS = (".tga", ".mdt")
# A material holds 24 texture rows (formats/threedi/threedi_3di3.h
# ThreediMaterial, which opennova-3di fills).
MATERIAL_ROWS = 24


def check_row_name(name, what):
    """An ExportError unless `name` is a texture row name opennova-3di takes:
    printable ASCII, at most 16 bytes, a file name without a path."""
    if not name:
        raise ExportError(f"{what}: a texture has no file name")
    if any(not " " <= c <= "~" for c in name):
        raise ExportError(f"{what}: the texture name '{name}' is not printable ASCII")
    if len(name) > ROW_NAME_BYTES:
        raise ExportError(f"{what}: the texture name '{name}' exceeds {ROW_NAME_BYTES} characters (its row's "
                          "field)")
    if any(c in name for c in "/\\:"):
        raise ExportError(f"{what}: the texture name '{name}' holds a path; a texture row names a file")


def check_file_name(name, what):
    """An ExportError unless `name` can name a texture file export writes:
    <stem>.tga or <stem>.mdt, one dot, at most 15 bytes."""
    check_row_name(name, what)
    stem, ext = os.path.splitext(name)
    if len(name) > FILE_NAME_BYTES or "." in stem or ext.lower() not in WRITTEN_EXTENSIONS:
        raise ExportError(f"{what}: export writes the texture '{name}' as a file, named <stem>.tga or <stem>.mdt "
                          f"with one dot and at most {FILE_NAME_BYTES} characters, as retail packs them")


def derived_stem(model, index_digits, lettered):
    """The stem of the texture files a model's materials derive,
    <stem>_<material index>[letter].<ext>: the model name's ASCII letters,
    digits, _ and -, cut to fit 15 bytes."""
    keep = FILE_NAME_BYTES - len(".tga") - len("_") - index_digits - (1 if lettered else 0)
    stem = "".join(c for c in model if c.isascii() and (c.isalnum() or c in "_-"))
    return stem[:keep].strip("_-") or "tex"


class TextureFile:
    """A texture file export writes beside the .3di: an image, as the game
    draws it (image_rows)."""

    def __init__(self, image):
        self.image = image

    def content(self):
        """What the file holds, to tell two files under one name apart."""
        return ("image", self.image.name_full)

    def describe(self):
        return f"the image {self.image.name}"

    def check(self, what):
        check_image(self.image, what)
        check_pixels(self.image, what)

    def write(self, path):
        write_tga(self.image, path)


class Row:
    """A texture row a material exports (the MTRL row: file name, slot, type,
    flags, frame) and the file export writes for it: a TextureFile, or None
    when the file is the author's to supply. A row whose name is derived
    carries its material's export index and its letter until names are
    given."""

    def __init__(self, name, slot, type=0, flags=0, frame=0, file=None, derive=None):
        self.name, self.slot, self.type, self.flags, self.frame = name, slot, type, flags, frame
        self.file = file
        self.derive = derive  # (material index, letter, extension) while the name is to be derived


class TextureRun:
    """The texture names one export run gives, over every model it exports
    (Export Model, Export All Models). The game finds a texture by its name
    alone, so a name holds one file across them: a second, different file
    under a name is refused before its model writes anything."""

    def __init__(self):
        self.names = {}  # name (lower case) -> (model, content, description)

    def claim(self, name, model, content, description, what):
        have = self.names.setdefault(name.lower(), (model, content, description))
        if have[1] != content:
            owner = "this model" if have[0] == model else f"the model {have[0]}"
            raise ExportError(f"{what}: its texture {name} would be {description}, but {owner} names {name} for "
                              f"{have[2]} (the game finds a texture by its name alone): give one another name")


# --- export -----------------------------------------------------------------

# A mesh a material draws on: its object's name, the names of its render UV
# map (UV0) and of the first other one (UV1), None where it lacks them, and
# the area its triangles of that material cover on UV0.
MeshUse = namedtuple("MeshUse", "name render second area")


class ModelMaterials:
    """One model's materials: in the order its strips first use them, then in
    export order (order()), and the texture files they name. `exporter` is the
    model's export: its registers, its notes, whether it is skinned and the
    scene's Write textures setting."""

    def __init__(self, exporter, run):
        self.exporter = exporter
        self.run = run.textures  # the TextureRun of the export run this model is part of
        self.used = []       # Blender materials (None: a mesh without one)
        self.first_use = {}  # material name (None) -> its index in first-use order
        self.meshes = {}     # material name (None) -> [MeshUse]
        self.textures = {}   # file name (lower case) -> (file name, TextureFile) to write

    def index_of(self, mat):
        """The material's first-use index, which a strip carries until
        order()."""
        key = mat.name if mat is not None else None
        if key not in self.first_use:
            self.first_use[key] = len(self.used)
            self.used.append(mat)
        return self.first_use[key]

    def record_uvs(self, ob, ev, mesh, uv0):
        """What an exported mesh's UV maps give the materials it draws with:
        `mesh` is the evaluated object `ev`'s mesh with its loop triangles,
        `uv0` its render UV map's coordinates (two per loop, None without a UV
        map). Export checks a material's textures against them."""
        layers = mesh.uv_layers
        render = next((l for l in layers if l.active_render), layers[0]).name if len(layers) else None
        second = next((l.name for l in layers if l.name != render), None) if render is not None else None
        tris = mesh.loop_triangles
        slots = np.empty(len(tris), dtype=np.int32)
        tris.foreach_get("material_index", slots)
        area = np.zeros(len(tris))
        if uv0 is not None and len(tris):
            loops = np.empty(len(tris) * 3, dtype=np.int32)
            tris.foreach_get("loops", loops)
            corners = np.asarray(uv0, dtype=np.float64).reshape(-1, 2)[loops].reshape(-1, 3, 2)
            e1, e2 = corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]
            area = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0])
        for slot in np.unique(slots):
            mat = export.slot_material(ev, int(slot))
            self.meshes.setdefault(mat.name if mat is not None else None, []).append(
                MeshUse(ob.name, render, second, float(area[slots == slot].sum())))

    def shader(self, mat):
        """The name's tag (Material_<i>_<SHADER>), else the default for the
        material's texture maps (default_shader): its texture entries' diffuse
        and detail slots, else the images its Base Color draws."""
        m = MATERIAL_RE.match(export.clean_name(mat.name)) if mat is not None else None
        if m:
            return m.group(2)
        if mat is None:
            maps = 0
        elif len(mat.o3d.textures) > 0:
            maps = len({t.slot for t in mat.o3d.textures if t.slot in (1, 2)})
        else:
            maps = len({node.image.name for node, _ in base_color(mat)[0]})
        return default_shader(maps, self.exporter.skinned)

    def strip_alpha(self, index):
        """Strips of a blending shader draw in the alpha pass (OED's
        material_alpha: the BLENDING capability bit; FFP_GLASS is one)."""
        mat = self.used[index]
        blending = shader_flags(self.shader(mat)) & FLAG_BLENDING
        return 1 if blending or (mat is not None and mat.o3d.alpha_strips) else 0

    def node_images(self, mat):
        """{slot: image}: the images a material's Base Color draws. The one on
        the render UV map is the diffuse texture (slot 1); one on the second UV
        map is the detail texture (slot 2), which the game multiplies in at
        twice its value on UV1 (FF_MT's Modulate2x). Every other node on the
        way (a colour adjustment, a procedural texture) is noted: the game
        draws the images as they are."""
        images, others = base_color(mat)
        found = {}
        for node, groups in images:
            slot = self.image_slot(mat, node, groups)
            if found.get(slot, node.image) != node.image:
                raise ExportError(f"{mat.name}: the images {found[slot].name} and {node.image.name} both feed its "
                                  f"Base Color on the {('render', 'second')[slot - 1]} UV map; the game draws one "
                                  "diffuse texture (and one detail texture on the second UV map): bake them into one")
            found[slot] = node.image
        if 2 in found and 1 not in found:
            raise ExportError(f"{mat.name}: its Base Color image {found[2].name} reads the second UV map; the game "
                              "draws the diffuse texture on the render UV map (a detail texture on the second "
                              "multiplies it)")
        for node in others:
            self.exporter.note(f"{mat.name}: its Base Color takes '{node_label(node)}', which the game does not draw" +
                               (": bake it into the image" if found else ""))
        return found

    def image_slot(self, mat, node, groups):
        """1 when an image node samples the render UV map of every mesh the
        material draws on, 2 when it samples the second UV map (the exported
        UV1); an ExportError for any other coordinates."""
        kind, value = image_uv(node, groups)
        if kind == "other":
            raise ExportError(f"{mat.name}: the image {node.image.name} reads its coordinates from "
                              f"'{node_label(value)}'; the game samples a UV map as it is: apply any mapping to the "
                              "UV map and read it with a UV Map node or none")
        if kind == "render":
            return 1
        slots = set()
        for use in self.meshes.get(mat.name, []):
            if value not in (use.render, use.second):
                raise ExportError(f"{mat.name}: the image {node.image.name} reads the UV map '{value}', which "
                                  f"{use.name} does not export (the game samples its render UV map and the first "
                                  "other one)")
            slots.add(1 if value == use.render else 2)
        if len(slots) > 1:
            raise ExportError(f"{mat.name}: the UV map '{value}' of the image {node.image.name} is the render UV "
                              "map of some of its meshes and the second of others")
        return slots.pop() if slots else 1

    def check_uvs(self, mat, shader, textured):
        """A mesh drawing a texture needs a UV map; a shader that derives its
        tangents from UV0 (the TANGENT capability) needs one with area."""
        key = mat.name if mat is not None else None
        caps = shader_flags(shader)
        for use in self.meshes.get(key, []):
            if textured and use.render is None:
                raise ExportError(f"{use.name}: it has no UV map, but its material {mat.name} draws a texture: "
                                  "unwrap it")
            if caps & FLAG_TANGENT and use.area <= 1e-12:
                self.exporter.note(f"{use.name}: its UV map has no area under {key or '(no material)'}, whose shader "
                                   f"{shader} derives its tangents from it, so its lighting comes out wrong")

    def order(self):
        """Put the materials in export order, the Material_<i> index, then
        first use; returns each first-use index's export index."""
        self.used.sort(key=lambda m: (int(MATERIAL_RE.match(export.clean_name(m.name)).group(1))
                                      if m is not None and MATERIAL_RE.match(export.clean_name(m.name)) else 1 << 30))
        return {self.first_use[mat.name if mat is not None else None]: new for new, mat in enumerate(self.used)}

    def generator_register(self, style, name, what):
        return self.exporter.register(name, what) if style > CTRL_REFERENCE_THRESHOLD else -1

    @staticmethod
    def generator_phase(style, phase, what):
        """A generator's phase byte (styles up to 112; above, that byte is the
        register index): the writer would clamp one outside it."""
        if style <= CTRL_REFERENCE_THRESHOLD:
            export.fixed(phase, 256.0, 0, 0xFF, what + " phase")

    def emit(self, lines, model):
        """The `material` records, in export order, for the model named
        `model`. Every texture they name is checked first (its name, its
        image and pixels, and against the other models of the run), so
        nothing is written for a model that fails; write_textures() writes
        the files once the model is built."""
        rows = [self.rows(mat) for mat in self.used]
        self.name_rows(model, rows)
        for mat, material_rows in zip(self.used, rows):
            what = mat.name if mat is not None else "(no material)"
            for row in (r for r in material_rows if r.file is not None):
                self.run.claim(row.name, model, row.file.content(), row.file.describe(), what)
                if row.name.lower() not in self.textures:
                    row.file.check(what)
                    self.textures[row.name.lower()] = (row.name, row.file)
        for mat, material_rows in zip(self.used, rows):
            self.emit_material(lines, mat, material_rows)

    def rows(self, mat):
        """A material's texture rows: its texture entries' when it has any,
        else the images its Base Color draws, for the slots its shader
        samples; each checked, derived names not yet given."""
        if mat is None:
            self.check_uvs(mat, self.shader(mat), False)
            return []
        p = mat.o3d
        shader = self.shader(mat)
        caps = shader_flags(shader)
        out = []
        if len(p.textures) > 0:
            for t in p.textures:
                name = t.name.strip()
                check_row_name(name, mat.name)
                file = None
                if t.image is not None and t.write:
                    if os.path.splitext(name)[1].lower() in WRITTEN_EXTENSIONS:
                        check_file_name(name, mat.name)
                        file = TextureFile(t.image)
                    else:
                        self.exporter.note(f"{mat.name}: Write writes .tga and .mdt files only; {name} is not "
                                           "written")
                out.append(Row(name, t.slot, t.type, t.flags, t.frame, file))
        else:
            found = self.node_images(mat)
            named = MATERIAL_RE.match(export.clean_name(mat.name))
            if not found and (caps & FLAG_DIFFUSE or not named):
                self.exporter.note(f"{mat.name}: no image feeds its Base Color, so it exports without a texture" +
                                   ("" if named else f", as {shader} (OED's shader for none)"))
            for slot, image in sorted(found.items()):
                if not caps & (FLAG_DIFFUSE if slot == 1 else FLAG_SECONDARY):
                    self.exporter.note(f"{mat.name}: its shader {shader} draws no "
                                       f"{('diffuse', 'detail')[slot - 1]} texture; the image {image.name} is not "
                                       "exported")
                    continue
                out.append(Row(None, slot, file=TextureFile(image),
                               derive=(self.used.index(mat), ("", "d")[slot - 1], ".tga")))
        if len(out) > MATERIAL_ROWS:
            raise ExportError(f"{mat.name}: {len(out)} texture rows; a material holds {MATERIAL_ROWS}")
        self.check_uvs(mat, shader, bool(out))
        return out

    def name_rows(self, model, rows):
        """Give each derived row its file name, <stem>_<material index>[letter]
        with the model's stem (derived_stem): d marks a detail texture. A file
        the model already derives for one image is named once."""
        derived = [row for material_rows in rows for row in material_rows if row.derive is not None]
        if not derived:
            return
        digits = len(str(max(row.derive[0] for row in derived)))
        stem = derived_stem(model, digits, any(row.derive[1] for row in derived))
        given = {}
        for row in derived:
            index, letter, ext = row.derive
            row.name = given.setdefault(row.file.content(), f"{stem}_{index}{letter}{ext}")
            row.derive = None

    def emit_material(self, lines, mat, rows):
        """One `material` record and what follows it."""
        fixed = export.fixed
        # A mesh without a material draws with the shader a material
        # without textures takes (shader()), its strips' pass included.
        shader = self.shader(mat)
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
            return
        for row in sorted(rows, key=lambda r: r.slot):
            lines.append(f"texture {quoted(row.name)} {row.slot} {row.type} {row.flags} {row.frame}")
        if p.anim_frames or p.anim_type or p.anim_time:
            if p.anim_type not in (0, 1):
                raise ExportError(f"{mat.name}: the flipbook's anim type is {p.anim_type}; it is 0 (time) or 1 "
                                  "(register)")
            if p.anim_type == 1:
                time_or_register = self.exporter.register(p.anim_register, f"{mat.name} texture flipbook")
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

    def write_textures(self, out_dir):
        """Write the model's texture files beside its .3di, when the scene's
        Write textures setting is on; the exporter calls it once the model is
        built, so a model that fails writes none."""
        if not self.exporter.settings.write_textures:
            return
        for name, file in self.textures.values():
            path = os.path.join(out_dir, name)
            try:
                file.write(path)
            except OSError as e:
                raise ExportError(f"could not write the texture {path}: {e}") from e


# --- import -----------------------------------------------------------------

def import_image(builder, name):
    """The Blender image of texture reference `name`, loaded once per import
    from the file `opennova-3di scene` resolved beside the model (None, with a
    note, when it found none)."""
    if name in builder.images:
        return builder.images[name]
    path = builder.sc["texfiles"].get(name)
    img = None
    if not path:
        builder.note(f"texture {name} not found beside the model")
    else:
        try:
            img = bpy.data.images.load(path, check_existing=True)
            img.name = name
            # The game samples colour and alpha independently: a texture's
            # alpha is often a mask a shader reads (specular, bump), not
            # opacity (the arms' camo averages 0.001). Blender's default
            # straight alpha premultiplies on load and loses the colour
            # wherever alpha is near zero; channel-packed keeps both.
            img.alpha_mode = "CHANNEL_PACKED"
            # Blender loads what it cannot decode (PCX, archive-compressed
            # files) as an image without pixels.
            if img.size[0] == 0:
                builder.note(f"texture {name}: Blender cannot read {os.path.basename(path)}")
        except RuntimeError:
            builder.note(f"texture {name}: Blender cannot read {os.path.basename(path)}")
    builder.images[name] = img
    return img


def import_materials(builder):
    """The model's materials as Blender materials, in the file's order."""
    out = []
    reg = builder.sc["registers"]
    alpha_by_material = {}
    for lod in builder.sc["lods"]:
        for part in lod["parts"]:
            for s in part["strips"]:
                alpha_by_material[s["material"]] = alpha_by_material.get(s["material"], False) or s["alpha"]

    def regname(style, index):
        if style > CTRL_REFERENCE_THRESHOLD and 0 <= index < len(reg):
            return reg[index]
        return ""

    for i, m in enumerate(builder.sc["materials"]):
        # The name carries the export index and the shader tag.
        mat = bpy.data.materials.new(f"Material_{i}_{m['shader']}")
        builder.made_materials.append(mat)
        p = mat.o3d
        flags = m["matflags"]
        p.alpha_test = bool(flags & 1)
        p.two_sided = bool(flags & 4)
        p.other_flags = flags & ~5 & 0xFF
        p.alpha_test_value = m["alphatest"]
        if m["alphatest"] and not p.alpha_test:
            builder.note(f"material {i}: an alpha-test value without the alpha-test flag")
        # Glass and emissive follow the shader on export (OED's rule).
        caps = shader_flags(m["shader"])
        if bool(m["glass"]) != bool(caps & FLAG_GLASS and m["reflect"] and any(m["reflect"][:3])):
            builder.note(f"material {i}: glass {m['glass']} is not what its shader {m['shader']} gives")
        if m["emissive"] != (2 if caps & FLAG_EMISSIVE else 0):
            builder.note(f"material {i}: emissive {m['emissive']} is not what its shader {m['shader']} gives")
        if m["reflect"]:
            p.reflect = [c / 255.0 for c in m["reflect"]]
        blended = bool(shader_flags(m["shader"]) & FLAG_BLENDING)
        p.alpha_strips = alpha_by_material.get(i, False) and not blended
        for name, slot, typ, tflags, frame in m["textures"]:
            t = p.textures.add()
            t.name = name
            t.slot, t.type, t.flags, t.frame = slot, typ, tflags, frame
            t.image = import_image(builder, name)
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
        shade(builder, mat, m, blended)
        out.append(mat)
    return out


def shade(builder, mat, m, blended):
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
    if 1 in by_slot and builder.images.get(by_slot[1]) is not None:
        tex = nodes.new("ShaderNodeTexImage")
        tex.image = builder.images[by_slot[1]]
        tex.location = (-600, 300)
        color = tex.outputs["Color"]
        if blended or mat.o3d.alpha_test:
            links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
            if hasattr(mat, "surface_render_method"):
                mat.surface_render_method = "BLENDED" if blended else "DITHERED"
    if 2 in by_slot and builder.images.get(by_slot[2]) is not None and color is not None:
        uv = nodes.new("ShaderNodeUVMap")
        uv.uv_map = "UV1"
        uv.location = (-900, -100)
        detail = nodes.new("ShaderNodeTexImage")
        detail.image = builder.images[by_slot[2]]
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
        if m["emissive"] or shader_flags(m["shader"]) & FLAG_EMISSIVE:
            links.new(color, bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = 1.0
    if m["glass"] or shader_flags(m["shader"]) & FLAG_GLASS:
        bsdf.inputs["Base Color"].default_value = (0.6, 0.75, 0.85, 1.0)
        bsdf.inputs["Alpha"].default_value = 0.35
        if hasattr(mat, "surface_render_method"):
            mat.surface_render_method = "BLENDED"
