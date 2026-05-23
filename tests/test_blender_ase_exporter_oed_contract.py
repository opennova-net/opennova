from __future__ import annotations

import importlib
import sys
import types
from pathlib import Path

import pytest

from tests.dcc_ase_assertions import assert_oed_render_mesh_names


def _import_ase_exporter_with_stubbed_blender_modules(monkeypatch: pytest.MonkeyPatch):
    sys.modules.pop("blender.ase_exporter", None)
    sys.modules.pop("blender", None)

    monkeypatch.setitem(sys.modules, "bpy", types.ModuleType("bpy"))
    monkeypatch.setitem(sys.modules, "bmesh", types.ModuleType("bmesh"))
    mathutils = types.ModuleType("mathutils")
    mathutils.Vector = lambda value=(): value
    monkeypatch.setitem(sys.modules, "mathutils", mathutils)

    from apps.importer.import_runner import _setup_blender_package

    _setup_blender_package()
    return importlib.import_module("blender.ase_exporter")


def test_blender_exporter_assigns_digit_leading_names_for_scratch_meshes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    ase_exporter = _import_ase_exporter_with_stubbed_blender_modules(monkeypatch)
    exporter = ase_exporter.AseExporter()

    assert exporter._render_mesh_export_name("CrateBody") == "01_CrateBody"
    assert exporter._render_mesh_export_name("Wheel") == "02_Wheel"
    assert exporter._render_mesh_export_name("7Panel") == "7Panel"
    assert exporter._render_mesh_export_name("PN03_attach") == "03_attach"


def test_blender_scratch_scene_exports_oed_render_mesh_names(tmp_path: Path) -> None:
    bpy = pytest.importorskip("bpy", reason="bpy is not installed in this environment")
    from apps.importer.import_runner import _setup_blender_package

    _setup_blender_package()
    sys.modules.pop("blender.ase_exporter", None)
    from blender.ase_exporter import AseExporter

    bpy.ops.wm.read_homefile(use_empty=True)
    mesh = bpy.data.meshes.new("ScratchMesh")
    mesh.from_pydata(
        [(-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.0, 0.5, 0.0)],
        [],
        [(0, 1, 2)],
    )
    mesh.update()
    obj = bpy.data.objects.new("ScratchCrate", mesh)
    bpy.context.collection.objects.link(obj)
    material = bpy.data.materials.new("CrateMaterial")
    material.diffuse_color = (0.4, 0.6, 0.8, 1.0)
    mesh.materials.append(material)
    bpy.context.view_layer.update()

    ase_path = tmp_path / "ScratchCrate.ase"
    exporter = AseExporter()
    exporter.export_textures = False
    assert exporter.export_scene(bpy.context.scene, str(ase_path))
    assert_oed_render_mesh_names((ase_path,))
