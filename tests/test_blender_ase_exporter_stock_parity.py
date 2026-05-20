"""Stock 3DI -> Blender scene -> ASE parity tests."""
from __future__ import annotations

import difflib
from pathlib import Path

import pytest

from pyopennova import ase_ffi
from pyopennova.asset_resolver import AssetResolver
from pyopennova.host_outputs import write_host_neutral_outputs
from pyopennova.legacy_tools.stock_roundtrip import (
    STOCK_THREEDI_FIXTURE_NAMES,
    discover_stock_3di_fixtures,
)
from pyopennova.threedi_ffi import free_model_3di3, read_model_3di3


FORBIDDEN_IMPORT_METADATA = {
    "_lod_index",
    "_part_index",
    "_material_names",
    "nl_ase_name",
    "nl_raw_position",
    "nl_raw_direction",
    "nl_parent_index",
    "nl_type_code",
    "opennova_zero_axis",
    "opennova_bone_index",
    "opennova_detail_map_mode",
    "opennova_has_detail_map",
    "opennova_detail_texture_path",
    "atten_start",
    "falloff",
    "tm_row2",
    "light_type",
}


@pytest.mark.parametrize("name", STOCK_THREEDI_FIXTURE_NAMES)
def test_stock_3di_imported_blender_scene_exports_matching_ase(
    name: str,
    tmp_path: Path,
) -> None:
    fixture = discover_stock_3di_fixtures(names=[name])[0]
    direct_dir = tmp_path / "direct"
    blender_dir = tmp_path / "blender"
    direct_dir.mkdir()
    blender_dir.mkdir()

    import bpy

    bpy.ops.wm.read_homefile(use_empty=True)

    model = read_model_3di3(str(fixture.reference_3di_path))
    try:
        written = write_host_neutral_outputs(model, str(direct_dir), name)
        direct_ase = direct_dir / f"{name}.ase"
        direct_project = direct_dir / f"{name}.3dp"
        assert direct_ase in {Path(path) for path in written}
        assert direct_project.is_file()

        with AssetResolver(str(fixture.directory)) as resolver:
            from opennova_blender.scene_builder import BlenderSceneBuilder

            builder = BlenderSceneBuilder(model, resolver=resolver)
            assert builder.build_basic_scene(name)

        _assert_scene_has_no_import_export_metadata(bpy)

        from blender.ase_exporter import AseExporter

        blender_ase = blender_dir / f"{name}.ase"
        exporter = AseExporter()
        exporter.export_textures = False
        assert exporter.export_scene(bpy.context.scene, str(blender_ase))

        direct_bytes = direct_ase.read_bytes()
        blender_bytes = blender_ase.read_bytes()
        assert blender_bytes == direct_bytes, _ase_mismatch_report(direct_ase, blender_ase)
    finally:
        free_model_3di3(model)


def _assert_scene_has_no_import_export_metadata(bpy_module) -> None:
    for obj in bpy_module.data.objects:
        leaked = sorted(FORBIDDEN_IMPORT_METADATA.intersection(obj.keys()))
        assert not leaked, f"{obj.name} leaked importer metadata: {leaked}"
        for group in getattr(obj, "vertex_groups", []):
            if hasattr(group, "keys"):
                try:
                    keys = group.keys()
                except TypeError:
                    continue
                leaked = sorted(FORBIDDEN_IMPORT_METADATA.intersection(keys))
                assert not leaked, f"{obj.name}.{group.name} leaked importer metadata: {leaked}"
    for mat in bpy_module.data.materials:
        leaked = sorted(FORBIDDEN_IMPORT_METADATA.intersection(mat.keys()))
        assert not leaked, f"{mat.name} leaked importer metadata: {leaked}"


def _ase_mismatch_report(expected: Path, actual: Path) -> str:
    expected_text = expected.read_text(encoding="utf-8", errors="replace").splitlines()
    actual_text = actual.read_text(encoding="utf-8", errors="replace").splitlines()
    diff = "\n".join(
        difflib.unified_diff(
            expected_text[:400],
            actual_text[:400],
            fromfile=str(expected),
            tofile=str(actual),
            lineterm="",
            n=3,
        )
    )
    return (
        f"Blender-exported ASE does not match direct ASE\n"
        f"expected: {expected}\n"
        f"actual:   {actual}\n"
        f"expected summary: {_ase_summary(expected)}\n"
        f"actual summary:   {_ase_summary(actual)}\n"
        f"first diff:\n{diff}"
    )


def _ase_summary(path: Path) -> dict:
    doc = ase_ffi.parse_file(str(path))
    try:
        return {
            "objects": int(doc.object_count),
            "lights": int(doc.light_count),
            "materials": int(doc.material_count),
            "flags": int(doc.flags),
            "skinned_flags": int(doc.skinned_flags),
            "object_names": [
                _cstr(doc.objects[i].name)
                for i in range(min(int(doc.object_count), 20))
            ],
            "light_names": [
                _cstr(doc.lights[i].name)
                for i in range(min(int(doc.light_count), 20))
            ],
            "material_names": [
                _cstr(doc.materials[i].name)
                for i in range(min(int(doc.material_count), 20))
            ],
        }
    finally:
        ase_ffi.free_document(doc)


def _cstr(value) -> str:
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", errors="replace")
