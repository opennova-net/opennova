from __future__ import annotations

from pathlib import Path

from pyopennova import materials


class FakeTexture:
    def __init__(self, name: str, slot: int, tex_type: int = 0, flags: int = 0, frame: int = 0):
        self.name = name.encode("utf-8")
        self.slot = slot
        self.type = tex_type
        self.flags = flags
        self.frame = frame


class FakeUvParams:
    style = 0
    phase = 0.0
    reg = -1
    gen_rate = 0.0
    start = 0.0
    end = 0.0


class FakeAlphaGen:
    style = 0
    phase = 0.0
    reg = -1
    rate = 0.0
    start = 0
    end = 0


class FakeRgbGen:
    style = 0
    phase = 0.0
    reg = -1
    rate = 0.0
    start_color = (0.0, 0.0, 0.0, 0.0)
    end_color = (0.0, 0.0, 0.0, 0.0)


class FakeTexAnim:
    num_frames = 0
    animation_type = 0
    cycle_frame_time = 0


class FakeMaterial:
    def __init__(self, texture_list):
        self.index = 5
        self.shader_name = b"VS_DOT3"
        self.texture_count = len(texture_list)
        self.textures = texture_list
        self.flags = (
            materials.THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST
            | materials.THREEDI_IR_MATERIAL_FLAG_TWO_SIDED
        )
        self.alpha_threshold = 0.5
        self.blend_mode = 1
        self.u_params = FakeUvParams()
        self.v_params = FakeUvParams()
        self.alpha_gen = FakeAlphaGen()
        self.rgb_gen = FakeRgbGen()
        self.animation = FakeTexAnim()
        self.reflect_color = (0.1, 0.2, 0.3, 0.4)
        self.is_glass = 1
        self.specular_intensity = 128
        self.luminosity = 64
        self.emissive_type = 2
        self.u_tiling = 2.0
        self.v_tiling = 0.0
        self.surface_type = 1
        self.pattrib = 0


class FakeResolver:
    def __init__(self, paths):
        self.paths = {key.lower(): str(path) for key, path in paths.items()}

    def resolve_texture(self, name: str):
        return self.paths.get(name.lower())


def test_describe_material_interprets_texture_slots_and_paths(tmp_path: Path):
    diffuse = tmp_path / "Diffuse.dds"
    normal = tmp_path / "Bump.tga"
    diffuse.write_bytes(b"DDS data")
    normal.write_bytes(b"tga data")
    mat = FakeMaterial([
        FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
        FakeTexture("Light.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
        FakeTexture("Bump.tga", materials.THREEDI_IR_TEX_SLOT_NORMAL, tex_type=5),
    ])

    desc = materials.describe_material(
        mat,
        resolver=FakeResolver({"Diffuse.tga": diffuse, "Bump.tga": normal}),
    )

    assert desc.name == "Material_5_VS_DOT3"
    assert desc.diffuse.name == "Diffuse.tga"
    assert desc.diffuse.path == str(diffuse)
    assert desc.detail.name == "Light.tga"
    assert desc.detail.missing
    assert desc.normal.type == 5
    assert desc.bump_mode == "normal_texture"
    assert desc.bump_uses_alpha
    assert desc.alpha_test
    assert desc.two_sided
    assert desc.glass
    assert desc.emissive
    assert desc.has_custom_tiling
    assert desc.effective_u_tiling == 2.0
    assert desc.effective_v_tiling == 1.0


def test_material_user_props_and_diagnostics_use_descriptor_data(tmp_path: Path):
    diffuse = tmp_path / "Diffuse.tga"
    diffuse.write_bytes(b"tga data")
    mat = FakeMaterial([
        FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
        FakeTexture("MissingDetail.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
    ])
    desc = materials.describe_material(mat, resolver=FakeResolver({"Diffuse.tga": diffuse}))

    props = materials.material_user_props(desc)
    diag = materials.material_diagnostics([desc])

    assert props["ase_diffuse_bitmap"] == "Diffuse.tga"
    assert props["opennova_diffuse_texture_path"] == str(diffuse)
    assert props["opennova_alpha_threshold"] == 0.5
    assert props["reflect_color"] == (0.1, 0.2, 0.3, 0.4)
    assert diag["diffuse_paths"] == [str(diffuse)]
    assert diag["missing_detail"] == ["5:MissingDetail.tga"]
    assert diag["opacity_maps"] == 1


def test_explicit_diffuse_slot_overrides_first_texture_fallback():
    mat = FakeMaterial([
        FakeTexture("Fallback.tga", 99),
        FakeTexture("Explicit.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
    ])

    desc = materials.describe_material(mat)

    assert desc.diffuse.name == "Explicit.tga"


def test_ase_texture_names_match_legacy_extension_and_truncation_rules():
    mat = FakeMaterial([
        FakeTexture("VeryLongDiffuseName.png", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
        FakeTexture("Overlay.pcx", materials.THREEDI_IR_TEX_SLOT_DETAIL),
    ])
    desc = materials.describe_material(mat)

    used = {}
    diffuse, detail = materials.ase_texture_names(desc, used)

    assert diffuse == "VeryLongDif.TGA"
    assert detail == "Overlay.PCX"
