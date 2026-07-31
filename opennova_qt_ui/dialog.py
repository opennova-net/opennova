"""DCC-agnostic PySide6 importer dialog."""
from __future__ import annotations

import logging
import os
import sys
from pathlib import Path

try:  # pragma: no cover - import availability is environment-dependent
    from PySide6 import QtCore, QtWidgets
except Exception:  # pragma: no cover
    QtCore = None
    QtWidgets = None

from opennova_jobs import (
    ACTIVE_JOB_STATUSES,
    JOB_DONE,
    JOB_ERROR,
    JOB_PENDING,
    JOB_RUNNING,
    ImportJob,
    ImportOptions,
    ImportRequest,
    ImportResult,
    validate_import_request,
)

from .collisions import CollisionDecision, output_collision_paths, resolve_output_collisions
from .filtering import filtered_items
from .preferences import load_from_path, save_to_path


log = logging.getLogger(__name__)

RESOURCE_COLUMNS = ("Type", "Name", "Model", "Output")
QUEUE_COLUMNS = ("Type", "Name", "Status", "Info")
TYPE_FILTERS = ("all", "weapon", "item")
LOG_FILTER_ALL = "All"
LOG_FILTER_CURRENT = "Current Job"
LOG_FILTER_ERRORS = "Errors"
LOG_FILTERS = (LOG_FILTER_ALL, LOG_FILTER_CURRENT, LOG_FILTER_ERRORS)
_DIALOG = None


def is_available() -> bool:
    return QtCore is not None and QtWidgets is not None


def dialog_title(version: str = "") -> str:
    return f"OpenNova Importer v{version}" if version else "OpenNova Importer"


def resource_table_row(item) -> tuple[str, str, str, str]:
    source = getattr(item, "source_model", "") or "(definition lookup)"
    stem = getattr(item, "output_stem", "") or Path(source).stem
    return (getattr(item, "type", ""), getattr(item, "name", ""), source, stem)


def close_importer_dialog() -> bool:
    global _DIALOG
    if _DIALOG is None:
        return False
    try:
        _DIALOG.close()
        return True
    finally:
        _DIALOG = None


def show_importer_dialog(backend, *, version: str = "", parent=None) -> bool:
    if not is_available():
        raise RuntimeError("PySide6 is not available in this Python environment.")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication(sys.argv)
    del app
    global _DIALOG
    if _DIALOG is None:
        _DIALOG = OpenNovaImporterDialog(backend=backend, version=version, parent=parent)
    _DIALOG.show()
    _DIALOG.raise_()
    _DIALOG.activateWindow()
    return True


if is_available():

    class _TaskSignals(QtCore.QObject):
        finished = QtCore.Signal(str, object)
        failed = QtCore.Signal(str, object)


    class _FunctionTask(QtCore.QRunnable):
        def __init__(self, task_id: str, callback):
            super().__init__()
            self.task_id = task_id
            self.callback = callback
            self.signals = _TaskSignals()

        def run(self) -> None:
            try:
                self.signals.finished.emit(self.task_id, self.callback())
            except BaseException as exc:  # noqa: BLE001 - report through UI
                self.signals.failed.emit(self.task_id, exc)


    class OpenNovaImporterDialog(QtWidgets.QDialog):
        def __init__(self, *, backend, version: str = "", parent=None):
            super().__init__(parent)
            self._backend = backend
            self._version = version
            self._all_items = []
            self._visible_items = []
            self._jobs: list[ImportJob] = []
            self._log_entries: list[dict[str, str]] = []
            self._active_tasks: dict[str, _FunctionTask] = {}
            self._loose_paths: list[str] = []
            self._updating_loose_file_edit = False
            self._applying_options = False
            self._scanning = False
            self._closing = False
            self._thread_pool = QtCore.QThreadPool(self)
            self._thread_pool.setMaxThreadCount(max(1, self._backend_max_workers()))

            self.setWindowTitle(dialog_title(version))
            self.setMinimumSize(980, 660)
            self.resize(1120, 740)
            self._build_ui()
            self._apply_style()
            self._connect_signals()
            self._apply_saved_preferences()
            self._apply_options(ImportOptions())
            self._set_status("Choose a game directory and scan.")
            self._refresh_queue()
            self._update_action_states()

        def closeEvent(self, event):
            global _DIALOG
            self._closing = True
            self._save_preferences()
            if _DIALOG is self:
                _DIALOG = None
            super().closeEvent(event)

        def _build_ui(self) -> None:
            root = QtWidgets.QVBoxLayout(self)
            root.setContentsMargins(16, 14, 16, 14)
            root.setSpacing(12)

            header = QtWidgets.QHBoxLayout()
            title = QtWidgets.QLabel("OpenNova Importer")
            title.setObjectName("OpenNovaTitle")
            self.version_label = QtWidgets.QLabel(f"v{self._version}" if self._version else "")
            self.version_label.setObjectName("OpenNovaVersion")
            self.version_label.setAlignment(
                QtCore.Qt.AlignmentFlag.AlignRight | QtCore.Qt.AlignmentFlag.AlignVCenter
            )
            header.addWidget(title, 1)
            header.addWidget(self.version_label)
            root.addLayout(header)

            body = QtWidgets.QHBoxLayout()
            body.setSpacing(16)
            self.tabs = QtWidgets.QTabWidget()
            self.tabs.addTab(self._build_definition_tab(), "Definitions")
            self.tabs.addTab(self._build_loose_tab(), "Loose .3di")
            body.addWidget(self.tabs, 1)
            body.addWidget(self._build_options_panel())
            root.addLayout(body, 1)

            self.status_label = QtWidgets.QLabel()
            self.status_label.setObjectName("OpenNovaStatus")
            self.status_label.setMinimumHeight(26)
            self.status_label.setTextInteractionFlags(QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
            root.addWidget(self.status_label)

            root.addWidget(self._build_queue_strip())
            self.details_panel = self._build_details_panel()
            self.details_panel.hide()
            root.addWidget(self.details_panel)

        def _apply_style(self) -> None:
            self.setStyleSheet("""
                QDialog {
                    background: #2f3438;
                    color: #f0f2f4;
                }
                QWidget {
                    color: #f0f2f4;
                }
                QLabel#OpenNovaTitle {
                    font-size: 18px;
                    font-weight: 600;
                }
                QLabel#OpenNovaVersion {
                    color: #b7c0c8;
                }
                QLabel#OpenNovaStatus {
                    background: #25292d;
                    border: 1px solid #4f565d;
                    border-radius: 4px;
                    padding: 6px 8px;
                    color: #dfe5ea;
                }
                QGroupBox {
                    border: 1px solid #545b62;
                    border-radius: 5px;
                    margin-top: 10px;
                    padding: 10px 8px 8px 8px;
                    color: #f0f2f4;
                    font-weight: 600;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 8px;
                    padding: 0 5px;
                    background: #2f3438;
                    color: #f0f2f4;
                }
                QLineEdit, QComboBox {
                    min-height: 24px;
                    background: #24282c;
                    border: 1px solid #565d64;
                    border-radius: 4px;
                    padding: 2px 6px;
                    selection-background-color: #4d7fb3;
                }
                QPushButton {
                    min-height: 26px;
                    padding: 3px 10px;
                    background: #485058;
                    border: 1px solid #68717a;
                    border-radius: 4px;
                }
                QPushButton:hover {
                    background: #58616a;
                }
                QPushButton:disabled {
                    color: #9aa3ab;
                    background: #393f45;
                    border-color: #4b5259;
                }
                QTableWidget, QPlainTextEdit {
                    gridline-color: #4b5259;
                    background: #24282c;
                    alternate-background-color: #2a2f34;
                    selection-background-color: #4d7fb3;
                    selection-color: #ffffff;
                }
                QHeaderView::section {
                    background: #3e454c;
                    color: #f0f2f4;
                    padding: 4px 6px;
                    border: 0;
                    border-right: 1px solid #555d65;
                    border-bottom: 1px solid #555d65;
                }
                QTabWidget::pane {
                    border: 1px solid #545b62;
                    border-radius: 4px;
                    top: -1px;
                }
                QTabBar::tab {
                    background: #3b4249;
                    border: 1px solid #545b62;
                    border-bottom: 0;
                    color: #dce2e7;
                    padding: 6px 14px;
                    margin-right: 2px;
                    outline: 0;
                }
                QTabBar::tab:selected {
                    background: #4a525a;
                    color: #ffffff;
                }
                QProgressBar {
                    border: 1px solid #565d64;
                    border-radius: 4px;
                    background: #24282c;
                    text-align: center;
                }
                QProgressBar::chunk {
                    background: #4d7fb3;
                    border-radius: 3px;
                }
                QWidget#OpenNovaQueueStrip {
                    background: #25292d;
                    border: 1px solid #4f565d;
                    border-radius: 4px;
                }
            """)

        def _build_definition_tab(self):
            tab = QtWidgets.QWidget()
            layout = QtWidgets.QVBoxLayout(tab)
            layout.setContentsMargins(12, 14, 12, 12)
            layout.setSpacing(10)
            path_group = QtWidgets.QGroupBox("Definition Import")
            path_layout = QtWidgets.QGridLayout(path_group)
            path_layout.setContentsMargins(10, 14, 10, 10)
            path_layout.setHorizontalSpacing(8)
            path_layout.setVerticalSpacing(8)
            path_layout.setColumnStretch(1, 1)
            self.game_dir_edit = QtWidgets.QLineEdit()
            self.output_root_edit = QtWidgets.QLineEdit()
            self.browse_game_button = QtWidgets.QPushButton("Browse")
            self.browse_output_button = QtWidgets.QPushButton("Browse")
            self.scan_button = QtWidgets.QPushButton("Scan")
            path_layout.addWidget(QtWidgets.QLabel("Game directory"), 0, 0)
            path_layout.addWidget(self.game_dir_edit, 0, 1)
            path_layout.addWidget(self.browse_game_button, 0, 2)
            path_layout.addWidget(self.scan_button, 0, 3)
            path_layout.addWidget(QtWidgets.QLabel("Output root"), 1, 0)
            path_layout.addWidget(self.output_root_edit, 1, 1)
            path_layout.addWidget(self.browse_output_button, 1, 2)
            layout.addWidget(path_group)

            filter_row = QtWidgets.QHBoxLayout()
            self.type_combo = QtWidgets.QComboBox()
            self.type_combo.addItems(TYPE_FILTERS)
            self.search_edit = QtWidgets.QLineEdit()
            self.search_edit.setPlaceholderText("Search")
            if hasattr(self.search_edit, "setClearButtonEnabled"):
                self.search_edit.setClearButtonEnabled(True)
            self.clear_search_button = QtWidgets.QPushButton("Clear")
            filter_row.addWidget(QtWidgets.QLabel("Type"))
            filter_row.addWidget(self.type_combo)
            filter_row.addWidget(QtWidgets.QLabel("Search"))
            filter_row.addWidget(self.search_edit, 1)
            filter_row.addWidget(self.clear_search_button)
            layout.addLayout(filter_row)

            self.resource_count_label = QtWidgets.QLabel("No resources scanned")
            layout.addWidget(self.resource_count_label)
            self.resource_table = QtWidgets.QTableWidget(0, len(RESOURCE_COLUMNS))
            self.resource_table.setHorizontalHeaderLabels(RESOURCE_COLUMNS)
            self.resource_table.setAlternatingRowColors(True)
            self.resource_table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectionBehavior.SelectRows)
            self.resource_table.setSelectionMode(QtWidgets.QAbstractItemView.SelectionMode.ExtendedSelection)
            self.resource_table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
            self.resource_table.verticalHeader().setVisible(False)
            header = self.resource_table.horizontalHeader()
            header.setStretchLastSection(True)
            header.setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeMode.ResizeToContents)
            header.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeMode.Stretch)
            header.setSectionResizeMode(2, QtWidgets.QHeaderView.ResizeMode.Stretch)
            header.setSectionResizeMode(3, QtWidgets.QHeaderView.ResizeMode.ResizeToContents)
            layout.addWidget(self.resource_table, 1)

            actions = QtWidgets.QHBoxLayout()
            actions.addStretch(1)
            self.import_selected_button = QtWidgets.QPushButton("Import Selected")
            self.import_visible_button = QtWidgets.QPushButton("Import Visible (0)")
            actions.addWidget(self.import_selected_button)
            actions.addWidget(self.import_visible_button)
            layout.addLayout(actions)
            return tab

        def _build_loose_tab(self):
            tab = QtWidgets.QWidget()
            layout = QtWidgets.QVBoxLayout(tab)
            layout.setContentsMargins(12, 14, 12, 12)
            layout.setSpacing(10)
            group = QtWidgets.QGroupBox("Loose .3di Import")
            form = QtWidgets.QGridLayout(group)
            form.setContentsMargins(10, 14, 10, 10)
            form.setHorizontalSpacing(8)
            form.setVerticalSpacing(8)
            form.setColumnStretch(1, 1)
            self.loose_file_edit = QtWidgets.QLineEdit()
            self.asset_dir_edit = QtWidgets.QLineEdit()
            self.loose_output_edit = QtWidgets.QLineEdit()
            self.browse_loose_button = QtWidgets.QPushButton("Browse")
            self.browse_asset_button = QtWidgets.QPushButton("Browse")
            self.browse_loose_output_button = QtWidgets.QPushButton("Browse")
            form.addWidget(QtWidgets.QLabel(".3di files"), 0, 0)
            form.addWidget(self.loose_file_edit, 0, 1)
            form.addWidget(self.browse_loose_button, 0, 2)
            form.addWidget(QtWidgets.QLabel("Asset search"), 1, 0)
            form.addWidget(self.asset_dir_edit, 1, 1)
            form.addWidget(self.browse_asset_button, 1, 2)
            form.addWidget(QtWidgets.QLabel("Output root"), 2, 0)
            form.addWidget(self.loose_output_edit, 2, 1)
            form.addWidget(self.browse_loose_output_button, 2, 2)
            layout.addWidget(group)
            layout.addStretch(1)
            actions = QtWidgets.QHBoxLayout()
            actions.addStretch(1)
            self.import_loose_button = QtWidgets.QPushButton("Import Loose")
            actions.addWidget(self.import_loose_button)
            layout.addLayout(actions)
            return tab

        def _build_options_panel(self):
            self.options_panel = QtWidgets.QGroupBox("Options")
            self.options_panel.setMinimumWidth(210)
            self.options_panel.setMaximumWidth(240)
            layout = QtWidgets.QVBoxLayout(self.options_panel)
            layout.setContentsMargins(10, 14, 10, 10)
            layout.setSpacing(12)

            scene_group = QtWidgets.QGroupBox("Scene")
            scene_layout = QtWidgets.QVBoxLayout(scene_group)
            scene_layout.setContentsMargins(10, 14, 10, 10)
            scene_layout.setSpacing(6)
            self.animations_check = QtWidgets.QCheckBox("Animations (DEF)")
            self.collisions_check = QtWidgets.QCheckBox("Collisions")
            self.occlusion_check = QtWidgets.QCheckBox("Occlusion")
            self.lights_check = QtWidgets.QCheckBox("Lights")
            self.arms_check = QtWidgets.QCheckBox("Arms model (weapon DEF)")
            for check in (
                self.animations_check,
                self.collisions_check,
                self.occlusion_check,
                self.lights_check,
                self.arms_check,
            ):
                scene_layout.addWidget(check)
            layout.addWidget(scene_group)

            files_group = QtWidgets.QGroupBox("Files to Write")
            files_layout = QtWidgets.QVBoxLayout(files_group)
            files_layout.setContentsMargins(10, 14, 10, 10)
            files_layout.setSpacing(6)
            caps = self._backend.capabilities()
            self.blend_check = QtWidgets.QCheckBox("Blender scene (.blend)") if caps.supports_blend else None
            self.max_check = QtWidgets.QCheckBox("3ds Max scene (.max)") if caps.supports_max else None
            self.project_check = QtWidgets.QCheckBox("Object workspace (.3dp)")
            self.ase_check = QtWidgets.QCheckBox("ASE (.ase)")
            self.glb_check = QtWidgets.QCheckBox("glTF 2.0 binary (.glb)") if caps.supports_blend else None
            self.fbx_check = QtWidgets.QCheckBox("FBX (.fbx)") if caps.supports_blend else None
            for check in (
                self.blend_check,
                self.max_check,
                self.project_check,
                self.ase_check,
                self.glb_check,
                self.fbx_check,
            ):
                if check is not None:
                    files_layout.addWidget(check)
            layout.addWidget(files_group)
            layout.addStretch(1)
            return self.options_panel

        def _build_queue_strip(self):
            strip = QtWidgets.QWidget()
            strip.setObjectName("OpenNovaQueueStrip")
            layout = QtWidgets.QHBoxLayout(strip)
            layout.setContentsMargins(8, 6, 8, 6)
            layout.setSpacing(8)
            self.queue_summary_label = QtWidgets.QLabel()
            self.queue_summary_label.setMinimumWidth(260)
            self.progress = QtWidgets.QProgressBar()
            self.progress.setMinimum(0)
            self.progress.setMaximum(1)
            self.progress.setMaximumWidth(220)
            self.details_toggle = QtWidgets.QPushButton("Show Details")
            self.details_toggle.setCheckable(True)
            layout.addWidget(self.queue_summary_label)
            layout.addWidget(self.progress)
            layout.addStretch(1)
            layout.addWidget(self.details_toggle)
            return strip

        def _build_details_panel(self):
            panel = QtWidgets.QWidget()
            layout = QtWidgets.QVBoxLayout(panel)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.setSpacing(10)

            queue_group = QtWidgets.QGroupBox("Queue")
            queue_layout = QtWidgets.QVBoxLayout(queue_group)
            queue_layout.setContentsMargins(10, 14, 10, 10)
            queue_layout.setSpacing(8)
            self.queue_table = QtWidgets.QTableWidget(0, len(QUEUE_COLUMNS))
            self.queue_table.setHorizontalHeaderLabels(QUEUE_COLUMNS)
            self.queue_table.setAlternatingRowColors(True)
            self.queue_table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectionBehavior.SelectRows)
            self.queue_table.setSelectionMode(QtWidgets.QAbstractItemView.SelectionMode.ExtendedSelection)
            self.queue_table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
            self.queue_table.verticalHeader().setVisible(False)
            queue_header = self.queue_table.horizontalHeader()
            queue_header.setStretchLastSection(True)
            queue_header.setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeMode.ResizeToContents)
            queue_header.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeMode.Stretch)
            queue_header.setSectionResizeMode(2, QtWidgets.QHeaderView.ResizeMode.ResizeToContents)
            queue_layout.addWidget(self.queue_table, 1)
            queue_buttons = QtWidgets.QHBoxLayout()
            self.remove_pending_button = QtWidgets.QPushButton("Remove Pending")
            self.retry_failed_button = QtWidgets.QPushButton("Retry Failed")
            self.clear_finished_button = QtWidgets.QPushButton("Clear Finished")
            queue_buttons.addWidget(self.remove_pending_button)
            queue_buttons.addWidget(self.retry_failed_button)
            queue_buttons.addWidget(self.clear_finished_button)
            queue_layout.addLayout(queue_buttons)
            self.job_detail = QtWidgets.QPlainTextEdit()
            self.job_detail.setReadOnly(True)
            self.job_detail.setMaximumHeight(130)
            queue_layout.addWidget(self.job_detail)
            detail_buttons = QtWidgets.QHBoxLayout()
            self.open_output_button = QtWidgets.QPushButton("Open Output")
            self.copy_output_button = QtWidgets.QPushButton("Copy Output Path")
            self.copy_error_button = QtWidgets.QPushButton("Copy Error")
            detail_buttons.addWidget(self.open_output_button)
            detail_buttons.addWidget(self.copy_output_button)
            detail_buttons.addWidget(self.copy_error_button)
            queue_layout.addLayout(detail_buttons)
            layout.addWidget(queue_group, 2)

            log_group = QtWidgets.QGroupBox("Log")
            log_layout = QtWidgets.QVBoxLayout(log_group)
            log_layout.setContentsMargins(10, 14, 10, 10)
            log_layout.setSpacing(8)
            log_row = QtWidgets.QHBoxLayout()
            self.log_filter_combo = QtWidgets.QComboBox()
            self.log_filter_combo.addItems(LOG_FILTERS)
            self.clear_log_button = QtWidgets.QPushButton("Clear")
            self.copy_log_path_button = QtWidgets.QPushButton("Copy Log Path")
            self.open_log_button = QtWidgets.QPushButton("Open Log")
            log_row.addWidget(QtWidgets.QLabel("Show"))
            log_row.addWidget(self.log_filter_combo)
            log_row.addStretch(1)
            log_row.addWidget(self.clear_log_button)
            log_row.addWidget(self.copy_log_path_button)
            log_row.addWidget(self.open_log_button)
            log_layout.addLayout(log_row)
            self.log_text = QtWidgets.QPlainTextEdit()
            self.log_text.setReadOnly(True)
            self.log_text.setMaximumBlockCount(2000)
            self.log_text.setMinimumHeight(140)
            log_layout.addWidget(self.log_text)
            layout.addWidget(log_group, 1)
            return panel

        def _connect_signals(self) -> None:
            self.browse_game_button.clicked.connect(lambda: self._browse_directory(self.game_dir_edit, "game"))
            self.browse_output_button.clicked.connect(lambda: self._browse_directory(self.output_root_edit, "output"))
            self.browse_asset_button.clicked.connect(lambda: self._browse_directory(self.asset_dir_edit, "game"))
            self.browse_loose_output_button.clicked.connect(lambda: self._browse_directory(self.loose_output_edit, "output"))
            self.browse_loose_button.clicked.connect(self._browse_loose_files)
            self.scan_button.clicked.connect(self._scan_definitions)
            self.type_combo.currentTextChanged.connect(lambda _value: self._apply_filters("Showing"))
            self.search_edit.textChanged.connect(lambda _value: self._apply_filters("Showing"))
            self.clear_search_button.clicked.connect(self.search_edit.clear)
            self.resource_table.itemSelectionChanged.connect(self._update_action_states)
            self.import_selected_button.clicked.connect(self._import_selected)
            self.import_visible_button.clicked.connect(self._import_visible)
            self.import_loose_button.clicked.connect(self._import_loose)
            self.loose_file_edit.textChanged.connect(self._loose_file_text_changed)
            self.queue_table.itemSelectionChanged.connect(self._on_queue_selection_changed)
            self.remove_pending_button.clicked.connect(self._remove_selected_pending)
            self.retry_failed_button.clicked.connect(self._retry_failed)
            self.clear_finished_button.clicked.connect(self._clear_finished)
            self.open_output_button.clicked.connect(self._open_selected_output_folder)
            self.copy_output_button.clicked.connect(self._copy_selected_output_path)
            self.copy_error_button.clicked.connect(self._copy_selected_error)
            self.clear_log_button.clicked.connect(self._clear_log)
            self.log_filter_combo.currentTextChanged.connect(lambda _value: self._render_log())
            self.open_log_button.clicked.connect(self._open_log_file)
            self.copy_log_path_button.clicked.connect(self._copy_log_path)
            self.details_toggle.toggled.connect(self._set_details_visible)
            for edit in (
                self.game_dir_edit,
                self.output_root_edit,
                self.loose_file_edit,
                self.loose_output_edit,
            ):
                edit.textChanged.connect(lambda _value: self._update_action_states())
            for check in self._all_option_checks():
                check.toggled.connect(lambda _value: self._on_options_changed())

        def _set_details_visible(self, visible: bool) -> None:
            self.details_panel.setVisible(visible)
            self.details_toggle.setText("Hide Details" if visible else "Show Details")

        def _all_option_checks(self) -> list:
            return [
                check
                for check in (
                    self.animations_check,
                    self.collisions_check,
                    self.occlusion_check,
                    self.lights_check,
                    self.arms_check,
                    self.blend_check,
                    self.max_check,
                    self.project_check,
                    self.ase_check,
                    self.glb_check,
                    self.fbx_check,
                )
                if check is not None
            ]

        def _settings_path(self) -> Path:
            override = os.environ.get("OPENNOVA_IMPORTER_SETTINGS_PATH")
            if override:
                return Path(override)
            appdata = os.environ.get("APPDATA")
            if appdata:
                return Path(appdata) / "OpenNova" / "importer_settings.json"
            return Path.home() / ".opennova" / "importer_settings.json"

        def _apply_saved_preferences(self) -> None:
            prefs = load_from_path(self._settings_path())
            if prefs.get("window_geometry"):
                try:
                    self.restoreGeometry(bytes.fromhex(prefs["window_geometry"]))
                except ValueError:
                    pass
            self.game_dir_edit.setText(prefs.get("last_game_dir", ""))
            self.asset_dir_edit.setText(prefs.get("last_game_dir", ""))
            self.output_root_edit.setText(prefs.get("last_output_dir", ""))
            self.loose_output_edit.setText(prefs.get("last_output_dir", ""))

        def _save_preferences(self) -> None:
            prefs = {
                "window_geometry": bytes(self.saveGeometry()).hex(),
                "last_game_dir": self.game_dir_edit.text().strip() or self.asset_dir_edit.text().strip(),
                "last_output_dir": self.output_root_edit.text().strip() or self.loose_output_edit.text().strip(),
                "last_loose_file_dir": str(Path(self._selected_loose_paths()[0]).parent)
                if self._selected_loose_paths()
                else "",
            }
            save_to_path(self._settings_path(), prefs)

        def _browse_directory(self, target, _kind: str) -> None:
            picked = QtWidgets.QFileDialog.getExistingDirectory(self, "Select directory", target.text())
            if picked:
                target.setText(picked)
                self._save_preferences()

        def _browse_loose_files(self) -> None:
            picked, _selected_filter = QtWidgets.QFileDialog.getOpenFileNames(
                self,
                "Select .3di file(s)",
                self.asset_dir_edit.text() or self.game_dir_edit.text(),
                "3DI (*.3di);;All files (*.*)",
            )
            if picked:
                self._set_loose_paths(picked)
                if not self.asset_dir_edit.text().strip():
                    self.asset_dir_edit.setText(str(Path(picked[0]).parent))
                self._save_preferences()

        def _set_loose_paths(self, paths) -> None:
            self._loose_paths = [str(path).strip() for path in paths if str(path).strip()]
            self._updating_loose_file_edit = True
            try:
                if len(self._loose_paths) == 1:
                    display = self._loose_paths[0]
                elif self._loose_paths:
                    display = f"{len(self._loose_paths)} files selected"
                else:
                    display = ""
                self.loose_file_edit.setText(display)
                self.loose_file_edit.setToolTip("\n".join(self._loose_paths))
            finally:
                self._updating_loose_file_edit = False
            self._update_action_states()

        def _loose_file_text_changed(self, value: str) -> None:
            if self._updating_loose_file_edit:
                return
            self._loose_paths = [value.strip()] if value.strip() else []
            self.loose_file_edit.setToolTip(value)
            self._update_action_states()

        def _selected_loose_paths(self) -> list[str]:
            return [path for path in self._loose_paths if path]

        def _scan_definitions(self) -> None:
            game_dir = self.game_dir_edit.text().strip()
            if not game_dir:
                self._set_status("Game directory is required.")
                return
            self._scanning = True
            self._set_status("Scanning definitions...")
            self._update_action_states()
            task = self._start_task("scan", lambda: self._backend.scan(game_dir))
            task.signals.finished.connect(self._scan_finished)
            task.signals.failed.connect(self._scan_failed)

        def _scan_finished(self, _task_id: str, result) -> None:
            self._finish_task(_task_id)
            self._scanning = False
            if not result.ok:
                self._all_items = []
                self._visible_items = []
                self._update_resource_table()
                self._set_status(f"Scan failed: {result.error}")
                self._log_append(f"Scan failed: {result.error}", level="error")
            else:
                self._all_items = list(result.items)
                self._apply_filters("Scanned")
                self._log_append(f"Scanned {len(self._all_items)} resource(s).")
            self._save_preferences()
            self._update_action_states()

        def _scan_failed(self, task_id: str, exc) -> None:
            self._finish_task(task_id)
            self._scanning = False
            self._all_items = []
            self._visible_items = []
            self._update_resource_table()
            self._set_status(f"Scan failed: {exc}")
            self._log_append(f"Scan failed: {exc}", level="error")
            self._update_action_states()

        def _apply_filters(self, status_prefix: str) -> None:
            self._visible_items = filtered_items(self._all_items, self.type_combo.currentText(), self.search_edit.text())
            self._update_resource_table()
            self._set_status(self._resource_status(status_prefix))

        def _update_resource_table(self) -> None:
            self.resource_table.setRowCount(len(self._visible_items))
            for row, item in enumerate(self._visible_items):
                for column, value in enumerate(resource_table_row(item)):
                    table_item = QtWidgets.QTableWidgetItem(value)
                    table_item.setData(QtCore.Qt.ItemDataRole.UserRole, row)
                    self.resource_table.setItem(row, column, table_item)
            if self._visible_items:
                self.resource_count_label.setText(f"Showing {len(self._visible_items)} of {len(self._all_items)} items")
            else:
                self.resource_count_label.setText("No scanned items.")
            self.import_visible_button.setText(f"Import Visible ({len(self._visible_items)})")
            self._update_action_states()

        def _resource_status(self, prefix: str) -> str:
            if not self._all_items:
                return "No scan results."
            return f"{prefix} {len(self._visible_items)}/{len(self._all_items)} items."

        def _selected_items(self) -> list:
            rows = sorted({index.row() for index in self.resource_table.selectionModel().selectedRows()})
            return [self._visible_items[row] for row in rows if 0 <= row < len(self._visible_items)]

        def _build_options(self) -> ImportOptions:
            return ImportOptions(
                import_animations=self.animations_check.isChecked(),
                import_collisions=self.collisions_check.isChecked(),
                import_occlusion=self.occlusion_check.isChecked(),
                import_lights=self.lights_check.isChecked(),
                import_arms=self.arms_check.isChecked(),
                write_blend=self.blend_check is not None and self.blend_check.isChecked(),
                write_max=self.max_check is not None and self.max_check.isChecked(),
                write_3dp=self.project_check.isChecked(),
                write_ase=self.ase_check.isChecked(),
                write_glb=self.glb_check is not None and self.glb_check.isChecked(),
                write_fbx=self.fbx_check is not None and self.fbx_check.isChecked(),
            )

        def _apply_options(self, options: ImportOptions) -> None:
            self._applying_options = True
            try:
                self.animations_check.setChecked(options.import_animations)
                self.collisions_check.setChecked(options.import_collisions)
                self.occlusion_check.setChecked(options.import_occlusion)
                self.lights_check.setChecked(options.import_lights)
                self.arms_check.setChecked(options.import_arms)
                if self.blend_check is not None:
                    self.blend_check.setChecked(options.write_blend)
                if self.max_check is not None:
                    self.max_check.setChecked(options.write_max)
                self.project_check.setChecked(options.write_3dp)
                self.ase_check.setChecked(options.write_ase)
                if self.glb_check is not None:
                    self.glb_check.setChecked(options.write_glb)
                if self.fbx_check is not None:
                    self.fbx_check.setChecked(options.write_fbx)
                self._sync_blender_dependent_outputs()
            finally:
                self._applying_options = False
            self._update_action_states()

        def _on_options_changed(self) -> None:
            if self._applying_options:
                return
            self._sync_blender_dependent_outputs()
            self._update_action_states()

        def _sync_blender_dependent_outputs(self) -> None:
            blender_enabled = self.blend_check is not None and self.blend_check.isChecked()
            for check in (self.glb_check, self.fbx_check):
                if check is None:
                    continue
                if not blender_enabled:
                    check.setChecked(False)
                check.setEnabled(blender_enabled)

        def _import_selected(self) -> None:
            selected = self._selected_items()
            if not selected:
                self._show_errors(["Select at least one item to queue."])
                return
            self._enqueue_requests([self._definition_request(item) for item in selected])

        def _import_visible(self) -> None:
            if not self._visible_items:
                self._show_errors(["There are no visible items to queue."])
                return
            self._enqueue_requests([self._definition_request(item) for item in self._visible_items])

        def _definition_request(self, item) -> ImportRequest:
            return ImportRequest.for_definition(
                base_dir=self.game_dir_edit.text().strip(),
                item_name=item.name,
                item_type=item.type,
                output_root=self.output_root_edit.text().strip(),
                output_stem=item.output_stem,
                options=self._build_options(),
            )

        def _import_loose(self) -> None:
            paths = self._selected_loose_paths()
            if not paths:
                self._show_errors(["Select at least one .3di file to queue."])
                return
            self._enqueue_requests(
                [
                    ImportRequest.for_loose(
                        threedi_path=path,
                        output_root=self.loose_output_edit.text().strip(),
                        base_dir=self.asset_dir_edit.text().strip(),
                        options=self._build_options(),
                    )
                    for path in paths
                ]
            )

        def _enqueue_requests(self, requests: list[ImportRequest]) -> None:
            errors: list[str] = []
            valid_requests: list[ImportRequest] = []
            for request in requests:
                request_errors = validate_import_request(request)
                if request_errors:
                    errors.append(f"{request.label}: {' '.join(request_errors)}")
                else:
                    valid_requests.append(request)
            if errors:
                self._show_errors(errors)
            if not valid_requests:
                return
            collision_choice = self._resolve_output_collisions(valid_requests)
            if collision_choice.canceled:
                self._log_append("Queue canceled because output folders already exist.")
                return
            if collision_choice.skipped_count:
                self._log_append(f"Skipped {collision_choice.skipped_count} existing output folder(s).")
            valid_requests = collision_choice.requests

            active_keys = {
                job.request.dedupe_key()
                for job in self._jobs
                if job.status in ACTIVE_JOB_STATUSES
            }
            queued = 0
            skipped = 0
            for request in valid_requests:
                key = request.dedupe_key()
                if key in active_keys:
                    skipped += 1
                    continue
                self._jobs.append(ImportJob(request=request))
                active_keys.add(key)
                queued += 1
            if queued:
                self._log_append(f"Queued {queued} job(s).")
                self._save_preferences()
                self._submit_pending_jobs()
            if skipped:
                self._log_append(f"Skipped {skipped} duplicate pending/running job(s).")
            self._refresh_queue()
            self._update_action_states()

        def _resolve_output_collisions(self, requests: list[ImportRequest]):
            collisions = output_collision_paths(requests)
            if not collisions:
                return resolve_output_collisions(requests, CollisionDecision.IMPORT_ANYWAY)
            shown = "\n".join(collisions[:8])
            if len(collisions) > 8:
                shown += f"\n...and {len(collisions) - 8} more."
            answer = QtWidgets.QMessageBox.question(
                self,
                "Existing Output",
                "Some output folders already exist. Existing files may be overwritten.\n\n"
                f"{shown}\n\n"
                "Choose Yes to import anyway, No to skip existing outputs, or Cancel to stop.",
                QtWidgets.QMessageBox.StandardButton.Yes
                | QtWidgets.QMessageBox.StandardButton.No
                | QtWidgets.QMessageBox.StandardButton.Cancel,
                QtWidgets.QMessageBox.StandardButton.Yes,
            )
            if answer == QtWidgets.QMessageBox.StandardButton.Cancel:
                decision = CollisionDecision.CANCEL
            elif answer == QtWidgets.QMessageBox.StandardButton.No:
                decision = CollisionDecision.SKIP
            else:
                decision = CollisionDecision.IMPORT_ANYWAY
            return resolve_output_collisions(requests, decision)

        def _backend_max_workers(self) -> int:
            value = getattr(self._backend, "max_workers", 1)
            try:
                return max(1, int(value))
            except Exception:
                return 1

        def _submit_pending_jobs(self) -> None:
            running = sum(1 for job in self._jobs if job.status == JOB_RUNNING)
            available = max(0, self._backend_max_workers() - running)
            pending = [job for job in self._jobs if job.status == JOB_PENDING][:available]
            for job in pending:
                job.mark_running()
                self._log_append(f"Running: {job.label}", job_id=job.id)
                task = self._start_task(job.id, lambda j=job: self._backend.execute(j.request))
                task.signals.finished.connect(self._job_finished)
                task.signals.failed.connect(self._job_failed)
            self._refresh_queue()

        def _job_finished(self, job_id: str, result) -> None:
            self._finish_task(job_id)
            job = self._find_job(job_id)
            if job is None:
                return
            job.finish(result)
            if result.ok:
                self._log_append(f"Done: {job.label}", job_id=job.id)
            else:
                self._log_append(f"FAILED: {job.label}: {result.error}", level="error", job_id=job.id)
            self._refresh_queue()
            self._submit_pending_jobs()
            self._update_action_states()

        def _job_failed(self, job_id: str, exc) -> None:
            self._finish_task(job_id)
            job = self._find_job(job_id)
            if job is None:
                return
            result = ImportResult.failure(
                job.request,
                error=str(exc),
                output_path=job.request.likely_output_dir,
                elapsed_seconds=job.elapsed_seconds,
            )
            job.finish(result)
            self._log_append(f"FAILED: {job.label}: {result.error}", level="error", job_id=job.id)
            self._refresh_queue()
            self._submit_pending_jobs()
            self._update_action_states()

        def _start_task(self, task_id: str, callback) -> _FunctionTask:
            task = _FunctionTask(task_id, callback)
            self._active_tasks[task_id] = task
            self._thread_pool.start(task)
            return task

        def _finish_task(self, task_id: str) -> None:
            self._active_tasks.pop(task_id, None)

        def _refresh_queue(self) -> None:
            self.queue_table.setRowCount(len(self._jobs))
            for row, job in enumerate(self._jobs):
                values = (job.request.item_type, job.request.display_name, job.status, self._job_info(job))
                for col, value in enumerate(values):
                    item = QtWidgets.QTableWidgetItem(value)
                    item.setData(QtCore.Qt.ItemDataRole.UserRole, job.id)
                    self.queue_table.setItem(row, col, item)
            counts = self._job_counts(self._jobs)
            worker_text = f", workers {counts[JOB_RUNNING]}/{self._backend_max_workers()}"
            self.queue_summary_label.setText(
                "Queue: {pending} pending, {running} running, {done} done, {error} failed{workers}".format(
                    workers=worker_text,
                    **counts,
                )
            )
            completed = counts[JOB_DONE] + counts[JOB_ERROR]
            self.progress.setMaximum(max(len(self._jobs), 1))
            self.progress.setValue(completed)
            self._update_job_details()

        def _job_counts(self, jobs: list[ImportJob]) -> dict[str, int]:
            return {
                JOB_PENDING: sum(1 for job in jobs if job.status == JOB_PENDING),
                JOB_RUNNING: sum(1 for job in jobs if job.status == JOB_RUNNING),
                JOB_DONE: sum(1 for job in jobs if job.status == JOB_DONE),
                JOB_ERROR: sum(1 for job in jobs if job.status == JOB_ERROR),
            }

        def _job_info(self, job: ImportJob) -> str:
            if job.result:
                if job.result.ok:
                    return f"{job.result.output_path} ({self._format_elapsed(job.elapsed_seconds)})"
                return job.result.error[:100]
            if job.status == JOB_RUNNING:
                return f"Running ({self._format_elapsed(job.elapsed_seconds)})"
            return job.request.likely_output_dir

        def _selected_job_ids(self) -> set[str]:
            ids: set[str] = set()
            for index in self.queue_table.selectionModel().selectedRows():
                item = self.queue_table.item(index.row(), 0)
                if item is not None:
                    ids.add(str(item.data(QtCore.Qt.ItemDataRole.UserRole)))
            return ids

        def _selected_jobs(self) -> list[ImportJob]:
            selected = self._selected_job_ids()
            return [job for job in self._jobs if job.id in selected]

        def _primary_selected_job(self) -> ImportJob | None:
            selected = self._selected_jobs()
            return selected[0] if selected else None

        def _find_job(self, job_id: str) -> ImportJob | None:
            return next((job for job in self._jobs if job.id == job_id), None)

        def _remove_selected_pending(self) -> None:
            selected = self._selected_job_ids()
            before = len(self._jobs)
            self._jobs = [
                job for job in self._jobs if not (job.id in selected and job.status == JOB_PENDING)
            ]
            removed = before - len(self._jobs)
            if removed:
                self._log_append(f"Removed {removed} pending job(s).")
            self._refresh_queue()
            self._update_action_states()

        def _retry_failed(self) -> None:
            selected = self._selected_job_ids()
            active_keys = {
                job.request.dedupe_key()
                for job in self._jobs
                if job.status in ACTIVE_JOB_STATUSES
            }
            retried = 0
            skipped = 0
            for job in self._jobs:
                if job.id not in selected or job.status != JOB_ERROR:
                    continue
                key = job.request.dedupe_key()
                if key in active_keys:
                    skipped += 1
                    continue
                job.retry()
                active_keys.add(key)
                retried += 1
            if retried:
                self._log_append(f"Retried {retried} failed job(s).")
                self._submit_pending_jobs()
            if skipped:
                self._log_append(f"Skipped {skipped} retry duplicate(s).")
            self._refresh_queue()
            self._update_action_states()

        def _clear_finished(self) -> None:
            before = len(self._jobs)
            self._jobs = [job for job in self._jobs if job.status not in (JOB_DONE, JOB_ERROR)]
            cleared = before - len(self._jobs)
            if cleared:
                self._log_append(f"Cleared {cleared} finished job(s).")
            self._refresh_queue()
            self._update_action_states()

        def _on_queue_selection_changed(self) -> None:
            self._update_action_states()
            self._update_job_details()
            if self.log_filter_combo.currentText() == LOG_FILTER_CURRENT:
                self._render_log()

        def _update_job_details(self) -> None:
            selected = self._selected_jobs()
            if not selected:
                text = "Select a queued job to see its request, options, output path, and error details."
            elif len(selected) > 1:
                counts = self._job_counts(selected)
                text = f"{len(selected)} jobs selected.\nPending: {counts[JOB_PENDING]}, Running: {counts[JOB_RUNNING]}, Done: {counts[JOB_DONE]}, Failed: {counts[JOB_ERROR]}"
            else:
                text = self._format_job_details(selected[0])
            self.job_detail.setPlainText(text)

        def _format_job_details(self, job: ImportJob) -> str:
            request = job.request
            lines = [
                f"Name: {request.display_name}",
                f"Type: {request.item_type}",
                f"Mode: {request.mode}",
                f"Status: {job.status}",
                f"Elapsed: {self._format_elapsed(job.elapsed_seconds)}",
                f"Game/asset dir: {request.base_dir or '(none)'}",
                f"Source file: {request.threedi_path or '(definition lookup)'}",
                f"Output stem: {request.output_stem or '(resolved at import time)'}",
                f"Output: {self._job_output_path(job)}",
                f"Options: {self._format_options(request.options)}",
            ]
            if job.result and job.result.written_files:
                lines.extend(["", "Written files:", *job.result.written_files])
            if job.result and job.result.warnings:
                lines.extend(["", "Warnings:", *job.result.warnings])
            error = self._job_error(job)
            if error:
                lines.extend(["", "Error:", error])
            return "\n".join(lines)

        def _format_options(self, options: ImportOptions) -> str:
            imports = [
                label
                for label, value in (
                    ("animations", options.import_animations),
                    ("collisions", options.import_collisions),
                    ("occlusion", options.import_occlusion),
                    ("lights", options.import_lights),
                    ("arms", options.import_arms),
                )
                if value
            ]
            outputs = [
                label
                for label, value in (
                    ("blend", options.write_blend),
                    ("max", options.write_max),
                    ("3dp", options.write_3dp),
                    ("ase", options.write_ase),
                    ("glb", options.write_glb),
                    ("fbx", options.write_fbx),
                )
                if value
            ]
            return f"import: {', '.join(imports) if imports else 'geometry only'}; files: {', '.join(outputs) if outputs else 'none'}"

        def _format_elapsed(self, seconds: float) -> str:
            if seconds <= 0:
                return "0s"
            if seconds < 60:
                return f"{seconds:.1f}s"
            minutes = int(seconds // 60)
            return f"{minutes}m {int(seconds % 60)}s"

        def _job_output_path(self, job: ImportJob) -> str:
            if job.result and job.result.output_path:
                return job.result.output_path
            return job.request.likely_output_dir

        def _job_error(self, job: ImportJob) -> str:
            if job.result and job.result.error:
                return job.result.error
            return job.error

        def _update_action_states(self) -> None:
            selected_items = self._selected_items() if hasattr(self, "resource_table") else []
            has_game_dir = bool(self.game_dir_edit.text().strip())
            has_output = bool(self.output_root_edit.text().strip())
            writes = self._build_options().writes_any_output_file() if hasattr(self, "project_check") else False
            self.scan_button.setEnabled(not self._scanning and has_game_dir)
            self.import_selected_button.setEnabled(has_output and writes and bool(selected_items))
            self.import_visible_button.setEnabled(has_output and writes and bool(self._visible_items))
            self.import_loose_button.setEnabled(bool(self._selected_loose_paths()) and bool(self.loose_output_edit.text().strip()) and writes)
            selected_jobs = self._selected_jobs() if hasattr(self, "queue_table") else []
            can_remove = any(job.status == JOB_PENDING for job in selected_jobs)
            can_retry = any(job.status == JOB_ERROR for job in selected_jobs)
            has_output_path = any(self._job_output_path(job) for job in selected_jobs)
            has_error = any(self._job_error(job) for job in selected_jobs)
            can_clear = any(job.status in (JOB_DONE, JOB_ERROR) for job in self._jobs)
            self.remove_pending_button.setEnabled(can_remove)
            self.retry_failed_button.setEnabled(can_retry)
            self.clear_finished_button.setEnabled(can_clear)
            self.open_output_button.setEnabled(has_output_path)
            self.copy_output_button.setEnabled(has_output_path)
            self.copy_error_button.setEnabled(has_error)

        def _show_errors(self, errors: list[str]) -> None:
            clean = [error for error in errors if error]
            if not clean:
                return
            message = "\n".join(clean[:8])
            if len(clean) > 8:
                message += f"\n...and {len(clean) - 8} more."
            self._log_append("ERROR: " + " | ".join(clean[:8]), level="error")
            QtWidgets.QMessageBox.critical(self, "OpenNova Importer", message)

        def _log_append(self, message: str, *, level: str = "info", job_id: str = "") -> None:
            import time

            self._log_entries.append(
                {
                    "time": time.strftime("%H:%M:%S"),
                    "message": message,
                    "level": level,
                    "job_id": job_id,
                }
            )
            if len(self._log_entries) > 2000:
                self._log_entries = self._log_entries[-2000:]
            self._render_log()

        def _render_log(self) -> None:
            selected = self._selected_job_ids() if hasattr(self, "queue_table") else set()
            filter_value = self.log_filter_combo.currentText() if hasattr(self, "log_filter_combo") else LOG_FILTER_ALL
            lines = []
            for entry in self._log_entries:
                if filter_value == LOG_FILTER_ERRORS and entry["level"] != "error":
                    continue
                if filter_value == LOG_FILTER_CURRENT and entry["job_id"] not in selected:
                    continue
                lines.append(f"[{entry['time']}] {entry['message']}")
            self.log_text.setPlainText("\n".join(lines) + ("\n" if lines else ""))
            self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())

        def _clear_log(self) -> None:
            self._log_entries.clear()
            self._render_log()

        def _current_log_path(self) -> str:
            for handler in logging.getLogger().handlers:
                if isinstance(handler, logging.FileHandler):
                    return handler.baseFilename
            return ""

        def _copy_log_path(self) -> None:
            path = self._current_log_path()
            if path:
                QtWidgets.QApplication.clipboard().setText(path)
                self._log_append(f"Copied log path: {path}")

        def _open_log_file(self) -> None:
            path = self._current_log_path()
            if not path:
                self._show_errors(["No log file is configured."])
                return
            self._open_path(Path(path))

        def _copy_selected_output_path(self) -> None:
            job = self._primary_selected_job()
            if job is None:
                return
            path = self._job_output_path(job)
            if path:
                QtWidgets.QApplication.clipboard().setText(path)
                self._log_append(f"Copied output path: {path}", job_id=job.id)

        def _copy_selected_error(self) -> None:
            job = self._primary_selected_job()
            if job is None:
                return
            error = self._job_error(job)
            if error:
                QtWidgets.QApplication.clipboard().setText(error)
                self._log_append(f"Copied error for {job.label}", job_id=job.id)

        def _open_selected_output_folder(self) -> None:
            job = self._primary_selected_job()
            if job is None:
                return
            raw_path = self._job_output_path(job)
            if not raw_path:
                return
            path = Path(raw_path)
            if not path.exists():
                path = path.parent
            if not path.exists():
                self._show_errors([f"Output folder does not exist: {raw_path}"])
                return
            self._open_path(path)

        def _open_path(self, path: Path) -> None:
            try:
                if hasattr(os, "startfile"):
                    os.startfile(str(path))  # type: ignore[attr-defined]
                elif sys.platform == "darwin":
                    import subprocess

                    subprocess.Popen(["open", str(path)])
                else:
                    import subprocess

                    subprocess.Popen(["xdg-open", str(path)])
            except OSError as exc:
                self._show_errors([f"Could not open path: {exc}"])

        def _set_status(self, message: str) -> None:
            self.status_label.setText(message)

else:

    class OpenNovaImporterDialog:  # pragma: no cover
        pass
