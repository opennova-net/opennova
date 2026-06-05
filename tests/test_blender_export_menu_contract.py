from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_blender_addon_exposes_ase_and_anim_export_menus() -> None:
    source = _read("blender/__init__.py")

    # ASE export
    assert "class EXPORT_OT_novalogic_ase" in source
    assert 'bl_idname = "export_scene.novalogic_ase"' in source
    assert 'self.layout.operator(EXPORT_OT_novalogic_ase.bl_idname, text="Novalogic ASE (.ase)")' in source

    # Animation (.adm + .bad) export
    assert "class EXPORT_OT_novalogic_anims" in source
    assert 'bl_idname = "export_scene.novalogic_anims"' in source
    assert "class NovalogicAnimClipItem" in source
    assert "from . import anim_exporter" in source
    assert (
        'self.layout.operator(EXPORT_OT_novalogic_anims.bl_idname, text="Novalogic Anims (.adm + .bad)")'
        in source
    )

    assert "TOPBAR_MT_file_export.append(menu_func_export)" in source

    # The Mixamo FBX importer remains out of scope (no File > Import menu).
    forbidden = [
        "Mixamo",
        "mixamo_importer",
        "IMPORT_OT_mixamo_novalogic",
        "menu_func_import",
        "TOPBAR_MT_file_import",
    ]
    for needle in forbidden:
        assert needle not in source, needle


def test_blender_manifest_mentions_animation_export() -> None:
    manifest = _read("blender/blender_manifest.toml")

    assert 'tagline = "Export Novalogic ASE models and ADM/BAD animations"' in manifest
    assert 'files = "Export Novalogic ASE models and ADM/BAD animation files"' in manifest
    assert '"Import-Export"' in manifest

    # Mixamo import is still out of scope.
    assert "Mixamo" not in manifest


def test_blender_anim_export_modules_are_shipped() -> None:
    present = [
        "blender/anim_exporter.py",
        "blender/opennova/bad_ffi.py",
        "blender/opennova/adm_ffi.py",
        "blender/opennova/bad_build.py",
        "blender/opennova/animation_build.py",
        "blender/opennova/coords.py",
    ]
    for rel in present:
        assert (ROOT / rel).exists(), f"expected shipped: {rel}"

    # The Mixamo importer and import-only definition helpers stay out of scope.
    absent = [
        "blender/mixamo_importer.py",
        "blender/opennova/definitions.py",
        "blender/opennova/model.py",
    ]
    for rel in absent:
        assert not (ROOT / rel).exists(), f"should not be shipped: {rel}"
