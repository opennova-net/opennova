"""Tests for the shared ASE submaterial writer in pyopennova/ase_from_ir.py.

These exercise ``populate_ase_submaterial`` directly against a fake C-like
submaterial fake. The real C ASE writer (libs/ase) is exercised by the
integration test_ase_writer_parity.py round-trip.
"""
from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from pyopennova import materials
from pyopennova.ase_from_ir import populate_ase_submaterial


def _new_sub() -> SimpleNamespace:
    return SimpleNamespace(
        name=b"",
        maps=[SimpleNamespace(value=b"") for _ in range(4)],
        uv_u_tiling=[0.0, 0.0],
        uv_v_tiling=[0.0, 0.0],
        diffuse=[0.0, 0.0, 0.0],
        ambient=[0.0, 0.0, 0.0],
        specular=[0.0, 0.0, 0.0],
        shine=0.0,
        shine_strength=0.0,
        transparency=0.0,
        wiresize=0.0,
        extra_flags=0,
        shading=0,
    )


class _FakeTexture:
    def __init__(self, name: str, slot: int, tex_type: int = 0):
        self.name = name.encode("utf-8")
        self.slot = slot
        self.type = tex_type
        self.flags = 0
        self.frame = 0


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


class _FakeIRMat:
    """Minimal IR material for describe_material()."""

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


def test_ase_writes_diffuse_and_detail_for_ff_mt(tmp_path: Path):
    diffuse = tmp_path / "Wall.dds"
    detail = tmp_path / "Detail.dds"
    diffuse.write_bytes(b"DDS")
    detail.write_bytes(b"DDS")
    resolver = _FakeResolver({"Wall.tga": diffuse, "Detail.tga": detail})

    ir_mat = _FakeIRMat(
        shader_name=b"FF_MT_OP",
        texture_count=2,
        textures=[
            _FakeTexture("Wall.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            _FakeTexture("Detail.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
        ],
    )
    desc = materials.describe_material(ir_mat, resolver=resolver)
    sub = _new_sub()
    populate_ase_submaterial(sub, desc, used_tex_names={})

    assert sub.maps[0].value == b"Wall.TGA"
    assert sub.maps[1].value == b"Detail.TGA"
    assert sub.shading == 0  # Blinn for FF_MT_OP


def test_ase_writes_opacity_map_for_alpha_test(tmp_path: Path):
    diffuse = tmp_path / "Bush.dds"
    diffuse.write_bytes(b"DDS")
    resolver = _FakeResolver({"Bush.tga": diffuse})

    ir_mat = _FakeIRMat(
        shader_name=b"FF_ST_AB",
        flags=materials.THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST,
        material_flags=materials.THREEDI_MATERIAL_FLAG_ALPHA_TEST,
        alpha_test_value_byte=128,
        alpha_threshold=0.5,
        texture_count=1,
        textures=[_FakeTexture("Bush.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE)],
    )
    desc = materials.describe_material(ir_mat, resolver=resolver)
    sub = _new_sub()
    populate_ase_submaterial(sub, desc, used_tex_names={})

    assert sub.maps[0].value == b"Bush.TGA"
    assert sub.maps[2].value == b"Bush.TGA"  # opacity reuses diffuse


def test_ase_writes_two_sided_extra_flag():
    ir_mat = _FakeIRMat(
        shader_name=b"FF_ST_OP",
        flags=materials.THREEDI_IR_MATERIAL_FLAG_TWO_SIDED,
        material_flags=materials.THREEDI_MATERIAL_FLAG_TWO_SIDED,
    )
    desc = materials.describe_material(ir_mat)
    sub = _new_sub()
    populate_ase_submaterial(sub, desc, used_tex_names={})

    assert sub.extra_flags & 1


def test_ase_writes_phong_shading_for_phongt():
    ir_mat = _FakeIRMat(shader_name=b"VS_PHONGT")
    desc = materials.describe_material(ir_mat)
    sub = _new_sub()
    populate_ase_submaterial(sub, desc, used_tex_names={})

    assert sub.shading == 1  # Phong


def test_ase_writes_glass_reflect_into_ambient():
    ir_mat = _FakeIRMat(
        shader_name=b"FFP_GLASS",
        is_glass=1,
        reflect_color=(0.4, 0.6, 0.8, 0.0),
    )
    desc = materials.describe_material(ir_mat)
    sub = _new_sub()
    populate_ase_submaterial(sub, desc, used_tex_names={})

    assert sub.ambient[0] == 0.4
    assert sub.ambient[1] == 0.6
    assert sub.ambient[2] == 0.8


def test_ase_writes_default_ambient_when_not_glass():
    ir_mat = _FakeIRMat(shader_name=b"FF_ST_OP")
    desc = materials.describe_material(ir_mat)
    sub = _new_sub()
    populate_ase_submaterial(sub, desc, used_tex_names={})

    # Non-glass falls back to the engine-style ambient defaults.
    assert sub.ambient[0] == sub.ambient[1] == sub.ambient[2]
    assert 0.0 < sub.ambient[0] < 0.1


def test_populate_ase_submaterial_round_trips_via_user_props(tmp_path: Path):
    """An IR material → MaterialDescriptor → user props → MaterialDescriptor →
    ASE submaterial pipeline produces the same key fields as the direct path."""
    diffuse = tmp_path / "Wall.dds"
    detail = tmp_path / "Detail.dds"
    diffuse.write_bytes(b"DDS")
    detail.write_bytes(b"DDS")
    resolver = _FakeResolver({"Wall.tga": diffuse, "Detail.tga": detail})

    ir_mat = _FakeIRMat(
        shader_name=b"FF_MT_AB",
        flags=materials.THREEDI_IR_MATERIAL_FLAG_TWO_SIDED,
        material_flags=materials.THREEDI_MATERIAL_FLAG_TWO_SIDED,
        texture_count=2,
        textures=[
            _FakeTexture("Wall.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            _FakeTexture("Detail.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
        ],
    )
    direct_desc = materials.describe_material(ir_mat, resolver=resolver)
    direct_sub = _new_sub()
    populate_ase_submaterial(direct_sub, direct_desc, used_tex_names={})

    props = materials.material_user_props(direct_desc)
    rebuilt_desc = materials.descriptor_from_user_props(props)
    rebuilt_sub = _new_sub()
    populate_ase_submaterial(rebuilt_sub, rebuilt_desc, used_tex_names={})

    assert direct_sub.maps[0].value == rebuilt_sub.maps[0].value
    assert direct_sub.maps[1].value == rebuilt_sub.maps[1].value
    assert direct_sub.shading == rebuilt_sub.shading
    assert (direct_sub.extra_flags & 1) == (rebuilt_sub.extra_flags & 1)
    assert direct_sub.uv_u_tiling[0] == rebuilt_sub.uv_u_tiling[0]
