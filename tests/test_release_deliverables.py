from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import zipfile

import pytest


ROOT = Path(__file__).resolve().parents[1]

FIXTURE_SOURCES = {
    "asset-importer": "onimport-v0.1.4.exe",
    "blender-ase-exporter": "opennova_blender-v0.0.4.zip",
    "max-ase-exporter": "opennova_max-v0.1.4.mzp",
    "modding-editor": "opennova-modtools-windows-v0.0.9.zip",
    "game-runtime": "opennova-runtime-windows-v0.0.9.zip",
    "modding-editor-macos": "opennova-modtools-macos-v0.0.9.zip",
    "game-runtime-macos": "opennova-runtime-macos-v0.0.9.zip",
}


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
    (dist / "onimport-v0.1.4.exe").write_bytes(b"MZ importer")
    _write_zip(
        dist / "opennova_blender-v0.0.4.zip",
        [
            "./blender_manifest.toml",
            "./__init__.py",
            "./lib/windows-x64/opennova.dll",
            "./lib/linux-x64/libopennova.so",
        ],
    )
    _write_zip(
        dist / "opennova_max-v0.1.4.mzp",
        [
            "mzp.run",
            "install.ms",
            "install.ds",
            "install.py",
            "OpenNovaMax-0.1.4.bundle/PackageContents.xml",
            "OpenNovaMax-0.1.4.bundle/Contents/macroscripts/OpenNovaExport.mcr",
            "OpenNovaMax-0.1.4.bundle/Contents/startup/opennova_max_startup.ms",
            "OpenNovaMax-0.1.4.bundle/Contents/startup/opennova_max_startup.py",
            "OpenNovaMax-0.1.4.bundle/Contents/python/opennova_max/__init__.py",
            "OpenNovaMax-0.1.4.bundle/Contents/python/pyopennova/lib/windows-x64/opennova.dll",
        ],
    )
    _write_zip(
        dist / "opennova-modtools-windows-v0.0.9.zip",
        [
            "opennova-modtools.exe",
            "libopennova.windows.template_release.x86_64.dll",
        ],
    )
    _write_zip(
        dist / "opennova-runtime-windows-v0.0.9.zip",
        [
            "opennova.exe",
            "libopennova.windows.template_release.x86_64.dll",
        ],
    )
    _write_zip(
        dist / "opennova-modtools-macos-v0.0.9.zip",
        [
            "opennova-modtools.app/Contents/Info.plist",
            "opennova-modtools.app/Contents/MacOS/OpenNova",
            "opennova-modtools.app/Contents/Frameworks/libopennova.macos.template_release.universal.dylib",
        ],
    )
    _write_zip(
        dist / "opennova-runtime-macos-v0.0.9.zip",
        [
            "opennova.app/Contents/Info.plist",
            "opennova.app/Contents/MacOS/OpenNova",
            "opennova.app/Contents/Frameworks/libopennova.macos.template_release.universal.dylib",
        ],
    )


def _keep_deliverable_fixtures(dist: Path, deliverable_ids: set[str]) -> None:
    for deliverable_id, source_name in FIXTURE_SOURCES.items():
        if deliverable_id not in deliverable_ids:
            (dist / source_name).unlink()


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
        release_version="v0.0.9",
    )

    public_names = sorted(path.name for path in stage.iterdir())
    assert public_names == [
        "opennova-3ds-max-ase-exporter-windows-v0.0.9.mzp",
        "opennova-asset-importer-windows-v0.0.9.exe",
        "opennova-blender-ase-exporter-v0.0.9.zip",
        "opennova-game-runtime-macos-v0.0.9.zip",
        "opennova-game-runtime-windows-v0.0.9.zip",
        "opennova-modding-editor-macos-v0.0.9.zip",
        "opennova-modding-editor-windows-v0.0.9.zip",
    ]
    assert sorted(item.public_name for item in result.items) == public_names

    text = body.read_text(encoding="utf-8")
    assert "## Release Assets" in text
    assert "opennova-blender-ase-exporter-v0.0.9.zip" in text
    assert "Install:" in text
    assert "Use:" in text
    assert "Blender" in text
    assert "3ds Max" in text


def test_release_validator_selects_manifest_deliverables_in_manifest_order(
    tmp_path: Path,
) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    selected_ids = {"modding-editor", "game-runtime"}
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _keep_deliverable_fixtures(dist, selected_ids)

    result = validator.validate_release_deliverables(
        repo_root=ROOT,
        dist_dir=dist,
        stage_dir=stage,
        release_body=body,
        release_version="v0.0.9",
        deliverable_ids=["game-runtime", "modding-editor"],
    )

    assert [item.id for item in result.items] == ["modding-editor", "game-runtime"]
    assert {path.name for path in stage.iterdir()} == {
        "opennova-modding-editor-windows-v0.0.9.zip",
        "opennova-game-runtime-windows-v0.0.9.zip",
    }
    body_text = body.read_text(encoding="utf-8")
    assert "opennova-modding-editor-windows-v0.0.9.zip" in body_text
    assert "opennova-game-runtime-windows-v0.0.9.zip" in body_text
    assert "opennova-asset-importer-windows-v0.0.9.exe" not in body_text
    assert "macos" not in body_text.lower()


def test_release_validator_cli_accepts_repeated_only_id(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    selected_ids = {"modding-editor", "game-runtime"}
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _keep_deliverable_fixtures(dist, selected_ids)

    exit_code = validator.main(
        [
            "--repo-root",
            str(ROOT),
            "--dist",
            str(dist),
            "--stage-dir",
            str(stage),
            "--release-body",
            str(body),
            "--release-version",
            "v0.0.9",
            "--only-id",
            "modding-editor",
            "--only-id",
            "game-runtime",
        ]
    )

    assert exit_code == 0
    assert {path.name for path in stage.iterdir()} == {
        "opennova-modding-editor-windows-v0.0.9.zip",
        "opennova-game-runtime-windows-v0.0.9.zip",
    }


@pytest.mark.parametrize(
    ("deliverable_ids", "message"),
    [
        (["not-a-deliverable"], "Unknown deliverable IDs: not-a-deliverable"),
        (
            ["modding-editor", "modding-editor"],
            "Duplicate deliverable IDs: modding-editor",
        ),
        ([], "At least one deliverable ID must be selected"),
    ],
)
def test_release_validator_rejects_invalid_selection(
    tmp_path: Path,
    deliverable_ids: list[str],
    message: str,
) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_deliverable_fixtures(dist)

    with pytest.raises(validator.DeliverableValidationError, match=message):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.9",
            deliverable_ids=deliverable_ids,
        )


def test_release_validator_rejects_unselected_dist_files(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_deliverable_fixtures(dist)

    with pytest.raises(validator.DeliverableValidationError, match="Unexpected files"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.9",
            deliverable_ids=["modding-editor", "game-runtime"],
        )


def test_release_validator_rejects_missing_archive_entry(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _write_zip(dist / "opennova_blender-v0.0.4.zip", ["blender_manifest.toml"])

    with pytest.raises(validator.DeliverableValidationError, match="lib/windows-x64/opennova.dll"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.9",
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
            release_version="0.0.9",
        )


def test_release_workflow_validates_and_publishes_staged_assets() -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    release_step = workflow.split("- name: Create GitHub Release", 1)[1]
    package_jobs = [
        "package-addon",
        "package-max-mzp",
        "package-importer",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
        "package-godot-macos-editor",
        "package-godot-macos-runtime",
    ]

    assert "scripts/validate_release_deliverables.py" in workflow
    assert "GITHUB_REF_NAME" in workflow
    assert "dist/release-assets/*" in release_step
    assert "dist/*.zip" not in release_step
    assert "dist/opennova_max-v*.mzp" not in release_step
    assert "dist/onimport-v*.exe" not in release_step

    for package_job in package_jobs:
        body = _workflow_job(workflow, package_job)
        assert "uses: actions/upload-artifact@v7" in body
        assert "archive: false" in body
        assert "if-no-files-found: error" in body
        assert "artifact_id:" in body

    release_job = _workflow_job(workflow, "release")
    assert "uses: actions/download-artifact@v8" in release_job
    assert "artifact-ids:" in release_job


def test_ci_validates_windows_package_artifacts_and_uses_versioned_upload_globs() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    package_jobs = [
        "package-addon",
        "package-max-mzp",
        "package-importer",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
    ]
    deferred_pr_jobs = ["package-addon", "package-max-mzp", "package-importer"]
    godot_package_jobs = ["package-godot-windows-editor", "package-godot-windows-runtime"]

    assert "validate-deliverables:" in workflow
    assert "package-godot-windows-editor:" in workflow
    assert "package-godot-windows-runtime:" in workflow
    assert "package-godot-macos-editor:" not in workflow
    assert "package-godot-macos-runtime:" not in workflow
    assert "package-godot:" not in workflow
    assert "package-godot-editor:" not in workflow
    assert "package-godot-runtime:" not in workflow
    assert "package-godot-macos:" not in workflow
    assert "scripts/package_godot_editor_windows.ps1" in workflow
    assert "scripts/package_godot_runtime_windows.ps1" in workflow
    assert "scripts/package_godot_editor_macos.sh" not in workflow
    assert "scripts/package_godot_runtime_macos.sh" not in workflow
    assert "BUILD_GODOT: \"0\"" in _workflow_job(workflow, "test")

    # Tool packages are retained for master/manual runs but leave the PR hot path.
    for package_job in deferred_pr_jobs:
        body = _workflow_job(workflow, package_job)
        assert "if: github.event_name != 'pull_request'" in body
        assert "needs:" not in _workflow_job(workflow, package_job)

    # Windows preview packages reuse the prebuilt GDExtension.
    assert "needs: [build-gdextension-windows]" in _workflow_job(workflow, "package-godot-windows-editor")
    assert "needs: [build-gdextension-windows]" in _workflow_job(workflow, "package-godot-windows-runtime")

    for package_job in godot_package_jobs:
        body = _workflow_job(workflow, package_job)
        assert "Cache Godot binary" in body
        assert "Cache Godot export templates" in body

    validate_job = _workflow_job(workflow, "validate-deliverables")
    assert (
        "needs: [test, godot-tests, package-addon, package-max-mzp, package-importer, "
        "package-godot-windows-editor, package-godot-windows-runtime]"
    ) in validate_job
    assert "always()" in validate_job
    for package_job in package_jobs:
        assert f"needs.{package_job}" in validate_job
    assert "Download pull request package artifacts" in validate_job
    assert "Download non-PR package artifacts" in validate_job
    assert "--release-version 0.0.0-ci" in validate_job
    assert "--only-id asset-importer" in validate_job
    assert "--only-id blender-ase-exporter" in validate_job
    assert "--only-id max-ase-exporter" in validate_job
    assert validate_job.count("--only-id modding-editor") == 2
    assert validate_job.count("--only-id game-runtime") == 2

    assert "dist/onimport-v*.exe" in workflow
    assert "dist/opennova-modtools-windows-v*.zip" in workflow
    assert "dist/opennova-runtime-windows-v*.zip" in workflow
    assert "dist/onimport.exe" not in workflow
    assert "dist/opennova-modtools-windows.zip" not in workflow
    assert "dist/opennova-runtime-windows.zip" not in workflow
    assert "opennova_release_assets" not in workflow


def test_windows_godot_tests_use_console_binary_for_bash_runner() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    godot_tests = _workflow_job(workflow, "godot-tests")
    # The candidate order lives in the shared resolver test_godot.sh sources
    # (scripts/godot_bin.sh, the W2-5 extraction).
    resolver = (ROOT / "scripts/godot_bin.sh").read_text(encoding="utf-8")
    test_script = (ROOT / "scripts/test_godot.sh").read_text(encoding="utf-8")

    assert "Godot_v${version}_win64_console.exe" in godot_tests
    assert "Godot_v${GODOT_VERSION}_win64_console.exe" in godot_tests
    assert 'source "$root/scripts/godot_bin.sh"' in test_script
    assert "Godot_v4.6.1-stable_win64_console.exe" in resolver
    assert resolver.index("Godot_v4.6.1-stable_win64_console.exe") < resolver.index(
        "Godot_v4.6.1-stable_win64.exe"
    )


def test_ci_builds_windows_gdextension_once_and_caches_with_sccache() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")

    # Regular CI is Windows-only; tag release CI retains the macOS deliverables.
    assert "os: [windows-latest]" in _workflow_job(workflow, "test")
    assert "os: [windows-latest]" in _workflow_job(workflow, "godot-tests")
    assert "macos-latest" not in workflow
    assert "build-gdextension-windows:" in workflow
    assert "build-gdextension-macos:" not in workflow
    assert "gdext_macos" not in workflow

    # The Windows build caches godot-cpp's objects and uses the optimized
    # symbol-bearing configuration for the template_debug test DLL.
    assert "mozilla-actions/sccache-action" in workflow
    assert "SCCACHE_GHA_ENABLED" in workflow
    assert "CMAKE_CXX_COMPILER_LAUNCHER: sccache" in workflow

    win_build = _workflow_job(workflow, "build-gdextension-windows")
    assert "-G Ninja" in win_build
    assert "ilammy/msvc-dev-cmd" in win_build
    assert 'b = "RelWithDebInfo"' in win_build
    assert 'b = "Debug"' not in win_build

    # Godot tests no longer wait for a macOS build.
    assert "needs: [build-gdextension-windows]" in _workflow_job(workflow, "godot-tests")
    consumers = [
        "godot-tests",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
    ]
    for job in consumers:
        body = _workflow_job(workflow, job)
        assert "actions/download-artifact" in body
        # No consumer recompiles the GDExtension inline.
        assert "cmake -S godot/engine" not in body

    for job in [
        "package-addon",
        "package-max-mzp",
        "package-importer",
        "package-godot-windows-editor",
        "package-godot-windows-runtime",
    ]:
        body = _workflow_job(workflow, job)
        assert "uses: actions/upload-artifact@v7" in body
        assert "archive: false" in body
        assert "if-no-files-found: error" in body
        assert "artifact_id:" in body

    # validate-deliverables downloads the exact package artifact IDs into dist/,
    # avoiding both gdext_* build artifacts and GitHub's wrapper zip format.
    validate_job = _workflow_job(workflow, "validate-deliverables")
    assert "uses: actions/download-artifact@v8" in validate_job
    assert "artifact-ids:" in validate_job
    assert "pattern: opennova_*" not in validate_job


def test_ci_runs_ctest_in_parallel() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    build_script = (ROOT / "scripts/build.sh").read_text(encoding="utf-8")

    assert '--parallel "$jobs"' in build_script
    assert '--parallel "$(nproc)"' in _workflow_job(workflow, "novaworld-server")


def test_godot_test_wrapper_allows_fixture_inner_classes() -> None:
    test_script = (ROOT / "scripts/test_godot.sh").read_text(encoding="utf-8")
    collection_patterns = re.findall(
        r"grep\s+-[qE]+\s+'([^']*does not extend GutTest)'",
        test_script,
    )

    assert collection_patterns
    assert any(pattern.startswith("Ignoring script ") for pattern in collection_patterns)
    assert all("Inner Class" not in pattern for pattern in collection_patterns)


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
