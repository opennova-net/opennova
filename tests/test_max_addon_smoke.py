"""Smoke tests for the opennova_max package.

The actual Max-API interactions can only run inside 3ds Max. These
tests just verify the package imports cleanly without ``pymxs``
available, that the constructor matches BlenderSceneBuilder's
signature, and that calling Max-bound helpers raises a descriptive
error when ``pymxs`` is missing.
"""
from __future__ import annotations

import shutil
import uuid
from pathlib import Path
from unittest.mock import patch

import pytest


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"


def test_addon_package_imports_without_pymxs():
    import opennova_max
    import opennova_max.animation
    import opennova_max.scene_builder
    import opennova_max.mesh
    import opennova_max.materials
    import opennova_max.import_runner
    import opennova_max.output_writers
    import opennova_max.qt_ui
    import opennova_max.ui

    assert opennova_max.import_loose is not None
    assert opennova_max.run_loose_import is not None
    assert opennova_max.run_batch is not None
    assert opennova_max.show_importer is not None
    assert opennova_max.get_version() != ""
    assert opennova_max.scene_builder.MaxSceneBuilder is not None
    assert opennova_max.qt_ui.dialog_title("9.8.7") == "OpenNova Importer v9.8.7"


def test_max_mzp_package_script_smokes_startup_and_animation_modules():
    package_script = (ROOT / "scripts" / "package_max_mzp.ps1").read_text(encoding="utf-8")

    assert "traceback.print_exc()" in package_script
    assert "opennova_max\\animation.py" in package_script
    assert "opennova_max\\ui.py" in package_script
    assert "pyopennova\\animation_build.py" in package_script
    assert "import opennova_max.animation" in package_script
    assert "import opennova_max.ui" in package_script
    assert "import pyopennova.animation_build" in package_script
    assert 'macroScript OpenNovaImporter category:"OpenNova"' in package_script
    assert 'menuMan.createActionItem "OpenNovaImporter" "OpenNova"' in package_script


def test_max_editable_startup_installer_registers_menu_on_launch():
    startup_installer = (ROOT / "scripts" / "install_max_editable_startup.ps1").read_text(encoding="utf-8")

    assert "opennova_max_editable_startup.ms" in startup_installer
    assert "opennova_max_editable_startup.py" in startup_installer
    assert "python.executeFile" in startup_installer
    assert "opennova_max.register_menu()" in startup_installer
    assert "OpenNova Max {loaded_version} loaded from editable install" in startup_installer
    assert "traceback.print_exc()" in startup_installer


def test_max_version_resolution_prefers_packaged_version(monkeypatch):
    import sys
    import types

    from opennova_max import version

    packaged = types.ModuleType("opennova_max._packaged_version")
    packaged.VERSION = "9.8.7"
    monkeypatch.setitem(sys.modules, "opennova_max._packaged_version", packaged)

    assert version.get_version() == "9.8.7"


def test_max_version_resolution_prefers_source_over_stale_metadata(monkeypatch):
    from opennova_max import version

    monkeypatch.setattr(version, "_packaged_version", lambda: "")
    monkeypatch.setattr(version, "_source_version", lambda: "0.1.5")
    monkeypatch.setattr(version, "_metadata_version", lambda: "0.1.4")

    assert version.get_version() == "0.1.5"


def test_max_version_resolution_uses_source_fallback(monkeypatch):
    from opennova_max import version

    monkeypatch.setattr(version, "_packaged_version", lambda: "")
    monkeypatch.setattr(version, "_metadata_version", lambda: "")

    pyproject = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
    expected = next(
        line.split("=", 1)[1].strip().strip('"')
        for line in pyproject.splitlines()
        if line.startswith("version = ")
    )
    assert version.get_version() == expected


def test_max_version_resolution_has_unknown_fallback(monkeypatch):
    from opennova_max import version

    monkeypatch.setattr(version, "_packaged_version", lambda: "")
    monkeypatch.setattr(version, "_metadata_version", lambda: "")
    monkeypatch.setattr(version, "_source_version", lambda: "")

    assert version.get_version() == "unknown"


def test_max_scene_builder_constructor_matches_blender_signature():
    """MaxSceneBuilder must accept the same keyword args as BlenderSceneBuilder.

    The orchestration layer dispatches on host without touching call
    sites, so the constructors have to stay in sync.
    """
    import inspect

    from opennova_max.scene_builder import MaxSceneBuilder

    params = inspect.signature(MaxSceneBuilder.__init__).parameters
    expected = {
        "self",
        "ir",
        "bad_file",
        "anim_context",
        "resolver",
        "import_collisions",
        "import_occlusion",
        "import_lights",
    }
    assert set(params.keys()) == expected


def test_max_scene_builder_build_basic_scene_returns_false_for_zero_lod_ir():
    """No LODs in the IR means nothing to build; constructor must not
    require pymxs since this branch returns before any rt() call."""
    from opennova_max.scene_builder import MaxSceneBuilder

    class FakeIR:
        lod_count = 0
        material_count = 0

    builder = MaxSceneBuilder(FakeIR(), resolver=None)
    assert builder.build_basic_scene("anything") is False


def test_bad_transform_helpers_match_on3diimporter_row_order():
    from opennova_max import scene_builder

    class FakeBone:
        rotation = (
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0,
        )

    assert scene_builder._bad_rotation_rows(FakeBone()) == (
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    )
    assert scene_builder._max_bad_rotation_rows(FakeBone()) == (
        (0.0, -1.0, 0.0),
        (0.0, 0.0, 1.0),
        (-1.0, 0.0, 0.0),
    )


def test_max_animation_rows_match_bad_transform_helpers():
    from opennova_max import animation, scene_builder

    class FakeBone:
        rotation = (
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0,
        )

    assert animation.max_rows_from_source_quat((0.0, 0.0, 0.0, 1.0)) == (
        scene_builder._max_bad_rotation_rows(FakeBone())
    )

    half_sqrt = 2.0 ** -0.5

    class RotatedBone:
        rotation = (
            1.0, 0.0, 0.0,
            0.0, 0.0, 1.0,
            0.0, -1.0, 0.0,
        )

    rows = animation.max_rows_from_source_quat((half_sqrt, 0.0, 0.0, half_sqrt))
    expected = scene_builder._max_bad_rotation_rows(RotatedBone())
    for row, expected_row in zip(rows, expected):
        assert row == pytest.approx(expected_row)


def test_max_animation_applies_root_motion_to_parent_and_bones():
    from opennova_max import animation
    from pyopennova.animation_build import SampledBoneFrame, SampledClip, SampledFrame

    class FakeRuntime:
        def __init__(self):
            self.sliderTime = None
            self.frameRate = None
            self.animationRange = None

        def Point3(self, x, y, z):
            return (float(x), float(y), float(z))

        def matrix3(self, row1, row2, row3, position):
            return {
                "rows": (row1, row2, row3),
                "position": position,
            }

        def interval(self, start, end):
            return (start, end)

    class FakeNode:
        def __init__(self, rt):
            object.__setattr__(self, "_rt", rt)
            object.__setattr__(self, "keys", [])

        def __setattr__(self, name, value):
            if name == "transform":
                self.keys.append((self._rt.sliderTime, value))
            object.__setattr__(self, name, value)

    rt = FakeRuntime()
    root_motion = FakeNode(rt)
    bone = FakeNode(rt)
    clip = SampledClip(
        name="walk",
        bad_name="walk",
        flags=0,
        fps=24,
        frame_count=1,
        start_frame=7,
        end_frame=7,
        is_reset=False,
        frames=(
            SampledFrame(
                frame_index=0,
                frame=7,
                root_motion_position=(1.0, 2.0, 3.0),
                max_root_motion_position=(4.0, 5.0, 6.0),
                bones=(
                    SampledBoneFrame(
                        bone_index=0,
                        name="BN01",
                        parent_index=-1,
                        world_rotation=(1.0, 0.0, 0.0, 0.0),
                        world_position=(10.0, 20.0, 30.0),
                        local_rotation=(1.0, 0.0, 0.0, 0.0),
                        local_position=(10.0, 20.0, 30.0),
                        source_rotation_xyzw=(0.0, 0.0, 0.0, 1.0),
                    ),
                ),
            ),
        ),
    )

    animation.apply_sampled_clips(rt, (clip,), (bone,), root_motion)

    assert rt.frameRate == 24
    assert rt.animationRange == (1, 1)
    assert root_motion.keys[0][0] == 1
    assert root_motion.keys[0][1]["position"] == (4.0, 5.0, 6.0)
    assert bone.keys[0][0] == 1
    assert bone.keys[0][1]["position"] == (14.0, 25.0, 36.0)


def test_max_animation_rebases_timeline_and_uses_reset_at_frame_zero():
    from opennova_max import animation
    from pyopennova.animation_build import SampledBoneFrame, SampledClip, SampledFrame

    class FakeRuntime:
        def __init__(self):
            self.sliderTime = None
            self.frameRate = None
            self.animationRange = None
            self.redraw_disabled = 0
            self.redraw_enabled = 0

        def Point3(self, x, y, z):
            return (float(x), float(y), float(z))

        def matrix3(self, row1, row2, row3, position):
            return {"rows": (row1, row2, row3), "position": position}

        def interval(self, start, end):
            return (start, end)

        def disableSceneRedraw(self):
            self.redraw_disabled += 1

        def enableSceneRedraw(self):
            self.redraw_enabled += 1

    class FakeNode:
        def __init__(self, rt):
            object.__setattr__(self, "_rt", rt)
            object.__setattr__(self, "keys", [])

        def __setattr__(self, name, value):
            if name == "transform":
                self.keys.append((self._rt.sliderTime, value))
            object.__setattr__(self, name, value)

    def bone_frame(position):
        return SampledBoneFrame(
            bone_index=0,
            name="BN01",
            parent_index=-1,
            world_rotation=(1.0, 0.0, 0.0, 0.0),
            world_position=position,
            local_rotation=(1.0, 0.0, 0.0, 0.0),
            local_position=position,
            source_rotation_xyzw=(0.0, 0.0, 0.0, 1.0),
        )

    reset = SampledClip(
        name="anim_reset",
        bad_name="reset",
        flags=0,
        fps=30,
        frame_count=3,
        start_frame=1,
        end_frame=3,
        is_reset=True,
        frames=(
            SampledFrame(
                frame_index=0,
                frame=1,
                max_root_motion_position=(99.0, 99.0, 7.0),
                bones=(bone_frame((1.0, 2.0, 3.0)),),
            ),
        ),
    )
    walk = SampledClip(
        name="walk",
        bad_name="walk",
        flags=0,
        fps=30,
        frame_count=2,
        start_frame=4,
        end_frame=5,
        is_reset=False,
        frames=(
            SampledFrame(
                frame_index=0,
                frame=4,
                max_root_motion_position=(10.0, 0.0, 1.0),
                bones=(bone_frame((1.0, 0.0, 0.0)),),
            ),
            SampledFrame(
                frame_index=1,
                frame=5,
                max_root_motion_position=(20.0, 0.0, 2.0),
                bones=(bone_frame((2.0, 0.0, 0.0)),),
            ),
        ),
    )

    rt = FakeRuntime()
    root_motion = FakeNode(rt)
    bone = FakeNode(rt)

    animation.apply_sampled_clips(rt, (reset, walk), (bone,), root_motion)

    assert rt.animationRange == (1, 2)
    assert rt.redraw_disabled == 1
    assert rt.redraw_enabled == 1
    assert [key[0] for key in root_motion.keys] == [0, 1, 2]
    assert root_motion.keys[0][1]["position"] == (0.0, 0.0, 7.0)
    assert bone.keys[0][1]["position"] == (1.0, 2.0, 10.0)
    assert root_motion.keys[1][1]["position"] == (10.0, 0.0, 1.0)
    assert root_motion.keys[2][1]["position"] == (20.0, 0.0, 2.0)


def test_max_scene_builder_animation_delegate_uses_shared_keyer(monkeypatch):
    from types import SimpleNamespace

    from opennova_max import animation, scene_builder
    from opennova_max.scene_builder import MaxSceneBuilder

    builder = object.__new__(MaxSceneBuilder)
    builder.armature_object = object()
    builder.bone_nodes = [object()]
    builder._bone_infos = [("BN01", -1, (0.0, 0.0, 0.0))]
    builder.root_motion_node = object()
    ctx = object()
    rt = object()
    calls = {}

    def fake_key_animation_context(*args):
        calls["args"] = args
        return SimpleNamespace(warnings=())

    monkeypatch.setattr(scene_builder, "_rt", lambda: rt)
    monkeypatch.setattr(animation, "key_animation_context", fake_key_animation_context)

    builder.build_animations_from_context(ctx)

    assert calls["args"] == (
        rt,
        ctx,
        builder.armature_object,
        builder.bone_nodes,
        builder._bone_infos,
        builder.root_motion_node,
    )


def test_max_bad_armature_creates_root_motion_parent(monkeypatch):
    from opennova_max import scene_builder
    from opennova_max.scene_builder import MaxSceneBuilder

    class FakeNode:
        def __init__(self):
            self.name = ""
            self.parent = None
            self.position = None

    class FakeRuntime:
        def Dummy(self):
            return FakeNode()

        def Point3(self, x, y, z):
            return (float(x), float(y), float(z))

    class FakeIR:
        lod_count = 0

    class FakeBone:
        def __init__(self, name, parent_index):
            self.name = name
            self.parent_index = parent_index
            self.position = (1.0, 2.0, 3.0)
            self.rotation = (
                1.0, 0.0, 0.0,
                0.0, 1.0, 0.0,
                0.0, 0.0, 1.0,
            )
            self.length = 0.25

    class FakeBad:
        num_bones = 2
        bones = (
            FakeBone(b"Root", -1),
            FakeBone(b"Child", 0),
        )

    def fake_create_bad_bone(_rt, name, _bone, start):
        node = FakeNode()
        node.name = name
        node.position = start
        return node

    builder = MaxSceneBuilder(FakeIR(), resolver=None)
    monkeypatch.setattr(scene_builder, "_rt", lambda: FakeRuntime())
    monkeypatch.setattr(scene_builder, "_create_bad_bone", fake_create_bad_bone)

    builder.build_armature_from_bad(FakeBad(), "ship")

    assert builder.armature_object.name == "Bip001"
    assert builder.root_motion_node is builder.armature_object
    assert builder.bone_nodes[0].parent is builder.armature_object
    assert builder.bone_nodes[1].parent is builder.bone_nodes[0]


def test_skin_weight_remap_uses_explicit_skin_bone_ids():
    from opennova_max.scene_builder import _remap_skin_weight_entries

    bone_indices, weights = _remap_skin_weight_entries(
        [(0, 0.5), (2, 0.25), (99, 1.0), (1, 0.0001)],
        {0: 3, 1: 7, 2: 9},
    )

    assert bone_indices == [3, 9]
    assert weights == [0.5, 0.25]


def test_parented_local_position_to_world_adds_parent_translation():
    from opennova_max.mesh import parented_local_position_to_world

    class Point:
        x = 10.0
        y = -2.0
        z = 5.5

    class Transform:
        position = Point()

    class Parent:
        transform = Transform()

    assert parented_local_position_to_world(Parent(), (1.0, 2.0, 3.0)) == (
        11.0,
        0.0,
        8.5,
    )
    assert parented_local_position_to_world(None, (1.0, 2.0, 3.0)) == (
        1.0,
        2.0,
        3.0,
    )


def test_max_material_records_texture_resolution_diagnostics(tmp_path: Path):
    from opennova_max import materials

    diffuse = tmp_path / "Diffuse.tga"
    diffuse.write_bytes(b"fake")

    class FakeTexture:
        def __init__(self, slot: int, name: str, tex_type: int = 0):
            self.slot = slot
            self.name = name.encode("utf-8")
            self.type = tex_type

    class FakeMaterialIR:
        index = 2
        shader_name = b"FF_ST_OP"
        flags = 0x01
        blend_mode = 0
        luminosity = 0
        alpha_threshold = 0.5
        texture_count = 2
        textures = [
            FakeTexture(materials.THREEDI_IR_TEX_SLOT_DIFFUSE, diffuse.name),
            FakeTexture(materials.THREEDI_IR_TEX_SLOT_DETAIL, "MissingDetail.tga"),
        ]

    class FakeResolver:
        def resolve_texture(self, name: str) -> str | None:
            if name.lower() == diffuse.name.lower():
                return str(diffuse)
            return None

    class FakeBitmap:
        def __init__(self, filename: str = ""):
            self.filename = filename
            self.name = ""

    class FakeRuntime:
        def StandardMaterial(self, name: str = ""):
            mat = type("FakeStandardMaterial", (), {})()
            mat.name = name
            mat.user_props = {}
            return mat

        def BitmapTexture(self, filename: str = ""):
            return FakeBitmap(filename)

        def color(self, r: int, g: int, b: int):
            return (r, g, b)

        def setUserProp(self, obj, key: str, value):
            obj.user_props[key] = value

        def showTextureMap(self, mat, bitmap, enabled: bool):
            mat.texture_shown = (bitmap, enabled)

    with patch("opennova_max.materials._rt", return_value=FakeRuntime()):
        with patch("opennova_max.materials._store_generator_props"):
            mat = materials.create_material(FakeMaterialIR(), resolver=FakeResolver())

    assert mat.diffuseMap.filename == str(diffuse)
    assert mat.diffuseMapEnable is True
    assert mat.opacityMap.filename == str(diffuse)
    assert mat.opacityMapEnable is True
    assert mat.opacityType == 2
    assert mat.user_props["opennova_diffuse_texture_path"] == str(diffuse)
    assert mat.user_props["opennova_diffuse_texture_missing"] == 0
    assert mat.user_props["opennova_detail_texture_path"] == ""
    assert mat.user_props["opennova_detail_texture_missing"] == 1
    assert mat.user_props["opennova_alpha_threshold"] == 0.5


def test_max_material_uses_normalized_bitmap_path_for_misnamed_dds(tmp_path: Path):
    from pyopennova.asset_resolver import AssetResolver
    from opennova_max import materials

    source = tmp_path / "Diffuse.TGA"
    source.write_bytes(b"DDS " + bytes(range(32)))

    class FakeTexture:
        slot = materials.THREEDI_IR_TEX_SLOT_DIFFUSE
        name = b"Diffuse.tga"
        type = 0

    class FakeMaterialIR:
        index = 3
        shader_name = b"FF_ST_OP"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 1
        textures = [FakeTexture()]

    class FakeBitmap:
        def __init__(self, filename: str = ""):
            self.filename = filename
            self.name = ""

    class FakeRuntime:
        def StandardMaterial(self, name: str = ""):
            mat = type("FakeStandardMaterial", (), {})()
            mat.name = name
            mat.user_props = {}
            return mat

        def BitmapTexture(self, filename: str = ""):
            return FakeBitmap(filename)

        def color(self, r: int, g: int, b: int):
            return (r, g, b)

        def setUserProp(self, obj, key: str, value):
            obj.user_props[key] = value

        def showTextureMap(self, mat, bitmap, enabled: bool):
            mat.texture_shown = (bitmap, enabled)

    with AssetResolver(str(tmp_path)) as resolver:
        with patch("opennova_max.materials._rt", return_value=FakeRuntime()):
            with patch("opennova_max.materials._store_generator_props"):
                mat = materials.create_material(FakeMaterialIR(), resolver=resolver)

    assert mat.diffuseMap.filename.endswith("Diffuse.dds")
    assert Path(mat.diffuseMap.filename).read_bytes() == source.read_bytes()
    assert mat.user_props["ase_diffuse_bitmap"] == "Diffuse.tga"
    assert mat.user_props["opennova_diffuse_texture_name"] == "Diffuse.tga"
    assert mat.user_props["opennova_diffuse_texture_path"].endswith("Diffuse.dds")


def test_max_material_keeps_primary_diffuse_map_for_detail_materials(tmp_path: Path):
    from opennova_max import materials

    diffuse = tmp_path / "Base.dds"
    detail = tmp_path / "Detail.dds"
    diffuse.write_bytes(b"DDS " + bytes(range(32)))
    detail.write_bytes(b"DDS " + bytes(range(32)))

    class FakeTexture:
        def __init__(self, slot: int, name: str):
            self.slot = slot
            self.name = name.encode("utf-8")
            self.type = 0

    class FakeMaterialIR:
        index = 4
        shader_name = b"FF_ST_OP"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 2
        textures = [
            FakeTexture(materials.THREEDI_IR_TEX_SLOT_DIFFUSE, diffuse.name),
            FakeTexture(materials.THREEDI_IR_TEX_SLOT_DETAIL, detail.name),
        ]

    class FakeResolver:
        def resolve_texture(self, name: str) -> str | None:
            if name.lower() == diffuse.name.lower():
                return str(diffuse)
            if name.lower() == detail.name.lower():
                return str(detail)
            return None

    class FakeBitmap:
        def __init__(self, filename: str = ""):
            self.filename = filename
            self.name = ""

    class FakeRuntime:
        def __init__(self):
            self.bitmap_paths = []

        def StandardMaterial(self, name: str = ""):
            mat = type("FakeStandardMaterial", (), {})()
            mat.name = name
            mat.user_props = {}
            return mat

        def BitmapTexture(self, filename: str = ""):
            self.bitmap_paths.append(filename)
            return FakeBitmap(filename)

        def color(self, r: int, g: int, b: int):
            return (r, g, b)

        def setUserProp(self, obj, key: str, value):
            obj.user_props[key] = value

        def showTextureMap(self, mat, bitmap, enabled: bool):
            mat.texture_shown = (bitmap, enabled)

    runtime = FakeRuntime()
    with patch("opennova_max.materials._rt", return_value=runtime):
        with patch("opennova_max.materials._store_generator_props"):
            mat = materials.create_material(FakeMaterialIR(), resolver=FakeResolver())

    assert mat.diffuseMap.filename == str(diffuse)
    assert mat.diffuseMapEnable is True
    assert mat.texture_shown == (mat.diffuseMap, True)
    assert runtime.bitmap_paths == [str(diffuse)]
    assert mat.user_props["opennova_has_diffuse_map"] == 1
    assert mat.user_props["opennova_detail_texture_path"] == str(detail)
    assert mat.user_props["opennova_detail_texture_missing"] == 0
    assert mat.user_props["opennova_has_detail_map"] == 1
    assert mat.user_props["opennova_detail_map_mode"] == "metadata_only"


def test_calling_rt_outside_max_raises_descriptive_error():
    """The lazy pymxs accessor must explain why it failed when the
    user tries to run the addon outside Max."""
    from opennova_max.mesh import _rt

    # Simulate "pymxs not installed" without actually uninstalling pymxs:
    # if pymxs IS available (e.g. inside Max), the import succeeds and we
    # can't drive the failure path. Skip in that case.
    try:
        import pymxs  # noqa: F401
    except ImportError:
        with pytest.raises(RuntimeError, match="3ds Max"):
            _rt()
    else:
        pytest.skip("pymxs is importable; cannot exercise the failure path")


def test_max_qt_ui_helpers_and_menu_script_are_ci_safe():
    from apps.importer.jobs import ScanItem
    from opennova_max import qt_ui
    from opennova_max import ui

    menu_script = ui.build_menu_script()

    assert 'macroScript OpenNovaImporter category:"OpenNova"' in menu_script
    assert 'ui.show_importer()' in menu_script
    assert "menuMan.registerMenuContext" in menu_script
    assert 'menuMan.createSubMenuItem "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaImporter" "OpenNova"' in menu_script
    assert 'importItem.setTitle "Importer..."' in menu_script
    assert "OpenNovaImporterRollout" not in menu_script
    assert qt_ui.dialog_title("9.8.7") == "OpenNova Importer v9.8.7"
    assert qt_ui.DEFAULT_SCENE_OPTIONS == {
        "collisions": True,
        "occlusion": True,
        "lights": True,
        "arms": True,
        "animations": False,
    }
    assert qt_ui.RESOURCE_COLUMNS == ("Type", "Name", "Model", "Output")
    assert qt_ui.resource_table_row(
        ui.DefinitionScanItem("M16A2", "weapon", "m16_1st.3di", "m16_1st")
    ) == ("weapon", "M16A2", "m16_1st.3di", "m16_1st")
    assert qt_ui.can_import_loose("C:/asset.3di", "C:/out", True, False, False)
    assert qt_ui.can_import_loose(["C:/a.3di", "C:/b.3di"], "C:/out", True, False, False)
    assert qt_ui.loose_paths_display(["C:/a.3di", "C:/b.3di"]) == "2 files selected"
    assert not qt_ui.can_import_loose("", "C:/out", True, False, False)
    assert not qt_ui.can_import_loose([], "C:/out", True, False, False)
    assert not qt_ui.can_import_loose("C:/asset.3di", "C:/out", False, False, False)
    assert qt_ui.can_import_definition(
        has_selection=True,
        output_root="C:/out",
        write_ase=True,
        write_3dp=False,
        write_max=False,
    )
    assert not qt_ui.can_import_batch(
        visible_count=0,
        output_root="C:/out",
        write_ase=True,
        write_3dp=False,
        write_max=False,
    )
    assert ui._format_scan_item(
        ScanItem(name="M16A2", type="weapon", source_model="m16_1st.3di", output_stem="m16_1st")
    ) == "[weapon] M16A2 -> m16_1st"
    assert ui._scan_definitions("") == (False, [], "Game directory is required.")


def test_show_importer_uses_qt_dialog(monkeypatch):
    from opennova_max import qt_ui, ui

    calls = []
    monkeypatch.setattr(qt_ui, "show_importer_dialog", lambda: calls.append("qt") or True)

    assert ui.show_importer() is True
    assert calls == ["qt"]
    assert not hasattr(ui, "show_legacy_importer")


def test_show_importer_propagates_qt_errors(monkeypatch):
    from opennova_max import qt_ui, ui

    def fail():
        raise RuntimeError("Qt unavailable")

    monkeypatch.setattr(qt_ui, "show_importer_dialog", fail)
    with pytest.raises(RuntimeError, match="Qt unavailable"):
        ui.show_importer()


def test_max_ui_resource_filters_type_and_search():
    from opennova_max import ui

    items = [
        ui.DefinitionScanItem("M16A2 Rifle", "weapon", "m16_1st.3di", "m16_1st"),
        ui.DefinitionScanItem("Desktop Fan with moving blades", "item", "Fan02.3di", "Fan02"),
        ui.DefinitionScanItem("Somalian RPG Soldier", "item", "ESomal03.3di", "ESomal03"),
    ]

    assert [item.name for item in ui._filtered_items(items, "weapon")] == ["M16A2 Rifle"]
    assert [item.output_stem for item in ui._filtered_items(items, "item", "fan")] == ["Fan02"]
    assert [item.output_stem for item in ui._filtered_items(items, "all", "rpg esomal")] == ["ESomal03"]
    assert [item.output_stem for item in ui._filtered_items(items, "all", "m16a2")] == ["m16_1st"]
    assert [item.output_stem for item in ui._filtered_items(items, "all", "m16-1st")] == ["m16_1st"]
    assert [item.output_stem for item in ui._filtered_items(items, "all", "m16rif")] == ["m16_1st"]
    assert [item.output_stem for item in ui._filtered_items(items, "all", "esoml")] == ["ESomal03"]
    assert [item.output_stem for item in ui._filtered_items(items, "all", "som 03")] == ["ESomal03"]
    assert ui._filtered_items(items, "item", "m16") == []
    assert ui._filtered_items(items, "all", "rifle fan") == []


def test_max_ui_definition_and_loose_validation_statuses():
    from opennova_max import ui

    item = ui.DefinitionScanItem("M16A2 Rifle", "weapon", "m16_1st.3di", "m16_1st")
    with patch("opennova_max.ui._set_status") as set_status:
        ok = ui._run_definition_item(
            item,
            "",
            "C:/out",
            import_arms=True,
            import_animations=True,
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            write_ase=True,
            write_3dp=True,
            write_max=True,
        )
    assert not ok
    set_status.assert_called_once_with("Definition import requires a game directory.")

    with patch("opennova_max.ui._set_status") as set_status:
        ok = ui._run_loose_from_ui(
            "",
            "",
            "",
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            write_ase=True,
            write_3dp=True,
            write_max=True,
        )
    assert not ok
    set_status.assert_called_once_with("Loose import requires a .3di path and output root.")


def test_max_ui_loose_single_import_uses_output_root(tmp_path: Path):
    from opennova_max import ui

    model = tmp_path / "Shed.3di"
    asset_dir = tmp_path / "assets"
    output_root = tmp_path / "out"
    with patch("opennova_max.import_runner.run_loose_import", return_value=True) as run_loose:
        with patch("opennova_max.ui._set_status") as set_status:
            ok = ui._run_loose_from_ui(
                str(model),
                str(asset_dir),
                str(output_root),
                import_collisions=True,
                import_occlusion=False,
                import_lights=True,
                write_ase=True,
                write_3dp=False,
                write_max=True,
            )

    assert ok
    run_loose.assert_called_once_with(
        str(model),
        str(output_root / "Shed"),
        output_stem="Shed",
        asset_base_dir=str(asset_dir),
        import_collisions=True,
        import_occlusion=False,
        import_lights=True,
        write_ase=True,
        write_3dp=False,
        write_max=True,
        reset_scene=True,
    )
    set_status.assert_called_once_with(f"Loose import complete -> {output_root / 'Shed'}")


def test_max_ui_loose_batch_uses_per_model_output_dirs(tmp_path: Path):
    from opennova_max import ui

    models = [tmp_path / "Shed.3di", tmp_path / "JetSki.3di"]
    asset_dir = tmp_path / "assets"
    output_root = tmp_path / "out"
    result_type = type("Result", (), {})
    results = [result_type(), result_type()]
    results[0].ok = True
    results[1].ok = True

    with patch("opennova_max.import_runner.run_batch", return_value=results) as run_batch:
        with patch("opennova_max.ui._set_status") as set_status:
            ok = ui._run_loose_from_ui(
                [str(path) for path in models],
                str(asset_dir),
                str(output_root),
                import_collisions=False,
                import_occlusion=True,
                import_lights=False,
                write_ase=False,
                write_3dp=True,
                write_max=False,
            )

    assert ok
    run_batch.assert_called_once()
    requests = run_batch.call_args.args[0]
    assert [request.resource for request in requests] == [str(path) for path in models]
    assert [request.output_dir for request in requests] == [
        str(output_root / "Shed"),
        str(output_root / "JetSki"),
    ]
    assert [request.output_stem for request in requests] == ["Shed", "JetSki"]
    assert all(request.base_dir == str(asset_dir) for request in requests)
    assert all(request.import_collisions is False for request in requests)
    assert all(request.import_occlusion is True for request in requests)
    assert all(request.import_lights is False for request in requests)
    assert all(request.write_ase is False for request in requests)
    assert all(request.write_3dp is True for request in requests)
    assert all(request.write_max is False for request in requests)
    set_status.assert_called_once_with(f"Loose batch complete: 2/2 files -> {output_root}")


def test_max_ui_loose_batch_rejects_duplicate_output_stems(tmp_path: Path):
    from opennova_max import ui

    paths = [tmp_path / "Shed.3di", tmp_path / "nested" / "Shed.3di"]
    with patch("opennova_max.import_runner.run_batch") as run_batch:
        with patch("opennova_max.ui._set_status") as set_status:
            ok = ui._run_loose_from_ui(
                [str(path) for path in paths],
                "",
                str(tmp_path / "out"),
                import_collisions=True,
                import_occlusion=True,
                import_lights=True,
                write_ase=True,
                write_3dp=True,
                write_max=True,
            )

    assert not ok
    run_batch.assert_not_called()
    set_status.assert_called_once_with("Loose import has duplicate output names: Shed")


def test_max_ui_loose_rejects_non_3di_paths(tmp_path: Path):
    from opennova_max import ui

    with patch("opennova_max.ui._set_status") as set_status:
        ok = ui._run_loose_from_ui(
            str(tmp_path / "Shed.txt"),
            "",
            str(tmp_path / "out"),
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            write_ase=True,
            write_3dp=True,
            write_max=True,
        )

    assert not ok
    set_status.assert_called_once_with(f"Loose imports require .3di files: {tmp_path / 'Shed.txt'}")


def test_shared_host_neutral_writer_produces_outputs_without_max():
    if not FIXTURE_3DI.is_file():
        pytest.skip("fixture missing (LFS not pulled?)")

    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.threedi_ffi import free_model_ir, read_model_ir

    out_dir = ROOT / f".tmp_test_host_outputs_{uuid.uuid4().hex}"
    out_dir.mkdir()
    ir = read_model_ir(str(FIXTURE_3DI))
    try:
        written = write_host_neutral_outputs(ir, str(out_dir), "Shed")
        names = {Path(path).name for path in written}
        assert "Shed.3dp" in names
        assert "Shed.3da" in names
        assert "Shed.ase" in names
        assert "Shed_bullet.ase" in names
        for path in written:
            assert Path(path).stat().st_size > 0
    finally:
        free_model_ir(ir)
        shutil.rmtree(out_dir, ignore_errors=True)


def test_max_native_batch_request_uses_exact_loose_output_dir():
    from opennova_max.import_runner import MaxImportRequest, execute_import_request

    request = MaxImportRequest(
        resource=str(FIXTURE_3DI),
        output_dir=str(ROOT / "out" / "Shed"),
    )
    with patch(
        "opennova_max.import_runner._run_loose_import_impl",
        return_value=[str(ROOT / "out" / "Shed" / "Shed.ase")],
    ) as run_loose:
        result = execute_import_request(request)

    assert result.ok
    assert result.output_path == str(ROOT / "out" / "Shed")
    assert run_loose.call_args.kwargs["output_dir"] == str(ROOT / "out" / "Shed")
    assert run_loose.call_args.kwargs["write_max"] is True


def test_max_native_batch_request_can_disable_max_output():
    from opennova_max.import_runner import MaxImportRequest, execute_import_request

    request = MaxImportRequest(
        resource=str(FIXTURE_3DI),
        output_dir=str(ROOT / "out" / "Shed"),
        write_max=False,
    )
    with patch(
        "opennova_max.import_runner._run_loose_import_impl",
        return_value=[str(ROOT / "out" / "Shed" / "Shed.ase")],
    ) as run_loose:
        result = execute_import_request(request)

    assert result.ok
    assert run_loose.call_args.kwargs["write_max"] is False


def test_max_output_writer_can_save_max_only(tmp_path: Path):
    from opennova_max import output_writers

    max_path = str(tmp_path / "Shed.max")
    with patch("opennova_max.output_writers.write_host_neutral_outputs") as shared:
        with patch("opennova_max.output_writers.save_max_scene", return_value=max_path) as save:
            written = output_writers.write_outputs(
                object(),
                str(tmp_path),
                "Shed",
                object(),
                write_ase=False,
                write_3dp=False,
                write_max=True,
            )

    assert written == [max_path]
    shared.assert_not_called()
    save.assert_called_once_with(str(tmp_path), "Shed")


def test_max_output_writer_delegates_host_neutral_outputs(tmp_path: Path):
    from opennova_max import output_writers

    ir = object()
    bad_file = object()
    shared_path = str(tmp_path / "Shed.ase")
    max_path = str(tmp_path / "Shed.max")

    builder = type("Builder", (), {
        "import_collisions": False,
        "import_occlusion": True,
        "import_lights": False,
        "bullet_lod_index": 4,
        "bad_file": bad_file,
    })()

    with patch(
        "opennova_max.output_writers.write_host_neutral_outputs",
        return_value=[shared_path],
    ) as shared:
        with patch("opennova_max.output_writers.save_max_scene", return_value=max_path):
            written = output_writers.write_outputs(
                ir,
                str(tmp_path),
                "Shed",
                builder,
                write_ase=True,
                write_3dp=False,
                write_max=True,
            )

    assert written == [shared_path, max_path]
    shared.assert_called_once_with(
        ir,
        str(tmp_path),
        "Shed",
        write_ase=True,
        write_3dp=False,
        include_collisions=False,
        include_occlusion=True,
        include_lights=False,
        bad_file=bad_file,
        bullet_lod_index=4,
    )
