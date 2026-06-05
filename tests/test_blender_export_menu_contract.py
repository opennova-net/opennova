from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_blender_addon_exposes_only_ase_export_menu() -> None:
    source = _read("blender/__init__.py")

    assert "class EXPORT_OT_novalogic_ase" in source
    assert 'bl_idname = "export_scene.novalogic_ase"' in source
    assert 'self.layout.operator(EXPORT_OT_novalogic_ase.bl_idname, text="Novalogic ASE (.ase)")' in source
    assert "TOPBAR_MT_file_export.append(menu_func_export)" in source

    forbidden = [
        "Mixamo",
        "mixamo_importer",
        "IMPORT_OT_mixamo_novalogic",
        "menu_func_import",
        "TOPBAR_MT_file_import",
        "Novalogic Anims",
        "EXPORT_OT_novalogic_anims",
        "anim_exporter",
        "ADM",
        "BAD",
    ]
    for needle in forbidden:
        assert needle not in source


def test_blender_manifest_is_ase_export_only() -> None:
    manifest = _read("blender/blender_manifest.toml")

    assert 'tagline = "Export Novalogic ASE files"' in manifest
    assert 'files = "Export Novalogic ASE files"' in manifest
    assert '"Import-Export"' in manifest

    for needle in ["Animation", "Mixamo", "ADM", "BAD", "import Mixamo"]:
        assert needle not in manifest


def test_removed_blender_animation_and_mixamo_modules_are_not_shipped() -> None:
    removed_paths = [
        "blender/anim_exporter.py",
        "blender/mixamo_importer.py",
        "blender/opennova/adm_ffi.py",
        "blender/opennova/bad_ffi.py",
        "blender/opennova/definitions.py",
        "blender/opennova/model.py",
    ]

    for rel in removed_paths:
        assert not (ROOT / rel).exists(), rel
