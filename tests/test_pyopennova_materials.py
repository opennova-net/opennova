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
    def __init__(self, texture_list, shader_name: bytes = b"VS_DOT3"):
        self.index = 5
        self.shader_name = shader_name
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
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            FakeTexture("Light.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
            FakeTexture("Bump.tga", materials.THREEDI_IR_TEX_SLOT_NORMAL, tex_type=5),
        ],
        shader_name=b"VS_DOT3DIFF2",
    )

    desc = materials.describe_material(
        mat,
        resolver=FakeResolver({"Diffuse.tga": diffuse, "Bump.tga": normal}),
    )

    assert desc.name == "Material_5_VS_DOT3DIFF2"
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


def test_describe_materials_passes_source_format_to_texture_resolver(tmp_path: Path):
    diffuse = tmp_path / "Diffuse.dds"
    diffuse.write_bytes(b"DDS data")
    mat = FakeMaterial([
        FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE, tex_type=0, flags=2),
    ])

    class FakeIR:
        source_format = 1
        material_count = 1
        materials = [mat]

    class RecordingResolver:
        def __init__(self):
            self.calls = []

        def resolve_texture(self, name: str, **kwargs):
            self.calls.append((name, kwargs))
            return str(diffuse)

    resolver = RecordingResolver()
    desc = materials.describe_materials(FakeIR(), resolver=resolver)[0]

    assert desc.diffuse.path == str(diffuse)
    assert resolver.calls == [
        (
            "Diffuse.tga",
            {
                "strategy": None,
                "source_format": 1,
                "slot": materials.THREEDI_IR_TEX_SLOT_DIFFUSE,
                "tex_type": 0,
                "flags": 2,
                "role": "diffuse",
            },
        )
    ]


def test_material_user_props_and_diagnostics_use_descriptor_data(tmp_path: Path):
    diffuse = tmp_path / "Diffuse.tga"
    diffuse.write_bytes(b"tga data")
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            FakeTexture("MissingDetail.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
        ],
        shader_name=b"FF_MT_OP",
    )
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
    mat = FakeMaterial(
        [
            FakeTexture("VeryLongDiffuseName.png", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            FakeTexture("Overlay.pcx", materials.THREEDI_IR_TEX_SLOT_DETAIL),
        ],
        shader_name=b"FF_MT_OP",
    )
    desc = materials.describe_material(mat)

    used = {}
    diffuse, detail = materials.ase_texture_names(desc, used)

    assert diffuse == "VeryLongDif.TGA"
    assert detail == "Overlay.PCX"


def test_shader_aware_texture_roles_preserve_unknown_detail_slot():
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            FakeTexture("Unexpected.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
        ],
        shader_name=b"FF_ST_OP",
    )

    desc = materials.describe_material(mat)

    assert desc.detail.name == ""
    assert len(desc.unknown_textures) == 1
    assert desc.unknown_textures[0].name == "Unexpected.tga"
    assert desc.unknown_textures[0].slot == materials.THREEDI_IR_TEX_SLOT_DETAIL


def test_slot_four_is_secondary_normal_for_bump_shaders(tmp_path: Path):
    secondary = tmp_path / "DetailN.tga"
    secondary.write_bytes(b"tga data")
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
            FakeTexture("DetailN.tga", materials.THREEDI_IR_TEX_SLOT_NORMAL_B, tex_type=5),
        ],
        shader_name=b"VS_DOT3DIFF2",
    )

    desc = materials.describe_material(
        mat,
        resolver=FakeResolver({"DetailN.tga": secondary}),
    )

    assert desc.normal.name == ""
    assert desc.secondary_normal.name == "DetailN.tga"
    assert desc.secondary_normal.role == "secondary_normal"
    assert desc.bump_mode == "normal_texture"
    assert desc.bump_uses_alpha
    assert desc.unknown_textures == ()


def test_material_texture_inventory_counts_shader_slot_type_roles():
    descs = [
        materials.describe_material(
            FakeMaterial(
                [
                    FakeTexture("Base.tga", materials.THREEDI_IR_TEX_SLOT_DIFFUSE),
                    FakeTexture("Detail.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
                ],
                shader_name=b"FF_MT_OP",
            )
        ),
        materials.describe_material(
            FakeMaterial(
                [
                    FakeTexture("Other.tga", materials.THREEDI_IR_TEX_SLOT_DETAIL),
                ],
                shader_name=b"FF_ST_OP",
            )
        ),
    ]

    inventory = materials.material_texture_inventory(descs)

    assert {
        "shader": "FF_MT_OP",
        "role": "detail",
        "slot": materials.THREEDI_IR_TEX_SLOT_DETAIL,
        "type": 0,
        "flags": 0,
        "count": 1,
    } in inventory
    assert {
        "shader": "FF_ST_OP",
        "role": "unknown",
        "slot": materials.THREEDI_IR_TEX_SLOT_DETAIL,
        "type": 0,
        "flags": 0,
        "count": 1,
    } in inventory


def test_ctypes_material_layout_exposes_3di3_material_fields():
    from pyopennova import threedi_ffi

    mat = threedi_ffi.ThreediIRMaterial()

    assert threedi_ffi.THREEDI_IR_MAX_MATERIAL_TEXTURES == 24
    assert len(mat.textures) == 24
    assert hasattr(mat, "material_flags")
    assert hasattr(mat, "alpha_test_value_byte")
    assert hasattr(mat, "rgb_gen2")
    assert hasattr(mat, "reflect_color2")
    assert hasattr(mat, "emissive_type2")
    assert hasattr(mat, "glass_type2")
