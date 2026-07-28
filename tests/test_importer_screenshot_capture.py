from __future__ import annotations

import os
from pathlib import Path

import pytest

try:
    from PySide6 import QtWidgets
except ImportError as exc:
    pytest.skip(f"PySide6 Qt widgets are unavailable: {exc}", allow_module_level=True)

EXPECTED_README_SCREENSHOTS = [
    "screenshots/importer.png",
    "screenshots/overview.png",
    "screenshots/object.png",
    "screenshots/mission.png",
    "screenshots/fonts.png",
    "screenshots/credits.png",
    "screenshots/strings.png",
    "screenshots/menus.png",
    "screenshots/music.png",
    "screenshots/sound.png",
    "screenshots/environment.png",
]

EXPECTED_EDITOR_CAPTURE_TARGETS = {
    "overview.png": "TERRAIN",
    "object.png": "OBJECT",
    "mission.png": "MISSION",
    "fonts.png": "FONTS",
    "credits.png": "CREDITS",
    "strings.png": "STRINGS",
    "menus.png": "MNU",
    "music.png": "MUSIC",
    "sound.png": "SOUND",
    "environment.png": "ENVIRONMENT",
}


@pytest.fixture
def qt_app():
    previous_platform = os.environ.get("QT_QPA_PLATFORM")
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    yield app
    app.processEvents()
    if previous_platform is None:
        os.environ.pop("QT_QPA_PLATFORM", None)
    else:
        os.environ["QT_QPA_PLATFORM"] = previous_platform


def test_build_importer_screenshot_dialog_populates_demo_state(qt_app, tmp_path, monkeypatch):
    monkeypatch.setenv("OPENNOVA_IMPORTER_SETTINGS_PATH", str(tmp_path / "settings.json"))

    from apps.importer.ui.screenshot_capture import build_importer_screenshot_dialog

    dialog = build_importer_screenshot_dialog(version="0.test")
    try:
        assert dialog.windowTitle() == "OpenNova Importer v0.test"
        assert dialog.resource_table.rowCount() >= 5
        assert dialog.resource_table.item(0, 1).text()
        assert dialog.max_check is not None
        assert "done" in dialog.queue_summary_label.text()
        assert "failed" in dialog.queue_summary_label.text()
    finally:
        dialog.close()


def test_capture_importer_screenshot_writes_png(qt_app, tmp_path, monkeypatch):
    monkeypatch.setenv("OPENNOVA_IMPORTER_SETTINGS_PATH", str(tmp_path / "settings.json"))

    from apps.importer.ui.screenshot_capture import capture_importer_screenshot

    out_path = capture_importer_screenshot(tmp_path / "importer.png", version="0.test")

    assert out_path == tmp_path / "importer.png"
    assert out_path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")


def test_qt_platform_is_not_forced_offscreen_on_windows(monkeypatch):
    from apps.importer.ui import screenshot_capture

    monkeypatch.setattr(screenshot_capture.sys, "platform", "win32")
    monkeypatch.delenv("QT_QPA_PLATFORM", raising=False)

    assert not screenshot_capture._should_force_offscreen_platform()


def test_screenshot_script_capture_importer():
    root = Path(__file__).resolve().parents[1]

    sh = (root / "scripts" / "capture_screenshots.sh").read_text(encoding="utf-8")

    assert "apps.importer.ui.screenshot_capture" in sh


def test_screenshot_script_search_parent_godot_bins():
    root = Path(__file__).resolve().parents[1]

    sh = (root / "scripts" / "capture_screenshots.sh").read_text(encoding="utf-8")

    assert "find_godot_bin" in sh


def test_screenshot_script_refresh_godot_script_cache():
    root = Path(__file__).resolve().parents[1]

    sh = (root / "scripts" / "capture_screenshots.sh").read_text(encoding="utf-8")

    assert "--import" in sh


def test_screenshot_script_verify_editor_outputs_are_fresh():
    root = Path(__file__).resolve().parents[1]

    sh = (root / "scripts" / "capture_screenshots.sh").read_text(encoding="utf-8")

    assert "-nt" in sh
    assert "environment.png" in sh


def test_readme_references_importer_screenshot():
    root = Path(__file__).resolve().parents[1]

    readme = (root / "README.md").read_text(encoding="utf-8")

    assert "screenshots/importer.png" in readme


def test_readme_references_all_workspace_screenshots():
    root = Path(__file__).resolve().parents[1]

    readme = (root / "README.md").read_text(encoding="utf-8")

    for screenshot in EXPECTED_README_SCREENSHOTS:
        assert screenshot in readme


def test_godot_capture_script_targets_all_editor_workspaces():
    root = Path(__file__).resolve().parents[1]

    script = (root / "godot" / "modtools" / "tools" / "screenshot_capture.gd").read_text(encoding="utf-8")

    for filename, workspace in EXPECTED_EDITOR_CAPTURE_TARGETS.items():
        assert filename in script
        assert f"EditorWorkstation.Workspace.{workspace}" in script


def test_godot_capture_script_uses_maximized_window():
    root = Path(__file__).resolve().parents[1]

    script = (root / "godot" / "modtools" / "tools" / "screenshot_capture.gd").read_text(encoding="utf-8")

    assert "Window.MODE_MAXIMIZED" in script
    assert "_maximize_window" in script


def test_godot_capture_script_has_repo_fixture_fallbacks():
    root = Path(__file__).resolve().parents[1]

    script = (root / "godot" / "modtools" / "tools" / "screenshot_capture.gd").read_text(encoding="utf-8")

    assert "fixtures/mus/jo_gamemus.bin" in script
    assert "fixtures/lwf/00TRa.LWF" in script
