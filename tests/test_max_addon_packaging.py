from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_max_package_script_builds_ase_only_mzp() -> None:
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
        "install.py",
        "OpenNovaMax-*.bundle",
        "register_menu()",
        "Novalogic ASE (.ase)",
    ]
    for needle in required:
        assert needle in script

    forbidden = [
        "OpenNovaImporter",
        "OpenNovaExportAnims",
        "opennova_qt_ui",
        "Novalogic Anims",
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
    assert "dist/*.zip" in ci

    assert "package-max-mzp:" in ci
    assert "scripts/package_max_mzp.ps1" in ci
    assert "opennova_max" in ci
    assert "dist/opennova_max-v*.mzp" in ci


def test_release_ships_blender_zip_and_max_mzp() -> None:
    release = _read(".github/workflows/release.yml")

    assert "package-addon:" in release
    assert "package-max-mzp:" in release
    assert "needs: [package-addon, package-max-mzp, package-importer, package-godot]" in release
    assert "dist/opennova_blender-v*.zip" in release
    assert "dist/opennova_max-v*.mzp" in release
    assert "3ds Max plugin" in release
