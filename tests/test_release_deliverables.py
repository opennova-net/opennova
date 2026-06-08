from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import zipfile

import pytest


ROOT = Path(__file__).resolve().parents[1]


def _load_validator():
    path = ROOT / "scripts" / "validate_release_deliverables.py"
    spec = importlib.util.spec_from_file_location("validate_release_deliverables", path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _write_zip(path: Path, names: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name in names:
            archive.writestr(name, f"{name}\n")


def _workflow_job(workflow: str, job_name: str) -> str:
    match = re.search(
        rf"^  {re.escape(job_name)}:\n(?P<body>.*?)(?=^  [A-Za-z0-9_-]+:|\Z)",
        workflow,
        re.MULTILINE | re.DOTALL,
    )
    assert match is not None, f"job not found: {job_name}"
    return match.group("body")


def _write_deliverable_fixtures(dist: Path) -> None:
    (dist / "onimport-v0.1.3.exe").write_bytes(b"MZ importer")
    _write_zip(
        dist / "opennova_blender-v0.0.3.zip",
        [
            "./blender_manifest.toml",
            "./__init__.py",
            "./lib/windows-x64/opennova.dll",
            "./lib/linux-x64/libopennova.so",
        ],
    )
    _write_zip(
        dist / "opennova_max-v0.1.3.mzp",
        [
            "mzp.run",
            "install.ms",
            "install.ds",
            "install.py",
            "OpenNovaMax-0.1.3.bundle/PackageContents.xml",
            "OpenNovaMax-0.1.3.bundle/Contents/macroscripts/OpenNovaExport.mcr",
            "OpenNovaMax-0.1.3.bundle/Contents/startup/opennova_max_startup.ms",
            "OpenNovaMax-0.1.3.bundle/Contents/startup/opennova_max_startup.py",
            "OpenNovaMax-0.1.3.bundle/Contents/python/opennova_max/__init__.py",
            "OpenNovaMax-0.1.3.bundle/Contents/python/pyopennova/lib/windows-x64/opennova.dll",
        ],
    )
    _write_zip(
        dist / "opennova-modtools-windows-v0.0.8.zip",
        [
            "opennova-modtools.exe",
            "libopennova.windows.template_release.x86_64.dll",
        ],
    )
    _write_zip(
        dist / "opennova-runtime-windows-v0.0.8.zip",
        [
            "opennova.exe",
            "libopennova.windows.template_release.x86_64.dll",
        ],
    )
    _write_zip(
        dist / "opennova-modtools-macos-v0.0.8.zip",
        [
            "opennova-modtools.app/Contents/Info.plist",
            "opennova-modtools.app/Contents/MacOS/OpenNova",
            "opennova-modtools.app/Contents/Frameworks/libopennova.macos.template_release.universal.dylib",
        ],
    )
    _write_zip(
        dist / "opennova-runtime-macos-v0.0.8.zip",
        [
            "opennova.app/Contents/Info.plist",
            "opennova.app/Contents/MacOS/OpenNova",
            "opennova.app/Contents/Frameworks/libopennova.macos.template_release.universal.dylib",
        ],
    )


def test_release_validator_stages_public_assets_and_release_body(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    dist.mkdir()
    _write_deliverable_fixtures(dist)

    result = validator.validate_release_deliverables(
        repo_root=ROOT,
        dist_dir=dist,
        stage_dir=stage,
        release_body=body,
        release_version="v0.0.8",
    )

    public_names = sorted(path.name for path in stage.iterdir())
    assert public_names == [
        "opennova-3ds-max-ase-exporter-windows-v0.0.8.mzp",
        "opennova-asset-importer-windows-v0.0.8.exe",
        "opennova-blender-ase-exporter-v0.0.8.zip",
        "opennova-game-runtime-macos-v0.0.8.zip",
        "opennova-game-runtime-windows-v0.0.8.zip",
        "opennova-modding-editor-macos-v0.0.8.zip",
        "opennova-modding-editor-windows-v0.0.8.zip",
    ]
    assert sorted(item.public_name for item in result.items) == public_names

    text = body.read_text(encoding="utf-8")
    assert "## Release Assets" in text
    assert "opennova-blender-ase-exporter-v0.0.8.zip" in text
    assert "Install:" in text
    assert "Use:" in text
    assert "Blender" in text
    assert "3ds Max" in text


def test_release_validator_rejects_missing_archive_entry(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _write_zip(dist / "opennova_blender-v0.0.3.zip", ["blender_manifest.toml"])

    with pytest.raises(validator.DeliverableValidationError, match="lib/windows-x64/opennova.dll"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.8",
        )


def test_release_validator_rejects_unexpected_dist_files(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    (dist / "mystery-tool.zip").write_bytes(b"extra")

    with pytest.raises(validator.DeliverableValidationError, match="Unexpected files"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.8",
        )


def test_release_workflow_validates_and_publishes_staged_assets() -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    release_step = workflow.split("- name: Create GitHub Release", 1)[1]

    assert "scripts/validate_release_deliverables.py" in workflow
    assert "GITHUB_REF_NAME" in workflow
    assert "dist/release-assets/*" in release_step
    assert "dist/*.zip" not in release_step
    assert "dist/opennova_max-v*.mzp" not in release_step
    assert "dist/onimport-v*.exe" not in release_step


def test_ci_validates_package_artifacts_and_uses_versioned_upload_globs() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    package_jobs = [
        "package-addon",
        "package-max-mzp",
        "package-importer",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
        "package-godot-macos-editor",
        "package-godot-macos-runtime",
    ]
    godot_package_jobs = [
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
        "package-godot-macos-editor",
        "package-godot-macos-runtime",
    ]

    assert "validate-deliverables:" in workflow
    assert "package-godot-windows-editor:" in workflow
    assert "package-godot-windows-runtime:" in workflow
    assert "package-godot-macos-editor:" in workflow
    assert "package-godot-macos-runtime:" in workflow
    assert "package-godot:" not in workflow
    assert "package-godot-editor:" not in workflow
    assert "package-godot-runtime:" not in workflow
    assert "package-godot-macos:" not in workflow
    assert "scripts/package_godot_editor_windows.ps1" in workflow
    assert "scripts/package_godot_runtime_windows.ps1" in workflow
    assert "scripts/package_godot_editor_macos.sh" in workflow
    assert "scripts/package_godot_runtime_macos.sh" in workflow
    assert "BUILD_GODOT: \"0\"" in _workflow_job(workflow, "test")
    # The non-Godot package jobs run independently (no needs).
    for package_job in package_jobs:
        if package_job in godot_package_jobs:
            continue  # these now need their build-gdextension-<os> job (asserted below)
        assert "needs:" not in _workflow_job(workflow, package_job)
    # The Godot package jobs reuse a prebuilt GDExtension (compiled once per OS by
    # build-gdextension-<os>) instead of recompiling it, so each needs its build job.
    assert "needs: [build-gdextension-windows]" in _workflow_job(workflow, "package-godot-windows-editor")
    assert "needs: [build-gdextension-windows]" in _workflow_job(workflow, "package-godot-windows-runtime")
    assert "needs: [build-gdextension-macos]" in _workflow_job(workflow, "package-godot-macos-editor")
    assert "needs: [build-gdextension-macos]" in _workflow_job(workflow, "package-godot-macos-runtime")
    assert (
        "needs: [test, godot-tests, package-addon, package-max-mzp, package-importer, "
        "package-godot-windows-editor, package-godot-windows-runtime, "
        "package-godot-macos-editor, package-godot-macos-runtime]"
    ) in _workflow_job(workflow, "validate-deliverables")
    for package_job in godot_package_jobs:
        body = _workflow_job(workflow, package_job)
        assert "Cache Godot binary" in body
        assert "Cache Godot export templates" in body
    assert "scripts/validate_release_deliverables.py --release-version 0.0.0-ci" in workflow
    assert "dist/onimport-v*.exe" in workflow
    assert "dist/opennova-modtools-windows-v*.zip" in workflow
    assert "dist/opennova-runtime-windows-v*.zip" in workflow
    assert "dist/onimport.exe" not in workflow
    assert "dist/opennova-modtools-windows.zip" not in workflow
    assert "dist/opennova-runtime-windows.zip" not in workflow


def test_ci_builds_gdextension_once_per_os_and_caches_with_sccache() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")

    # The GDExtension (godot/engine + libs/ + the pinned godot-cpp submodule) is
    # compiled once per OS in dedicated jobs, not in every consumer.
    assert "build-gdextension-windows:" in workflow
    assert "build-gdextension-macos:" in workflow

    # Those build jobs cache godot-cpp's objects across runs via sccache.
    assert "mozilla-actions/sccache-action" in workflow
    assert "SCCACHE_GHA_ENABLED" in workflow
    assert "CMAKE_CXX_COMPILER_LAUNCHER: sccache" in workflow

    # Windows must use the Ninja generator (the Visual Studio generator ignores
    # CMAKE_*_COMPILER_LAUNCHER) with the MSVC environment activated.
    win_build = _workflow_job(workflow, "build-gdextension-windows")
    assert "-G Ninja" in win_build
    assert "ilammy/msvc-dev-cmd" in win_build

    # godot-tests and the Godot package jobs consume the prebuilt DLLs: each needs a
    # build job and downloads the artifact rather than recompiling.
    assert (
        "needs: [build-gdextension-windows, build-gdextension-macos]"
        in _workflow_job(workflow, "godot-tests")
    )
    consumers = [
        "godot-tests",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
        "package-godot-macos-editor",
        "package-godot-macos-runtime",
    ]
    for job in consumers:
        body = _workflow_job(workflow, job)
        assert "actions/download-artifact" in body
        # No consumer recompiles the GDExtension inline.
        assert "cmake -S godot/engine" not in body

    # validate-deliverables downloads every artifact into dist/, so it must scope
    # the download to the deliverables (opennova_*) and skip the gdext_* build
    # artifacts, which the validator rejects as unexpected files in dist/.
    assert "pattern: opennova_*" in _workflow_job(workflow, "validate-deliverables")


def test_release_splits_godot_editor_and_runtime_package_jobs() -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    package_jobs = [
        "package-addon",
        "package-max-mzp",
        "package-importer",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
        "package-godot-macos-editor",
        "package-godot-macos-runtime",
    ]
    godot_package_jobs = [
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
        "package-godot-macos-editor",
        "package-godot-macos-runtime",
    ]

    assert "package-godot-windows-editor:" in workflow
    assert "package-godot-windows-runtime:" in workflow
    assert "package-godot-macos-editor:" in workflow
    assert "package-godot-macos-runtime:" in workflow
    assert "package-godot:" not in workflow
    assert "package-godot-editor:" not in workflow
    assert "package-godot-runtime:" not in workflow
    assert "package-godot-macos:" not in workflow
    assert "scripts/package_godot_editor_windows.ps1" in workflow
    assert "scripts/package_godot_runtime_windows.ps1" in workflow
    assert "scripts/package_godot_editor_macos.sh" in workflow
    assert "scripts/package_godot_runtime_macos.sh" in workflow
    assert "BUILD_GODOT: \"0\"" in _workflow_job(workflow, "test")
    for package_job in package_jobs:
        assert "needs:" not in _workflow_job(workflow, package_job)
    assert (
        "needs: [test, package-addon, package-max-mzp, package-importer, "
        "package-godot-windows-editor, package-godot-windows-runtime, "
        "package-godot-macos-editor, package-godot-macos-runtime]"
    ) in _workflow_job(workflow, "release")
    for package_job in godot_package_jobs:
        body = _workflow_job(workflow, package_job)
        assert "Cache Godot binary" in body
        assert "Cache Godot export templates" in body


def test_godot_package_wrappers_target_editor_and_runtime() -> None:
    windows_editor = (ROOT / "scripts/package_godot_editor_windows.ps1").read_text(encoding="utf-8")
    windows_runtime = (ROOT / "scripts/package_godot_runtime_windows.ps1").read_text(encoding="utf-8")
    windows_shared = (ROOT / "scripts/package_godot_windows.ps1").read_text(encoding="utf-8")
    macos_editor = (ROOT / "scripts/package_godot_editor_macos.sh").read_text(encoding="utf-8")
    macos_runtime = (ROOT / "scripts/package_godot_runtime_macos.sh").read_text(encoding="utf-8")
    macos_shared = (ROOT / "scripts/package_godot_macos.sh").read_text(encoding="utf-8")

    assert "-Target editor" in windows_editor
    assert "-Target runtime" in windows_runtime
    assert "[ValidateSet(\"all\", \"editor\", \"runtime\")]" in windows_shared
    assert "package_godot_macos.sh editor" in macos_editor
    assert "package_godot_macos.sh runtime" in macos_runtime
    assert "TARGET=\"${1:-all}\"" in macos_shared


def test_readme_lists_public_asset_names_and_install_hints() -> None:
    readme = (ROOT / "README.md").read_text(encoding="utf-8")

    for name in [
        "opennova-asset-importer-windows-v<version>.exe",
        "opennova-blender-ase-exporter-v<version>.zip",
        "opennova-3ds-max-ase-exporter-windows-v<version>.mzp",
        "opennova-modding-editor-windows-v<version>.zip",
        "opennova-game-runtime-windows-v<version>.zip",
    ]:
        assert name in readme
    assert "Install from Blender" in readme
    assert "Run the MZP" in readme
    assert "Extract the zip" in readme


def test_blender_manifest_names_the_ase_and_anim_exporter() -> None:
    manifest = (ROOT / "blender/blender_manifest.toml").read_text(encoding="utf-8")

    assert 'name = "OpenNova Blender ASE & Anim Exporter"' in manifest
    assert 'tagline = "Export Novalogic ASE models and ADM/BAD animations"' in manifest
