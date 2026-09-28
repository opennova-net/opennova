# Materials and textures: the `material` records a model's Blender materials
# export as, the texture files written beside its .3di, and the Blender
# material an imported record becomes (docs/threedi/o3d-scene-format.md). The
# shader tags and their capability words are the engine's own table, read from
# `opennova-3di catalog`; the OED rules decide what a shader implies
# (docs/adr/0047-blender-3di-exporter.md, decision 7).
#
# A material is authored the way Blender draws it, through its Principled BSDF:
#   Base Color        the image feeding it (through reroutes, groups, Mix and
#                     colour nodes) on the render UV map is the diffuse
#                     texture (slot 1); one on the second UV map is the detail
#                     texture (slot 2), which the game multiplies in at twice
#                     its value (FF_MT's Modulate2x). Without an image, its
#                     colour is written as a small swatch texture.
#   Alpha             a Math node Greater Than (Less Than: inverted) with a
#                     constant threshold is the engine's alpha test.
#   Normal            a Normal Map node (tangent space) with an image is the
#                     normal map (slot 3, type 4, an .mdt file), written with
#                     the game's green.
#   Emission          on: the shader export picks glows (a *_LUM one).
#   Backface Culling  off: two-sided (material flag 4; bullets hit the faces
#                     from both sides unless its Both sides setting says No).
#   Render Method     Blended: the strips draw in the alpha pass, and the
#                     shader export picks blends.
# The Shader property names the engine shader; left empty, export picks one
# (automatic_shader). An image loaded unchanged from a file the game reads is
# that file: its row names it and export copies it beside the .3di; any other
# image is written as a TGA named after the model. The texture list carries
# only what the nodes cannot say (flipbook frames, row flags, a normal map
# file, a file Blender cannot open); a slot it lists is taken from it.

import itertools
import os
import shutil
import struct
from collections import namedtuple

import bpy
import numpy as np

from . import export
from .o3dtext import CTRL_REFERENCE_THRESHOLD, ExportError, fmt, quoted


# Shader capability bits (runtime/renderer/material_descriptor.h
# MATERIAL_FLAG_*, probed per technique by the effect loader [orig:
# HLSLEffect_LoadFromFile @ 0x5ae690]), read per tag from `opennova-3di
# catalog`: TexNormal1 (the slot 3 texture) is FLAG_NORMAL, the TANGENT input
# semantic FLAG_TANGENT, the #UV twins' UV transform FLAG_UVGEN.
FLAG_EMISSIVE, FLAG_ALPHA, FLAG_DIFFUSE, FLAG_SECONDARY, FLAG_NORMAL = 0x1, 0x2, 0x4, 0x8, 0x10
FLAG_NORMAL_B = 0x20
FLAG_BLENDING, FLAG_GLASS, FLAG_SKINNED, FLAG_TANGENT, FLAG_UVGEN = 0x1000, 0x2000, 0x4000, 0x8000, 0x10000
# The texture a row's slot binds to, as the capability bit of the shaders
# that sample it: TexDiffuse1, TexDiffuse2, TexNormal1, TexNormal2.
SLOT_SAMPLED = {1: FLAG_DIFFUSE, 2: FLAG_SECONDARY, 3: FLAG_NORMAL, 4: FLAG_NORMAL_B}

# The colour Blender draws a mesh without a material in (its default
# surface), linear.
DEFAULT_SURFACE = (0.8, 0.8, 0.8, 1.0)


# --- the shader table -----------------------------------------------------------

def shader_table():
    """The engine's shader tags and capability words, in table order; an
    ExportError when `opennova-3di catalog` gave none (every shader flag export
    writes depends on it)."""
    from . import catalog, catalog_error
    table = catalog(retry=True)[2]
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
    """OED's shader for a material that names none: the first table row of
    the model's kind (skinned or not) drawing that many texture maps (diffuse,
    detail), find_material_index_by_flags (5fc5b4f6a^ engine/formats/oed/
    convert_internal.cpp): FF_ST_OP for one map, FF_MT_OP for two, FFP_GLASS
    for none; VS_SKBASIC / VS_SKGLASS on a skinned model."""
    wanted = max(0, min(2, map_count))
    table = shader_table()
    for name, flags in table:
        if bool(flags & FLAG_SKINNED) != skinned:
            continue
        if (1 if flags & FLAG_DIFFUSE else 0) + (1 if flags & FLAG_SECONDARY else 0) == wanted:
            return name
    return table[0][0]


# What a material's settings ask of the shader export picks, in the order they
# weigh when no row draws them all: (asked, row flags) -> whether the row
# draws that.
ASKS = (
    ("alpha blending", lambda asked, f: (f & (FLAG_ALPHA | FLAG_BLENDING) == FLAG_ALPHA | FLAG_BLENDING) if asked
     else not f & FLAG_BLENDING),
    ("a glow", lambda asked, f: bool(f & FLAG_EMISSIVE) == asked),
    ("moving UVs", lambda asked, f: bool(f & FLAG_UVGEN) == asked),
    ("a normal map", lambda asked, f: bool(f & FLAG_NORMAL and f & FLAG_TANGENT) if asked else not f & FLAG_NORMAL),
)


def automatic_shader(map_count, skinned, asks):
    """The shader a material with no Shader set draws with, and the labels of
    what it asks that the shader draws otherwise. `asks` is (alpha blending
    from Render Method Blended, a glow from Emission, moving UVs from a U or V
    generator, a tangent-space normal map). OED's rule takes the first table
    row of the model's kind drawing that many texture maps (default_shader);
    among those rows (never a glass one) the first drawing what the material
    asks wins, weighing the asks in that order when none draws them all (our
    rule). The UV generators move UVs under the #UV twins only [orig:
    Material_ApplyShaderParameters @ 0x58DE4F..0x58DE56]."""
    rows = [(name, flags) for name, flags in shader_table()
            if bool(flags & FLAG_SKINNED) == skinned and not flags & FLAG_GLASS and
            (1 if flags & FLAG_DIFFUSE else 0) + (1 if flags & FLAG_SECONDARY else 0) == map_count]
    if not rows:
        return default_shader(map_count, skinned), []

    def draws(flags):
        return tuple(test(asked, flags) for (_, test), asked in zip(ASKS, asks))
    name, flags = max(rows, key=lambda row: draws(row[1]))
    return name, [("with " if asked else "without ") + label
                  for (label, _), asked, drawn in zip(ASKS, asks, draws(flags)) if not drawn]


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


def fed(sock):
    """Whether a node feeds the input past its muted links and nodes
    (links_into); an input fed by none draws its own value, as Blender draws
    it."""
    return next(links_into(sock), None) is not None


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
    attributes, constant colours), which the game does not draw. Only a Mix
    node's colours are followed, not its factor, and not the colour a Mix's
    constant factor leaves out (0 takes A, 1 takes B)."""
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


# The game's tangent frame is each vertex's dP/du and dP/dv on its D3D UVs,
# whose v runs down the texture [orig: BuildTransformMatrix @ 0x421B80
# (ModSuperOed.exe); formats/threedi/threedi_build.cpp derive_tangents], and
# its tangent-space shaders read a normal map's green along that dP/dv (the
# retail effects rotate the light into Tangent, Binormal, Normal: _vsSkDfT.fx
# vsTanSkinDot3DirPS; godot/shaders/object/normal/tangent_uv1.gdshaderinc).
# Blender's Normal Map node reads green along its own v, which runs up the
# texture, so a Blender (OpenGL) normal map's green is the game's inverted.
# An .mdt row is already a normal map, which the game samples as it is
# [orig: Material_LoadStageTexture @ 0x5B16F0; Texture_LoadAsNormalMap @
# 0x58C480; renderer::material_texture_transform].
NormalMap = namedtuple("NormalMap", "node image green_down between")


def green_flip(node, groups):
    """The image node a Combine Color node rebuilds with its green inverted
    (Separate Color's red and blue as they are, 1 - its green), the form
    Blender reads a green-down (DirectX, the game's) normal map in; None for
    anything else."""
    if node.type != "COMBINE_COLOR" or node.mode != "RGB":
        return None

    def one(sock, at):
        found = list(links_into(sock, at))
        return found[0] if len(found) == 1 else None
    red, green, blue = (one(node.inputs[k], groups) for k in ("Red", "Green", "Blue"))
    if red is None or green is None or blue is None:
        return None
    split = red[0]
    if split.type != "SEPARATE_COLOR" or split.mode != "RGB" or red[1].identifier != "Red" or \
            blue[0] != split or blue[1].identifier != "Blue":
        return None
    invert = green[0]
    if invert.type != "MATH" or invert.operation != "SUBTRACT" or invert.inputs[0].is_linked or \
            invert.inputs[0].default_value != 1.0:
        return None
    source = one(invert.inputs[1], green[2])
    if source is None or source[0] != split or source[1].identifier != "Green":
        return None
    image = one(split.inputs["Color"], red[2])
    if image is None or image[0].type != "TEX_IMAGE" or image[1].identifier != "Color" or image[0].image is None:
        return None
    return image[0]


def normal_map(mat):
    """What drives a material's normals: None for nothing; a NormalMap of the
    node on its Principled BSDF's Normal (through reroutes and groups), with,
    for a Normal Map node, the image node it reads, straight (green up) or
    through a green flip (green_flip: green down), or the node between that
    export cannot read."""
    bsdf = principled(mat)
    if bsdf is None:
        return None
    for node, _, groups in links_into(bsdf.inputs["Normal"]):
        if node.type != "NORMAL_MAP":
            return NormalMap(node, None, False, None)
        for src, out, inner in links_into(node.inputs["Color"], groups):
            if src.type == "TEX_IMAGE" and out.identifier == "Color" and src.image is not None:
                return NormalMap(node, src, False, None)
            flipped = green_flip(src, inner)
            if flipped is not None:
                return NormalMap(node, flipped, True, None)
            return NormalMap(node, None, False, src)
        return NormalMap(node, None, False, None)
    return None


def draws_normal_map(mat):
    """Whether a material's nodes give a normal map: a tangent-space Normal
    Map node reading an image."""
    found = normal_map(mat)
    return found is not None and found.image is not None and found.node.type == "NORMAL_MAP" and \
        found.node.space == "TANGENT"


# --- a material's settings -------------------------------------------------------

def two_sided(mat):
    """Backface Culling off draws both sides: material flag 4 (no culling)
    and, unless the Both sides setting says otherwise, the bullet faces'
    flag 1 (face_flags)."""
    return mat is not None and not mat.use_backface_culling


def blended(mat):
    """Render Method Blended: the strips draw in the alpha pass."""
    return mat is not None and mat.surface_render_method == "BLENDED"


def emission(mat):
    """Emission on: a Principled BSDF whose Emission Strength and Emission
    Color are not zero (or come from nodes)."""
    bsdf = principled(mat)
    if bsdf is None:
        return False
    strength, colour = bsdf.inputs["Emission Strength"], bsdf.inputs["Emission Color"]
    return (fed(strength) or strength.default_value > 0.0) and \
        (fed(colour) or any(c > 0.0 for c in colour.default_value[:3]))


def moving_uvs(mat):
    """A U or V generator: the UVs move (under a #UV shader)."""
    return mat is not None and bool(mat.o3d.u_style or mat.o3d.v_style)


def alpha_test(mat):
    """The alpha test a material asks for, as (threshold byte, inverted):
    its Principled BSDF's Alpha taken from a Math node Greater Than (Less Than
    for the inverted test) against a constant threshold, Blender's own alpha
    clip; None without one. The engine passes a pixel whose alpha is above
    the threshold byte, or at most it when inverted [orig:
    CGfxDevice_SetAlphaTestRef @ 0x6770a0]."""
    bsdf = principled(mat)
    if bsdf is None:
        return None
    for node, _, _ in links_into(bsdf.inputs["Alpha"]):
        if node.type == "MATH" and node.operation in ("GREATER_THAN", "LESS_THAN") and not node.inputs[1].is_linked:
            value = round(node.inputs[1].default_value * 255)
            return max(0, min(255, value)), node.operation == "LESS_THAN"
    return None


def face_flags(mat):
    """A material's bullet-face flags: 1 (both sides) as its Both sides
    setting says, by default following two-sided, as OED took both from one
    render attribute (export_3di.cpp material_flags); the others are the
    material's face settings."""
    if mat is None:
        return 0
    p = mat.o3d
    both = two_sided(mat) if p.face_both_sides == "DRAWN" else p.face_both_sides == "YES"
    return ((1 if both else 0) | (0x100 if p.face_never_hit else 0) |
            (0x800 if p.face_front_only else 0) | (p.face_other_flags & ~0x901))


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


def check_working_space(what):
    """Blender holds colour as linear Rec.709 light unless the blend file's
    working space is another, which would need a primaries conversion."""
    space = bpy.data.colorspace.working_space
    if space != "Linear Rec.709":
        raise ExportError(f"{what}: the blend file's working colour space is {space}; export encodes colour as "
                          "sRGB from Linear Rec.709 only (Color Management > Working Space)")


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
    if colour:
        check_working_space(f"image {image.name}")
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


def write_rows(path, w, h, rows):
    """Uncompressed 32-bit truecolor TGA, rows bottom-up (the retail shape),
    of RGBA chunks of values 0..1."""
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 8)
    with open(path, "wb") as f:
        f.write(header)
        for values in rows:
            # float64 and rint preserve Python round's ties-to-even result.
            values = np.clip(np.rint(values * 255.0), 0, 255).astype(np.uint8)
            f.write(values[:, [2, 1, 0, 3]].tobytes())


def write_tga(image, path):
    """The image as the game draws it (image_rows), as a 32-bit TGA."""
    rows = image_rows(image)
    first = next(rows)  # its checks run before the file is opened
    write_rows(path, image.size[0], image.size[1], itertools.chain([first], rows))


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
# writes is named in at most export.FILE_NAME_BYTES (15), as every texture
# retail packs is (a PFF entry's 16-byte name field, formats/pff/pff.h). The
# game asks for a row's name cut three characters past its first dot
# ("x.dds.tga" asks for "x.dds"), and for that name's .dds sibling first when
# one exists [orig:
# Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B598;
# renderer::material_texture_query, material_dds_sibling]: a written file's
# name has one dot.
ROW_NAME_BYTES = 16
WRITTEN_EXTENSIONS = (".tga", ".mdt")
# The files the game reads a texture from: .tga and .mdt through its TGA
# reader, .pcx through its PCX reader, a .dds sibling [orig:
# Texture_LoadByNameWithChannel @ 0x58B4FE..0x58B6E6].
GAME_EXTENSIONS = (".tga", ".dds", ".mdt", ".pcx")
# A material holds 24 texture rows (formats/threedi/threedi_3di3.h
# ThreediMaterial, which opennova-3di fills).
MATERIAL_ROWS = 24


def check_row_name(name, what):
    """An ExportError unless `name` is a texture row name opennova-3di takes:
    printable ASCII, at most 16 bytes, a file name without a folder. An empty
    name is a row that names no file, which the format holds: 63 rows of the
    JO models are empty (M24_1st's VS_BMTXMIRRT material keeps one in slot 2,
    Chair3's FF_ST_OP one in slot 1)."""
    if any(not " " <= c <= "~" for c in name):
        raise ExportError(f"{what}: the texture name '{name}' is not printable ASCII")
    if len(name) > ROW_NAME_BYTES:
        raise ExportError(f"{what}: the texture name '{name}' exceeds {ROW_NAME_BYTES} characters (its row's "
                          "field)")
    if any(c in name for c in "/\\"):
        raise ExportError(f"{what}: the texture name '{name}' names a folder; the game finds a texture by its "
                          "file name alone")


def file_name_ok(name, extensions):
    """Whether `name` can name a texture file export writes: printable ASCII,
    one dot, one of `extensions`, at most 15 bytes."""
    stem, ext = os.path.splitext(name)
    return 0 < len(name) <= export.FILE_NAME_BYTES and all(" " <= c <= "~" and c not in "/\\:" for c in name) and \
        stem and "." not in stem and ext.lower() in extensions


def check_file_name(name, what):
    """An ExportError unless `name` can name a texture file export writes:
    <stem>.tga or <stem>.mdt, one dot, at most 15 bytes."""
    check_row_name(name, what)
    if not name:
        raise ExportError(f"{what}: a texture row whose image export writes (Write) needs a file name")
    if not file_name_ok(name, WRITTEN_EXTENSIONS):
        raise ExportError(f"{what}: export writes the texture '{name}' as a file, named <stem>.tga or <stem>.mdt "
                          f"with one dot and at most {export.FILE_NAME_BYTES} characters, as retail packs them")


def derived_stem(model, index_digits, lettered):
    """The stem of the texture files a model's materials derive,
    <stem>_<material index>[letter].<ext>: the model name's ASCII letters,
    digits, _ and -, cut to fit 15 bytes."""
    keep = export.FILE_NAME_BYTES - len(".tga") - len("_") - index_digits - (1 if lettered else 0)
    stem = "".join(c for c in model if c.isascii() and (c.isalnum() or c in "_-"))
    return stem[:keep].strip("_-") or "tex"


def game_query(name):
    """The file a texture row's name asks the game for: the name cut three
    characters past its first dot."""
    dot = name.find(".")
    return name[:dot + 4] if dot >= 0 and dot + 4 < len(name) else name


def dds_sibling(query):
    dot = query.rfind(".")
    return (query[:dot] if dot >= 0 else query) + ".dds"


def file_reference(image):
    """(row name, file) for an image that is a texture file as it stands: a
    still image loaded unchanged (no unsaved edits, not packed) from a .tga,
    .dds, .mdt or .pcx file the game reads, whose own name fits a written
    file. The row names it by the image's name when the game's lookup of
    that name finds the file (retail's `x.dds.tga` finds `x.dds`), else by the
    file's own name. None for any other image, which export writes as a
    TGA."""
    if image.source != "FILE" or image.packed_file is not None or image.is_dirty:
        return None
    path = os.path.normpath(bpy.path.abspath(image.filepath, library=image.library))
    file = os.path.basename(path)
    if not file_name_ok(file, GAME_EXTENSIONS):
        return None
    name = export.clean_name(image.name)
    query = game_query(name)
    try:
        check_row_name(name, "")
    except ExportError:
        name = file
    if file.lower() not in (query.lower(), dds_sibling(query).lower()):
        name = file
    return name, path


# --- texture files ------------------------------------------------------------

class ImageFile:
    """A texture file export writes from an image, as the game draws it
    (image_rows)."""

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

    def write(self, path, exporter):
        write_tga(self.image, path)


class NormalMapFile(ImageFile):
    """A normal map a Normal Map node reads straight, as Blender's (OpenGL)
    normals: written with the game's green, inverted."""

    def content(self):
        return ("normal map", self.image.name_full)

    def describe(self):
        return f"the normal map {self.image.name}"

    def write(self, path, exporter):
        rows = image_rows(self.image)
        first = next(rows)  # its checks run before the file is opened

        def flipped(chunks):
            for values in chunks:
                values[:, 1] = 1.0 - values[:, 1]
                yield values
        write_rows(path, self.image.size[0], self.image.size[1], flipped(itertools.chain([first], rows)))


class CopiedFile:
    """A texture file as it stands (file_reference), copied beside the .3di
    under its own name unless a file of that name is already there."""

    def __init__(self, path):
        self.path = path

    def content(self):
        return ("file", os.path.normcase(self.path))

    def describe(self):
        return f"the file {self.path}"

    def check(self, what):
        pass  # its row names it whether or not the file is there to copy

    def write(self, path, exporter):
        if os.path.normcase(os.path.abspath(path)) == os.path.normcase(self.path):
            return
        if not os.path.isfile(self.path):
            exporter.note(f"the texture {os.path.basename(path)} is not copied: {self.path} is missing")
        elif os.path.exists(path):
            with open(path, "rb") as a, open(self.path, "rb") as b:
                if a.read() != b.read():
                    exporter.note(f"the texture {path} is not replaced: it differs from {self.path}")
        else:
            shutil.copyfile(self.path, path)


class SwatchFile:
    """A texture of one colour (linear RGBA), for a material whose Base Color
    has no image: 8 by 8 pixels, the size of retail's smallest textures."""

    SIDE = 8

    def __init__(self, rgba):
        self.rgba = tuple(float(c) for c in rgba)

    def pixel(self):
        """The colour sRGB-encoded, its alpha as it is, as 0..1 values."""
        rgb = srgb_encode(np.array(self.rgba[:3], dtype=np.float64))
        return np.concatenate([rgb, [self.rgba[3]]])

    def content(self):
        return ("swatch", tuple(int(v) for v in np.clip(np.rint(self.pixel() * 255.0), 0, 255)))

    def describe(self):
        return "a colour swatch"

    def check(self, what):
        check_working_space(what)

    def write(self, path, exporter):
        write_rows(path, self.SIDE, self.SIDE, [np.tile(self.pixel(), (self.SIDE * self.SIDE, 1))])


class Row:
    """A texture row a material exports (the MTRL row: file name, slot, type,
    flags, frame) and the file export writes for it (an ImageFile,
    NormalMapFile, CopiedFile or SwatchFile, under `file_name` when that is
    not the row's name), or None when the file is the author's to supply. A
    row whose name is derived carries its material's export index and its
    letter until names are given."""

    def __init__(self, name, slot, type=0, flags=0, frame=0, file=None, file_name=None, derive=None):
        self.name, self.slot, self.type, self.flags, self.frame = name, slot, type, flags, frame
        self.file = file
        self.file_name = file_name  # the written file's name, when it is not the row's
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

# A mesh a material sits on: its object's name, the names of its render UV
# map (UV0) and of the first other one (UV1), None where it lacks them, the
# area its triangles of that material cover on UV0, and whether any triangle
# draws with it (a material the mesh only holds in a slot draws nothing).
MeshUse = namedtuple("MeshUse", "name render second area drawn")


class ModelMaterials:
    """One model's materials: in the order its strips first use them, then in
    export order (order()), and the texture files they name. `exporter` is the
    model's export: its registers, its notes, whether it is skinned and the
    scene's Write textures setting; `run` the export run it is part of."""

    def __init__(self, exporter, run):
        self.exporter = exporter
        self.run = run.textures  # the TextureRun of the export run this model is part of
        self.used = []       # Blender materials (None: a mesh without one)
        self.first_use = {}  # material name (None) -> its index in first-use order
        self.meshes = {}     # material name (None) -> [MeshUse]
        self.choices = {}    # material name (None) -> (shader, named by the material, what it draws otherwise)
        self.textures = {}   # file name (lower case) -> (file name, the file) to write

    def index_of(self, mat):
        """The material's first-use index, which a strip carries until
        order()."""
        key = mat.name if mat is not None else None
        if key not in self.first_use:
            self.first_use[key] = len(self.used)
            self.used.append(mat)
        return self.first_use[key]

    def record_mesh(self, ob, ev, mesh, uv0):
        """What an exported mesh gives the model's materials: `mesh` is the
        evaluated object `ev`'s mesh with its loop triangles, `uv0` its render
        UV map's coordinates (two per loop, None without a UV map). Its UV maps
        are what export checks the textures of the materials it draws with
        against. A material in a slot no face draws with is part of the model
        when its Export order places it: retail models keep such materials
        (880 of them in 208 of the 2,413 JO models), and import gives each its
        order (keep_unused)."""
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
        drawn = [int(slot) for slot in np.unique(slots)]
        for slot in drawn:
            mat = export.slot_material(ev, slot)
            self.meshes.setdefault(mat.name if mat is not None else None, []).append(
                MeshUse(ob.name, render, second, float(area[slots == slot].sum()), True))
        for slot in range(len(ev.material_slots)):
            mat = export.slot_material(ev, slot)
            if slot not in drawn and mat is not None and mat.o3d.order >= 0:
                self.index_of(mat)
                self.meshes.setdefault(mat.name, []).append(MeshUse(ob.name, render, second, 0.0, False))

    def strip_alpha(self, index):
        """Strips draw in the alpha pass under a blending shader (OED's
        material_alpha: the BLENDING capability bit; FFP_GLASS is one) or a
        Blended render method. The shader export picks blends exactly when the
        material is Blended."""
        mat = self.used[index]
        named = mat.o3d.shader.strip() if mat is not None else ""
        return 1 if blended(mat) or (named and shader_flags(named) & FLAG_BLENDING) else 0

    def order(self):
        """Put the materials in export order, their Export order, then first
        use; returns each first-use index's export index."""
        self.used.sort(key=lambda m: m.o3d.order if m is not None and m.o3d.order >= 0 else 1 << 30)
        return {self.first_use[mat.name if mat is not None else None]: new for new, mat in enumerate(self.used)}

    def choice(self, mat):
        """(shader, whether the material names it, what of its settings the
        shader draws otherwise): its Shader, else automatic_shader() for its
        textures (a diffuse one always: an image, a texture entry or a swatch;
        a detail one from its Base Color's second UV map or its texture list;
        a normal map from a Normal Map node or its texture list) and
        settings."""
        key = mat.name if mat is not None else None
        if key not in self.choices:
            named = mat.o3d.shader.strip() if mat is not None else ""
            if named:
                self.choices[key] = (named, True, [])
            else:
                listed = {t.slot for t in mat.o3d.textures} if mat is not None else set()
                detail = 2 in listed or (mat is not None and 2 in self.node_images(mat))
                asks = (blended(mat), emission(mat), moving_uvs(mat), 3 in listed or draws_normal_map(mat))
                shader, otherwise = automatic_shader(2 if detail else 1, self.exporter.skinned, asks)
                self.choices[key] = (shader, False, otherwise)
        return self.choices[key]

    def shader(self, mat):
        return self.choice(mat)[0]

    def node_images(self, mat):
        """{slot: image}: the images a material's Base Color draws. The one on
        the render UV map is the diffuse texture (slot 1); one on the second UV
        map is the detail texture (slot 2), which the game multiplies in at
        twice its value on UV1 (FF_MT's Modulate2x)."""
        found = {}
        for node, groups in base_color(mat)[0]:
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
            if not use.drawn:
                continue
            if textured and use.render is None:
                raise ExportError(f"{use.name}: it has no UV map, but its material {key} draws a texture: unwrap "
                                  "it")
            if caps & FLAG_TANGENT and use.area <= 1e-12:
                self.exporter.note(f"{use.name}: its UV map has no area under {key or '(no material)'}, whose shader "
                                   f"{shader} derives its tangents from it, so its lighting comes out wrong")

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
                file_name = row.file_name or row.name
                self.run.claim(file_name, model, row.file.content(), row.file.describe(), what)
                if file_name.lower() not in self.textures:
                    row.file.check(what)
                    self.textures[file_name.lower()] = (file_name, row.file)
                if isinstance(row.file, SwatchFile):
                    self.exporter.note(f"{what}: no image feeds its Base Color, so it draws with the colour "
                                       f"swatch {row.name}")
        for mat, material_rows in zip(self.used, rows):
            self.emit_material(lines, mat, material_rows)

    def rows(self, mat):
        """A material's texture rows: its texture list's, then for each slot
        the list does not give, its nodes' (the Base Color images, for the
        slots its shader samples), and a colour swatch when its shader draws a
        diffuse texture that nothing else gives; derived names not yet given."""
        what = mat.name if mat is not None else "(no material)"
        shader, named, otherwise = self.choice(mat)
        caps = shader_flags(shader)
        if otherwise:
            kind = "skinned" if self.exporter.skinned else "rigid"
            self.exporter.note(f"{what}: the engine has no {kind} shader for its textures "
                               f"{' and '.join(otherwise)} as its settings ask; it draws with {shader}")
        if named and emission(mat) and not caps & FLAG_EMISSIVE:
            self.exporter.note(f"{what}: Emission is on, but its shader {shader} does not glow (a *_LUM one does)")
        if named and moving_uvs(mat) and not caps & FLAG_UVGEN:
            self.exporter.note(f"{what}: its U/V generator moves nothing under {shader}; only a #UV shader moves "
                               "UVs")
        out = []
        listed = {}
        if mat is not None:
            for t in mat.o3d.textures:
                name = t.name
                check_row_name(name, what)
                if not name and caps & SLOT_SAMPLED.get(t.slot, 0):
                    self.exporter.note(f"{what}: its slot {t.slot} texture row names no file, and its shader {shader} "
                                       "samples that slot: the game has no texture to load there")
                file = None
                if t.image is not None and t.write:
                    if not name or os.path.splitext(name)[1].lower() in WRITTEN_EXTENSIONS:
                        check_file_name(name, what)
                        file = ImageFile(t.image)
                    else:
                        self.exporter.note(f"{what}: Write writes .tga and .mdt files only; {name} is not written")
                out.append(Row(name, t.slot, t.type, t.flags, t.frame, file))
                listed.setdefault(t.slot, []).append(t.image)
        images = self.node_images(mat) if mat is not None else {}
        index = self.used.index(mat)
        for slot, capability, letter, role in ((1, FLAG_DIFFUSE, "", "diffuse"), (2, FLAG_SECONDARY, "d", "detail")):
            image = images.get(slot)
            if slot in listed:
                if image is not None and image not in listed[slot]:
                    self.exporter.note(f"{what}: its texture list gives slot {slot}, so the {role} image "
                                       f"{image.name} is not exported")
                continue
            if image is not None and not caps & capability:
                self.exporter.note(f"{what}: its shader {shader} draws no {role} texture; the image {image.name} is "
                                   "not exported")
            elif image is not None:
                reference = file_reference(image)
                if reference is not None:
                    out.append(Row(reference[0], slot, file=CopiedFile(reference[1]),
                                   file_name=os.path.basename(reference[1])))
                else:
                    out.append(Row(None, slot, file=ImageFile(image), derive=(index, letter, ".tga")))
            elif slot == 1 and caps & FLAG_DIFFUSE:
                out.append(Row(None, 1, file=SwatchFile(self.swatch_colour(mat)), derive=(index, "", ".tga")))
        normal = self.normal_row(mat, shader, listed.get(3), index) if mat is not None else None
        if normal is not None:
            out.append(normal)
        if mat is not None and images and not all(slot in listed for slot in images):
            for node in base_color(mat)[1]:
                self.exporter.note(f"{what}: its Base Color takes '{node_label(node)}', which the game does not draw: "
                                   "bake it into the image")
        if len(out) > MATERIAL_ROWS:
            raise ExportError(f"{what}: {len(out)} texture rows; a material holds {MATERIAL_ROWS}")
        self.check_uvs(mat, shader, any(not isinstance(r.file, SwatchFile) for r in out))
        return out

    def normal_row(self, mat, shader, listed, index):
        """The normal map row a material's Normal Map node gives (slot 3,
        type 4, an .mdt): its image as it stands when read through a green
        flip and loaded unchanged from an .mdt file, else written, its green
        inverted when read straight. None when its texture list gives slot 3,
        its shader samples no normal map, or no Normal Map node reads an
        image, each noted. A shader reading object-space normals, another
        space, a UV map the game does not derive tangents from and a node
        between the image and the Normal Map are refused."""
        found = normal_map(mat)
        if found is None:
            return None
        what = mat.name
        caps = shader_flags(shader)
        if listed is not None:
            image = found.image.image if found.image is not None else None
            if image is not None and image not in listed:
                self.exporter.note(f"{what}: its texture list gives slot 3, so the Normal Map node's image "
                                   f"{image.name} is not exported")
            return None
        if found.node.type != "NORMAL_MAP":
            self.exporter.note(f"{what}: its Normal comes from '{node_label(found.node)}'; the game's normal map is "
                               "a Normal Map node's image, so it is not exported")
            return None
        unsampled = f"{what}: its shader {shader} samples no normal map, so its Normal Map node is not exported "                     "(VS_DOT3DIFF, VS_PHONGT and the other bump shaders sample one)"
        if self.choice(mat)[1] and not caps & FLAG_NORMAL:
            self.exporter.note(unsampled)
            return None
        if found.node.space != "TANGENT":
            raise ExportError(f"{what}: its Normal Map node is in {found.node.space.lower().replace('_', ' ')} space; "
                              "the game reads tangent-space normal maps")
        if found.between is not None:
            raise ExportError(f"{what}: its Normal Map node reads '{node_label(found.between)}'; export takes an "
                              "image wired into it straight (Blender's green-up normals, which it writes with the "
                              "game's green) or through a green flip (Separate Color, 1 - Green, Combine Color: a "
                              "green-down file, written as it is)")
        if found.image is None:
            self.exporter.note(f"{what}: its Normal Map node reads no image, so it is not exported")
            return None
        if not caps & FLAG_NORMAL:
            self.exporter.note(unsampled)
            return None
        if not caps & FLAG_TANGENT:
            raise ExportError(f"{what}: its shader {shader} reads an object-space normal map, but a Normal Map node "
                              "gives tangent-space normals: choose a tangent-space shader (VS_DOT3DIFF, VS_PHONGT, "
                              "VS_SKBUMPDIFFT, ...) or name the object-space file in the texture list")
        uv = found.node.uv_map
        for use in self.meshes.get(mat.name, []):
            if uv and uv != use.render:
                raise ExportError(f"{what}: its Normal Map node reads the UV map '{uv}'; the game derives its "
                                  f"tangents from {use.name}'s render UV map")
        strength = found.node.inputs["Strength"]
        if strength.is_linked or abs(strength.default_value - 1.0) > 1e-6:
            self.exporter.note(f"{what}: its Normal Map strength is not exported; the game draws the normal map at "
                               "full strength")
        image = found.image.image
        check_image(image, what)
        if not image.colorspace_settings.is_data:
            self.exporter.note(f"{what}: the normal map {image.name} is read as {image.colorspace_settings.name} "
                               "colour; normals are data: set its Color Space to Non-Color")
        if found.green_down:
            reference = file_reference(image)
            if reference is not None and reference[1].lower().endswith(".mdt"):
                return Row(reference[0], 3, 4, file=CopiedFile(reference[1]),
                           file_name=os.path.basename(reference[1]))
            return Row(None, 3, 4, file=ImageFile(image), derive=(index, "n", ".mdt"))
        return Row(None, 3, 4, file=NormalMapFile(image), derive=(index, "n", ".mdt"))

    def swatch_colour(self, mat):
        """The colour a material without a Base Color image draws in: its Base
        Color and Alpha values, or an RGB node's colour. Nodes that hold no
        image (a procedural texture, an attribute) cannot be drawn, and
        neither can a material with no Principled BSDF: those, and a mesh
        without a material, draw in Blender's default grey, with a note."""
        bsdf = principled(mat)
        what = mat.name if mat is not None else "(no material)"
        if bsdf is None:
            self.exporter.note(f"{what}: " + ("a mesh without a material" if mat is None else
                                              "it has no Principled BSDF, whose Base Color export reads") +
                               " draws in Blender's default grey")
            return DEFAULT_SURFACE
        alpha = bsdf.inputs["Alpha"]
        a = 1.0 if fed(alpha) else alpha.default_value
        base = bsdf.inputs["Base Color"]
        # Muted links and nodes feed nothing: the input draws its own colour.
        sources = list(links_into(base))
        if not sources:
            return (*base.default_value[:3], a)
        if len(sources) == 1 and sources[0][0].type == "RGB":
            return (*sources[0][1].default_value[:3], a)
        self.exporter.note(f"{what}: its Base Color comes from '{node_label(sources[0][0])}', which holds no image, "
                           "so it draws in Blender's default grey: bake it to an image")
        return (*DEFAULT_SURFACE[:3], a)

    def name_rows(self, model, rows):
        """Give each derived row its file name, <stem>_<material index>[letter]
        with the model's stem (derived_stem): d marks a detail texture, n a
        normal map. A file the model already derives for one image or colour
        is named once."""
        derived = [row for material_rows in rows for row in material_rows if row.derive is not None]
        if not derived:
            return
        digits = len(str(max(row.derive[0] for row in derived)))
        stem = derived_stem(model, digits, any(row.derive[1] for row in derived))
        self.check_stem(stem)
        given = {}
        for row in derived:
            index, letter, ext = row.derive
            row.name = given.setdefault((row.file.content(), ext), f"{stem}_{index}{letter}{ext}")
            row.derive = None

    def check_stem(self, stem):
        """An ExportError when another model of the scene writes into this
        model's folder under a name that cuts to the same stem (gunmodel_a and
        gunmodel_b both derive gunmodel_0.tga): exported one after the other,
        each would write over the other's textures, and the game finds a
        texture by its name alone. The other model's stem is taken at every
        cut its own rows could give it."""
        own = self.exporter.model

        def folder(model):
            return os.path.normcase(os.path.normpath(os.path.dirname(export.output_path(model))))
        for other in export.model_roots(self.exporter.scene):
            if other is own or folder(other) != folder(own):
                continue
            name = export.model_name(other)
            if any(derived_stem(name, digits, lettered) == stem for digits in (1, 2, 3) for lettered in (False, True)):
                raise ExportError(f"{own.name}: its textures are named {stem}_<material index>, as {other.name}'s "
                                  "would be, which writes into the same folder (the game finds a texture by its "
                                  "name alone): give one of them another Model name or folder")

    def emit_material(self, lines, mat, rows):
        """One `material` record and what follows it."""
        fixed = export.fixed
        shader = self.shader(mat)
        if len(shader) > 32:
            raise ExportError(f"{mat.name}: the shader tag '{shader}' exceeds 32 characters")
        caps = shader_flags(shader)
        lines.append(f"material {quoted(shader)}  # {mat.name if mat is not None else '(no material)'}")
        p = mat.o3d if mat is not None else None
        test = alpha_test(mat)
        flags = (1 if test else 0) | (2 if test and test[1] else 0) | (4 if two_sided(mat) else 0) | \
            (p.other_flags & ~7 & 0xFF if p is not None else 0)
        if flags:
            lines.append(f"matflags {flags}")
        if test:
            lines.append(f"alphatest {test[0]}")
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
        for row in sorted(rows, key=lambda r: r.slot):
            lines.append(f"texture {quoted(row.name)} {row.slot} {row.type} {row.flags} {row.frame}")
        if p is None:
            return
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

    def generator_register(self, style, name, what):
        return self.exporter.register(name, what) if style > CTRL_REFERENCE_THRESHOLD else -1

    @staticmethod
    def generator_phase(style, phase, what):
        """A generator's phase byte (styles up to 112; above, that byte is the
        register index): the writer would clamp one outside it."""
        if style <= CTRL_REFERENCE_THRESHOLD:
            export.fixed(phase, 256.0, 0, 0xFF, what + " phase")

    def write_textures(self, out_dir):
        """Write the model's texture files beside its .3di, when the scene's
        Write textures setting is on; the exporter calls it once the model is
        built, so a model that fails writes none."""
        if not self.exporter.settings.write_textures:
            return
        for name, file in self.textures.values():
            path = os.path.join(out_dir, name)
            try:
                file.write(path, self.exporter)
            except OSError as e:
                raise ExportError(f"could not write the texture {path}: {e}") from e


# --- import -----------------------------------------------------------------

def import_image(builder, name):
    """The Blender image of texture reference `name`, loaded once per import
    from the file `opennova-3di scene` resolved beside the model (None, with a
    note, when it found none; None for an empty name, a row that names no
    file). An image this load makes is named after the reference, which export
    names it by (file_reference)."""
    if name in builder.images:
        return builder.images[name]
    path = builder.sc["texfiles"].get(name)
    img = None
    if name and not path:
        builder.note(f"texture {name} not found beside the model")
    elif path:
        try:
            img = bpy.data.images.load(path, check_existing=True)
            if img.users == 0:
                # An image this load made (one the scene already uses keeps
                # its own name and settings).
                builder.data(img)
                img.name = name
                # The game samples colour and alpha independently: a
                # texture's alpha is often a mask a shader reads (specular,
                # bump), not opacity (the arms' camo averages 0.001).
                # Blender's default straight alpha premultiplies on load and
                # loses the colour wherever alpha is near zero; channel-packed
                # keeps both.
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
    """The model's materials as Blender materials in the file's order, laid
    out as export reads them: the shader and the export order as properties,
    the flags as Blender's own settings, a slot's lone plain row as the image
    node the slot's nodes give when that image names it as the row does (a
    tangent-space shader's .mdt normal map too), and every other row in the
    texture list."""
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
        mat = bpy.data.materials.new(f"{builder.sc['name']}_{i}")
        builder.data(mat)
        p = mat.o3d
        p.shader = m["shader"]
        p.order = i
        flags = m["matflags"]
        mat.use_backface_culling = not flags & 4
        p.other_flags = flags & ~7 & 0xFF
        if flags & 2 and not flags & 1:
            builder.note(f"material {i}: an inverted alpha test without the test, which changes nothing; not "
                         "carried")
        if m["alphatest"] and not flags & 1:
            builder.note(f"material {i}: an alpha-test value without the alpha-test flag; not carried")
        # Glass and emissive follow the shader on export (OED's rule).
        caps = shader_flags(m["shader"])
        if bool(m["glass"]) != bool(caps & FLAG_GLASS and m["reflect"] and any(m["reflect"][:3])):
            builder.note(f"material {i}: glass {m['glass']} is not what its shader {m['shader']} gives")
        if m["emissive"] != (2 if caps & FLAG_EMISSIVE else 0):
            builder.note(f"material {i}: emissive {m['emissive']} is not what its shader {m['shader']} gives")
        if m["reflect"]:
            p.reflect = [c / 255.0 for c in m["reflect"]]
        blend = bool(caps & FLAG_BLENDING) or alpha_by_material.get(i, False)
        mat.surface_render_method = "BLENDED" if blend else "DITHERED"
        by_slot = {}
        for row in m["textures"]:
            by_slot.setdefault(row[1], []).append(row)
        shown = {}  # slot -> the image its node shows
        tangent = caps & FLAG_NORMAL and caps & FLAG_TANGENT
        for slot, rows in by_slot.items():
            images = [import_image(builder, row[0]) for row in rows]
            # A slot's lone plain row is its node's image when that image
            # names it as the row does; the normal map's when the shader reads
            # tangent-space normals from an .mdt file (read through a green
            # flip, since the file holds the game's green).
            plain = (0, 0, 0) if slot in (1, 2) else (4, 0, 0) if slot == 3 and tangent else None
            reference = file_reference(images[0]) if images[0] is not None else None
            if len(rows) == 1 and rows[0][2:] == plain and reference is not None and reference[0] == rows[0][0] and \
                    (slot != 3 or reference[1].lower().endswith(".mdt")):
                shown[slot] = images[0]
                continue
            for (name, s, typ, tflags, frame), img in zip(rows, images):
                t = p.textures.add()
                t.name = name
                t.slot, t.type, t.flags, t.frame = s, typ, tflags, frame
                t.image = img
                t.write = False  # the file beside the model already serves it
            preview = next((img for row, img in zip(rows, images) if img is not None and row[4] == 0), None)
            if slot in (1, 2) and preview is not None:
                shown[slot] = preview  # the list gives the slot; the node shows its first frame
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
        shade(mat, m, shown, blend)
        out.append(mat)
    return out


def keep_unused(builder, mats, lod_objects):
    """Put the model's materials no strip draws with in the slots of its first
    mesh, where export takes them back by the Export order import gave each
    (ModelMaterials.record_mesh): retail keeps such materials, and the model's
    material table keeps its indices. `lod_objects` are the objects each LOD
    built; a model without a mesh cannot hold them, and says so."""
    drawn = {s["material"] for lod in builder.sc["lods"] for part in lod["parts"] for s in part["strips"]}
    unused = [mat for i, mat in enumerate(mats) if i not in drawn]
    if not unused:
        return
    holder = next((ob for objs in lod_objects for ob in objs if ob.type == "MESH" and len(ob.data.polygons)), None)
    if holder is None:
        builder.note(f"{len(unused)} materials no strip draws with are not carried: the model has no mesh to hold "
                     "them")
        return
    for mat in unused:
        holder.data.materials.append(mat)


def shade(mat, m, shown, blend):
    """The node tree export reads the material from: the slot 1 image as the
    Base Color (times the slot 2 detail image on the second UV map, UV1, at
    twice its value, FF_MT's Modulate2x), the slot 3 .mdt through a green flip
    into a Normal Map node on Normal, a Math node Greater Than (Less Than when
    inverted) at the alpha-test threshold on Alpha, the image's alpha on
    Alpha when the material blends, Emission for a *_LUM shader, and a tint
    for glass."""
    tree = mat.node_tree
    nodes, links = tree.nodes, tree.links
    bsdf = principled(mat)
    caps = shader_flags(m["shader"])
    color = alpha = None
    if 1 in shown:
        tex = nodes.new("ShaderNodeTexImage")
        tex.image = shown[1]
        tex.location = (-600, 300)
        color, alpha = tex.outputs["Color"], tex.outputs["Alpha"]
    if 2 in shown and color is not None:
        uv = nodes.new("ShaderNodeUVMap")
        uv.uv_map = "UV1"
        uv.location = (-900, -100)
        detail = nodes.new("ShaderNodeTexImage")
        detail.image = shown[2]
        detail.location = (-600, -100)
        links.new(uv.outputs["UV"], detail.inputs["Vector"])
        mix = nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        mix.blend_type = "MULTIPLY"
        socket(mix.inputs, "Factor_Float").default_value = 1.0
        mix.location = (-300, 200)
        links.new(color, socket(mix.inputs, "A_Color"))
        links.new(detail.outputs["Color"], socket(mix.inputs, "B_Color"))
        scale = nodes.new("ShaderNodeVectorMath")
        scale.operation = "SCALE"
        scale.inputs[3].default_value = 2.0
        scale.location = (-120, 200)
        links.new(socket(mix.outputs, "Result_Color"), scale.inputs[0])
        color = scale.outputs[0]
    if color is not None:
        links.new(color, bsdf.inputs["Base Color"])
    if m["matflags"] & 1:
        test = nodes.new("ShaderNodeMath")
        test.operation = "LESS_THAN" if m["matflags"] & 2 else "GREATER_THAN"
        test.inputs[1].default_value = m["alphatest"] / 255.0
        test.location = (-300, -250)
        if alpha is not None:
            links.new(alpha, test.inputs[0])
        links.new(test.outputs[0], bsdf.inputs["Alpha"])
    elif blend and alpha is not None:
        links.new(alpha, bsdf.inputs["Alpha"])
    if 3 in shown:
        shown[3].colorspace_settings.name = "Non-Color"
        normal = nodes.new("ShaderNodeTexImage")
        normal.image = shown[3]
        normal.location = (-1100, -500)
        split = nodes.new("ShaderNodeSeparateColor")
        split.location = (-800, -500)
        invert = nodes.new("ShaderNodeMath")
        invert.operation = "SUBTRACT"
        invert.inputs[0].default_value = 1.0
        invert.location = (-600, -550)
        join = nodes.new("ShaderNodeCombineColor")
        join.location = (-400, -500)
        node = nodes.new("ShaderNodeNormalMap")
        node.location = (-200, -500)
        links.new(normal.outputs["Color"], split.inputs["Color"])
        links.new(split.outputs["Red"], join.inputs["Red"])
        links.new(split.outputs["Green"], invert.inputs[1])
        links.new(invert.outputs[0], join.inputs["Green"])
        links.new(split.outputs["Blue"], join.inputs["Blue"])
        links.new(join.outputs["Color"], node.inputs["Color"])
        links.new(node.outputs["Normal"], bsdf.inputs["Normal"])
    if m["emissive"] or caps & FLAG_EMISSIVE:
        if color is not None:
            links.new(color, bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.0
    if m["glass"] or caps & FLAG_GLASS:
        bsdf.inputs["Base Color"].default_value = (0.6, 0.75, 0.85, 1.0)
        if not bsdf.inputs["Alpha"].is_linked:
            bsdf.inputs["Alpha"].default_value = 0.35
