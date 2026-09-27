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
import addon_under_test  # noqa: E402

import bpy  # noqa: E402
import numpy as np  # noqa: E402

addon, CLI = addon_under_test.load()
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


def model(name, mat=None, uv_maps=(("UVMap", UVS),)):
    """A model root, its LOD 0, part PN01 and a one-triangle mesh drawing with
    `mat`; returns (root, mesh object)."""
    root = empty(name)
    root.o3d.output_path = os.path.join(OUT, name, name + ".3di").replace("\\", "/")
    lod = empty(name + "_LOD0", root)
    lod["_lod_index"] = 0
    part = empty("PN01", lod)
    me = bpy.data.meshes.new(name)
    me.from_pydata(TRIANGLE, [], [(0, 1, 2)])
    for label, coords in uv_maps:
        layer = me.uv_layers.new(name=label)
        for loop, uv in zip(layer.data, coords):
            loop.uv = uv
    if mat is not None:
        me.materials.append(mat)
    ob = bpy.data.objects.new("01 Mesh0", me)
    bpy.context.collection.objects.link(ob)
    ob.parent = part
    return root, ob


def export_model(root):
    """(notes, the model read back: its materials as importer.read_o3d gives
    them, the texture files beside it)."""
    bpy.context.view_layer.update()
    os.makedirs(os.path.dirname(root.o3d.output_path), exist_ok=True)
    _, notes = export.export_model(bpy.context, root)
    sc, _ = o3dtext.import_text(bpy.context, ["scene"], root.o3d.output_path, "scene.o3d", importer.read_o3d)
    return notes, sc


def refused(root, *fragments):
    """The ExportError the export raises, which must name every fragment."""
    try:
        export.export_model(bpy.context, root)
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


def textures(sc, index=0):
    """A read-back material's texture rows (name, slot, type, flags, frame)."""
    return sc["materials"][index]["textures"]


def tga_pixels(path):
    """A 32-bit TGA's BGRA bytes, bottom-up."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[2] == 2 and data[16] == 32, data[:18]
    return data[18:]


# --- geom-2: the diffuse texture is the image feeding Base Color ---------------

@case
def base_color_only_from_its_feed():
    # A normal map and a roughness image are not the diffuse texture: with no
    # image on Base Color the material exports none, and says so.
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
    assert not any(name.startswith(("rifle_rough", "rifle_n")) for name, *_ in textures(sc)), textures(sc)
    assert any("no image feeds its Base Color" in n for n in notes), notes


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
    # only its A, so the image on its B is not drawn, and A a Mix whose
    # factor is a mask image's alpha, neither of which is a diffuse texture.
    left_out = tree.nodes.new("ShaderNodeMix")
    left_out.data_type = "RGBA"
    materials.socket(left_out.inputs, "Factor_Float").default_value = 0.0
    tree.links.new(image_node(tree, image("grp_unused", (0, 1, 0, 1))).outputs["Color"],
                   materials.socket(left_out.inputs, "B_Color"))
    masked = tree.nodes.new("ShaderNodeMix")
    masked.data_type = "RGBA"
    tree.links.new(image_node(tree, image("grp_mask", (1, 1, 1, 1))).outputs["Alpha"],
                   materials.socket(masked.inputs, "Factor_Float"))
    tree.links.new(materials.socket(masked.outputs, "Result_Color"), materials.socket(left_out.inputs, "A_Color"))
    tree.links.new(materials.socket(left_out.outputs, "Result_Color"), call.inputs[0])
    root, _ = model("grouped", mat)
    notes, sc = export_model(root)
    names = [t[0] for t in textures(sc)]
    assert len(names) == 1 and names[0].lower().startswith("grp_albedo"), names
    assert any("Hue/Saturation" in n or "Hue Saturation" in n for n in notes), notes


@case
def two_images_on_one_uv_map_refused():
    mat, tree, bsdf = principled_material("TwoImages")
    mix = tree.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    tree.links.new(image_node(tree, image("two_a", (1, 0, 0, 1))).outputs["Color"], materials.socket(mix.inputs, "A_Color"))
    tree.links.new(image_node(tree, image("two_b", (0, 1, 0, 1))).outputs["Color"], materials.socket(mix.inputs, "B_Color"))
    tree.links.new(materials.socket(mix.outputs, "Result_Color"), bsdf.inputs["Base Color"])
    root, _ = model("twoimages", mat)
    refused(root, "two_a", "two_b", "bake them into one")


@case
def detail_texture_on_the_second_uv_map():
    mat, tree, bsdf = principled_material("Detailed")
    mix = tree.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    tree.links.new(image_node(tree, image("det_base", (0.5, 0.5, 0.5, 1))).outputs["Color"],
                   materials.socket(mix.inputs, "A_Color"))
    tree.links.new(image_node(tree, image("det_grain", (0.5, 0.5, 0.5, 1)), uv_map="Lightmap").outputs["Color"],
                   materials.socket(mix.inputs, "B_Color"))
    tree.links.new(materials.socket(mix.outputs, "Result_Color"), bsdf.inputs["Base Color"])
    root, _ = model("detailed", mat, uv_maps=(("UVMap", UVS), ("Lightmap", UVS)))
    notes, sc = export_model(root)
    rows = sorted((slot, name.lower()) for name, slot, *_ in textures(sc))
    assert [slot for slot, _ in rows] == [1, 2] and rows[0][1].startswith("det_base") and \
        rows[1][1].startswith("det_grain"), rows
    assert sc["materials"][0]["shader"] == "FF_MT_OP", sc["materials"][0]["shader"]


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
    mat, tree, bsdf = principled_material("Udim")
    udim = bpy.data.images.new("rifle_udim", width=4, height=4, tiled=True)
    udim.tiles.new(tile_number=1002)
    tree.links.new(image_node(tree, udim).outputs["Color"], bsdf.inputs["Base Color"])
    root, _ = model("udim", mat)
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


# --- geom-6: UV maps -------------------------------------------------------------

@case
def textured_mesh_without_uv_map_refused():
    mat, tree, bsdf = principled_material("NoUV")
    tree.links.new(image_node(tree, image("nouv", (1, 1, 1, 1))).outputs["Color"], bsdf.inputs["Base Color"])
    root, ob = model("nouv", mat, uv_maps=())
    refused(root, ob.name, "no UV map", "NoUV")


@case
def tangent_shader_without_uv_area_noted():
    mat, tree, bsdf = principled_material("Material_0_VS_DOT3DIFF")
    tree.links.new(image_node(tree, image("flat", (1, 1, 1, 1))).outputs["Color"], bsdf.inputs["Base Color"])
    root, ob = model("flatuv", mat, uv_maps=(("UVMap", [(0.5, 0.5)] * 3),))
    notes, _ = export_model(root)
    assert any(ob.name in n and "no area" in n for n in notes), notes


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
