from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Callable

import pytest

from opennova_jobs import ImportRequest, ImportResult, ScanItem, ScanResult


os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

pytest.importorskip("PySide6")
pytest.importorskip("pytestqt")


@dataclass
class FakeBackend:
    supports_blend: bool = True
    supports_glb: bool = True
    supports_fbx: bool = True
    supports_parallel: bool = False
    scan_result: ScanResult = field(default_factory=lambda: ScanResult(ok=True, items=[]))
    execute_result: Callable[[ImportRequest], ImportResult] | None = None
    requests: list[ImportRequest] = field(default_factory=list)
    shutdown_called: bool = False

    def capabilities(self):
        from opennova_qt_ui.backend import BackendCapabilities

        return BackendCapabilities(
            name="Fake",
            supports_blend=self.supports_blend,
            supports_glb=self.supports_glb,
            supports_fbx=self.supports_fbx,
            supports_parallel=self.supports_parallel,
        )

    def scan(self, base_dir: str) -> ScanResult:
        return self.scan_result

    def execute(self, request: ImportRequest) -> ImportResult:
        self.requests.append(request)
        if self.execute_result is None:
            return ImportResult.success(request, output_path=request.likely_output_dir)
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

    dialog = OpenNovaImporterDialog(backend=FakeBackend(), version="0.test")
    qtbot.addWidget(dialog)
    assert dialog.windowTitle() == "OpenNova Importer v0.test"


def test_dialog_uses_roomy_tabs_without_activity_or_presets(qtbot):
    from opennova_qt_ui import OpenNovaImporterDialog

    dialog = OpenNovaImporterDialog(backend=FakeBackend())
    qtbot.addWidget(dialog)

    assert not hasattr(dialog, "preset_combo")
    assert not hasattr(dialog, "preset_description")
    assert not hasattr(dialog, "main_splitter")
    assert not hasattr(dialog, "activity_tab")
    assert [dialog.tabs.tabText(index) for index in range(dialog.tabs.count())] == [
        "Definitions",
        "Loose .3di",
    ]
    assert dialog.details_panel.isHidden()
    assert dialog.details_toggle.isCheckable()
    assert "Details" in dialog.details_toggle.text()
    dialog.details_toggle.click()
    assert not dialog.details_panel.isHidden()
    assert dialog.queue_table is not None
    assert dialog.log_text is not None


def test_capabilities_drive_output_options(qtbot):
    from opennova_qt_ui import OpenNovaImporterDialog

    dialog = OpenNovaImporterDialog(
        backend=FakeBackend(supports_blend=True, supports_glb=False, supports_fbx=False)
    )
    qtbot.addWidget(dialog)
    assert dialog.blend_check is not None
    assert dialog.glb_check is None
    assert dialog.fbx_check is None


def test_scan_button_populates_and_filters_table(qtbot, tmp_path):
    from opennova_qt_ui import OpenNovaImporterDialog

    dialog = OpenNovaImporterDialog(backend=FakeBackend(scan_result=ScanResult(ok=True, items=_items())))
    qtbot.addWidget(dialog)

    dialog.game_dir_edit.setText(str(tmp_path))
    dialog.scan_button.click()
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 3, timeout=2000)

    dialog.search_edit.setText("Arm")
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 1, timeout=2000)
    assert dialog.resource_table.item(0, 1).text() == "Armry01"


def test_import_selected_enqueues_and_executes_request(qtbot, tmp_path):
    from opennova_qt_ui import OpenNovaImporterDialog

    backend = FakeBackend(scan_result=ScanResult(ok=True, items=_items()))
    dialog = OpenNovaImporterDialog(backend=backend)
    qtbot.addWidget(dialog)

    game_dir = tmp_path / "game"
    output_dir = tmp_path / "out"
    game_dir.mkdir()
    output_dir.mkdir()
    dialog.game_dir_edit.setText(str(game_dir))
    dialog.output_root_edit.setText(str(output_dir))
    dialog.scan_button.click()
    qtbot.waitUntil(lambda: dialog.resource_table.rowCount() == 3, timeout=2000)

    dialog.resource_table.selectRow(0)
    dialog.import_selected_button.click()
    qtbot.waitUntil(lambda: len(backend.requests) == 1, timeout=3000)

    assert backend.requests[0].item_type in ("weapon", "item")
    assert dialog.queue_table.rowCount() == 1
