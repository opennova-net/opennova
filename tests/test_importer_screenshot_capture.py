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
]


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


def test_readme_references_importer_screenshot():
    root = Path(__file__).resolve().parents[1]

    readme = (root / "README.md").read_text(encoding="utf-8")

    assert "screenshots/importer.png" in readme


def test_readme_references_current_tool_screenshots():
    root = Path(__file__).resolve().parents[1]

    readme = (root / "README.md").read_text(encoding="utf-8")

    for screenshot in EXPECTED_README_SCREENSHOTS:
        assert screenshot in readme
