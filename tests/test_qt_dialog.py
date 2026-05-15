"""Headless tests for OpenNovaImporterDialog using a FakeBackend.

Run with: pytest tests/test_qt_dialog.py
Set QT_QPA_PLATFORM=offscreen if not already in the environment.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Callable

import pytest

from opennova_jobs import (
    ImportRequest,
    ImportResult,
    ScanItem,
    ScanResult,
)


os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


pytest.importorskip("PySide6")
pytest.importorskip("pytestqt")


@dataclass
class FakeBackend:
    name: str = "Fake"
    supports_blend: bool = True
    supports_max: bool = False
    supports_parallel: bool = False
    scan_result: ScanResult = field(default_factory=lambda: ScanResult(ok=True, items=[]))
    execute_result: Callable[[ImportRequest], ImportResult] | None = None
    last_request: ImportRequest | None = None
    shutdown_called: bool = False

    def capabilities(self):
        from opennova_qt_ui.backend import BackendCapabilities
        return BackendCapabilities(
            name=self.name,
            supports_blend=self.supports_blend,
            supports_max=self.supports_max,
            supports_parallel=self.supports_parallel,
        )

    def scan(self, base_dir: str) -> ScanResult:
        return self.scan_result

    def execute(self, request: ImportRequest) -> ImportResult:
        self.last_request = request
        if self.execute_result is None:
            return ImportResult.success(request, output_path="ok")
        return self.execute_result(request)

    def shutdown(self) -> None:
        self.shutdown_called = True


def _items() -> list[ScanItem]:
    return [
        ScanItem(name="M16A2", type="weapon", source_model="m16a2.3di", output_stem="m16a2"),
        ScanItem(name="AK74", type="weapon", source_model="ak74.3di", output_stem="ak74"),
        ScanItem(name="Armry01", type="item", source_model="Armry01.3di", output_stem="Armry01"),
    ]


def test_dialog_constructs_with_backend(qtbot):
    from opennova_qt_ui import OpenNovaImporterDialog

    dialog = OpenNovaImporterDialog(backend=FakeBackend())
    qtbot.addWidget(dialog)
    assert dialog.windowTitle().startswith("OpenNova Importer")


def test_capabilities_drive_options_panel(qtbot):
    from opennova_qt_ui import OpenNovaImporterDialog

    backend = FakeBackend(supports_blend=True, supports_max=False)
    dialog = OpenNovaImporterDialog(backend=backend)
    qtbot.addWidget(dialog)
    assert dialog.blend_check is not None
    assert dialog.max_check is None


def test_scan_button_populates_table(qtbot, tmp_path):
    from opennova_qt_ui import OpenNovaImporterDialog

    backend = FakeBackend(scan_result=ScanResult(ok=True, items=_items()))
    dialog = OpenNovaImporterDialog(backend=backend)
    qtbot.addWidget(dialog)

    dialog.game_dir_edit.setText(str(tmp_path))
    dialog.scan_button.click()
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 3, timeout=2000)
    assert dialog.resource_table.rowCount() == 3


def test_filter_narrows_table(qtbot, tmp_path):
    from opennova_qt_ui import OpenNovaImporterDialog

    backend = FakeBackend(scan_result=ScanResult(ok=True, items=_items()))
    dialog = OpenNovaImporterDialog(backend=backend)
    qtbot.addWidget(dialog)
    dialog.game_dir_edit.setText(str(tmp_path))
    dialog.scan_button.click()
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 3, timeout=2000)

    dialog.search_edit.setText("Arm")
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 1, timeout=2000)
    assert dialog.resource_table.item(0, 1).text() == "Armry01"


def test_import_selected_calls_backend_execute(qtbot, tmp_path):
    from opennova_qt_ui import OpenNovaImporterDialog

    backend = FakeBackend(scan_result=ScanResult(ok=True, items=_items()))
    dialog = OpenNovaImporterDialog(backend=backend)
    qtbot.addWidget(dialog)

    dialog.game_dir_edit.setText(str(tmp_path / "game"))
    dialog.output_root_edit.setText(str(tmp_path / "out"))
    dialog.scan_button.click()
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 3, timeout=2000)

    dialog.resource_table.selectRow(0)
    dialog.import_selected_button.click()
    qtbot.waitUntil(lambda: backend.last_request is not None, timeout=2000)

    assert backend.last_request is not None
    assert backend.last_request.item_type in ("weapon", "item")
