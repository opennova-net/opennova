from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_max_package_script_builds_ase_and_anim_mzp() -> None:
    script_path = ROOT / "scripts/package_max_mzp.ps1"
    assert script_path.exists()

    script = script_path.read_text(encoding="utf-8")

    required = [
        "opennova_max-v$Version.mzp",
        "OpenNovaExport.mcr",
        "PackageContents.xml",
        "opennova_max_startup.ms",
        "opennova_max_startup.py",
        "mzp.run",
        "install.ms",
        "install.ds",
        "install.py",
        "OpenNovaMax-*.bundle",
        'Description="macroscripts parts"',
        'Description="post-start-up scripts parts"',
        'BUNDLE_NAME = "$BundleName"',
        "OPENNOVA_MAX_INSTALL_ROOT",
        "PackageContents.xml.disabled-by-opennova-upgrade",
        "OpenNovaMax-locked.bundle",
        "System.IO.Compression.ZipFile",
        "on droppable window node: point: do",
        "on drop window node: point: do",
        "OPENNOVA_MAX_PROFILE_ROOT",
        "_cleanup_max_ui_files",
        "*.mnux",
        ".opennova-cleanup-",
        "Removed stale OpenNova Max UI menu entries",
        "register_menu()",
        "Novalogic ASE (.ase)",
        "maxOps.GetICuiMenuMgr()",
        "#cuiRegisterMenus",
        "OpenNovaExportAse`OpenNova",
        "OpenNovaExportAnims`OpenNova",
        "Novalogic Anims (.adm + .bad)",
        "anim_scene_exporter.py",
        "menuMgr.GetMenuById",
        "eed3eaef-ea24-4342-aacc-9dfd87f9a4f4",
    ]
    for needle in required:
        assert needle in script

    forbidden = [
        "OpenNovaImporter",
        "opennova_qt_ui",
        'Description="macro scripts"',
        'Description="post-start-up scripts"',
    ]
    for needle in forbidden:
        assert needle not in script


def test_max_python_package_exposes_startup_entrypoints() -> None:
    source = _read("opennova_max/__init__.py")

    assert "register_menu" in source
    assert "get_version" in source


def test_ci_ships_blender_zip_and_max_mzp() -> None:
    ci = _read(".github/workflows/ci.yml")

    assert "package-addon:" in ci
    assert "scripts/package_addon.sh" in ci
    assert "opennova_blender" in ci
    assert "dist/opennova_blender-v*.zip" in ci

    assert "package-max-mzp:" in ci
    assert "scripts/package_max_mzp.ps1" in ci
    assert "opennova_max" in ci
    assert "dist/opennova_max-v*.mzp" in ci
    assert "validate-deliverables:" in ci
    assert "actions/download-artifact@v8" in ci
    assert "artifact-ids:" in ci
    assert "merge-multiple: true" in ci
    assert "dist/release-assets/*" not in ci


def test_release_ships_blender_zip_and_max_mzp() -> None:
    release = _read(".github/workflows/release.yml")

    assert "package-addon:" in release
    assert "package-max-mzp:" in release
    assert (
        "needs: [test, package-addon, package-max-mzp, package-importer, package-godot-windows-editor, package-godot-windows-runtime, package-godot-macos-editor, package-godot-macos-runtime]"
        in release
    )
    assert "dist/opennova_blender-v*.zip" in release
    assert "dist/opennova_max-v*.mzp" in release
    assert "scripts/validate_release_deliverables.py" in release
    assert "dist/release-assets/*" in release
