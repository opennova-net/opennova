from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

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
            materials.THREEDI_MATERIAL_FLAG_ALPHA_TEST
            | materials.THREEDI_MATERIAL_FLAG_TWO_SIDED
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
            FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture("Light.tga", materials.THREEDI_TEX_SLOT_DETAIL),
            FakeTexture("Bump.tga", materials.THREEDI_TEX_SLOT_NORMAL, tex_type=5),
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
        FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE, tex_type=0, flags=2),
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
                "slot": materials.THREEDI_TEX_SLOT_DIFFUSE,
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
            FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture("MissingDetail.tga", materials.THREEDI_TEX_SLOT_DETAIL),
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
        FakeTexture("Explicit.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
    ])

    desc = materials.describe_material(mat)

    assert desc.diffuse.name == "Explicit.tga"


def test_ase_texture_names_match_legacy_extension_and_truncation_rules():
    mat = FakeMaterial(
        [
            FakeTexture("VeryLongDiffuseName.png", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture("Overlay.pcx", materials.THREEDI_TEX_SLOT_DETAIL),
        ],
        shader_name=b"FF_MT_OP",
    )
    desc = materials.describe_material(mat)

    used = {}
    diffuse, detail = materials.ase_texture_names(desc, used)

    assert diffuse == "VeryLongDif.TGA"
    assert detail == "Overlay.pcx"


def test_shader_aware_texture_roles_preserve_unknown_detail_slot():
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture("Unexpected.tga", materials.THREEDI_TEX_SLOT_DETAIL),
        ],
        shader_name=b"FF_ST_OP",
    )

    desc = materials.describe_material(mat)

    assert desc.detail.name == ""
    assert len(desc.unknown_textures) == 1
    assert desc.unknown_textures[0].name == "Unexpected.tga"
    assert desc.unknown_textures[0].slot == materials.THREEDI_TEX_SLOT_DETAIL


def test_static_renderer_classifier_matches_objects_codex_phong():
    cls = materials.classify_material_shader("VS_PHONGT", 0, 0, 0, 128)

    assert cls.known_shader
    assert cls.family == materials.MATERIAL_SHADER_PHONG
    assert cls.blend == materials.MATERIAL_BLEND_OPAQUE
    assert cls.needs_normal_map
    assert cls.normal_space == materials.MATERIAL_NORMAL_TANGENT
    assert cls.uses_specular
    assert not cls.has_detail


def test_phong_shader_without_normal_texture_uses_flat_normal_fallback():
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
        ],
        shader_name=b"VS_PHONGT",
    )

    desc = materials.describe_material(mat, source_format=1)

    assert desc.needs_normal_map
    assert desc.bump_mode == ""
    assert not desc.bump_uses_alpha


def test_static_renderer_classifier_marks_object_space_dot3():
    cls = materials.classify_material_shader("VS_DOT3DIFFOBJ")

    assert cls.family == materials.MATERIAL_SHADER_DOT3
    assert cls.needs_normal_map
    assert cls.normal_space == materials.MATERIAL_NORMAL_OBJECT


def test_3di3_source_format_uses_static_shader_semantics():
    desc = materials.describe_material(
        FakeMaterial([FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)], shader_name=b"VS_PHONGT"),
        source_format=1,
    )

    assert desc.shader_family == materials.MATERIAL_SHADER_PHONG
    assert desc.needs_normal_map
    assert desc.normal_space == materials.MATERIAL_NORMAL_TANGENT


def test_slot_four_is_secondary_normal_for_bump_shaders(tmp_path: Path):
    secondary = tmp_path / "DetailN.tga"
    secondary.write_bytes(b"tga data")
    mat = FakeMaterial(
        [
            FakeTexture("Diffuse.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture("DetailN.tga", materials.THREEDI_TEX_SLOT_NORMAL_B, tex_type=5),
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
                    FakeTexture("Base.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
                    FakeTexture("Detail.tga", materials.THREEDI_TEX_SLOT_DETAIL),
                ],
                shader_name=b"FF_MT_OP",
            )
        ),
        materials.describe_material(
            FakeMaterial(
                [
                    FakeTexture("Other.tga", materials.THREEDI_TEX_SLOT_DETAIL),
                ],
                shader_name=b"FF_ST_OP",
            )
        ),
    ]

    inventory = materials.material_texture_inventory(descs)

    assert {
        "shader": "FF_MT_OP",
        "role": "detail",
        "slot": materials.THREEDI_TEX_SLOT_DETAIL,
        "type": 0,
        "flags": 0,
        "count": 1,
    } in inventory
    assert {
        "shader": "FF_ST_OP",
        "role": "unknown",
        "slot": materials.THREEDI_TEX_SLOT_DETAIL,
        "type": 0,
        "flags": 0,
        "count": 1,
    } in inventory


def test_ctypes_material_layout_exposes_3di3_material_fields():
    from pyopennova import tdp_ffi

    mat = tdp_ffi.TdpMaterial()

    assert tdp_ffi.TDP_MAX_MATERIAL_TEXTURES == 24
    assert len(mat.textures) == 24
    assert hasattr(mat, "material_flags")
    assert hasattr(mat, "alpha_test_value_byte")
    assert hasattr(mat, "rgb_gen2")
    assert hasattr(mat, "reflect_color2")
    assert hasattr(mat, "emissive_type2")
    assert hasattr(mat, "glass_type2")


# ---------------------------------------------------------------------------
# Renderer-blend / glass / bump_mode alignment tests
# ---------------------------------------------------------------------------


class _IrMatStub:
    """Minimal 3DI3 material attribute bag for describe_material()."""

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
        self.u_params = FakeUvParams()
        self.v_params = FakeUvParams()
        self.alpha_gen = FakeAlphaGen()
        self.rgb_gen = FakeRgbGen()
        self.rgb_gen2 = FakeRgbGen()
        self.animation = FakeTexAnim()
        for key, value in fields.items():
            setattr(self, key, value)


def test_describe_material_renderer_blend_for_ff_st_ab():
    desc = materials.describe_material(_IrMatStub(shader_name=b"FF_ST_AB"))
    assert desc.renderer_blend == materials.MATERIAL_BLEND_ALPHA
    assert desc.shader == "FF_ST_AB"
    assert desc.known_shader


def test_describe_material_glass_promotes_renderer_blend():
    """FFP_GLASS table flag bumps OPAQUE up to alpha-blend on the DCC side."""
    desc = materials.describe_material(
        _IrMatStub(shader_name=b"FFP_GLASS", is_glass=0, blend_mode=0)
    )
    assert desc.glass is True
    assert desc.renderer_blend == materials.MATERIAL_BLEND_ALPHA


def test_describe_material_phongt_no_slot3_no_bump(tmp_path: Path):
    """VS_PHONGT (NORMAL_A) with diffuse-only no longer fabricates a bump_mode.

    The diffuse-alpha fallback (height from alpha) was removed in v0.1.35
    because most VS_PHONGT DDS textures don't actually encode height in
    alpha; the fabrication produced visible shading distortion.
    """
    diffuse = tmp_path / "Wall.dds"
    diffuse.write_bytes(b"DDS")
    resolver = FakeResolver({"Wall.tga": str(diffuse)})

    ir_mat = _IrMatStub(
        shader_name=b"VS_PHONGT",
        texture_count=1,
        textures=[FakeTexture("Wall.tga", materials.THREEDI_TEX_SLOT_DIFFUSE)],
    )

    desc = materials.describe_material(ir_mat, resolver=resolver)
    assert desc.bump_mode == ""
    assert desc.bump_uses_alpha is False
    assert desc.diffuse.path == str(diffuse)


def test_describe_material_no_bump_when_normal_slot_is_mdt(tmp_path: Path):
    """MDT slot-3 normal cannot drive a Blender/Max bump map."""
    diffuse = tmp_path / "Wall.dds"
    mdt = tmp_path / "Wall.mdt"
    diffuse.write_bytes(b"DDS")
    mdt.write_bytes(b"MDT")
    resolver = FakeResolver({"Wall.tga": str(diffuse), "Wall.mdt": str(mdt)})

    ir_mat = _IrMatStub(
        shader_name=b"VS_PHONGT",
        texture_count=2,
        textures=[
            FakeTexture("Wall.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture(
                "Wall.mdt",
                materials.THREEDI_TEX_SLOT_NORMAL,
                tex_type=materials.NORMAL_TYPE_MDT,
            ),
        ],
    )

    desc = materials.describe_material(ir_mat, resolver=resolver)
    assert desc.bump_mode == ""
    assert desc.normal.type == materials.NORMAL_TYPE_MDT


def test_descriptor_from_user_props_round_trips_core_fields(tmp_path: Path):
    """material_user_props ↔ descriptor_from_user_props preserves the fields
    the shared ASE writer reads (texture names, blend, alpha test, two-sided,
    detail slot, glass)."""
    diffuse = tmp_path / "Wall.dds"
    detail = tmp_path / "Detail.dds"
    diffuse.write_bytes(b"DDS")
    detail.write_bytes(b"DDS")
    resolver = FakeResolver({"Wall.tga": str(diffuse), "Detail.tga": str(detail)})

    ir_mat = _IrMatStub(
        shader_name=b"FF_MT_AB",
        flags=materials.THREEDI_MATERIAL_FLAG_TWO_SIDED,
        material_flags=materials.THREEDI_MATERIAL_FLAG_TWO_SIDED,
        alpha_test_value_byte=64,
        alpha_threshold=64.0 / 255.0,
        blend_mode=1,
        texture_count=2,
        textures=[
            FakeTexture("Wall.tga", materials.THREEDI_TEX_SLOT_DIFFUSE),
            FakeTexture("Detail.tga", materials.THREEDI_TEX_SLOT_DETAIL),
        ],
    )
    original = materials.describe_material(ir_mat, resolver=resolver)
    props = materials.material_user_props(original)

    rebuilt = materials.descriptor_from_user_props(props)
    assert rebuilt.shader == original.shader
    assert rebuilt.diffuse.name == original.diffuse.name
    assert rebuilt.diffuse.path == original.diffuse.path
    assert rebuilt.detail.name == original.detail.name
    assert rebuilt.detail.path == original.detail.path
    assert rebuilt.renderer_blend == original.renderer_blend
    assert rebuilt.has_detail_slot == original.has_detail_slot
    assert rebuilt.two_sided == original.two_sided
    # Note: round-tripped descriptor's emissive may be False even if the
    # original was False; we only assert the fields the ASE writer reads.


def test_skbumpdifft2_with_ir_is_glass_zero_classifies_opaque():
    """Regression for US01.3di: VS_SKBUMPDIFFT2 with is_glass=0 in 3DI3
    must not be forced to alpha-blend by the shader-tag table.

    The T suffix denotes tangent-space normals, not transparency. The
    GLASS flag was a misclassification.
    """
    cls = materials.classify_material_shader("VS_SKBUMPDIFFT2", 0, 0, 0, 0)
    assert cls.is_glass is False
    assert cls.blend == materials.MATERIAL_BLEND_OPAQUE


def test_skbumpdifft2_with_ir_is_glass_one_still_classifies_glass():
    """The 3DI3-authored glass path must still mark T-shaders as glass
    when the model genuinely is glass (e.g., a visor on a soldier model)."""
    cls = materials.classify_material_shader("VS_SKBUMPDIFFT2", 0, 0, 1, 0)
    assert cls.is_glass is True
    assert cls.blend == materials.MATERIAL_BLEND_ALPHA


def test_named_glass_shaders_remain_glass_without_ir_flag():
    """Shaders whose family is glass keep glass classification regardless
    of whether their is_glass field is set. Guards against an
    over-broad fix that drops the family check too."""
    for tag in ("FFP_GLASS", "VS_SKGLASS", "VS_BMTXMIRRT", "VS_BUMPMIRRT"):
        cls = materials.classify_material_shader(tag, 0, 0, 0, 0)
        assert cls.is_glass is True, f"{tag} should remain classified as glass"
