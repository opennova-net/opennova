# Materials and textures: the `material` records a model's Blender materials
# export as, the texture files written beside its .3di, and the Blender
# material an imported record becomes (docs/threedi/o3d-scene-format.md). The
# shader tags and their capability words are the engine's own table, read from
# `opennova-3di catalog`; the OED rules decide what a shader implies
# (docs/adr/0047-blender-3di-exporter.md, decision 7).

import os
import re
import struct

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


def shader_of(mat, skinned):
    """The name's tag (Material_<i>_<SHADER>), else the default for the
    material's texture maps (default_shader)."""
    m = MATERIAL_RE.match(export.clean_name(mat.name)) if mat is not None else None
    if m:
        return m.group(2)
    if mat is None:
        maps = 0
    elif len(mat.o3d.textures) > 0:
        maps = len({t.slot for t in mat.o3d.textures if t.slot in (1, 2)})
    else:
        maps = 1 if material_image(mat) is not None else 0
    return default_shader(maps, skinned)


def face_flags(mat):
    """A material's bullet-face flags: 1 (both sides) follows Two sided, as
    OED took both from one render attribute (export_3di.cpp material_flags);
    the others are the material's face settings."""
    if mat is None:
        return 0
    p = mat.o3d
    return ((1 if p.two_sided else 0) | (0x100 if p.face_never_hit else 0) |
            (0x800 if p.face_front_only else 0) | (p.face_other_flags & ~0x901))


# --- images -----------------------------------------------------------------

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


# --- export -----------------------------------------------------------------

class ModelMaterials:
    """One model's materials: in the order its strips first use them, then in
    export order (order()), and the texture files they name. `exporter` is the
    model's export: its registers, its notes, whether it is skinned and the
    scene's Write textures setting."""

    def __init__(self, exporter):
        self.exporter = exporter
        self.used = []       # Blender materials (None: a mesh without one)
        self.first_use = {}  # material name (None) -> its index in first-use order
        self.textures = {}   # file name (lower case) -> (file name, image)

    def index_of(self, mat):
        """The material's first-use index, which a strip carries until
        order()."""
        key = mat.name if mat is not None else None
        if key not in self.first_use:
            self.first_use[key] = len(self.used)
            self.used.append(mat)
        return self.first_use[key]

    def strip_alpha(self, index):
        """Strips of a blending shader draw in the alpha pass (OED's
        material_alpha: the BLENDING capability bit; FFP_GLASS is one)."""
        mat = self.used[index]
        blending = shader_flags(shader_of(mat, self.exporter.skinned)) & FLAG_BLENDING
        return 1 if blending or (mat is not None and mat.o3d.alpha_strips) else 0

    def order(self):
        """Put the materials in export order, the Material_<i> index, then
        first use; returns each first-use index's export index."""
        self.used.sort(key=lambda m: (int(MATERIAL_RE.match(export.clean_name(m.name)).group(1))
                                      if m is not None and MATERIAL_RE.match(export.clean_name(m.name)) else 1 << 30))
        return {self.first_use[mat.name if mat is not None else None]: new for new, mat in enumerate(self.used)}

    def claim_texture(self, name, image, mat):
        """A texture file export writes: one image per file name (Windows
        names match without case)."""
        have = self.textures.get(name.lower())
        if have is not None and have[1] != image:
            raise ExportError(f"{mat.name}: the images '{have[1].name}' and '{image.name}' would both be written "
                              f"as {name}: give each its own file name (one derived from an image's name keeps "
                              "its first 12 characters)")
        self.textures[name.lower()] = (name, image)

    def generator_register(self, style, name, what):
        return self.exporter.register(name, what) if style > CTRL_REFERENCE_THRESHOLD else -1

    @staticmethod
    def generator_phase(style, phase, what):
        """A generator's phase byte (styles up to 112; above, that byte is the
        register index): the writer would clamp one outside it."""
        if style <= CTRL_REFERENCE_THRESHOLD:
            export.fixed(phase, 256.0, 0, 0xFF, what + " phase")

    def emit(self, lines):
        """The `material` records, in export order."""
        fixed = export.fixed
        for mat in self.used:
            # A mesh without a material draws with the shader a material
            # without textures takes (shader_of), its strips' pass included.
            shader = shader_of(mat, self.exporter.skinned)
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
                        name = os.path.splitext(export.clean_name(t.image.name))[0][:12] + ".tga"
                    if not name:
                        raise ExportError(f"{mat.name}: a texture entry has neither a file name nor an image")
                    if len(name) > 16:
                        raise ExportError(f"{mat.name}: texture name '{name}' exceeds 16 characters")
                    lines.append(f"texture {quoted(name)} {t.slot} {t.type} {t.flags} {t.frame}")
                    if t.image is not None and t.write:
                        if name.lower().endswith(".tga"):
                            self.claim_texture(name, t.image, mat)
                        else:
                            self.exporter.note(f"{mat.name}: Write TGA writes .tga files only; {name} is not written")
            elif caps & FLAG_DIFFUSE:
                image = material_image(mat)
                if image is not None:
                    name = os.path.splitext(export.clean_name(image.name))[0][:12] + ".tga"
                    lines.append(f"texture {quoted(name)}")
                    self.claim_texture(name, image, mat)
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
        """Write every claimed texture beside the .3di, when the scene's Write
        textures setting is on."""
        if self.exporter.settings.write_textures:
            for name, image in self.textures.values():
                write_tga(image, os.path.join(out_dir, name))


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
