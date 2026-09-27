"""The add-on's materials and textures, from scratch and hermetic (every file
lands in a fresh temporary directory):

    blender -b --factory-startup --python-exit-code 1 --python tests/blender/materials_test.py -- <opennova-3di.exe>
    blender -b --factory-startup --python-exit-code 1 --python tests/blender/materials_test.py -- --installed

Each case builds a one-triangle model, exports it through opennova-3di and
reads the .3di back with `opennova-3di scene`.
"""
import os
import sys
import tempfile
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addon_harness  # noqa: E402

import bpy  # noqa: E402
import numpy as np  # noqa: E402

addon, CLI = addon_harness.load()
materials, export, importer, o3dtext = addon.materials, addon.export, addon.importer, addon.o3dtext
OUT = tempfile.mkdtemp(prefix="opennova_materials_test_")
FAILURES = []
TRIANGLE = [(0.0, -0.1, 0.0), (0.1, -0.1, 0.0), (0.0, -0.1, 0.1)]
UVS = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]


def case(fn):
    """Run one case on an empty scene; a failure is reported, not raised."""
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    try:
        fn()
        print("PASS", fn.__name__)
    except Exception:  # noqa: BLE001
        FAILURES.append(fn.__name__)
        print("FAIL", fn.__name__)
        traceback.print_exc()
    return fn


def empty(name, parent=None):
    ob = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(ob)
    ob.parent = parent
    return ob


def model(name, *mats, uv_maps=(("UVMap", UVS),)):
    """A model root, its LOD 0 and part PN01, with a one-triangle mesh for
    each material (None: a mesh without one); returns (root, the meshes)."""
    root = empty(name)
    root.o3d.output_path = os.path.join(OUT, name, name + ".3di").replace("\\", "/")
    lod = empty(name + "_LOD0", root)
    lod["_lod_index"] = 0
    part = empty("PN01", lod)
    obs = []
    for n, mat in enumerate(mats or (None,)):
        me = bpy.data.meshes.new(f"{name}{n}")
        me.from_pydata([(x + 0.2 * n, y, z) for x, y, z in TRIANGLE], [], [(0, 1, 2)])
        for label, coords in uv_maps:
            layer = me.uv_layers.new(name=label)
            for loop, uv in zip(layer.data, coords):
                loop.uv = uv
        if mat is not None:
            me.materials.append(mat)
        ob = bpy.data.objects.new(f"01 Mesh{n}", me)
        bpy.context.collection.objects.link(ob)
        ob.parent = part
        obs.append(ob)
    return root, obs


def folder(root):
    return os.path.dirname(root.o3d.output_path)


def export_model(root, run=None):
    """(notes, the model read back as importer.read_o3d gives it)."""
    bpy.context.view_layer.update()
    os.makedirs(folder(root), exist_ok=True)
    _, notes = export.export_model(bpy.context, root, run)
    sc, _ = o3dtext.import_text(bpy.context, ["scene"], root.o3d.output_path, "scene.o3d", importer.read_o3d)
    return notes, sc


def refused(root, *fragments, run=None):
    """The ExportError the export raises, which must name every fragment."""
    bpy.context.view_layer.update()
    os.makedirs(folder(root), exist_ok=True)
    try:
        export.export_model(bpy.context, root, run)
    except o3dtext.ExportError as e:
        for fragment in fragments:
            assert fragment in str(e), (fragment, str(e))
        return str(e)
    raise AssertionError(f"{root.name} exported; expected an error naming {fragments}")


def principled_material(name):
    mat = bpy.data.materials.new(name)
    tree = mat.node_tree
    return mat, tree, next(n for n in tree.nodes if n.type == "BSDF_PRINCIPLED")


def image(name, rgba, size=4, float_buffer=False, colour_space=None):
    img = bpy.data.images.new(name, width=size, height=size, alpha=True, float_buffer=float_buffer)
    if colour_space is not None:
        img.colorspace_settings.name = colour_space  # before the pixels: it regenerates the image
    img.pixels[:] = list(rgba) * (size * size)
    return img


def image_node(tree, img, uv_map=None):
    node = tree.nodes.new("ShaderNodeTexImage")
    node.image = img
    if uv_map is not None:
        uv = tree.nodes.new("ShaderNodeUVMap")
        uv.uv_map = uv_map
        tree.links.new(uv.outputs["UV"], node.inputs["Vector"])
    return node


def textured(name, img, uv_map=None):
    """A Principled material whose Base Color is the image."""
    mat, tree, bsdf = principled_material(name)
    tree.links.new(image_node(tree, img, uv_map).outputs["Color"], bsdf.inputs["Base Color"])
    return mat


def textures(sc, index=0):
    """A read-back material's texture rows (name, slot, type, flags, frame)."""
    return sc["materials"][index]["textures"]


def tga_pixels(path):
    """A 32-bit TGA's BGRA bytes, bottom-up."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[2] == 2 and data[16] == 32, data[:18]
    return data[18:]


def first_pixel(root, name):
    """The BGRA bytes of the first pixel of the texture `name` beside the model."""
    return tuple(tga_pixels(os.path.join(folder(root), name))[:4])


# --- geom-2: the diffuse texture is the image feeding Base Color ---------------

@case
def base_color_only_from_its_feed():
    # A normal map and a roughness image are not the diffuse texture: with no
    # image on Base Color the material draws its colour, and says so.
    mat, tree, bsdf = principled_material("Rifle Paint")
    bsdf.inputs["Base Color"].default_value = (0.2, 0.3, 0.1, 1.0)
    rough = image_node(tree, image("rifle_rough", (0.8, 0.8, 0.8, 1.0), colour_space="Non-Color"))
    tree.links.new(rough.outputs["Color"], bsdf.inputs["Roughness"])
    normal = image_node(tree, image("rifle_n", (0.5, 0.5, 1.0, 1.0), colour_space="Non-Color"))
    node = tree.nodes.new("ShaderNodeNormalMap")
    tree.links.new(normal.outputs["Color"], node.inputs["Color"])
    tree.links.new(node.outputs["Normal"], bsdf.inputs["Normal"])
    root, _ = model("paint", mat)
    notes, sc = export_model(root)
    # (the normal map is the normal map, geom-11)
    assert textures(sc) == [("paint_0.tga", 1, 0, 0, 0), ("paint_0n.mdt", 3, 4, 0, 0)], textures(sc)
    assert first_pixel(root, "paint_0.tga")[:3] != first_pixel(root, "paint_0n.mdt")[:3]
    assert any("no image feeds its Base Color" in n and "paint_0.tga" in n for n in notes), notes


@case
def base_color_through_groups_and_mixes():
    mat, tree, bsdf = principled_material("Grouped")
    group = bpy.data.node_groups.new("Albedo", "ShaderNodeTree")
    group.interface.new_socket("Tint", in_out="INPUT", socket_type="NodeSocketColor")
    group.interface.new_socket("Color", in_out="OUTPUT", socket_type="NodeSocketColor")
    inside = image_node(group, image("grp_albedo", (1.0, 0.0, 0.0, 1.0)))
    mix = group.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    group_in, group_out = group.nodes.new("NodeGroupInput"), group.nodes.new("NodeGroupOutput")
    group.links.new(inside.outputs["Color"], materials.socket(mix.inputs, "A_Color"))
    group.links.new(group_in.outputs[0], materials.socket(mix.inputs, "B_Color"))
    group.links.new(materials.socket(mix.outputs, "Result_Color"), group_out.inputs[0])
    call = tree.nodes.new("ShaderNodeGroup")
    call.node_tree = group
    hue = tree.nodes.new("ShaderNodeHueSaturation")
    tree.links.new(call.outputs[0], hue.inputs["Color"])
    tree.links.new(hue.outputs["Color"], bsdf.inputs["Base Color"])
    # The tint the group multiplies in: a Mix whose constant factor 0 takes
    # only its A, so the image on its B is not drawn, and on A a Mix whose
    # factor is a mask image's alpha; neither image is a diffuse texture.
    left_out = tree.nodes.new("ShaderNodeMix")
    left_out.data_type = "RGBA"
    materials.socket(left_out.inputs, "Factor_Float").default_value = 0.0
    tree.links.new(image_node(tree, image("grp_unused", (0, 1, 0, 1))).outputs["Color"],
                   materials.socket(left_out.inputs, "B_Color"))
    masked = tree.nodes.new("ShaderNodeMix")
    masked.data_type = "RGBA"
    tree.links.new(image_node(tree, image("grp_mask", (0, 0, 1, 1))).outputs["Alpha"],
                   materials.socket(masked.inputs, "Factor_Float"))
    tree.links.new(materials.socket(masked.outputs, "Result_Color"), materials.socket(left_out.inputs, "A_Color"))
    tree.links.new(materials.socket(left_out.outputs, "Result_Color"), call.inputs[0])
    root, _ = model("grouped", mat)
    notes, sc = export_model(root)
    assert textures(sc) == [("grouped_0.tga", 1, 0, 0, 0)], textures(sc)
    assert first_pixel(root, "grouped_0.tga") == (0, 0, 255, 255)  # the group's red image
    assert any(hue.name in n for n in notes), notes


@case
def two_images_on_one_uv_map_refused():
    mat, tree, bsdf = principled_material("TwoImages")
    mix = tree.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    tree.links.new(image_node(tree, image("two_a", (1, 0, 0, 1))).outputs["Color"],
                   materials.socket(mix.inputs, "A_Color"))
    tree.links.new(image_node(tree, image("two_b", (0, 1, 0, 1))).outputs["Color"],
                   materials.socket(mix.inputs, "B_Color"))
    tree.links.new(materials.socket(mix.outputs, "Result_Color"), bsdf.inputs["Base Color"])
    root, _ = model("twoimages", mat)
    refused(root, "two_a", "two_b", "bake them into one")


@case
def detail_texture_on_the_second_uv_map():
    mat, tree, bsdf = principled_material("Detailed")
    mix = tree.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    tree.links.new(image_node(tree, image("det_base", (1, 0, 0, 1))).outputs["Color"],
                   materials.socket(mix.inputs, "A_Color"))
    tree.links.new(image_node(tree, image("det_grain", (0, 1, 0, 1)), uv_map="Lightmap").outputs["Color"],
                   materials.socket(mix.inputs, "B_Color"))
    tree.links.new(materials.socket(mix.outputs, "Result_Color"), bsdf.inputs["Base Color"])
    root, _ = model("detailed", mat, uv_maps=(("UVMap", UVS), ("Lightmap", UVS)))
    notes, sc = export_model(root)
    assert textures(sc) == [("detailed_0.tga", 1, 0, 0, 0), ("detailed_0d.tga", 2, 0, 0, 0)], textures(sc)
    assert sc["materials"][0]["shader"] == "FF_MT_OP", sc["materials"][0]["shader"]
    assert first_pixel(root, "detailed_0.tga") == (0, 0, 255, 255)
    assert first_pixel(root, "detailed_0d.tga") == (0, 255, 0, 255)


@case
def mapped_coordinates_refused():
    mat, tree, bsdf = principled_material("Tiled UVs")
    node = image_node(tree, image("mapped", (1, 1, 1, 1)))
    mapping = tree.nodes.new("ShaderNodeMapping")
    coords = tree.nodes.new("ShaderNodeTexCoord")
    tree.links.new(coords.outputs["UV"], mapping.inputs["Vector"])
    tree.links.new(mapping.outputs["Vector"], node.inputs["Vector"])
    tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
    root, _ = model("mapped", mat)
    refused(root, "Mapping", "UV map")


# --- geom-9: one still image per texture --------------------------------------

@case
def udim_refused():
    udim = bpy.data.images.new("rifle_udim", width=4, height=4, tiled=True)
    udim.tiles.new(tile_number=1002)
    root, _ = model("udim", textured("Udim", udim))
    refused(root, "rifle_udim", "UDIM")


# --- geom-1: float images are sRGB-encoded colour ----------------------------

def save_png(img, path, depth):
    settings = bpy.context.scene.render.image_settings
    settings.file_format = "PNG"
    settings.color_depth = depth
    settings.color_mode = "RGBA"
    view = bpy.context.scene.view_settings
    view.view_transform = "Standard"
    view.look = "None"
    img.save_render(path, scene=bpy.context.scene)


@case
def float_images_encode_srgb():
    path = os.path.join(OUT, "tga")
    os.makedirs(path, exist_ok=True)
    # A byte image's bytes are written as they are (the retail rounding).
    raw = [-0.1, 0.5, 1.1, 1.0, 0.5 / 255, 1.5 / 255, 2.5 / 255, 0.25, 0.125, 0.875, 0.375, 0.0]
    byte = bpy.data.images.new("bytes", width=3, height=1, alpha=True)
    byte.pixels[:] = raw
    held = list(byte.pixels[:])
    materials.write_tga(byte, os.path.join(path, "bytes.tga"))
    expected = bytes(max(0, min(255, round(held[i + c] * 255))) for i in range(0, 12, 4) for c in (2, 1, 0, 3))
    assert tga_pixels(os.path.join(path, "bytes.tga")) == expected
    # A float colour image holds linear light: 0.5 is sRGB 188; non-colour
    # data (a mask, a normal map) keeps its values.
    lin = image("linear", (0.5, 0.5, 0.5, 1.0), size=1, float_buffer=True)
    materials.write_tga(lin, os.path.join(path, "linear.tga"))
    assert tga_pixels(os.path.join(path, "linear.tga")) == bytes((188, 188, 188, 255))
    data = image("data", (0.5, 0.5, 0.5, 1.0), size=1, float_buffer=True, colour_space="Non-Color")
    materials.write_tga(data, os.path.join(path, "data.tga"))
    assert tga_pixels(os.path.join(path, "data.tga")) == bytes((128, 128, 128, 255))
    # A 16-bit PNG exports the colours its 8-bit twin does: Blender holds it
    # linear and premultiplied, the TGA straight sRGB.
    src = image("src", (0.5, 0.25, 1.0, 0.5), size=2)
    for depth in ("8", "16"):
        save_png(src, os.path.join(path, f"grey{depth}.png"), depth)
    eight = bpy.data.images.load(os.path.join(path, "grey8.png"))
    sixteen = bpy.data.images.load(os.path.join(path, "grey16.png"))
    assert not eight.is_float and sixteen.is_float
    materials.write_tga(eight, os.path.join(path, "eight.tga"))
    materials.write_tga(sixteen, os.path.join(path, "sixteen.tga"))
    a = np.frombuffer(tga_pixels(os.path.join(path, "eight.tga")), dtype=np.uint8).astype(int)
    b = np.frombuffer(tga_pixels(os.path.join(path, "sixteen.tga")), dtype=np.uint8).astype(int)
    assert np.abs(a - b).max() <= 1, (a[:8], b[:8])


# --- C3, geom-3, geom-4, addon-3: texture names and when files are written ------

@case
def derived_names_fit_retail():
    # <model stem>_<material index>[d].tga, ASCII, one dot, at most 15 bytes;
    # a long model name is cut, and one image in two materials is one file.
    shared = image("shared", (0, 0, 1, 1))
    root, _ = model("tfa_akm", textured("A", shared), textured("B", shared),
                    textured("C", image("other", (0, 1, 0, 1))))
    root.o3d.model_name = "tfa_akm_rifle"  # the stem comes from the model name, cut to fit
    _, sc = export_model(root)
    names = [textures(sc, i)[0][0] for i in range(3)]
    assert names == ["tfa_akm_r_0.tga", "tfa_akm_r_0.tga", "tfa_akm_r_2.tga"], names
    assert all(len(n) <= 15 and n.count(".") == 1 and n.isascii() for n in names)
    assert sorted(os.listdir(folder(root))) == ["tfa_akm.3di", "tfa_akm_r_0.tga", "tfa_akm_r_2.tga"]
    assert materials.derived_stem("стволы-x", 1, False) == "x"
    assert materials.derived_stem("????", 1, False) == "tex"


@case
def texture_entry_names_follow_the_cli():
    def entry(name, write=False, img=None):
        mat = bpy.data.materials.new("Entry")
        t = mat.o3d.textures.add()
        t.name, t.write, t.image = name, write, img
        return mat
    for name, fragment in (("ствол.tga", "not printable ASCII"), ("a_seventeen_c.tga", "exceeds 16"),
                           ("tex/foo.tga", "holds a path"), ("", "no file name")):
        root, _ = model("entry", entry(name))
        refused(root, fragment)
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
    # A retail x.dds.tga row is a name like any other; export writes a file
    # only under a one-dot name of at most 15 bytes.
    root, _ = model("retail", entry("EXT_dbtr.dds.tga"))
    _, sc = export_model(root)
    assert textures(sc) == [("EXT_dbtr.dds.tga", 1, 0, 0, 0)], textures(sc)
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    root, _ = model("written", entry("EXT_dbtr.dds.tga", True, image("w", (1, 1, 1, 1))))
    refused(root, "one dot")


@case
def nothing_written_for_a_model_that_fails():
    # A bad image in the second material stops the export before the first
    # material's texture or the .3di is written.
    bad = image("nan", (0.5, 0.5, 0.5, 1.0), size=1, float_buffer=True)
    bad.pixels[:] = [float("nan"), 0.5, 0.5, 1.0]
    root, _ = model("failing", textured("Good", image("good", (1, 1, 1, 1))), textured("Bad", bad))
    refused(root, "nan", "non-finite")
    assert os.listdir(folder(root)) == [], os.listdir(folder(root))
    # Nor when the CLI refuses the model after the add-on's checks passed.
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    root, _ = model("clirefuses", textured("Fine", image("fine", (1, 1, 1, 1))))
    build = export.export_text

    def refuse(*args, **kwargs):
        raise o3dtext.ExportError("scene text:1: refused")
    export.export_text = refuse
    try:
        refused(root, "refused")
    finally:
        export.export_text = build
    assert os.listdir(folder(root)) == [], os.listdir(folder(root))


@case
def one_name_one_file_across_a_run():
    # Two models whose stems cut to one name cannot write two images under it
    # in one run (Export All Models).
    run = export.ExportRun()
    a, _ = model("gunmodel_a", textured("GunA", image("gun_a", (1, 0, 0, 1))))
    b, _ = model("gunmodel_b", textured("GunB", image("gun_b", (0, 1, 0, 1))))
    b.o3d.output_path = os.path.join(folder(a), "gunmodel_b.3di").replace("\\", "/")
    export_model(a, run)
    refused(b, "gunmodel_0.tga", "gunmodel_a", run=run)
    assert first_pixel(a, "gunmodel_0.tga") == (0, 0, 255, 255)


@case
def a_material_holds_24_rows():
    mat = bpy.data.materials.new("Rows")
    for i in range(25):
        t = mat.o3d.textures.add()
        t.name, t.frame = f"frame{i}.tga", i
    root, _ = model("rows", mat)
    refused(root, "25 texture rows", "24")


# --- geom-14, geom-5: the shader and flags from Blender's settings --------------

def material_record(sc, index=0):
    return sc["materials"][index]


@case
def shader_property_and_automatic_choice():
    def settings(name, blend=False, glow=False, uvgen=False, shader=""):
        mat = textured(name, image(name.lower(), (1, 1, 1, 1)))
        mat.surface_render_method = "BLENDED" if blend else "DITHERED"
        if glow:
            materials.principled(mat).inputs["Emission Strength"].default_value = 1.0
        mat.o3d.u_style = 32 if uvgen else 0
        mat.o3d.shader = shader
        return mat
    cases = (("Plain", {}, "FF_ST_OP", 0), ("Blend", {"blend": True}, "FF_ST_AB", 1),
             ("Glow", {"glow": True}, "FF_ST_OP_LUM", 0),
             ("BlendGlow", {"blend": True, "glow": True}, "FF_ST_AB_LUM", 1),
             ("Moving", {"uvgen": True}, "FF_ST_OP#UV", 0), ("Named", {"shader": "VS_PHONGT"}, "VS_PHONGT", 0),
             ("NamedBlend", {"shader": "FF_ST_OP", "blend": True}, "FF_ST_OP", 1))
    mats = [settings(name, **kw) for name, kw, _, _ in cases]
    root, _ = model("shaders", *mats)
    notes, sc = export_model(root)
    for i, (name, _, shader, alpha) in enumerate(cases):
        assert material_record(sc, i)["shader"] == shader, (name, material_record(sc, i)["shader"])
        strips = [s for part in sc["lods"][0]["parts"] for s in part["strips"] if s["material"] == i]
        assert strips and all(s["alpha"] == bool(alpha) for s in strips), (name, strips)
    assert material_record(sc, 2)["emissive"] == 2 and material_record(sc, 0)["emissive"] == 0
    # No skinned shader blends one texture: the automatic choice says so.
    assert materials.automatic_shader(1, True, (False,) * 4) == ("VS_SKBASIC", [])
    assert materials.automatic_shader(1, True, (True, False, False, False)) == \
        ("VS_SKBASIC", ["with alpha blending"])


@case
def flags_from_blender_settings():
    culled = textured("Culled", image("culled", (1, 1, 1, 1)))
    culled.use_backface_culling = True
    both = textured("BothSides", image("both", (1, 1, 1, 1)))
    both.use_backface_culling = False
    clipped = textured("Clipped", image("clipped", (1, 1, 1, 0.5)))
    inverted = textured("Inverted", image("inverted", (1, 1, 1, 0.5)))
    for mat, op in ((clipped, "GREATER_THAN"), (inverted, "LESS_THAN")):
        mat.use_backface_culling = True
        tree, bsdf = mat.node_tree, materials.principled(mat)
        test = tree.nodes.new("ShaderNodeMath")
        test.operation = op
        test.inputs[1].default_value = 0.25
        tex = next(n for n in tree.nodes if n.type == "TEX_IMAGE")
        tree.links.new(tex.outputs["Alpha"], test.inputs[0])
        tree.links.new(test.outputs[0], bsdf.inputs["Alpha"])
    root, _ = model("flags", culled, both, clipped, inverted)
    _, sc = export_model(root)
    got = [(m["matflags"], m["alphatest"]) for m in sc["materials"]]
    assert got == [(0, 0), (4, 0), (1, 64), (3, 64)], got
    # The bullet faces' both-sides flag follows two-sided too.
    faces = [f for c in sc["cobjs"] for f in c["faces"]]
    assert sorted(f[4] & 1 for f in faces) == [0, 0, 0, 1], faces


@case
def colour_only_materials_draw_a_swatch():
    # geom-5: no image is no longer additive glass: its Base Color is written
    # as a swatch with a textured shader, and a mesh without a material draws
    # in Blender's default grey; a shader the author names keeps OED's rule.
    colour, _, bsdf = principled_material("Paint")
    bsdf.inputs["Base Color"].default_value = (0.5, 0.0, 1.0, 1.0)
    glass = bpy.data.materials.new("Glass")
    glass.o3d.shader = "FFP_GLASS"
    root, _ = model("swatch", colour, None, glass)
    notes, sc = export_model(root)
    shaders = [m["shader"] for m in sc["materials"]]
    assert shaders == ["FF_ST_OP", "FF_ST_OP", "FFP_GLASS"], shaders
    rows = [textures(sc, i) for i in range(len(sc["materials"]))]
    assert rows == [[("swatch_0.tga", 1, 0, 0, 0)], [("swatch_1.tga", 1, 0, 0, 0)], []], rows
    assert first_pixel(root, "swatch_0.tga") == (255, 0, 188, 255)  # BGRA of linear (0.5, 0, 1) as sRGB
    assert first_pixel(root, "swatch_1.tga") == (231, 231, 231, 255)  # Blender's default 0.8 grey
    assert len(tga_pixels(os.path.join(folder(root), "swatch_0.tga"))) == 8 * 8 * 4
    assert any("swatch_0.tga" in n for n in notes) and any("without a material" in n for n in notes), notes


# --- addon-7: nodes own slots 1 and 2 after import; references and copies -------

def textured_file(name, rgba, where):
    """An image loaded from a TGA file written at `where`."""
    img = image(name + "_src", rgba)
    path = os.path.join(where, name + ".tga")
    img.filepath_raw = path
    img.file_format = "TARGA_RAW"
    img.save()
    bpy.data.images.remove(img)
    return bpy.data.images.load(path)


@case
def imported_images_live_in_the_nodes():
    art = os.path.join(OUT, "art")
    os.makedirs(art, exist_ok=True)
    mat, tree, bsdf = principled_material("Rifle")
    tree.links.new(image_node(tree, textured_file("rifle_d", (1, 0, 0, 1), art)).outputs["Color"],
                   bsdf.inputs["Base Color"])
    flip = bpy.data.materials.new("Flipbook")
    for frame in range(2):
        t = flip.o3d.textures.add()
        t.name, t.flags, t.frame, t.write = f"flip{frame}.tga", 1, frame, False
    flip.o3d.anim_frames = 2
    root, _ = model("imported", mat, flip)
    _, sc = export_model(root)
    # An unchanged .tga file is the texture as it stands: named, and copied.
    assert textures(sc, 0) == [("rifle_d.tga", 1, 0, 0, 0)], textures(sc, 0)
    assert os.path.isfile(os.path.join(folder(root), "rifle_d.tga"))
    # Import it again: the image is its Base Color node, the list keeps only
    # the flipbook's frames.
    imported, _ = importer.import_file(bpy.context, root.o3d.output_path)
    mats = {m.o3d.order: m for ob in imported.children_recursive if ob.type == "MESH" for m in ob.data.materials}
    assert len(mats[0].o3d.textures) == 0, [t.name for t in mats[0].o3d.textures]
    assert [export.clean_name(n.image.name) for n, _ in materials.base_color(mats[0])[0]] == ["rifle_d.tga"]
    assert [(t.name, t.frame) for t in mats[1].o3d.textures] == [("flip0.tga", 0), ("flip1.tga", 1)]
    # A new image in the node is what exports: no list overrides it.
    node = next(n for n in mats[0].node_tree.nodes if n.type == "TEX_IMAGE")
    node.image = image("repainted", (0, 1, 0, 1))
    imported.o3d.output_path = os.path.join(OUT, "reimported", "reimported.3di").replace("\\", "/")
    _, sc = export_model(imported)
    # (named after the model, whose name the import kept)
    assert textures(sc, 0) == [("imported_0.tga", 1, 0, 0, 0)], textures(sc, 0)
    assert first_pixel(imported, "imported_0.tga") == (0, 255, 0, 255)
    assert [t[0] for t in textures(sc, 1)] == ["flip0.tga", "flip1.tga"]


@case
def a_copy_never_replaces_another_file():
    art = os.path.join(OUT, "art2")
    os.makedirs(art, exist_ok=True)
    root, _ = model("copies", textured("Copied", textured_file("copied", (0, 0, 1, 1), art)))
    os.makedirs(folder(root), exist_ok=True)
    with open(os.path.join(folder(root), "copied.tga"), "wb") as f:
        f.write(b"another file")
    notes, _ = export_model(root)
    with open(os.path.join(folder(root), "copied.tga"), "rb") as f:
        assert f.read() == b"another file"
    assert any("not replaced" in n for n in notes), notes


@case
def texture_row_tooltips():
    props = addon.O3DTexture.bl_rna.properties
    assert "override" in props["flags"].description and "flipbook" in props["flags"].description
    assert "clamp" not in props["flags"].description.lower()


# --- geom-11: normal maps ------------------------------------------------------------

def normal_mapped(name, img, space="TANGENT", flip=False, shader="", strength=1.0, uv_map=""):
    """A material whose Normal Map node reads `img`, straight or through a
    green flip (Separate Color, 1 - Green, Combine Color)."""
    mat, tree, bsdf = principled_material(name)
    tree.links.new(image_node(tree, image(name.lower() + "_d", (1, 1, 1, 1))).outputs["Color"],
                   bsdf.inputs["Base Color"])
    node = tree.nodes.new("ShaderNodeNormalMap")
    node.space = space
    node.uv_map = uv_map
    node.inputs["Strength"].default_value = strength
    source = image_node(tree, img).outputs["Color"]
    if flip:
        split, invert, join = (tree.nodes.new(t) for t in ("ShaderNodeSeparateColor", "ShaderNodeMath",
                                                             "ShaderNodeCombineColor"))
        invert.operation = "SUBTRACT"
        invert.inputs[0].default_value = 1.0
        tree.links.new(source, split.inputs["Color"])
        tree.links.new(split.outputs["Red"], join.inputs["Red"])
        tree.links.new(split.outputs["Green"], invert.inputs[1])
        tree.links.new(invert.outputs[0], join.inputs["Green"])
        tree.links.new(split.outputs["Blue"], join.inputs["Blue"])
        source = join.outputs["Color"]
    tree.links.new(source, node.inputs["Color"])
    tree.links.new(node.outputs["Normal"], bsdf.inputs["Normal"])
    mat.o3d.shader = shader
    return mat


def normal_image(name, rgba=(0.25, 0.75, 1.0, 1.0)):
    return image(name, rgba, colour_space="Non-Color")


@case
def blender_normal_maps_export_with_the_games_green():
    # A Normal Map node's OpenGL (green-up) image is written as an .mdt with
    # its green inverted, and the automatic shader samples tangent-space
    # normals; one read through a green flip is written as it is.
    root, _ = model("bumped", normal_mapped("Straight", normal_image("up")),
                    normal_mapped("Flipped", normal_image("down"), flip=True))
    notes, sc = export_model(root)
    assert [m["shader"] for m in sc["materials"]] == ["VS_DOT3DIFF", "VS_DOT3DIFF"], sc["materials"]
    assert textures(sc, 0)[1] == ("bumped_0n.mdt", 3, 4, 0, 0), textures(sc, 0)
    assert textures(sc, 1)[1] == ("bumped_1n.mdt", 3, 4, 0, 0), textures(sc, 1)
    # BGRA: blue 255, green 0.75 -> 191 (straight: 255 - 191 = 64), red 64.
    assert first_pixel(root, "bumped_0n.mdt") == (255, 64, 64, 255), first_pixel(root, "bumped_0n.mdt")
    assert first_pixel(root, "bumped_1n.mdt") == (255, 191, 64, 255), first_pixel(root, "bumped_1n.mdt")


@case
def a_game_normal_map_file_is_the_file():
    # An unchanged .mdt read through the green flip is the game's own file:
    # named and copied as it stands; import lays it out that way again.
    art = os.path.join(OUT, "mdt")
    os.makedirs(art, exist_ok=True)
    tga = textured_file("ground_n", (0.5, 0.25, 1.0, 1.0), art)
    path = os.path.join(art, "ground_n.mdt")
    os.replace(bpy.path.abspath(tga.filepath), path)
    bpy.data.images.remove(tga)
    mdt = bpy.data.images.load(path)
    mdt.colorspace_settings.name = "Non-Color"
    root, _ = model("mdtfile", normal_mapped("GroundFile", mdt, flip=True))
    _, sc = export_model(root)
    assert textures(sc, 0)[1] == ("ground_n.mdt", 3, 4, 0, 0), textures(sc, 0)
    with open(path, "rb") as a, open(os.path.join(folder(root), "ground_n.mdt"), "rb") as b:
        assert a.read() == b.read()
    imported, _ = importer.import_file(bpy.context, root.o3d.output_path)
    mat = next(m for ob in imported.children_recursive if ob.type == "MESH" for m in ob.data.materials)
    assert len(mat.o3d.textures) == 0, [t.name for t in mat.o3d.textures]
    found = materials.normal_map(mat)
    assert found.node.type == "NORMAL_MAP" and found.green_down and \
        export.clean_name(found.image.image.name) == "ground_n.mdt", found


@case
def normal_maps_the_game_cannot_draw():
    img = normal_image("bad_n")
    for mat, fragments in ((normal_mapped("ObjectSpace", img, space="OBJECT"), ("object space", "tangent-space")),
                           (normal_mapped("PhongO", img, shader="VS_PHONGO"), ("VS_PHONGO", "object-space")),
                           (normal_mapped("OtherUV", img, uv_map="Lightmap"), ("Lightmap", "render UV map"))):
        root, _ = model("badnormal", mat, uv_maps=(("UVMap", UVS), ("Lightmap", UVS)))
        refused(root, *fragments)
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
    between = normal_mapped("Between", img)
    tree = between.node_tree
    node = next(n for n in tree.nodes if n.type == "NORMAL_MAP")
    hue = tree.nodes.new("ShaderNodeHueSaturation")
    tree.links.new(node.inputs["Color"].links[0].from_socket, hue.inputs["Color"])
    tree.links.new(hue.outputs["Color"], node.inputs["Color"])
    root, _ = model("between", between)
    refused(root, hue.name, "straight", "green flip")


@case
def normal_maps_noted_not_exported():
    weak = normal_mapped("Weak", image("weak_n", (0.5, 0.5, 1, 1)), strength=0.5)  # also sRGB-tagged
    plain = normal_mapped("PlainShader", normal_image("plain_n"), shader="FF_ST_OP")
    bumped, tree, bsdf = principled_material("Bumped")
    bump = tree.nodes.new("ShaderNodeBump")
    tree.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    root, _ = model("noted", weak, plain, bumped)
    notes, sc = export_model(root)
    assert any("Weak" in n and "full strength" in n for n in notes), notes
    assert any("Weak" in n and "Non-Color" in n for n in notes), notes
    assert any("PlainShader" in n and "samples no normal map" in n for n in notes), notes
    assert any("Bumped" in n and bump.name in n for n in notes), notes
    assert [t[1] for t in textures(sc, 1)] == [1] and [t[1] for t in textures(sc, 2)] == [1]


# --- geom-6: UV maps -------------------------------------------------------------

@case
def textured_mesh_without_uv_map_refused():
    root, obs = model("nouv", textured("NoUV", image("nouv", (1, 1, 1, 1))), uv_maps=())
    refused(root, obs[0].name, "no UV map", "NoUV")


@case
def tangent_shader_without_uv_area_noted():
    mat = textured("Flat", image("flat", (1, 1, 1, 1)))
    mat.o3d.shader = "VS_DOT3DIFF"
    root, obs = model("flatuv", mat, uv_maps=(("UVMap", [(0.5, 0.5)] * 3),))
    notes, _ = export_model(root)
    assert any(obs[0].name in n and "no area" in n for n in notes), notes


@case
def float_images_under_another_working_space_refused():
    # Another working colour space would need a primaries conversion. The
    # operator refuses while images carry unsaved pixels.
    for img in list(bpy.data.images):
        bpy.data.images.remove(img)
    assert bpy.ops.wm.set_working_color_space(working_space="ACEScg", convert_colors=False) == {"FINISHED"}
    try:
        lin = image("aces", (0.5, 0.5, 0.5, 1.0), size=1, float_buffer=True)
        try:
            materials.write_tga(lin, os.path.join(OUT, "aces.tga"))
            raise AssertionError("a float image exported under ACEScg")
        except o3dtext.ExportError as e:
            assert "ACEScg" in str(e), e
    finally:
        for img in list(bpy.data.images):
            bpy.data.images.remove(img)
        bpy.ops.wm.set_working_color_space(working_space="Linear Rec.709", convert_colors=False)


if FAILURES:
    raise SystemExit(f"materials_test: {len(FAILURES)} failed: {', '.join(FAILURES)}")
print("MATERIALS_TEST_OK")
