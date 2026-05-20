"""Scene-contract tests for Blender ASE export.

The ASE exporter must translate visible Blender scene state and naming
conventions. It must not rely on importer-authored custom properties to
reconstruct source .3di facts.
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_blender_exporter_source_has_no_bullet_ase_path() -> None:
    source = _read("blender/ase_exporter.py")

    assert "_BulletLOD" not in source
    assert "_bullet" not in source


def test_blender_exporter_source_does_not_read_importer_metadata() -> None:
    source = _read("blender/ase_exporter.py")

    forbidden = [
        "_lod_index",
        "nl_ase_name",
        "opennova_zero_axis",
        "opennova_bone_index",
        'obj.get("light_type"',
        'obj.get("atten_start"',
        'obj.get("falloff"',
        '"tm_row2" in obj',
        'key.startswith("opennova_")',
        'key.startswith("ase_")',
        "descriptor_from_user_props",
    ]

    for needle in forbidden:
        assert needle not in source


def test_blender_importer_source_does_not_write_exporter_metadata() -> None:
    source = _read("opennova_blender/scene_builder.py")

    forbidden = [
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
        "material_user_props",
        'light_obj["atten_start"]',
        'light_obj["falloff"]',
        'light_obj["tm_row2"]',
        'light_obj["light_type"]',
    ]

    for needle in forbidden:
        assert needle not in source


def test_blender_exporter_source_does_not_auto_emit_lod_files() -> None:
    source = _read("blender/ase_exporter.py")

    assert "_find_lod_roots" not in source
    assert "_find_lod0_root" not in source
    assert "_lod{" not in source
