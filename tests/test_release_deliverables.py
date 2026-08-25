from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import zipfile

import pytest


ROOT = Path(__file__).resolve().parents[1]

FIXTURE_SOURCES = {
    "asset-importer": "onimport-v0.1.5.exe",
    "blender-ase-exporter": "opennova_blender-v0.0.5.zip",
    "windows-apps": "opennova-windows-v0.0.10.zip",
    "windows-game": "opennova-game-windows-v0.0.10.zip",
}

# The DEV zip: game + ONED + loose sources — one
# copy of the data, no packed archives. The GDExtension DLL rides per flavour.
WINDOWS_APPS_ENTRIES = [
    "opennova-modtools.exe",
    "opennova.exe",
    "assets/items.def",
    "assets/mnml.bms",
    "assets/mnml.cpt",
]

# The GAME zip (tagged releases): opennova.exe + the packed game it
# default-mounts from its own directory, plus the loose-by-contract files.
WINDOWS_GAME_ENTRIES = [
    "opennova.exe",
    "localres.pff",
    "menumus.sbf",
    "gamemus.sbf",
    "earlyerr.txt",
]


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
    (dist / "onimport-v0.1.5.exe").write_bytes(b"MZ importer")
    _write_zip(
        dist / "opennova_blender-v0.0.5.zip",
        [
            "./blender_manifest.toml",
            "./__init__.py",
            "./lib/windows-x64/opennova.dll",
            "./lib/linux-x64/libopennova.so",
        ],
    )
    _write_zip(
        dist / FIXTURE_SOURCES["windows-apps"],
        WINDOWS_APPS_ENTRIES + ["libopennova.windows.template_release.x86_64.dll"],
    )
    _write_zip(
        dist / FIXTURE_SOURCES["windows-game"],
        WINDOWS_GAME_ENTRIES + ["libopennova.windows.template_release.x86_64.dll"],
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
        release_version="v0.0.10",
    )

    public_names = sorted(path.name for path in stage.iterdir())
    assert public_names == [
        "opennova-asset-importer-windows-v0.0.10.exe",
        "opennova-blender-ase-exporter-v0.0.10.zip",
        "opennova-game-windows-v0.0.10.zip",
        "opennova-windows-v0.0.10.zip",
    ]
    assert sorted(item.public_name for item in result.items) == public_names

    text = body.read_text(encoding="utf-8")
    assert "## Release Assets" in text
    assert "opennova-blender-ase-exporter-v0.0.10.zip" in text
    assert "Install:" in text
    assert "Use:" in text
    assert "Blender" in text
    # The one Windows zip's install hint tells the user the layout is load-bearing.
    assert "the root is the game dir" in text


def test_release_validator_selects_manifest_deliverables_in_manifest_order(
    tmp_path: Path,
) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    selected_ids = {"blender-ase-exporter", "windows-apps"}
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _keep_deliverable_fixtures(dist, selected_ids)

    result = validator.validate_release_deliverables(
        repo_root=ROOT,
        dist_dir=dist,
        stage_dir=stage,
        release_body=body,
        release_version="v0.0.10",
        deliverable_ids=["windows-apps", "blender-ase-exporter"],
    )

    assert [item.id for item in result.items] == ["blender-ase-exporter", "windows-apps"]
    assert {path.name for path in stage.iterdir()} == {
        "opennova-blender-ase-exporter-v0.0.10.zip",
        "opennova-windows-v0.0.10.zip",
    }
    body_text = body.read_text(encoding="utf-8")
    assert "opennova-blender-ase-exporter-v0.0.10.zip" in body_text
    assert "opennova-windows-v0.0.10.zip" in body_text
    assert "opennova-asset-importer-windows-v0.0.10.exe" not in body_text
    assert "macos" not in body_text.lower()


def test_release_validator_cli_accepts_repeated_only_id(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    selected_ids = {"blender-ase-exporter", "windows-apps"}
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
            "v0.0.10",
            "--only-id",
            "blender-ase-exporter",
            "--only-id",
            "windows-apps",
        ]
    )

    assert exit_code == 0
    assert {path.name for path in stage.iterdir()} == {
        "opennova-blender-ase-exporter-v0.0.10.zip",
        "opennova-windows-v0.0.10.zip",
    }


@pytest.mark.parametrize(
    ("deliverable_ids", "message"),
    [
        (["not-a-deliverable"], "Unknown deliverable IDs: not-a-deliverable"),
        (
            ["windows-apps", "windows-apps"],
            "Duplicate deliverable IDs: windows-apps",
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
            release_version="0.0.10",
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
            release_version="0.0.10",
            deliverable_ids=["windows-apps"],
        )


def _write_debug_mode_windows_zip(dist: Path) -> None:
    # What pull-request CI packages: --export-debug exes beside the template_debug
    # GDExtension (see scripts/package_godot_windows.ps1 -ExportMode debug).
    _write_zip(
        dist / FIXTURE_SOURCES["windows-apps"],
        WINDOWS_APPS_ENTRIES + ["libopennova.windows.template_debug.x86_64.dll"],
    )
    _write_zip(
        dist / FIXTURE_SOURCES["windows-game"],
        WINDOWS_GAME_ENTRIES + ["libopennova.windows.template_debug.x86_64.dll"],
    )


def test_release_validator_selects_the_gdextension_flavour(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    godot_ids = ["windows-apps", "windows-game"]
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _keep_deliverable_fixtures(dist, set(godot_ids))

    # A release-mode zip (the default flavour) does not satisfy the debug flavour...
    with pytest.raises(
        validator.DeliverableValidationError,
        match="libopennova.windows.template_debug.x86_64.dll",
    ):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=stage,
            release_body=body,
            release_version="0.0.10",
            deliverable_ids=godot_ids,
            gdextension_target="template_debug",
        )

    # ...and a debug-mode zip satisfies template_debug (the PR CI path) but not the
    # default release flavour.
    _write_debug_mode_windows_zip(dist)
    result = validator.validate_release_deliverables(
        repo_root=ROOT,
        dist_dir=dist,
        stage_dir=stage,
        release_body=body,
        release_version="0.0.10",
        deliverable_ids=godot_ids,
        gdextension_target="template_debug",
    )
    assert [item.id for item in result.items] == godot_ids
    with pytest.raises(
        validator.DeliverableValidationError,
        match="libopennova.windows.template_release.x86_64.dll",
    ):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=stage,
            release_body=body,
            release_version="0.0.10",
            deliverable_ids=godot_ids,
        )

    # Only godot-cpp template flavours are accepted.
    with pytest.raises(validator.DeliverableValidationError, match="Unknown GDExtension target"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=stage,
            release_body=body,
            release_version="0.0.10",
            deliverable_ids=godot_ids,
            gdextension_target="editor",
        )


def test_release_validator_requires_both_exes_in_the_windows_zip(tmp_path: Path) -> None:
    # ONED runs opennova.exe from beside itself; a zip with only one of
    # the two is the version-skew failure the single zip exists to prevent.
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_zip(
        dist / FIXTURE_SOURCES["windows-apps"],
        ["opennova-modtools.exe", "libopennova.windows.template_release.x86_64.dll"],
    )

    with pytest.raises(validator.DeliverableValidationError, match="opennova.exe"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.10",
            deliverable_ids=["windows-apps"],
        )


def test_release_validator_requires_the_packed_game_in_the_game_zip(tmp_path: Path) -> None:
    # The game zip root is the game dir: without localres.pff a downloaded
    # opennova.exe boots to the empty picker — the no-game download this exists to fix.
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    entries = [e for e in WINDOWS_GAME_ENTRIES if e != "localres.pff"]
    _write_zip(
        dist / FIXTURE_SOURCES["windows-game"],
        entries + ["libopennova.windows.template_release.x86_64.dll"],
    )

    with pytest.raises(validator.DeliverableValidationError, match="localres.pff"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.10",
            deliverable_ids=["windows-game"],
        )


def test_release_validator_requires_the_loose_sources_in_the_dev_zip(tmp_path: Path) -> None:
    # The dev zip's whole point is the editable loose tree beside the apps.
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    entries = [e for e in WINDOWS_APPS_ENTRIES if not e.startswith("assets/")]
    _write_zip(
        dist / FIXTURE_SOURCES["windows-apps"],
        entries + ["libopennova.windows.template_release.x86_64.dll"],
    )

    with pytest.raises(validator.DeliverableValidationError, match="assets/items.def"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.10",
            deliverable_ids=["windows-apps"],
        )


def test_release_validator_cli_accepts_gdextension_target(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    stage = dist / "release-assets"
    body = tmp_path / "release-body.md"
    dist.mkdir()
    _write_debug_mode_windows_zip(dist)

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
            "0.0.0-ci",
            "--gdextension-target",
            "template_debug",
            "--only-id",
            "windows-apps",
            "--only-id",
            "windows-game",
        ]
    )

    assert exit_code == 0
    assert {path.name for path in stage.iterdir()} == {
        "opennova-windows-v0.0.0-ci.zip",
        "opennova-game-windows-v0.0.0-ci.zip",
    }


def test_release_validator_rejects_missing_archive_entry(tmp_path: Path) -> None:
    validator = _load_validator()
    dist = tmp_path / "dist"
    dist.mkdir()
    _write_deliverable_fixtures(dist)
    _write_zip(dist / "opennova_blender-v0.0.5.zip", ["blender_manifest.toml"])

    with pytest.raises(validator.DeliverableValidationError, match="lib/windows-x64/opennova.dll"):
        validator.validate_release_deliverables(
            repo_root=ROOT,
            dist_dir=dist,
            stage_dir=dist / "release-assets",
            release_body=tmp_path / "release-body.md",
            release_version="0.0.10",
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
            release_version="0.0.10",
        )


def test_release_workflow_validates_and_publishes_staged_assets() -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    release_step = workflow.split("- name: Create GitHub Release", 1)[1]
    package_jobs = [
        "package-addon",
        "package-importer",
        "package-godot-windows",
    ]

    assert "scripts/validate_release_deliverables.py" in workflow
    assert "GITHUB_REF_NAME" in workflow
    assert "dist/release-assets/*" in release_step
    assert "dist/*.zip" not in release_step
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
        "package-importer",
        "package-godot-windows",
    ]
    deferred_pr_jobs = ["package-addon", "package-importer"]

    assert "validate-deliverables:" in workflow
    # One Windows package job: ONED and the runtime ship in ONE zip (ONED runs
    # opennova.exe from beside itself), so the per-app jobs and
    # their wrapper scripts are gone.
    assert "package-godot-windows:" in workflow
    assert "package-godot-windows-editor:" not in workflow
    assert "package-godot-windows-runtime:" not in workflow
    assert "package-godot-macos-editor:" not in workflow
    assert "package-godot-macos-runtime:" not in workflow
    assert "package-godot:" not in workflow
    assert "package-godot-editor:" not in workflow
    assert "package-godot-runtime:" not in workflow
    assert "package-godot-macos:" not in workflow
    assert "scripts/package_godot_windows.ps1" in workflow
    assert "scripts/package_godot_editor_windows.ps1" not in workflow
    assert "scripts/package_godot_runtime_windows.ps1" not in workflow
    assert "scripts/package_godot_editor_macos.sh" not in workflow
    assert "scripts/package_godot_runtime_macos.sh" not in workflow
    assert "BUILD_GODOT: \"0\"" in _workflow_job(workflow, "test")

    # Tool packages are retained for master/manual runs but leave the PR hot path.
    for package_job in deferred_pr_jobs:
        body = _workflow_job(workflow, package_job)
        assert "if: github.event_name != 'pull_request'" in body
        assert "needs:" not in _workflow_job(workflow, package_job)

    # The Windows preview package reuses the prebuilt GDExtension.
    windows_job = _workflow_job(workflow, "package-godot-windows")
    assert "needs: [build-gdextension-windows]" in windows_job
    assert "Cache Godot binary" in windows_job
    assert "Cache Godot export templates" in windows_job

    validate_job = _workflow_job(workflow, "validate-deliverables")
    assert (
        "needs: [test, godot-tests, package-addon, package-importer, package-godot-windows]"
    ) in validate_job
    assert "always()" in validate_job
    for package_job in package_jobs:
        assert f"needs.{package_job}" in validate_job
    assert "Download pull request package artifacts" in validate_job
    assert "Download non-PR package artifacts" in validate_job
    assert "--release-version 0.0.0-ci" in validate_job
    assert "--only-id asset-importer" in validate_job
    assert "--only-id blender-ase-exporter" in validate_job
    # Both flavours are validated on PRs AND master runs.
    assert validate_job.count("--only-id windows-apps") == 2
    assert validate_job.count("--only-id windows-game") == 2
    assert "game_artifact_id" in validate_job
    assert "--only-id modding-editor" not in validate_job
    assert "--only-id game-runtime" not in validate_job
    # PR packages are debug-mode exports (template_debug GDExtension inside);
    # master/manual packages are release-mode like the release workflow.
    pr_validate, non_pr_validate = validate_job.split("- name: Validate non-PR deliverables", 1)
    assert "--gdextension-target template_debug" in pr_validate
    assert "--gdextension-target template_release" in non_pr_validate

    assert "dist/onimport-v*.exe" in workflow
    assert "dist/opennova-windows-v*.zip" in workflow
    assert "dist/opennova-game-windows-v*.zip" in workflow
    assert "dist/opennova-modtools-windows-v*.zip" not in workflow
    assert "dist/opennova-runtime-windows-v*.zip" not in workflow
    assert "dist/onimport.exe" not in workflow
    assert "dist/opennova-windows.zip" not in workflow
    assert "opennova_release_assets" not in workflow

    # The PR comment links both zips.
    links_job = _workflow_job(workflow, "pr-build-links")
    assert "needs: [package-godot-windows]" in links_job
    assert "opennova-windows.zip" in links_job
    assert "opennova-game-windows.zip" in links_job
    assert "opennova-modtools-windows.zip" not in links_job
    assert "opennova-runtime-windows.zip" not in links_job


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


def test_ci_builds_windows_gdextension_per_flavour_and_caches_with_sccache() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")

    # Regular CI is Windows-only; macOS delivery was removed entirely 2026-08-11.
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
    # One matrix leg per godot-cpp flavour, in parallel: a pull request builds
    # template_debug only (the editor/test flavour, also what its debug-mode
    # package ships); master pushes and manual runs add template_release. The
    # target -> CMake config mapping lives in the build step, never in
    # matrix.include (an include entry whose target is absent from the active
    # list is added as an extra combination and would resurrect the release leg).
    assert (
        "target: ${{ github.event_name == 'pull_request' "
        "&& fromJSON('[\"template_debug\"]') "
        "|| fromJSON('[\"template_debug\",\"template_release\"]') }}"
    ) in win_build
    assert "\n        include:" not in win_build
    # Build dirs keep their historical names: sccache keys embed the absolute
    # paths of godot-cpp's generated headers under the build dir, so a rename
    # invalidates every cached godot-cpp object.
    assert 'template_debug   = @{ config = "RelWithDebInfo"; dir = "build-godot-debug" }' in win_build
    assert 'template_release = @{ config = "Release"; dir = "build-godot-release" }' in win_build
    assert '"Debug"' not in win_build
    assert "name: gdext_windows_${{ matrix.target }}" in win_build

    # Consumers download the flavour(s) this run built instead of recompiling;
    # the editor binary that runs GUT needs template_debug only.
    assert "needs: [build-gdextension-windows]" in _workflow_job(workflow, "godot-tests")
    assert "name: gdext_windows_template_debug" in _workflow_job(workflow, "godot-tests")
    consumers = ["godot-tests", "package-godot-windows"]
    for job in consumers:
        body = _workflow_job(workflow, job)
        assert "actions/download-artifact" in body
        # No consumer recompiles the GDExtension inline.
        assert "cmake -S godot/src" not in body
    windows_job = _workflow_job(workflow, "package-godot-windows")
    assert "pattern: gdext_windows_*" in windows_job
    assert "merge-multiple: true" in windows_job
    # PRs package in debug export mode (debug template + template_debug DLL);
    # master/manual runs package in release mode like the release workflow.
    assert (
        "-SkipBuild -ExportMode "
        "${{ github.event_name == 'pull_request' && 'debug' || 'release' }}"
    ) in windows_job

    for job in ["package-addon", "package-importer", "package-godot-windows"]:
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


def test_ci_engine_test_job_builds_with_ninja_and_sccache() -> None:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    build_script = (ROOT / "scripts/build.sh").read_text(encoding="utf-8")
    test_job = _workflow_job(workflow, "test")

    # The Visual Studio generator ignores CMAKE_*_COMPILER_LAUNCHER, so the
    # ~640-TU engine compile ran uncached (~16 min) on every push. Ninja + sccache
    # come from the job environment; scripts/build.sh stays generator-agnostic
    # (CMake reads CMAKE_GENERATOR and the launcher variables from the
    # environment on a fresh configure), so local runs are unchanged.
    assert "CMAKE_GENERATOR: Ninja" in test_job
    assert "CMAKE_CXX_COMPILER_LAUNCHER: sccache" in test_job
    assert "CMAKE_C_COMPILER_LAUNCHER: sccache" in test_job
    assert "SCCACHE_GHA_ENABLED" in test_job
    assert "ilammy/msvc-dev-cmd" in test_job
    assert "mozilla-actions/sccache-action" in test_job
    assert "sccache --show-stats" in test_job
    assert " -G " not in build_script


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


def test_release_ships_the_game_zip_only() -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    package_jobs = [
        "package-addon",
        "package-importer",
        "package-godot-windows",
    ]

    assert "package-godot-windows:" in workflow
    assert "package-godot-windows-editor:" not in workflow
    assert "package-godot-windows-runtime:" not in workflow
    assert "package-godot:" not in workflow
    assert "package-godot-editor:" not in workflow
    assert "package-godot-runtime:" not in workflow
    # macOS delivery removed 2026-08-11 (maintainer decision): release ships
    # Windows only; the engine stays portable but nothing packages for macOS.
    assert "macos" not in workflow.lower()
    assert "scripts/package_godot_windows.ps1" in workflow
    assert "scripts/package_godot_editor_windows.ps1" not in workflow
    assert "scripts/package_godot_runtime_windows.ps1" not in workflow
    assert "BUILD_GODOT: \"0\"" in _workflow_job(workflow, "test")
    for package_job in package_jobs:
        assert "needs:" not in _workflow_job(workflow, package_job)
    assert (
        "needs: [test, package-addon, package-importer, package-godot-windows]"
    ) in _workflow_job(workflow, "release")
    windows_job = _workflow_job(workflow, "package-godot-windows")
    assert "Cache Godot binary" in windows_job
    assert "Cache Godot export templates" in windows_job
    # Tagged releases ship the packed game only; the dev zip (game + ONED +
    # sources) comes from master CI builds.
    assert "dist/opennova-game-windows-v*.zip" in windows_job
    assert "dist/opennova-windows-v*.zip" not in windows_job
    release_job = _workflow_job(workflow, "release")
    assert "--only-id windows-game" in release_job
    assert "--only-id windows-apps" not in release_job
    assert "--only-id asset-importer" in release_job
    assert "--only-id blender-ase-exporter" in release_job


def test_godot_package_script_builds_both_flavours() -> None:
    windows_shared = (ROOT / "scripts/package_godot_windows.ps1").read_text(encoding="utf-8")

    # The per-app wrappers and the -Target switch are gone: one script, two zips.
    assert not (ROOT / "scripts/package_godot_editor_windows.ps1").exists()
    assert not (ROOT / "scripts/package_godot_runtime_windows.ps1").exists()
    assert "-Target" not in windows_shared
    assert 'opennova-windows-v$Version.zip' in windows_shared
    assert 'opennova-game-windows-v$Version.zip' in windows_shared
    assert "opennova-modtools-windows" not in windows_shared
    assert "opennova-runtime-windows" not in windows_shared
    # Both presets are exported and boot-smoked.
    assert '-PresetName "OpenNova Mod Tools"' in windows_shared
    assert '-PresetName "OpenNova Runtime"' in windows_shared
    # The dev zip stages the TRACKED assets (never a wildcard copy — the working
    # assets/ holds untracked retail binaries) with an LFS pointer guard; the
    # game zip is packed by ONED's hidden CLI.
    assert "git -C $ROOT ls-files -z assets" in windows_shared
    assert "version https://git-lfs" in windows_shared
    assert "Copy-GameSources" in windows_shared
    assert "Invoke-PackGame" in windows_shared
    assert "--pack-game" in windows_shared
    assert "localres.pff" in windows_shared
    # -ExportMode release (default; the release workflow) exports with
    # --export-release and ships template_release; -ExportMode debug (PR CI)
    # exports with --export-debug and ships template_debug.
    assert "[ValidateSet(\"debug\", \"release\")]" in windows_shared
    assert '[string]$ExportMode = "release"' in windows_shared
    assert "--path godot --export-$ExportMode " in windows_shared
    assert "--path godot --export-release " not in windows_shared


def test_readme_lists_public_asset_names_and_install_hints() -> None:
    readme = (ROOT / "README.md").read_text(encoding="utf-8")

    for name in [
        "opennova-asset-importer-windows-v<version>.exe",
        "opennova-blender-ase-exporter-v<version>.zip",
        "opennova-game-windows-v<version>.zip",
        "opennova-windows-v<version>.zip",
    ]:
        assert name in readme
    assert "opennova-modding-editor-windows" not in readme
    assert "opennova-game-runtime-windows" not in readme
    assert "Install from Blender" in readme
    assert "Extract the zip" in readme


def test_blender_manifest_names_the_ase_and_anim_exporter() -> None:
    manifest = (ROOT / "blender/blender_manifest.toml").read_text(encoding="utf-8")

    assert 'name = "OpenNova Blender ASE & Anim Exporter"' in manifest
    assert 'tagline = "Export Novalogic ASE models and ADM/BAD animations"' in manifest
