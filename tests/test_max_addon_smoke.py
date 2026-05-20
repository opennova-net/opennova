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
from types import SimpleNamespace
from unittest.mock import patch

import pytest


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"


class _FakeOneBasedList:
    def __init__(self, size: int):
        self.values = [None] * int(size)

    def __setitem__(self, index: int, value):
        idx = int(index)
        if 1 <= idx <= len(self.values):
            self.values[idx - 1] = value
            return
        if 0 <= idx < len(self.values):
            self.values[idx] = value
            return
        raise IndexError(idx)


class _FakeZeroBasedList:
    def __init__(self, size: int, initial=None):
        if initial is None:
            self.values = [None] * int(size)
        else:
            self.values = list(initial)

    def __setitem__(self, index: int, value):
        idx = int(index)
        if 0 <= idx < len(self.values):
            self.values[idx] = value
            return
        raise IndexError(idx)


class _FakeMaxMaterial:
    def __init__(self, name: str):
        self.name = name
        self.user_props = {}


class _FakeMaxMultiMaterial:
    def __init__(self, numsubs: int):
        self.name = ""
        self.numsubs = int(numsubs)
        self.materialList = _FakeOneBasedList(numsubs)
        self.materialIDList = _FakeZeroBasedList(
            numsubs,
            initial=range(1, int(numsubs) + 1),
        )
        self.user_props = {}


class _FakeMaxMesh:
    def __init__(self, vertices=None, faces=None):
        self.name = ""
        self.vertices = list(vertices or [])
        self.faces = list(faces or [])
        self.face_mat_ids = {}
        self.smoothing_groups = {}
        self.user_props = {}
        self.modifiers = []


class _FakeSkin:
    def __init__(self):
        self.bones = []
        self.weights = {}


class _FakeSkinOps:
    def addBone(self, skin, bone, bone_id):
        skin.bones.append((bone, int(bone_id)))

    def ReplaceVertexWeights(self, skin, vertex_index, bone_indices, weights):
        skin.weights[int(vertex_index)] = (list(bone_indices), list(weights))


class _FakeMeshOps:
    def getNumMaps(self, _mesh):
        return 0

    def setNumMaps(self, _mesh, _count, keep=True):
        return None

    def setMapSupport(self, _mesh, _channel, _enabled):
        return None

    def setNumMapVerts(self, _mesh, _channel, _count):
        return None

    def setMapVert(self, _mesh, _channel, _index, _value):
        return None

    def setNumMapFaces(self, _mesh, _channel, _count):
        return None

    def setMapFace(self, _mesh, _channel, _index, _value):
        return None


class _FakeMaxRuntime:
    def __init__(self):
        self.skinOps = _FakeSkinOps()
        self.meshop = _FakeMeshOps()

    def Point3(self, x, y, z):
        return (float(x), float(y), float(z))

    def mesh(self, *, vertices, faces):
        return _FakeMaxMesh(vertices, faces)

    def MultiMaterial(self, numsubs: int):
        return _FakeMaxMultiMaterial(numsubs)

    def multimaterial(self, numsubs: int):
        return _FakeMaxMultiMaterial(numsubs)

    def setSubMtl(self, multi, slot, mat):
        multi.materialList.__setitem__(slot, mat)

    def setUserProp(self, obj, key: str, value):
        obj.user_props[key] = value

    def getUserProp(self, obj, key: str):
        return obj.user_props.get(key)

    def setFaceMatID(self, mesh, face_index, material_id):
        mesh.face_mat_ids[int(face_index)] = int(material_id)

    def setFaceSmoothGroup(self, mesh, face_index, smoothing_group):
        mesh.smoothing_groups[int(face_index)] = int(smoothing_group)

    def getNumFaces(self, mesh):
        return len(mesh.faces)

    def Skin(self):
        return _FakeSkin()

    def addModifier(self, mesh, modifier):
        mesh.modifiers.append(modifier)
        mesh.face_mat_ids = {idx: 1 for idx in range(1, len(mesh.faces) + 1)}

    def Array(self, *values):
        return list(values)


def test_addon_package_imports_without_pymxs():
    import opennova_max
    import opennova_max.animation
    import opennova_max.anim_exporter
    import opennova_max.ase_scene_exporter
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
    assert opennova_max.export_ase is not None
    assert opennova_max.export_anims is not None
    assert opennova_max.get_version() != ""
    assert opennova_max.scene_builder.MaxSceneBuilder is not None
    assert opennova_max.qt_ui.dialog_title("9.8.7") == "OpenNova Importer v9.8.7"


def test_max_dcc_roundtrip_helper_imports_without_pymxs():
    from pyopennova.legacy_tools import max_dcc_roundtrip

    assert max_dcc_roundtrip.STOCK_FIXTURES == (
        "akcrate",
        "Armry01",
        "Beret",
        "mp5_1st",
        "US01",
    )
    assert max_dcc_roundtrip.run_all is not None


def test_max_dcc_batch_runner_scripts_are_present():
    runner = ROOT / "scripts" / "max_dcc_roundtrip_runner.py"
    wrapper = ROOT / "scripts" / "test_max_dcc.ps1"
    pytest_harness = ROOT / "tests" / "test_max_dcc_roundtrip.py"

    runner_text = runner.read_text(encoding="utf-8")
    wrapper_text = wrapper.read_text(encoding="utf-8")
    pytest_text = pytest_harness.read_text(encoding="utf-8")

    assert "pyopennova.legacy_tools.max_dcc_roundtrip" in runner_text
    assert "OPENNOVA_MAX_DCC_RESULT" in runner_text
    assert '"successes": successes' in runner_text
    assert "OPENNOVA_3DSMAXBATCH" in wrapper_text
    assert "OPENNOVA_MAX_DCC_OUTPUT_ROOT" in wrapper_text
    assert "$summary.failed -eq $false" in wrapper_text
    assert "OPENNOVA_MAX_DCC_FIXTURES" in runner_text
    assert "3dsmaxbatch.exe" in wrapper_text
    assert "max_dcc_roundtrip_runner.py" in wrapper_text
    assert "subprocess.run" in pytest_text
    assert "pytest.importorskip(\"pymxs\"" not in pytest_text


def test_max_dependency_group_is_for_local_harness_not_pymxs():
    pyproject = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
    assert "pymxs is supplied by 3ds Max's bundled Python" in pyproject
    assert "max = [" in pyproject

    max_group = pyproject.split("max = [", 1)[1].split("]", 1)[0]
    assert '"pytest>=8"' in max_group
    assert "pymxs" not in max_group.lower()


def test_max_mzp_package_script_smokes_startup_and_animation_modules():
    package_script = (ROOT / "scripts" / "package_max_mzp.ps1").read_text(encoding="utf-8")
    cmake_lists = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    assert "traceback.print_exc()" in package_script
    assert "option(OPENNOVA_STAGE_PYTHON_LIB" in cmake_lists
    assert "-DOPENNOVA_STAGE_PYTHON_LIB=OFF" in package_script
    assert "Contents/macroscripts/OpenNovaImporter.mcr" in package_script
    assert "OpenNovaImporterMacro" in package_script
    assert "opennova_max\\maxscript\\OpenNovaImporter.mcr" in package_script
    assert "opennova_max\\animation.py" in package_script
    assert "opennova_max\\ui.py" in package_script
    assert "pyopennova\\animation_build.py" in package_script
    assert "import opennova_max.animation" in package_script
    assert "import opennova_max.ui" in package_script
    assert "import pyopennova.animation_build" in package_script
    assert "fileIn macroPath" in package_script
    assert "..\\macroscripts\\OpenNovaImporter.mcr" in package_script
    assert 'module_file = Path(getattr(opennova_max, "__file__", "")).resolve()' in package_script
    assert "loaded from {module_file.parent}" in package_script
    assert "OpenNovaMax-*.bundle" in package_script
    assert "_cleanup_existing_bundles(root, destination)" in package_script
    assert "Removed old OpenNova Max bundles:" in package_script
    assert "Disabled locked old OpenNova Max bundles for next restart:" in package_script
    assert "PackageContents.xml.disabled-by-opennova-upgrade" in package_script
    assert "No old OpenNova Max bundles were found." in package_script
    assert "OpenNovaMax-0.0.1.bundle" in package_script
    assert "OpenNovaMax-locked.bundle" in package_script
    assert "UnrelatedPlugin.bundle" in package_script
    assert 'menuMan.createActionItem "OpenNovaImporter" "OpenNova"' in package_script
    assert "maxOps.GetICuiMenuMgr" in package_script
    assert "#cuiRegisterMenus" in package_script
    assert "OpenNovaExportAse`OpenNova" in package_script
    assert "Menu registration should not dynamically define the OpenNova macro" in package_script


def test_max_editable_startup_installer_registers_menu_on_launch():
    startup_installer = (ROOT / "scripts" / "install_max_editable_startup.ps1").read_text(encoding="utf-8")

    assert "[switch]$DisableInstalledBundles" in startup_installer
    assert "OpenNova-OpenNovaImporter.mcr" in startup_installer
    assert "opennova_max\\maxscript\\OpenNovaImporter.mcr" in startup_installer
    assert "OpenNovaExportAse" in startup_installer
    assert "OpenNovaExportAnims" in startup_installer
    assert "OpenNovaMax-*.bundle" in startup_installer
    assert "PackageContents.xml.disabled-by-opennova-editable" in startup_installer
    assert "Disabled OpenNova Max bundles for editable testing:" in startup_installer
    assert "opennova_max_editable_startup.ms" in startup_installer
    assert "opennova_max_editable_startup.py" in startup_installer
    assert "fileIn macroPath" in startup_installer
    assert "OpenNova editable macro load failed:" in startup_installer
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
    monkeypatch.setattr(version, "_source_version", lambda: "0.1.7")
    monkeypatch.setattr(version, "_metadata_version", lambda: "0.1.6")

    assert version.get_version() == "0.1.7"


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
    """No LODs in the 3DI3 model means nothing to build; constructor must not
    require pymxs since this branch returns before any rt() call."""
    from opennova_max.scene_builder import MaxSceneBuilder

    class FakeIR:
        lod_count = 0
        material_count = 0

    builder = MaxSceneBuilder(FakeIR(), resolver=None)
    assert builder.build_basic_scene("anything") is False


def test_max_scene_builder_uses_bad_skeleton_without_animation_context(monkeypatch):
    from opennova_max import mesh, scene_builder
    from opennova_max.scene_builder import MaxSceneBuilder

    class FakeIR:
        lod_count = 1
        material_count = 0
        mesh_type = 3

    calls = []
    builder = MaxSceneBuilder(
        FakeIR(),
        bad_file=object(),
        anim_context=None,
        resolver=None,
        import_collisions=False,
        import_occlusion=False,
        import_lights=False,
    )

    monkeypatch.setattr(mesh, "build_part_hierarchy", lambda *args, **kwargs: ("root", {0: "part"}))
    monkeypatch.setattr(mesh, "build_lod_meshes", lambda *args, **kwargs: [SimpleNamespace(name="01 Mesh0")])
    monkeypatch.setattr(MaxSceneBuilder, "_store_material_diagnostics", lambda self, root: None)
    monkeypatch.setattr(MaxSceneBuilder, "_build_additional_lods", lambda self, name: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_scene_markers", lambda self: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_user_points", lambda self: None)
    monkeypatch.setattr(
        MaxSceneBuilder,
        "build_armature_from_bad",
        lambda self, bad_file, name: calls.append(("bad", bad_file, name)),
    )
    monkeypatch.setattr(
        MaxSceneBuilder,
        "bind_meshes_to_armature",
        lambda self: calls.append(("bind",)),
    )
    monkeypatch.setattr(
        MaxSceneBuilder,
        "build_armature_from_parts",
        lambda self, name: calls.append(("parts", name)),
    )
    monkeypatch.setattr(
        MaxSceneBuilder,
        "build_animations_from_context",
        lambda self, ctx: calls.append(("anim", ctx)),
    )

    assert builder.build_basic_scene("WPN") is True

    assert calls == [("bad", builder.bad_file, "WPN"), ("bind",)]


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

    assert bone_indices == [3, 9, 7]
    assert weights == [0.5, 0.25, 0.0001]


def test_max_multimaterial_slots_match_global_material_ids(monkeypatch):
    from opennova_max import materials

    rt = _FakeMaxRuntime()
    mat0 = _FakeMaxMaterial("Material_0")
    mat2 = _FakeMaxMaterial("Material_2")
    monkeypatch.setattr(materials, "_rt", lambda: rt)

    multi = materials.create_multimaterial("mesh_mats", [2, 0], {0: mat0, 2: mat2})

    assert multi.numsubs == 3
    assert multi.materialList.values == [mat0, None, mat2]
    assert multi.materialIDList.values == [1, 2, 3]
    assert multi.user_props["opennova_submaterial_count"] == 3
    assert multi.user_props["opennova_material_ids"] == "2,0"
    assert multi.user_props["opennova_max_material_ids"] == "3,1"
    assert mat2.user_props["opennova_max_material_id"] == 3
    assert mat0.user_props["opennova_max_material_id"] == 1


def test_max_mesh_writes_global_face_material_ids(monkeypatch):
    from pyopennova.mesh_build import FlatMesh
    from opennova_max import materials
    from opennova_max.mesh import _build_max_mesh

    rt = _FakeMaxRuntime()
    monkeypatch.setattr(materials, "_rt", lambda: rt)
    fm = FlatMesh(
        name="01 Mesh0",
        part_index=0,
        vertices=[(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        faces=[(0, 1, 2), (0, 2, 1), (1, 0, 2)],
        face_material_ids=[1, 2, 0],
        smoothing_groups=[1, 1, 1],
        material_id_set=[1, 2, 0],
    )
    mat0 = _FakeMaxMaterial("Material_0")
    mat1 = _FakeMaxMaterial("Material_1")
    mat2 = _FakeMaxMaterial("Material_2")

    mesh = _build_max_mesh(
        rt,
        fm,
        {
            0: mat0,
            1: mat1,
            2: mat2,
        },
    )

    assert [mesh.face_mat_ids[i] for i in (1, 2, 3)] == [2, 3, 1]
    assert mesh.material.materialList.values == [mat0, mat1, mat2]
    assert mesh.material.materialIDList.values == [1, 2, 3]
    assert mesh.user_props["opennova_material_ids"] == "1,2,0"
    assert mesh.user_props["opennova_max_material_ids"] == "2,3,1"
    assert mesh.user_props["opennova_source_face_material_ids"] == "1,2,0"
    assert mesh.user_props["opennova_face_material_ids"] == "2,3,1"
    assert mesh.user_props["opennova_face_material_id_mode"] == "global"


def test_max_skin_binding_restores_face_material_ids(monkeypatch):
    from opennova_max import scene_builder
    from opennova_max.scene_builder import MaxSceneBuilder

    rt = _FakeMaxRuntime()
    monkeypatch.setattr(scene_builder, "_rt", lambda: rt)
    mesh = _FakeMaxMesh(
        vertices=[(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        faces=[(0, 1, 2), (0, 2, 1), (1, 0, 2)],
    )
    mesh.name = "01 Mesh0"
    mesh.user_props["opennova_face_material_ids"] = "1,2,5"
    mesh.face_mat_ids = {1: 1, 2: 2, 3: 5}

    builder = object.__new__(MaxSceneBuilder)
    builder.armature_object = object()
    builder.bone_nodes = [object(), object()]
    builder._bone_infos = [("BN01", -1, (0.0, 0.0, 0.0)), ("BN02", 0, (1.0, 0.0, 0.0))]
    builder.mesh_objects = [mesh]
    builder._mesh_bone_data = {
        mesh.name: [
            [(0, 1.0)],
            [(1, 1.0)],
            [(0, 0.5), (1, 0.5)],
        ],
    }

    builder.bind_meshes_to_armature()

    assert [mesh.face_mat_ids[i] for i in (1, 2, 3)] == [1, 2, 5]
    assert mesh.user_props["opennova_skin_material_faces_restored"] == 3
    assert mesh.user_props["opennova_skin_bound_vertices"] == 3


def test_max_secondary_merge_builds_its_own_materials(monkeypatch):
    from opennova_max import materials, mesh
    from opennova_max.scene_builder import MaxSceneBuilder

    class FakeIR:
        material_count = 2
        materials = ("arms_mat_0", "arms_mat_1")

    main_materials = {0: "weapon_mat_0"}
    main_builder = SimpleNamespace(
        root_object="root",
        part_nodes={0: "part"},
        material_dict=main_materials,
        armature_object="armature",
        root_motion_node="root_motion",
        bone_nodes=["bone"],
        _bone_infos=[("BN01", -1, (0.0, 0.0, 0.0))],
    )
    created = []
    mesh_material_dicts = []
    bind_calls = []

    def fake_create_material(
        mat_ir,
        *,
        resolver=None,
        ctrl_resolver=None,
        source_format=None,
        uv1_tiling_override=None,
    ):
        created.append((mat_ir, resolver, ctrl_resolver, source_format, uv1_tiling_override))
        return f"created_{mat_ir}"

    def fake_build_lod_meshes(*args, **kwargs):
        mesh_material_dicts.append(kwargs["material_dict"])
        return [SimpleNamespace(name="arms_mesh")]

    monkeypatch.setattr(materials, "create_material", fake_create_material)
    monkeypatch.setattr(mesh, "build_lod_meshes", fake_build_lod_meshes)
    monkeypatch.setattr(MaxSceneBuilder, "bind_meshes_to_armature", lambda self: bind_calls.append(self.mesh_objects))

    builder = MaxSceneBuilder(FakeIR(), resolver="resolver")

    assert builder.merge_with_existing_scene(main_builder) is True

    assert builder.material_dict == {
        0: "created_arms_mat_0",
        1: "created_arms_mat_1",
    }
    assert builder.material_dict is not main_materials
    assert mesh_material_dicts == [builder.material_dict]
    assert [call[0] for call in created] == ["arms_mat_0", "arms_mat_1"]
    assert bind_calls == [[builder.mesh_objects[0]]]


def test_max_scene_builder_passes_derived_uv1_tiling_to_materials(monkeypatch):
    from pyopennova import materials as shared_materials
    from opennova_max import materials, mesh
    from opennova_max.scene_builder import MaxSceneBuilder

    class FakeMaterial:
        index = 7
        shader_name = b"FF_MT_OP"

    class FakeIR:
        material_count = 1
        materials = [FakeMaterial()]
        lod_count = 1

    created = []

    def fake_create_material(
        mat_ir,
        *,
        resolver=None,
        ctrl_resolver=None,
        source_format=None,
        uv1_tiling_override=None,
    ):
        created.append((mat_ir, uv1_tiling_override))
        return "mat"

    monkeypatch.setattr(shared_materials, "derive_uv1_tilings", lambda ir: {7: (2.0, 6.0)})
    monkeypatch.setattr(materials, "create_material", fake_create_material)
    monkeypatch.setattr(mesh, "build_part_hierarchy", lambda *args, **kwargs: ("root", {}))
    monkeypatch.setattr(mesh, "build_lod_meshes", lambda *args, **kwargs: [SimpleNamespace(name="mesh")])
    monkeypatch.setattr(MaxSceneBuilder, "_store_material_diagnostics", lambda self, root: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_scene_markers", lambda self: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_user_points", lambda self: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_occlusion_visualization", lambda self: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_collision_visualization", lambda self: None)
    monkeypatch.setattr(MaxSceneBuilder, "create_scene_lights", lambda self: None)

    builder = MaxSceneBuilder(FakeIR())

    assert builder.build_basic_scene("fixture") is True
    assert created == [(FakeIR.materials[0], (2.0, 6.0))]


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


def test_max_build_lod_meshes_preserves_source_vertex_indexing(monkeypatch):
    from opennova_max import mesh

    calls = []
    fm = SimpleNamespace(part_index=0, name="01 Mesh0", vertex_bone_data=[])

    def fake_flatten_lod(ir, lod_index, **kwargs):
        calls.append((ir, lod_index, kwargs))
        return [fm]

    node = SimpleNamespace(name="01 Mesh0")
    monkeypatch.setattr(mesh, "flatten_lod", fake_flatten_lod)
    monkeypatch.setattr(mesh, "_rt", lambda: SimpleNamespace())
    monkeypatch.setattr(mesh, "_build_max_mesh", lambda rt, flat_mesh, materials: node)
    monkeypatch.setattr(mesh, "set_parent_and_local_position", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(mesh, "_set_hidden", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(mesh, "_set_user_prop", lambda *_args, **_kwargs: None)

    out = mesh.build_lod_meshes(
        "ir",
        0,
        part_nodes={0: "part"},
        material_dict={},
        include_empty_parts=True,
        track_bone_data=True,
    )

    assert out == [node]
    assert calls == [
        (
            "ir",
            0,
            {
                "include_empty_parts": True,
                "track_bone_data": True,
                "preserve_source_indexing": True,
            },
        )
    ]


def test_max_create_mesh_node_defaults_helper_faces_to_flat_smoothing(monkeypatch):
    from opennova_max import mesh

    rt = _FakeMaxRuntime()
    monkeypatch.setattr(mesh, "_rt", lambda: rt)

    node = mesh.create_mesh_node(
        "helper",
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        [(0, 1, 2)],
    )

    assert node.smoothing_groups[1] == 0


def test_max_source_vertex_normals_take_first_authored_corner_normal():
    from opennova_max.mesh import _source_vertex_normals

    fm = SimpleNamespace(
        vertices=[(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        faces=[(0, 1, 2), (0, 2, 1)],
        face_normals=[
            (1.0, 0.0, 0.0),
            (0.0, 1.0, 0.0),
            (0.0, 0.0, 1.0),
            (-1.0, 0.0, 0.0),
            (0.0, -1.0, 0.0),
            (0.0, 0.0, -1.0),
        ],
    )

    assert _source_vertex_normals(fm) == [
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    ]


def test_max_explicit_vertex_normals_are_written_to_edit_normals_modifier():
    from opennova_max.mesh import _apply_explicit_vertex_normals

    class FakeEditNormals:
        def __init__(self):
            self.calls = []

        def MakeExplicit(self, **kwargs):
            self.calls.append(("MakeExplicit", kwargs["node"]))

        def GetNormalID(self, face, corner, **kwargs):
            self.calls.append(("GetNormalID", face, corner, kwargs["node"]))
            return face * 10 + corner

        def SetNormal(self, normal_id, value, **kwargs):
            self.calls.append(("SetNormal", normal_id, value, kwargs["node"]))

        def SetNormalExplicit(self, normal_id, **kwargs):
            self.calls.append(("SetNormalExplicit", normal_id, kwargs["explicit"], kwargs["node"]))

        def SetFaceNormalSpecified(self, face, corner, **kwargs):
            self.calls.append(("SetFaceNormalSpecified", face, corner, kwargs["specified"], kwargs["node"]))

    class FakeRuntime:
        def __init__(self):
            self.modifier = FakeEditNormals()
            self.added = []

        def Edit_Normals(self):
            return self.modifier

        def addModifier(self, node, modifier):
            self.added.append((node, modifier))

        def Point3(self, x, y, z):
            return (x, y, z)

    rt = FakeRuntime()
    node = object()
    fm = SimpleNamespace(faces=[(0, 1, 2)])

    _apply_explicit_vertex_normals(
        rt,
        node,
        fm,
        [(1.0, 0.0, 0.0), None, (0.0, 0.0, 1.0)],
    )

    assert rt.added == [(node, rt.modifier)]
    assert ("SetNormal", 11, (1.0, 0.0, 0.0), node) in rt.modifier.calls
    assert ("SetNormal", 13, (0.0, 0.0, 1.0), node) in rt.modifier.calls
    assert ("SetNormalExplicit", 11, True, node) in rt.modifier.calls
    assert ("SetFaceNormalSpecified", 1, 3, True, node) in rt.modifier.calls


def test_max_zero_axis_center_uses_native_zero_matrix(monkeypatch):
    from opennova_max import scene_builder

    class FakeRuntime:
        def Point3(self, x, y, z):
            return (x, y, z)

        def Matrix3(self, row1, row2, row3, position):
            return SimpleNamespace(row1=row1, row2=row2, row3=row3, position=position)

        def setUserProp(self, obj, key, value):
            obj.user_props[key] = value

    node = SimpleNamespace(position=(1.0, 2.0, 3.0), user_props={})
    monkeypatch.setattr(scene_builder, "_rt", lambda: FakeRuntime())

    scene_builder._apply_zero_axis_matrix(node)

    assert node.transform.row1 == (0.0, 0.0, 0.0)
    assert node.transform.row2 == (0.0, 0.0, 0.0)
    assert node.transform.row3 == (0.0, 0.0, 0.0)
    assert node.transform.position == (1.0, 2.0, 3.0)
    assert node.user_props == {}


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
        shader_name = b"FF_MT_OP"
        flags = 0x01
        blend_mode = 0
        luminosity = 0
        alpha_threshold = 0.5
        texture_count = 2
        textures = [
            FakeTexture(materials.THREEDI_TEX_SLOT_DIFFUSE, diffuse.name),
            FakeTexture(materials.THREEDI_TEX_SLOT_DETAIL, "MissingDetail.tga"),
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
            self.coords = SimpleNamespace()
            self.coords = SimpleNamespace()

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
        slot = materials.THREEDI_TEX_SLOT_DIFFUSE
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
        shader_name = b"FF_MT_OP"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 2
        textures = [
            FakeTexture(materials.THREEDI_TEX_SLOT_DIFFUSE, diffuse.name),
            FakeTexture(materials.THREEDI_TEX_SLOT_DETAIL, detail.name),
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
    assert mat.selfIllumMap.filename == str(detail)
    assert mat.selfIllumMapEnable is True
    assert runtime.bitmap_paths == [str(diffuse), str(detail)]
    assert mat.user_props["opennova_has_diffuse_map"] == 1
    assert "opennova_detail_map_mode" not in mat.user_props


def test_max_phongt_no_slot3_no_bump(tmp_path: Path):
    """VS_PHONGT with no slot-3 texture imports flat-shaded.

    The previous diffuse-alpha bump fabrication produced visibly distorted
    shading on materials whose DDS alpha was opacity / unused rather than
    height. Until we have a per-material flag indicating that the alpha
    channel actually contains height data, no bump is wired.
    """
    from opennova_max import materials

    diffuse = tmp_path / "Roof.dds"
    diffuse.write_bytes(b"DDS " + bytes(range(32)))

    class FakeTexture:
        slot = materials.THREEDI_TEX_SLOT_DIFFUSE
        name = b"Roof.tga"
        type = 0

    class FakeMaterialIR:
        index = 6
        shader_name = b"VS_PHONGT"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 1
        textures = [FakeTexture()]

    class FakeResolver:
        def resolve_texture(self, name: str, **_kwargs) -> str | None:
            if name.lower() == "roof.tga":
                return str(diffuse)
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
        mat = materials.create_material(FakeMaterialIR(), resolver=FakeResolver())

    assert mat.diffuseMap.filename == str(diffuse)
    assert not hasattr(mat, "bumpMap"), "no bumpMap for VS_PHONGT diffuse-only"
    assert mat.user_props.get("opennova_bump_map_mode") != "diffuse_alpha"
    assert mat.user_props.get("opennova_has_normal_map", 0) == 0


def test_max_material_applies_static_renderer_alpha_blend():
    from opennova_max import materials

    class FakeMaterialIR:
        index = 2
        shader_name = b"FF_ST_AB"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 0
        textures = []

    class FakeRuntime:
        def StandardMaterial(self, name: str = ""):
            mat = type("FakeStandardMaterial", (), {})()
            mat.name = name
            mat.user_props = {}
            return mat

        def color(self, r: int, g: int, b: int):
            return (r, g, b)

        def setUserProp(self, obj, key: str, value):
            obj.user_props[key] = value

    with patch("opennova_max.materials._rt", return_value=FakeRuntime()):
        mat = materials.create_material(FakeMaterialIR())

    assert mat.opacity == 70.0
    assert mat.user_props["opennova_renderer_blend"] == "alpha_blend"


def test_max_dot3_normal_uses_normal_slot_not_bump(tmp_path: Path):
    """VS_DOT3DIFF with a non-MDT, non-TGA-alpha slot-3 texture wires the
    tangent-space normal map into ``mat.normalMap`` (Max's dedicated slot),
    not ``mat.bumpMap``."""
    from opennova_max import materials

    diffuse = tmp_path / "Tank.dds"
    normal = tmp_path / "TankN.dds"
    diffuse.write_bytes(b"DDS")
    normal.write_bytes(b"DDS")

    class FakeTextureDiffuse:
        slot = materials.THREEDI_TEX_SLOT_DIFFUSE
        name = b"Tank.tga"
        type = 0  # diffuse-color, not normal type

    class FakeTextureNormal:
        slot = materials.THREEDI_TEX_SLOT_NORMAL
        name = b"TankN.tga"
        type = 0  # tangent-space normal (NOT MDT, NOT TGA-alpha)

    class FakeMaterialIR:
        index = 4
        shader_name = b"VS_DOT3DIFF"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 2
        textures = [FakeTextureDiffuse(), FakeTextureNormal()]

    class FakeResolver:
        def resolve_texture(self, name: str, **_kwargs) -> str | None:
            mapping = {"tank.tga": str(diffuse), "tankn.tga": str(normal)}
            return mapping.get(name.lower())

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
        mat = materials.create_material(FakeMaterialIR(), resolver=FakeResolver())

    assert hasattr(mat, "normalMap"), "DOT3 normal goes to mat.normalMap"
    assert mat.normalMap.filename == str(normal)
    assert getattr(mat, "normalMapEnable", None) is True
    assert not hasattr(mat, "bumpMap"), "Tangent-space DOT3 should not touch bumpMap"
    assert mat.user_props["opennova_bump_map_mode"] == "normal_dot3"
    assert mat.user_props["opennova_has_normal_map"] == 1


def test_max_alpha_test_honors_inverted_threshold():
    """ALPHA_INVERT bit flips the alpha-test threshold (mirrors gsys clip-inv)."""
    from opennova_max import materials
    from pyopennova.materials import (
        THREEDI_MATERIAL_FLAG_ALPHA_INVERT,
        THREEDI_MATERIAL_FLAG_ALPHA_TEST,
        THREEDI_MATERIAL_FLAG_ALPHA_INVERT,
        THREEDI_MATERIAL_FLAG_ALPHA_TEST,
    )

    class FakeMaterialIR:
        index = 0
        shader_name = b"FF_ST_AB"
        flags = THREEDI_MATERIAL_FLAG_ALPHA_TEST | THREEDI_MATERIAL_FLAG_ALPHA_INVERT
        material_flags = THREEDI_MATERIAL_FLAG_ALPHA_TEST | THREEDI_MATERIAL_FLAG_ALPHA_INVERT
        alpha_test_value_byte = 64
        alpha_threshold = 64.0 / 255.0
        blend_mode = 0
        luminosity = 0
        texture_count = 0
        textures = []

    class FakeRuntime:
        def StandardMaterial(self, name: str = ""):
            mat = type("FakeStandardMaterial", (), {})()
            mat.name = name
            mat.user_props = {}
            return mat

        def color(self, r: int, g: int, b: int):
            return (r, g, b)

        def setUserProp(self, obj, key: str, value):
            obj.user_props[key] = value

    with patch("opennova_max.materials._rt", return_value=FakeRuntime()):
        mat = materials.create_material(FakeMaterialIR())

    expected_threshold = 1.0 - 64.0 / 255.0
    assert mat.opacity == pytest.approx(expected_threshold * 100.0, abs=0.01)
    assert mat.opacityType == 2
    assert mat.user_props["opennova_alpha_inverted"] == 1
    assert mat.user_props["opennova_alpha_test"] == 1


def test_max_detail_uses_native_secondary_map_slot(tmp_path: Path):
    """FF_MT_OP with diffuse + detail keeps diffuse visible and stores detail
    in a native material slot the current-scene ASE exporter can inspect."""
    from opennova_max import materials

    diffuse = tmp_path / "Wall.dds"
    detail = tmp_path / "Detail.dds"
    diffuse.write_bytes(b"DDS")
    detail.write_bytes(b"DDS")

    class FakeTextureDiffuse:
        slot = materials.THREEDI_TEX_SLOT_DIFFUSE
        name = b"Wall.tga"
        type = 0

    class FakeTextureDetail:
        slot = materials.THREEDI_TEX_SLOT_DETAIL
        name = b"Detail.tga"
        type = 0

    class FakeMaterialIR:
        index = 1
        shader_name = b"FF_MT_OP"
        flags = 0
        blend_mode = 0
        luminosity = 0
        texture_count = 2
        textures = [FakeTextureDiffuse(), FakeTextureDetail()]

    class FakeResolver:
        def resolve_texture(self, name: str, **_kwargs) -> str | None:
            mapping = {"wall.tga": str(diffuse), "detail.tga": str(detail)}
            return mapping.get(name.lower())

    class FakeBitmap:
        def __init__(self, filename: str = ""):
            self.filename = filename
            self.name = ""
            self.coords = SimpleNamespace()

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
        mat = materials.create_material(
            FakeMaterialIR(),
            resolver=FakeResolver(),
            uv1_tiling_override=(2.0, 6.0),
        )

    assert mat.diffuseMap.filename == str(diffuse)
    assert mat.selfIllumMap.filename == str(detail)
    assert mat.selfIllumMap.coords.U_Tiling == 2.0
    assert mat.selfIllumMap.coords.V_Tiling == 6.0
    assert "opennova_detail_map_mode" not in mat.user_props


def test_max_texture_map_assignment_falls_back_to_indexed_slots():
    from opennova_max import materials

    class StrictMaterial:
        def __init__(self):
            object.__setattr__(self, "maps", {})
            object.__setattr__(self, "mapEnables", {})

        def __setattr__(self, name, value):
            if name in {"diffuseMap", "diffuseMapEnable"}:
                raise AttributeError(name)
            object.__setattr__(self, name, value)

    mat = StrictMaterial()
    bitmap = object()

    assigned = materials._assign_texture_map(
        SimpleNamespace(setProperty=lambda *args: None),
        mat,
        "diffuseMap",
        "diffuseMapEnable",
        2,
        bitmap,
    )

    assert assigned
    assert mat.maps[2] is bitmap
    assert mat.mapEnables[2] is True


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
    from opennova_jobs import ScanItem
    from opennova_max import qt_ui
    from opennova_max import ui

    menu_script = ui.build_menu_script()
    macro_script = (ROOT / "opennova_max" / "maxscript" / "OpenNovaImporter.mcr").read_text(encoding="utf-8")

    assert 'macroScript OpenNovaImporter category:"OpenNova"' in macro_script
    assert 'macroScript OpenNovaExportAse category:"OpenNova"' in macro_script
    assert 'macroScript OpenNovaExportAnims category:"OpenNova"' in macro_script
    assert 'ui.show_importer()' in macro_script
    assert 'ui.export_ase()' in macro_script
    assert 'ui.export_anims()' in macro_script
    assert "macroScript OpenNovaImporter" not in menu_script
    assert "macroScript OpenNovaExportAse" not in menu_script
    assert "macroScript OpenNovaExportAnims" not in menu_script
    assert "menuMan.registerMenuContext" in menu_script
    assert 'menuMan.createSubMenuItem "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaImporter" "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAse" "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAnims" "OpenNova"' in menu_script
    assert 'importItem.setTitle "Importer..."' in menu_script
    assert 'aseItem.setTitle "Novalogic ASE (.ase)"' in menu_script
    assert 'animItem.setTitle "Novalogic Anims (.adm + .bad)"' in menu_script
    assert 'aseOpenNovaItem.setTitle "Novalogic ASE (.ase)"' in menu_script
    assert 'animOpenNovaItem.setTitle "Novalogic Anims (.adm + .bad)"' in menu_script
    assert "maxOps.GetICuiMenuMgr" in menu_script
    assert "#cuiRegisterMenus" in menu_script
    assert "OpenNovaExportAse`OpenNova" in menu_script
    assert "OpenNovaExportAnims`OpenNova" in menu_script
    assert "openNovaCreateModernAction openNovaMenu OPENNOVA_ASE_ACTION_GUID" in menu_script
    assert "openNovaCreateModernAction openNovaMenu OPENNOVA_ANIM_ACTION_GUID" in menu_script
    assert "OpenNova modern menu registration failed:" in menu_script
    assert "OpenNova modern menu refresh failed:" in menu_script
    assert "OpenNova menu action missing:" in menu_script
    assert "openNovaRemoveLegacyMenu mainMenuBar \"OpenNova\"" in menu_script
    assert "openNovaBuildLegacyOpenNovaMenu mainMenuBar" in menu_script
    assert "OpenNova legacy menu registration context already exists; rebuilding menu." in menu_script
    assert "if menuMan.registerMenuContext 0x5cb72810 then" not in menu_script
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
    assert qt_ui.can_import_loose("C:/asset.3di", "C:/out", False, False, False, True)
    assert qt_ui.loose_paths_display(["C:/a.3di", "C:/b.3di"]) == "2 files selected"
    assert not qt_ui.can_import_loose("", "C:/out", True, False, False)
    assert not qt_ui.can_import_loose([], "C:/out", True, False, False)
    assert not qt_ui.can_import_loose("C:/asset.3di", "C:/out", False, False, False)
    assert qt_ui.writes_any_output(False, False, False, True)
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
    assert qt_ui.can_import_batch(
        visible_count=1,
        output_root="C:/out",
        write_ase=False,
        write_3dp=False,
        write_max=False,
        copy_textures=True,
    )
    assert ui._format_scan_item(
        ScanItem(name="M16A2", type="weapon", source_model="m16_1st.3di", output_stem="m16_1st")
    ) == "[weapon] M16A2 -> m16_1st"


def test_max_qt_preferences_round_trip_and_dialog_paths(tmp_path: Path, monkeypatch):
    from opennova_max import preferences, qt_ui

    settings = tmp_path / "settings.json"
    monkeypatch.setenv(preferences.SETTINGS_ENV_VAR, str(settings))

    assert preferences.load_preferences() == {
        "last_resource_dir": "",
        "last_output_dir": "",
        "last_loose_file_dir": "",
    }

    assert preferences.save_preferences({
        "last_resource_dir": " C:/game ",
        "last_output_dir": "C:/out",
        "last_loose_file_dir": "C:/loose",
        "unknown": "ignored",
    })
    assert preferences.load_preferences() == {
        "last_resource_dir": "C:/game",
        "last_output_dir": "C:/out",
        "last_loose_file_dir": "C:/loose",
    }
    assert qt_ui.dialog_paths_from_preferences(preferences.load_preferences()) == {
        "game_dir": "C:/game",
        "asset_dir": "C:/game",
        "output_root": "C:/out",
        "loose_output": "C:/out",
        "loose_file_dir": "C:/loose",
    }


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



def test_shared_host_neutral_writer_produces_outputs_without_max():
    if not FIXTURE_3DI.is_file():
        pytest.skip("fixture missing (LFS not pulled?)")

    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.threedi_ffi import free_model_3di3, read_model_3di3

    out_dir = ROOT / f".tmp_test_host_outputs_{uuid.uuid4().hex}"
    out_dir.mkdir()
    ir = read_model_3di3(str(FIXTURE_3DI))
    try:
        written = write_host_neutral_outputs(ir, str(out_dir), "Shed")
        names = {Path(path).name for path in written}
        assert "Shed.3dp" in names
        assert "Shed.3da" in names
        assert "Shed.ase" in names
        assert "Shed_bullet.ase" not in names
        project_text = (out_dir / "Shed.3dp").read_text(encoding="utf-8", errors="replace")
        assert "poly_collision_lod 0" in project_text
        assert "_bullet.ase" not in project_text
        for path in written:
            assert Path(path).stat().st_size > 0
    finally:
        free_model_3di3(ir)
        shutil.rmtree(out_dir, ignore_errors=True)


def test_max_scene_builder_has_no_hidden_bullet_lod_builder():
    from opennova_max.scene_builder import MaxSceneBuilder

    assert not hasattr(MaxSceneBuilder, "create_bullet_lod")
    assert not hasattr(MaxSceneBuilder, "_create_bullet_attach_markers")


def test_blender_scene_builder_source_has_no_hidden_bullet_lod_builder():
    source = (ROOT / "opennova_blender" / "scene_builder.py").read_text(
        encoding="utf-8",
        errors="replace",
    )

    assert "def create_bullet_lod" not in source
    assert "bullet_lod_index" not in source


def test_max_native_batch_request_uses_exact_loose_output_dir():
    from types import SimpleNamespace
    from opennova_max.import_runner import execute_import_request

    # Simulate the legacy MaxImportRequest field layout (duck-typed by execute_import_request).
    request = SimpleNamespace(
        mode="loose",
        resource=str(FIXTURE_3DI),
        output_dir=str(ROOT / "out" / "Shed"),
        output_stem="",
        base_dir="",
        options=None,
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
    assert run_loose.call_args.kwargs["copy_textures"] is True


def test_max_native_batch_request_can_disable_max_output():
    from types import SimpleNamespace
    from opennova_max.import_runner import execute_import_request

    # Simulate the legacy MaxImportRequest field layout with write_max=False.
    request = SimpleNamespace(
        mode="loose",
        resource=str(FIXTURE_3DI),
        output_dir=str(ROOT / "out" / "Shed"),
        output_stem="",
        base_dir="",
        write_max=False,
        copy_textures=True,
        options=None,
    )
    with patch(
        "opennova_max.import_runner._run_loose_import_impl",
        return_value=[str(ROOT / "out" / "Shed" / "Shed.ase")],
    ) as run_loose:
        result = execute_import_request(request)

    assert result.ok
    assert run_loose.call_args.kwargs["write_max"] is False
    assert run_loose.call_args.kwargs["copy_textures"] is True


def test_definition_plan_resolves_reset_bad_when_animation_import_is_disabled(monkeypatch):
    from pyopennova import resource_plan
    from pyopennova import definitions

    class FakeResolver:
        def resolve(self, name: str):
            if name.lower().endswith(".3di"):
                return f"C:/game/{name}"
            return None

    monkeypatch.setattr(
        definitions,
        "process_def_files",
        lambda resolver: (
            [
                definitions.WeaponContext(
                    name="WPN_TEST",
                    graphic1=definitions.Graphic1(main="wpn_test", arms=None),
                    anim_adm="wpn_test.adm",
                )
            ],
            [],
        ),
    )
    monkeypatch.setattr(
        definitions,
        "build_animation_context",
        lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("animation context should not load")),
    )
    monkeypatch.setattr(definitions, "resolve_reset_bad_path", lambda adm_field, resolver: "C:/game/wpn_rst.bad")

    plan = resource_plan.resolve_definition_import(
        base_dir="C:/game",
        item_name="WPN_TEST",
        item_type="weapon",
        resolver=FakeResolver(),
        import_animations=False,
    )

    assert plan is not None
    assert plan.animation_context is None
    assert plan.reset_bad_path == "C:/game/wpn_rst.bad"


def test_resolve_reset_bad_path_reads_only_adm_reset(monkeypatch):
    from pyopennova import definitions

    class FakeEntry:
        def __init__(self, key: str, value: str):
            self.key = key.encode("utf-8")
            self.value = value.encode("utf-8")

    class FakeAdm:
        count = 2
        entries = (
            FakeEntry("anim_reset", "WPN_RST"),
            FakeEntry("anim_fire", "WPN_FIRE"),
        )

    class FakeResolver:
        def __init__(self):
            self.requests = []

        def resolve(self, name: str):
            self.requests.append(name)
            return {
                "WPN.adm": "C:/game/WPN.adm",
                "WPN_RST.bad": "C:/game/WPN_RST.bad",
            }.get(name)

    freed = []
    resolver = FakeResolver()
    fake_adm = FakeAdm()
    monkeypatch.setattr(definitions, "parse_adm", lambda path: fake_adm)
    monkeypatch.setattr(definitions, "free_adm", lambda adm: freed.append(adm))
    monkeypatch.setattr(
        definitions,
        "parse_bad",
        lambda path: (_ for _ in ()).throw(AssertionError("reset path resolution should not parse BAD")),
    )

    assert definitions.resolve_reset_bad_path("WPN", resolver=resolver) == "C:/game/WPN_RST.bad"
    assert resolver.requests == ["WPN.adm", "WPN_RST.bad"]
    assert freed == [fake_adm]


def test_max_definition_import_binds_reset_bad_without_animation_context(tmp_path: Path):
    from opennova_max import import_runner

    bad_file = object()
    main_ir = object()
    built = []

    class FakeBuilder:
        def __init__(self, ir, **kwargs):
            self.ir = ir
            self.kwargs = kwargs
            self.bad_file = kwargs.get("bad_file")
            self.apply_called = False
            built.append(self)

        def build_basic_scene(self, name: str):
            self.name = name
            return True

        def apply_animations(self):
            self.apply_called = True
            return True

    plan = SimpleNamespace(
        export_name="WPN_TEST",
        scene_name="WPN_TEST",
        reset_bad_path="C:/game/WPN_RST.bad",
        animation_context=object(),
        models=(SimpleNamespace(role="main", path="C:/game/WPN.3di"),),
    )

    with patch("pyopennova.resource_plan.resolve_definition_import", return_value=plan):
        with patch("pyopennova.bad_ffi.parse_bad", return_value=bad_file) as parse_bad:
            with patch("pyopennova.bad_ffi.free_bad") as free_bad:
                with patch("pyopennova.threedi_ffi.read_model", return_value=main_ir):
                    with patch("pyopennova.threedi_ffi.free_model_3di3") as free_ir:
                        with patch("opennova_max.scene_builder.MaxSceneBuilder", FakeBuilder):
                            with patch("opennova_max.output_writers.write_outputs", return_value=["out.max"]):
                                written, project_dir = import_runner._run_definition_import_impl(
                                    base_dir=str(tmp_path),
                                    item_name="WPN_TEST",
                                    item_type="weapon",
                                    output_dir=str(tmp_path / "out"),
                                    output_stem="",
                                    import_arms=False,
                                    import_animations=False,
                                    import_collisions=False,
                                    import_occlusion=False,
                                    import_lights=False,
                                    write_ase=False,
                                    write_3dp=False,
                                    write_max=True,
                                    reset_scene=False,
                                )

    assert written == ["out.max"]
    assert project_dir == str(tmp_path / "out" / "WPN_TEST")
    assert built[0].bad_file is bad_file
    assert built[0].kwargs["anim_context"] is None
    assert built[0].apply_called is False
    parse_bad.assert_called_once_with("C:/game/WPN_RST.bad")
    free_bad.assert_called_once_with(bad_file)
    free_ir.assert_called_once_with(main_ir)


def test_max_definition_import_returns_empty_when_main_scene_build_fails(tmp_path: Path):
    from opennova_max import import_runner

    bad_file = object()
    main_ir = object()

    class FakeBuilder:
        def __init__(self, ir, **kwargs):
            self.ir = ir
            self.kwargs = kwargs

        def build_basic_scene(self, name: str):
            return False

        def apply_animations(self):
            raise AssertionError("animations should not be applied after build failure")

    plan = SimpleNamespace(
        export_name="WPN_TEST",
        scene_name="WPN_TEST",
        reset_bad_path="C:/game/WPN_RST.bad",
        animation_context=object(),
        models=(SimpleNamespace(role="main", path="C:/game/WPN.3di"),),
    )

    with patch("pyopennova.resource_plan.resolve_definition_import", return_value=plan):
        with patch("pyopennova.bad_ffi.parse_bad", return_value=bad_file):
            with patch("pyopennova.bad_ffi.free_bad") as free_bad:
                with patch("pyopennova.threedi_ffi.read_model", return_value=main_ir):
                    with patch("pyopennova.threedi_ffi.free_model_3di3") as free_ir:
                        with patch("opennova_max.scene_builder.MaxSceneBuilder", FakeBuilder):
                            with patch("opennova_max.output_writers.write_outputs") as write_outputs:
                                written, project_dir = import_runner._run_definition_import_impl(
                                    base_dir=str(tmp_path),
                                    item_name="WPN_TEST",
                                    item_type="weapon",
                                    output_dir=str(tmp_path / "out"),
                                    output_stem="",
                                    import_arms=False,
                                    import_animations=True,
                                    import_collisions=False,
                                    import_occlusion=False,
                                    import_lights=False,
                                    write_ase=False,
                                    write_3dp=False,
                                    write_max=True,
                                    reset_scene=False,
                                )

    assert written == []
    assert project_dir == str(tmp_path / "out" / "WPN_TEST")
    write_outputs.assert_not_called()
    free_bad.assert_called_once_with(bad_file)
    free_ir.assert_called_once_with(main_ir)


def test_max_definition_import_applies_animations_after_secondary_merge(tmp_path: Path):
    from opennova_max import import_runner

    events = []
    bad_file = object()
    main_ir = "main_ir"
    arms_ir = "arms_ir"
    anim_context = object()

    class FakeBuilder:
        def __init__(self, ir, **kwargs):
            self.ir = ir
            self.kwargs = kwargs
            self.bad_file = kwargs.get("bad_file")

        def build_basic_scene(self, name: str):
            events.append(("build", self.ir, name))
            return True

        def merge_with_existing_scene(self, main_builder):
            events.append(("merge", self.ir, main_builder.ir))
            return True

        def apply_animations(self):
            events.append(("apply", self.ir))
            return True

    plan = SimpleNamespace(
        export_name="WPN_TEST",
        scene_name="WPN_TEST",
        reset_bad_path="C:/game/WPN_RST.bad",
        animation_context=anim_context,
        models=(
            SimpleNamespace(role="main", path="C:/game/WPN.3di"),
            SimpleNamespace(role="arms", path="C:/game/WPN_ARMS.3di"),
        ),
    )

    with patch("pyopennova.resource_plan.resolve_definition_import", return_value=plan):
        with patch("pyopennova.bad_ffi.parse_bad", return_value=bad_file):
            with patch("pyopennova.bad_ffi.free_bad"):
                with patch("pyopennova.threedi_ffi.read_model", side_effect=[main_ir, arms_ir]):
                    with patch("pyopennova.threedi_ffi.free_model_3di3"):
                        with patch("opennova_max.scene_builder.MaxSceneBuilder", FakeBuilder):
                            with patch("opennova_max.output_writers.write_outputs", return_value=["out.max"]):
                                written, _project_dir = import_runner._run_definition_import_impl(
                                    base_dir=str(tmp_path),
                                    item_name="WPN_TEST",
                                    item_type="weapon",
                                    output_dir=str(tmp_path / "out"),
                                    output_stem="",
                                    import_arms=True,
                                    import_animations=True,
                                    import_collisions=False,
                                    import_occlusion=False,
                                    import_lights=False,
                                    write_ase=False,
                                    write_3dp=False,
                                    write_max=True,
                                    reset_scene=False,
                                )

    assert written == ["out.max"]
    assert events == [
        ("build", main_ir, "WPN_TEST"),
        ("merge", arms_ir, main_ir),
        ("apply", main_ir),
    ]


def test_max_output_writer_can_save_max_only(tmp_path: Path):
    from opennova_max import output_writers

    max_path = str(tmp_path / "Shed.max")
    with patch("opennova_max.output_writers.write_host_neutral_outputs") as shared:
        with patch("opennova_max.output_writers.copy_model_textures", return_value=[]) as copy_textures:
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
    copy_textures.assert_called_once()


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
        "bad_file": bad_file,
    })()

    with patch(
        "opennova_max.output_writers.write_host_neutral_outputs",
        return_value=[shared_path],
    ) as shared:
        with patch("opennova_max.output_writers.copy_model_textures", return_value=[]) as copy_textures:
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
        collision_lod_index=None,
    )
    copy_textures.assert_called_once_with(ir, str(tmp_path), resolver=None)
