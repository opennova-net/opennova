from __future__ import annotations

from pathlib import Path
import tomllib


ROOT = Path(__file__).resolve().parents[1]


def _pyproject() -> dict[str, object]:
    with (ROOT / "pyproject.toml").open("rb") as handle:
        return tomllib.load(handle)


def test_bpy_is_required_project_dependency() -> None:
    config = _pyproject()

    project = config["project"]
    assert isinstance(project, dict)
    required = project.get("dependencies", [])
    assert isinstance(required, list)
    assert any(str(dep).partition(">=")[0] == "bpy" for dep in required)


def test_default_ctest_runs_full_pytest_suite_without_marker_gates() -> None:
    files = [
        ROOT / "CMakeLists.txt",
        ROOT / "scripts" / "build.sh",
        ROOT / "scripts" / "test_python.sh",
    ]
    for path in files:
        text = path.read_text(encoding="utf-8")
        assert "OPENNOVA_ENABLE_BLENDER_PYTHON_TESTS" not in text
        assert "not blender" not in text
        assert "--group blender" not in text


def test_package_importer_build_uses_default_project_dependencies() -> None:
    script = (ROOT / "scripts" / "package_importer_windows.ps1").read_text(
        encoding="utf-8",
    )

    assert "--group blender" not in script
