from __future__ import annotations

import os
import time
from dataclasses import dataclass, field
from typing import Callable

import pytest

from opennova_jobs import ImportRequest, ImportResult, ScanItem, ScanResult

try:
    from PySide6 import QtWidgets
except ImportError as exc:
    pytest.skip(f"PySide6 Qt widgets are unavailable: {exc}", allow_module_level=True)


class _QtBot:
    def __init__(self) -> None:
        self._previous_qt_platform = os.environ.get("QT_QPA_PLATFORM")
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        self.app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
        self.widgets: list[QtWidgets.QWidget] = []

    def addWidget(self, widget: QtWidgets.QWidget) -> None:
        self.widgets.append(widget)

    def waitUntil(
        self,
        callback: Callable[[], bool],
        *,
        timeout: int = 1000,
        interval: int = 10,
    ) -> None:
        deadline = time.monotonic() + (timeout / 1000)
        while time.monotonic() < deadline:
            self.app.processEvents()
            if callback():
                return
            time.sleep(interval / 1000)
        raise AssertionError("condition was not met before timeout")

    def close(self) -> None:
        for widget in reversed(self.widgets):
            widget.close()
        self.app.processEvents()
        if self._previous_qt_platform is None:
            os.environ.pop("QT_QPA_PLATFORM", None)
        else:
            os.environ["QT_QPA_PLATFORM"] = self._previous_qt_platform


@pytest.fixture
def qtbot() -> _QtBot:
    bot = _QtBot()
    yield bot
    bot.close()


@dataclass
class FakeBackend:
    supports_blend: bool = True
    supports_max: bool = False
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
            supports_max=self.supports_max,
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
        backend=FakeBackend(supports_blend=True, supports_max=True)
    )
    qtbot.addWidget(dialog)
    assert dialog.blend_check is not None
    assert dialog.max_check is not None
    assert dialog.glb_check is not None
    assert dialog.fbx_check is not None


def test_glb_and_fbx_checks_follow_blender_output(qtbot):
    from opennova_qt_ui import OpenNovaImporterDialog

    dialog = OpenNovaImporterDialog(backend=FakeBackend(supports_blend=True, supports_max=False))
    qtbot.addWidget(dialog)

    assert dialog.blend_check.isChecked()
    assert dialog.glb_check.isEnabled()
    assert dialog.fbx_check.isEnabled()

    dialog.glb_check.setChecked(True)
    dialog.fbx_check.setChecked(True)
    dialog.blend_check.setChecked(False)

    assert not dialog.glb_check.isChecked()
    assert not dialog.fbx_check.isChecked()
    assert not dialog.glb_check.isEnabled()
    assert not dialog.fbx_check.isEnabled()

    options = dialog._build_options()
    assert not options.write_blend
    assert not options.write_glb
    assert not options.write_fbx


def test_ase_without_native_scene_output_is_rejected(qtbot, tmp_path):
    from opennova_qt_ui import OpenNovaImporterDialog

    dialog = OpenNovaImporterDialog(backend=FakeBackend(supports_blend=True, supports_max=False))
    qtbot.addWidget(dialog)

    game_dir = tmp_path / "game"
    output_dir = tmp_path / "out"
    game_dir.mkdir()
    output_dir.mkdir()
    dialog.game_dir_edit.setText(str(game_dir))
    dialog.output_root_edit.setText(str(output_dir))
    dialog.blend_check.setChecked(False)
    dialog.project_check.setChecked(True)
    dialog.ase_check.setChecked(True)

    request = ImportRequest.for_definition(
        base_dir=str(game_dir),
        item_name="M16A2",
        item_type="weapon",
        output_root=str(output_dir),
        options=dialog._build_options(),
    )
    from opennova_jobs import validate_import_request

    assert "ASE export requires .blend or .max output." in validate_import_request(request)


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
