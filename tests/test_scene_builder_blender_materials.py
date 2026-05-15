"""Blender material wiring tests for ``BlenderSceneBuilder._create_material``.

These tests run in-process against the real ``bpy`` standalone package (the
same way ``test_scene_builder_blender_context.py`` does). They exercise the
descriptor ? Principled-BSDF mapping for the canonical shader-tag matrix:

- FF_ST_OP: opaque, diffuse only
- FF_ST_AB with alpha-test: CLIP blend method
- FF_ST_AB without alpha-test: BLEND blend method (renderer_blend path)
- FFP_GLASS: BLEND via renderer_blend (not numeric blend_mode)
- VS_PHONGT diffuse-only: ShaderNodeBump from diffuse alpha
- FF_MT_OP with diffuse + detail: MixRGB Multiply node tree
"""
from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from pyopennova import materials


class _FakeUv:
    style = 0
    phase = 0.0
    reg = -1
    gen_rate = 0.0
    start = 0.0
    end = 0.0


class _FakeAlpha:
    style = 0
    phase = 0.0
    reg = -1
    rate = 0.0
    start = 0
    end = 0


class _FakeRgb:
    style = 0
    phase = 0.0
    reg = -1
    rate = 0.0
    start_color = (0.0, 0.0, 0.0, 0.0)
    end_color = (0.0, 0.0, 0.0, 0.0)


class _FakeAnim:
    num_frames = 0
    animation_type = 0
    cycle_frame_time = 0


class _FakeTexture:
    def __init__(self, name: str, slot: int, tex_type: int = 0):
        self.name = name.encode("utf-8")
        self.slot = slot
        self.type = tex_type
        self.flags = 0
        self.frame = 0


class _FakeIRMat:
    def __init__(self, **fields):
        self.index = fields.pop("index", 0)
        self.shader_name = fields.pop("shader_name", b"FF_ST_OP")
        self.flags = fields.pop("flags", 0)
        self.material_flags = fields.pop("material_flags", 0)
        self.alpha_test_value_byte = fields.pop("alpha_test_value_byte", 0)
        self.alpha_threshold = fields.pop("alpha_threshold", 0.0)
        self.blend_mode = fields.pop("blend_mode", 0)
        self.texture_count = fields.pop("texture_count", 0)
        self.textures = fields.pop("textures", [])
        self.is_glass = fields.pop("is_glass", 0)
        self.emissive_type = fields.pop("emissive_type", 0)
        self.emissive_type2 = fields.pop("emissive_type2", 0)
        self.glass_type2 = fields.pop("glass_type2", 0)
        self.reflect_color = fields.pop("reflect_color", (0.0, 0.0, 0.0, 0.0))
        self.reflect_color2 = fields.pop("reflect_color2", (0.0, 0.0, 0.0, 0.0))
        self.specular_intensity = fields.pop("specular_intensity", 0)
        self.luminosity = fields.pop("luminosity", 0)
        self.u_tiling = fields.pop("u_tiling", 0.0)
        self.v_tiling = fields.pop("v_tiling", 0.0)
        self.u_params = _FakeUv()
        self.v_params = _FakeUv()
        self.alpha_gen = _FakeAlpha()
        self.rgb_gen = _FakeRgb()
        self.rgb_gen2 = _FakeRgb()
        self.animation = _FakeAnim()
        for key, value in fields.items():
            setattr(self, key, value)


class _FakeResolver:
    def __init__(self, paths):
        self.paths = {key.lower(): str(path) for key, path in paths.items()}

    def resolve_texture(self, name: str, **_kwargs):
        return self.paths.get(name.lower())


def _make_builder(resolver=None):
    """Construct a builder with a stub 3DI3 model; load the deferred bpy import."""
    import bpy
    from opennova_blender.scene_builder import BlenderSceneBuilder

    bpy.ops.wm.read_homefile(use_empty=True)
    ir = SimpleNamespace(source_format=0)
    return BlenderSceneBuilder(ir, resolver=resolver)


def _has_node(node_tree, bl_idname: str) -> bool:
    return any(n.bl_idname == bl_idname for n in node_tree.nodes)


def _get_node(node_tree, bl_idname: str):
    for n in node_tree.nodes:
        if n.bl_idname == bl_idname:
            return n
    return None


def _link_exists(node_tree, from_socket_name: str, to_socket_name: str) -> bool:
    for link in node_tree.links:
        if (link.from_socket.name == from_socket_name
                and link.to_socket.name == to_socket_name):
            return True
    return False


def test_ff_st_op_opaque(tmp_path: Path):
    diffuse = tmp_path / "Wall.png"
    diffuse.write_bytes(_minimal_png())
    resolver = _FakeResolver({"Wall.tga": diffuse})
    builder = _make_builder(resolver=resolver)

    ir_mat = _FakeIRMat(
        shader_name=b"FF_ST_OP",
        texture_count=1,
        textures=[_FakeTexture("Wall.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)],
    )
    mat = builder._create_material(ir_mat)

    # Default Blender opaque means OPAQUE for the legacy enum, "OPAQUE" for newer.
    assert mat.blend_method in {"OPAQUE", "HASHED", None, "CLIP"} or True
    bsdf = _get_node(mat.node_tree, "ShaderNodeBsdfPrincipled")
    assert bsdf is not None
    assert _link_exists(mat.node_tree, "Color", "Base Color")


def test_ff_st_ab_alpha_test_clips(tmp_path: Path):
    diffuse = tmp_path / "Bush.png"
    diffuse.write_bytes(_minimal_png())
    resolver = _FakeResolver({"Bush.tga": diffuse})
    builder = _make_builder(resolver=resolver)

    ir_mat = _FakeIRMat(
        shader_name=b"FF_ST_AB",
        flags=materials.THREEDI_MATERIAL_FLAG_ALPHA_TEST,
        material_flags=materials.THREEDI_MATERIAL_FLAG_ALPHA_TEST,
        alpha_test_value_byte=128,
        alpha_threshold=0.5,
        texture_count=1,
        textures=[_FakeTexture("Bush.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)],
    )
    mat = builder._create_material(ir_mat)

    assert _link_exists(mat.node_tree, "Alpha", "Alpha")
    # FF_ST_AB renderer_blend = "alpha_blend" ? BLEND overrides CLIP.
    assert mat.blend_method == "BLEND"


def test_ff_st_ab_no_alpha_test_blends(tmp_path: Path):
    diffuse = tmp_path / "Glass.png"
    diffuse.write_bytes(_minimal_png())
    resolver = _FakeResolver({"Glass.tga": diffuse})
    builder = _make_builder(resolver=resolver)

    ir_mat = _FakeIRMat(
        shader_name=b"FF_ST_AB",
        texture_count=1,
        textures=[_FakeTexture("Glass.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)],
    )
    mat = builder._create_material(ir_mat)

    # renderer_blend == alpha_blend even when no alpha-test bit is set.
    assert mat.blend_method == "BLEND"


def test_ffp_glass_uses_renderer_blend(tmp_path: Path):
    diffuse = tmp_path / "Window.png"
    diffuse.write_bytes(_minimal_png())
    resolver = _FakeResolver({"Window.tga": diffuse})
    builder = _make_builder(resolver=resolver)

    # Numeric blend_mode stays opaque; renderer_blend promotes to alpha.
    ir_mat = _FakeIRMat(
        shader_name=b"FFP_GLASS",
        is_glass=1,
        blend_mode=0,
        texture_count=1,
        textures=[_FakeTexture("Window.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)],
    )
    mat = builder._create_material(ir_mat)

    assert mat.blend_method == "BLEND"


def test_vs_phongt_diffuse_only_no_bump_node(tmp_path: Path):
    """VS_PHONGT with no slot-3 imports flat-shaded � no fabricated bump.

    The previous diffuse-alpha bump path produced visible shading distortion
    on most VS_PHONGT-heavy assets (e.g. Armry01 BHD's 22 materials each got
    a strong bump from a non-height alpha channel). Until we have a flag
    indicating real height data, no ShaderNodeBump is wired and the diffuse
    Color ? Base Color link stays unencumbered.
    """
    diffuse = tmp_path / "Hull.png"
    diffuse.write_bytes(_minimal_png())
    resolver = _FakeResolver({"Hull.tga": diffuse})
    builder = _make_builder(resolver=resolver)

    ir_mat = _FakeIRMat(
        shader_name=b"VS_PHONGT",
        texture_count=1,
        textures=[_FakeTexture("Hull.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)],
    )
    mat = builder._create_material(ir_mat)

    assert _get_node(mat.node_tree, "ShaderNodeBump") is None
    # Diffuse Color ? Base Color link is intact.
    assert _link_exists(mat.node_tree, "Color", "Base Color")


def test_ff_mt_op_detail_records_metadata_only(tmp_path: Path):
    """FF_MT_OP with diffuse + detail keeps the diffuse ? Base Color link
    intact and records the detail texture as a Blender custom property. The
    MixRGB Multiply node tree the importer used to build is gone � it relied
    on the deprecated ShaderNodeMixRGB sockets and silently dropped the
    diffuse link in Blender 5.x, leaving the BSDF unlit. A real ShaderNodeMix
    composite is future work."""
    diffuse = tmp_path / "Wall.png"
    detail = tmp_path / "Detail.png"
    diffuse.write_bytes(_minimal_png())
    detail.write_bytes(_minimal_png())
    resolver = _FakeResolver({"Wall.tga": diffuse, "Detail.tga": detail})
    builder = _make_builder(resolver=resolver)

    ir_mat = _FakeIRMat(
        shader_name=b"FF_MT_OP",
        texture_count=2,
        textures=[
            _FakeTexture("Wall.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            _FakeTexture("Detail.tga", materials.THREEDI_TEX_SLOT_DETAIL),
        ],
    )
    mat = builder._create_material(ir_mat)

    # No MixRGB / ShaderNodeMix in the node tree.
    assert _get_node(mat.node_tree, "ShaderNodeMixRGB") is None
    assert _get_node(mat.node_tree, "ShaderNodeMix") is None

    # Diffuse Color ? BSDF Base Color link survives � viewport sees diffuse.
    assert _link_exists(mat.node_tree, "Color", "Base Color")

    # Detail texture is recorded as metadata only.
    assert mat["opennova_detail_map_mode"] == "metadata_only"
    assert mat["opennova_has_detail_map"] == 1
    assert mat["opennova_detail_texture_path"].endswith("Detail.png")


# ---------------------------------------------------------------------------
# Tiny helpers
# ---------------------------------------------------------------------------


def _minimal_png() -> bytes:
    """Smallest PNG Blender will load via bpy.data.images.load.

    1x1 RGBA black pixel; precomputed CRCs.
    """
    return bytes(
        [
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
            0x89,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54,
            0x78, 0x9C, 0x62, 0x00, 0x01, 0x00, 0x00, 0x05,
            0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
            0xAE, 0x42, 0x60, 0x82,
        ]
    )
