from __future__ import annotations

import importlib
import sys
import types
from pathlib import Path

import pytest

from tests.blender_subprocess import run_blender_python


SCRATCH_ASE_EXPORT_SCRIPT = r"""
import sys
from pathlib import Path

import bpy

from apps.importer.import_runner import _setup_blender_package
from tests.dcc_ase_assertions import assert_oed_render_mesh_names


def create_scratch_material_mesh_scene():
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


_setup_blender_package()
from blender.ase_exporter import AseExporter

create_scratch_material_mesh_scene()
ase_path = Path(sys.argv[1])
exporter = AseExporter()
exporter.export_textures = False
assert exporter.export_scene(bpy.context.scene, str(ase_path))
assert_oed_render_mesh_names((ase_path,))
"""


OPERATOR_ASE_EXPORT_SCRIPT = r"""
import importlib
import sys
from pathlib import Path

import bpy

from tests.dcc_ase_assertions import assert_oed_render_mesh_names


def create_scratch_material_mesh_scene():
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


def unregister_addon_if_needed(addon):
    try:
        addon.unregister()
    except (RuntimeError, ValueError):
        pass


addon = importlib.import_module("blender")
unregister_addon_if_needed(addon)
addon.register()
try:
    create_scratch_material_mesh_scene()
    ase_path = Path(sys.argv[1])
    result = bpy.ops.export_scene.novalogic_ase(
        filepath=str(ase_path),
        export_textures=False,
    )
    assert result == {"FINISHED"}
    assert ase_path.is_file()
    assert_oed_render_mesh_names((ase_path,))
finally:
    unregister_addon_if_needed(addon)
"""


def _clear_blender_package_modules() -> None:
    for name in list(sys.modules):
        if name == "blender" or name.startswith("blender."):
            sys.modules.pop(name, None)


def _import_ase_exporter_with_stubbed_blender_modules(monkeypatch: pytest.MonkeyPatch):
    _clear_blender_package_modules()

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
    ase_path = tmp_path / "ScratchCrate.ase"
    run_blender_python(SCRATCH_ASE_EXPORT_SCRIPT, ase_path)


def test_blender_export_operator_writes_oed_valid_ase(tmp_path: Path) -> None:
    ase_path = tmp_path / "ScratchCrateOperator.ase"
    run_blender_python(OPERATOR_ASE_EXPORT_SCRIPT, ase_path)
