"""Qt importer dialog for 3ds Max."""
from __future__ import annotations

from pathlib import Path
from .version import get_version

try:  # pragma: no cover - exercised in local Max, not CI
    from PySide6 import QtCore, QtWidgets

    QT_BINDING = "PySide6"
except Exception:  # pragma: no cover
    try:
        from PySide2 import QtCore, QtWidgets

        QT_BINDING = "PySide2"
    except Exception:
        QtCore = None
        QtWidgets = None
        QT_BINDING = ""


RESOURCE_COLUMNS = ("Type", "Name", "Model", "Output")
TYPE_FILTERS = ("all", "weapon", "item")
DEFAULT_SCENE_OPTIONS = {
    "collisions": True,
    "occlusion": True,
    "lights": True,
    "arms": True,
    "animations": False,
}
_DIALOG = None


def is_available() -> bool:
    return QtCore is not None and QtWidgets is not None


def _qt_enum(container, name: str, nested_name: str):
    value = getattr(container, name, None)
    if value is not None:
        return value
    return getattr(getattr(container, nested_name), name)


def dialog_title(version: str | None = None) -> str:
    return f"OpenNova Importer v{version or get_version()}"


def resource_table_row(item) -> tuple[str, str, str, str]:
    source = getattr(item, "source_model", "") or "(definition lookup)"
    stem = getattr(item, "output_stem", "") or Path(source).stem
    return (
        getattr(item, "type", ""),
        getattr(item, "name", ""),
        source,
        stem,
    )


def writes_any_output(
    write_ase: bool,
    write_3dp: bool,
    write_max: bool,
    copy_textures: bool = False,
) -> bool:
    return bool(write_ase or write_3dp or write_max or copy_textures)


def can_import_loose(
    threedi_path,
    output_dir: str,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool,
    copy_textures: bool = False,
) -> bool:
    return bool(
        _loose_path_strings(threedi_path)
        and output_dir.strip()
        and writes_any_output(write_ase, write_3dp, write_max, copy_textures)
    )


def loose_paths_display(paths) -> str:
    normalized = _loose_path_strings(paths)
    if len(normalized) == 1:
        return normalized[0]
    if normalized:
        return f"{len(normalized)} files selected"
    return ""


def _loose_path_strings(value) -> list[str]:
    if isinstance(value, (list, tuple, set)):
        raw_paths = value
    else:
        raw_paths = [value]
    return [str(path).strip() for path in raw_paths if str(path).strip()]


def dialog_paths_from_preferences(prefs: dict) -> dict[str, str]:
    resource_dir = str(prefs.get("last_resource_dir") or "").strip()
    output_dir = str(prefs.get("last_output_dir") or "").strip()
    loose_file_dir = str(prefs.get("last_loose_file_dir") or "").strip()
    return {
        "game_dir": resource_dir,
        "asset_dir": resource_dir,
        "output_root": output_dir,
        "loose_output": output_dir,
        "loose_file_dir": loose_file_dir,
    }


def can_import_definition(
    *,
    has_selection: bool,
    output_root: str,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool,
    copy_textures: bool = False,
) -> bool:
    return bool(
        has_selection
        and output_root.strip()
        and writes_any_output(write_ase, write_3dp, write_max, copy_textures)
    )


def can_import_batch(
    *,
    visible_count: int,
    output_root: str,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool,
    copy_textures: bool = False,
) -> bool:
    return bool(
        visible_count > 0
        and output_root.strip()
        and writes_any_output(write_ase, write_3dp, write_max, copy_textures)
    )


def _dialog_alive(dialog) -> bool:
    """Return True iff ``dialog`` still has a live underlying C++ widget.

    Max can tear down the parent main window across workspace switches /
    interpreter reloads, leaving the cached Python wrapper dangling. Calling
    a cheap method like ``objectName()`` raises ``RuntimeError`` ("Internal
    C++ object already deleted") in that case. Portable across PySide2 and
    PySide6 with no ``shiboken`` import.
    """
    if dialog is None:
        return False
    try:
        dialog.objectName()
        return True
    except RuntimeError:
        return False


def show_importer_dialog() -> bool:
    """Show the Qt importer dialog."""
    if not is_available():
        raise RuntimeError("Qt/PySide is not available in this Python environment; run the importer from 3ds Max.")

    app = QtWidgets.QApplication.instance()
    if app is None:  # pragma: no cover - Max normally owns QApplication
        app = QtWidgets.QApplication([])

    global _DIALOG
    if not _dialog_alive(_DIALOG):
        _DIALOG = OpenNovaImporterDialog(parent=_max_parent())
    _DIALOG.show()
    _DIALOG.raise_()
    _DIALOG.activateWindow()
    return True


def close_importer_dialog() -> bool:
    global _DIALOG
    if _DIALOG is None:
        return False
    try:
        _DIALOG.close()
        return True
    finally:
        _DIALOG = None


def _max_parent():
    try:  # pragma: no cover - only works inside Max
        import qtmax

        for getter_name in ("GetQMaxMainWindow", "GetQMaxApplicationWindow"):
            getter = getattr(qtmax, getter_name, None)
            if getter is not None:
                return getter()
    except BaseException:
        pass
    return None


if is_available():  # pragma: no cover - UI construction is local-Max validated

    class OpenNovaImporterDialog(QtWidgets.QDialog):
        def __init__(self, parent=None):
            super().__init__(parent)
            self._all_items = []
            self._visible_items = []
            self._loose_paths = []
            self._updating_loose_file_edit = False
            self._busy = False
            self._last_loose_file_dir = ""
            self._version = get_version()
            self.setWindowTitle(dialog_title(self._version))
            self.setMinimumSize(900, 620)
            self.resize(980, 680)
            self._build_ui()
            self._apply_style()
            self._connect_signals()
            self._apply_saved_preferences()

            from . import ui as ui_helpers

            ui_helpers.add_status_listener(self._set_status)
            self._set_status("Ready")
            self._update_resource_table()
            self._update_actions()

        def closeEvent(self, event):
            from . import ui as ui_helpers

            ui_helpers.remove_status_listener(self._set_status)
            global _DIALOG
            if _DIALOG is self:
                _DIALOG = None
            super().closeEvent(event)

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
                QLabel {
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
                QLineEdit:disabled, QComboBox:disabled {
                    color: #9aa3ab;
                    background: #2b3035;
                    border-color: #4b5259;
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
                QTableWidget {
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
                QTabBar::tab:focus {
                    outline: 0;
                }
                QTabBar::tab:selected {
                    background: #4a525a;
                    color: #ffffff;
                }
                QTabBar::tab:selected:focus {
                    outline: 0;
                }
            """)

        def _build_ui(self) -> None:
            root = QtWidgets.QVBoxLayout(self)
            root.setContentsMargins(16, 14, 16, 14)
            root.setSpacing(12)

            header = QtWidgets.QHBoxLayout()
            title = QtWidgets.QLabel("OpenNova Importer")
            title.setObjectName("OpenNovaTitle")
            self.version_label = QtWidgets.QLabel(f"v{self._version}")
            self.version_label.setObjectName("OpenNovaVersion")
            self.version_label.setAlignment(
                _qt_enum(QtCore.Qt, "AlignRight", "AlignmentFlag")
                | _qt_enum(QtCore.Qt, "AlignVCenter", "AlignmentFlag")
            )
            header.addWidget(title, 1)
            header.addWidget(self.version_label)
            root.addLayout(header)

            body = QtWidgets.QHBoxLayout()
            body.setSpacing(16)
            self.tabs = QtWidgets.QTabWidget()
            self.tabs.setFocusPolicy(_qt_enum(QtCore.Qt, "NoFocus", "FocusPolicy"))
            self.tabs.tabBar().setFocusPolicy(_qt_enum(QtCore.Qt, "NoFocus", "FocusPolicy"))
            self.tabs.addTab(self._build_definition_tab(), "Definitions")
            self.tabs.addTab(self._build_loose_tab(), "Loose .3di")
            body.addWidget(self.tabs, 1)
            body.addWidget(self._build_options_panel())
            root.addLayout(body, 1)

            self.status_label = QtWidgets.QLabel()
            self.status_label.setObjectName("OpenNovaStatus")
            self.status_label.setMinimumHeight(24)
            self.status_label.setTextInteractionFlags(
                _qt_enum(QtCore.Qt, "TextSelectableByMouse", "TextInteractionFlag")
            )
            root.addWidget(self.status_label)

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
            self.resource_table.setSelectionBehavior(
                _qt_enum(QtWidgets.QAbstractItemView, "SelectRows", "SelectionBehavior")
            )
            self.resource_table.setSelectionMode(
                _qt_enum(QtWidgets.QAbstractItemView, "SingleSelection", "SelectionMode")
            )
            self.resource_table.setEditTriggers(
                _qt_enum(QtWidgets.QAbstractItemView, "NoEditTriggers", "EditTrigger")
            )
            self.resource_table.verticalHeader().setVisible(False)
            header = self.resource_table.horizontalHeader()
            header.setStretchLastSection(True)
            header.setSectionResizeMode(0, _qt_enum(QtWidgets.QHeaderView, "ResizeToContents", "ResizeMode"))
            header.setSectionResizeMode(1, _qt_enum(QtWidgets.QHeaderView, "Stretch", "ResizeMode"))
            header.setSectionResizeMode(2, _qt_enum(QtWidgets.QHeaderView, "Stretch", "ResizeMode"))
            header.setSectionResizeMode(3, _qt_enum(QtWidgets.QHeaderView, "ResizeToContents", "ResizeMode"))
            layout.addWidget(self.resource_table, 1)

            actions = QtWidgets.QHBoxLayout()
            actions.addStretch(1)
            self.import_selected_button = QtWidgets.QPushButton("Import Selected")
            self.import_batch_button = QtWidgets.QPushButton("Import Filtered Batch")
            actions.addWidget(self.import_selected_button)
            actions.addWidget(self.import_batch_button)
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
            self.options_panel.setMinimumWidth(185)
            self.options_panel.setMaximumWidth(220)
            layout = QtWidgets.QVBoxLayout(self.options_panel)
            layout.setContentsMargins(10, 14, 10, 10)
            layout.setSpacing(12)

            scene_group = QtWidgets.QGroupBox("Scene")
            scene_layout = QtWidgets.QVBoxLayout(scene_group)
            scene_layout.setContentsMargins(10, 14, 10, 10)
            scene_layout.setSpacing(6)
            self.collisions_check = QtWidgets.QCheckBox("Collisions")
            self.occlusion_check = QtWidgets.QCheckBox("Occlusion")
            self.lights_check = QtWidgets.QCheckBox("Lights")
            self.arms_check = QtWidgets.QCheckBox("Arms")
            self.animations_check = QtWidgets.QCheckBox("Animations")
            for check, checked in (
                (self.collisions_check, DEFAULT_SCENE_OPTIONS["collisions"]),
                (self.occlusion_check, DEFAULT_SCENE_OPTIONS["occlusion"]),
                (self.lights_check, DEFAULT_SCENE_OPTIONS["lights"]),
                (self.arms_check, DEFAULT_SCENE_OPTIONS["arms"]),
                (self.animations_check, DEFAULT_SCENE_OPTIONS["animations"]),
            ):
                check.setChecked(checked)
                scene_layout.addWidget(check)
            layout.addWidget(scene_group)

            output_group = QtWidgets.QGroupBox("Output")
            output_layout = QtWidgets.QVBoxLayout(output_group)
            output_layout.setContentsMargins(10, 14, 10, 10)
            output_layout.setSpacing(6)
            self.ase_check = QtWidgets.QCheckBox("ASE")
            self.project_check = QtWidgets.QCheckBox("3DP/3DA")
            self.max_check = QtWidgets.QCheckBox("MAX")
            self.textures_check = QtWidgets.QCheckBox("Textures")
            for check in (self.ase_check, self.project_check, self.max_check, self.textures_check):
                check.setChecked(True)
                output_layout.addWidget(check)
            layout.addWidget(output_group)
            layout.addStretch(1)
            return self.options_panel

        def _connect_signals(self) -> None:
            self.browse_game_button.clicked.connect(lambda: self._browse_directory(self.game_dir_edit, "resource"))
            self.browse_output_button.clicked.connect(lambda: self._browse_directory(self.output_root_edit, "output"))
            self.browse_asset_button.clicked.connect(lambda: self._browse_directory(self.asset_dir_edit, "resource"))
            self.browse_loose_output_button.clicked.connect(lambda: self._browse_directory(self.loose_output_edit, "output"))
            self.browse_loose_button.clicked.connect(self._browse_loose_file)
            self.scan_button.clicked.connect(self._scan_definitions)
            self.type_combo.currentTextChanged.connect(lambda _value: self._apply_filters("Showing"))
            self.search_edit.textChanged.connect(lambda _value: self._apply_filters("Showing"))
            self.clear_search_button.clicked.connect(self.search_edit.clear)
            self.resource_table.itemSelectionChanged.connect(self._update_actions)
            self.import_selected_button.clicked.connect(self._import_selected)
            self.import_batch_button.clicked.connect(self._import_batch)
            self.import_loose_button.clicked.connect(self._import_loose)
            self.loose_file_edit.textChanged.connect(self._loose_file_text_changed)

            for edit in (
                self.game_dir_edit,
                self.output_root_edit,
                self.loose_file_edit,
                self.loose_output_edit,
            ):
                edit.textChanged.connect(lambda _value: self._update_actions())
            for check in (self.ase_check, self.project_check, self.max_check, self.textures_check):
                check.toggled.connect(lambda _value: self._update_actions())

        def _apply_saved_preferences(self) -> None:
            from .preferences import load_preferences

            paths = dialog_paths_from_preferences(load_preferences())
            if paths["game_dir"]:
                self.game_dir_edit.setText(paths["game_dir"])
            if paths["asset_dir"]:
                self.asset_dir_edit.setText(paths["asset_dir"])
            if paths["output_root"]:
                self.output_root_edit.setText(paths["output_root"])
            if paths["loose_output"]:
                self.loose_output_edit.setText(paths["loose_output"])
            self._last_loose_file_dir = paths["loose_file_dir"]

        def _browse_directory(self, target, preference_kind: str) -> None:
            picked = QtWidgets.QFileDialog.getExistingDirectory(self, "Select directory", target.text())
            if picked:
                target.setText(picked)
                if preference_kind == "resource":
                    self._remember_preferences(resource_dir=picked)
                elif preference_kind == "output":
                    self._remember_preferences(output_dir=picked)

        def _browse_loose_file(self) -> None:
            picked, _selected_filter = QtWidgets.QFileDialog.getOpenFileNames(
                self,
                "Select .3di file(s)",
                self._loose_file_initial_dir(),
                "3DI (*.3di);;All files (*.*)",
            )
            if picked:
                self._set_loose_paths(picked)
                self._last_loose_file_dir = str(Path(picked[0]).parent)
                if not self.asset_dir_edit.text().strip():
                    self.asset_dir_edit.setText(str(Path(picked[0]).parent))
                self._remember_current_loose_paths()

        def _set_loose_paths(self, paths) -> None:
            self._loose_paths = _loose_path_strings(paths)
            self._updating_loose_file_edit = True
            try:
                self.loose_file_edit.setText(loose_paths_display(self._loose_paths))
                self.loose_file_edit.setToolTip("\n".join(self._loose_paths))
            finally:
                self._updating_loose_file_edit = False
            self._update_actions()

        def _loose_file_text_changed(self, _value: str) -> None:
            if not self._updating_loose_file_edit:
                self._loose_paths = []
                self.loose_file_edit.setToolTip(self.loose_file_edit.text())

        def _selected_loose_paths(self) -> list[str]:
            return self._loose_paths or _loose_path_strings(self.loose_file_edit.text())

        def _loose_file_initial_dir(self) -> str:
            for candidate in (
                self._last_loose_file_dir,
                self.asset_dir_edit.text(),
                self.game_dir_edit.text(),
            ):
                if str(candidate).strip():
                    return str(candidate).strip()
            return self.loose_file_edit.text()

        def _remember_preferences(
            self,
            *,
            resource_dir: str | None = None,
            output_dir: str | None = None,
            loose_file_dir: str | None = None,
        ) -> None:
            from .preferences import load_preferences, save_preferences

            prefs = load_preferences()
            if resource_dir is not None and resource_dir.strip():
                prefs["last_resource_dir"] = resource_dir.strip()
            if output_dir is not None and output_dir.strip():
                prefs["last_output_dir"] = output_dir.strip()
            if loose_file_dir is not None and loose_file_dir.strip():
                prefs["last_loose_file_dir"] = loose_file_dir.strip()
            save_preferences(prefs)

        def _remember_current_definition_paths(self) -> None:
            self._remember_preferences(
                resource_dir=self.game_dir_edit.text(),
                output_dir=self.output_root_edit.text(),
            )

        def _remember_current_loose_paths(self) -> None:
            loose_paths = self._selected_loose_paths()
            loose_file_dir = self._last_loose_file_dir
            if loose_paths:
                loose_file_dir = str(Path(loose_paths[0]).parent)
                self._last_loose_file_dir = loose_file_dir
            self._remember_preferences(
                resource_dir=self.asset_dir_edit.text(),
                output_dir=self.loose_output_edit.text(),
                loose_file_dir=loose_file_dir,
            )

        def _scan_definitions(self) -> None:
            game_dir = self.game_dir_edit.text().strip()
            if not game_dir:
                self._set_status("Game directory is required.")
                return
            self._remember_current_definition_paths()

            def run():
                from . import ui as ui_helpers

                ok, items, error = ui_helpers._scan_definitions(game_dir)
                if not ok:
                    self._all_items = []
                    self._visible_items = []
                    self._update_resource_table()
                    self._set_status(f"Scan failed: {error}")
                    return
                self._all_items = sorted(
                    items,
                    key=lambda item: (
                        getattr(item, "type", "").casefold(),
                        getattr(item, "name", "").casefold(),
                    ),
                )
                self._apply_filters("Scanned")

            self._run_busy("Scanning definitions...", run)

        def _apply_filters(self, status_prefix: str) -> None:
            from . import ui as ui_helpers

            self._visible_items = ui_helpers._filtered_items(
                self._all_items,
                self.type_combo.currentText(),
                self.search_edit.text(),
            )
            self._update_resource_table()
            self._set_status(self._resource_status(status_prefix))

        def _update_resource_table(self) -> None:
            self.resource_table.setRowCount(len(self._visible_items))
            for row, item in enumerate(self._visible_items):
                for column, value in enumerate(resource_table_row(item)):
                    table_item = QtWidgets.QTableWidgetItem(value)
                    table_item.setData(_qt_enum(QtCore.Qt, "UserRole", "ItemDataRole"), row)
                    self.resource_table.setItem(row, column, table_item)

            if self._visible_items:
                self.resource_count_label.setText(
                    f"Showing {len(self._visible_items)} of {len(self._all_items)} resources"
                )
            else:
                self.resource_count_label.setText("No resources scanned")
            self._update_actions()

        def _resource_status(self, prefix: str) -> str:
            if not self._all_items:
                return "No resources scanned."
            filters = []
            if self.type_combo.currentText() != "all":
                filters.append(self.type_combo.currentText())
            if self.search_edit.text().strip():
                filters.append(f"search '{self.search_edit.text().strip()}'")
            suffix = f" ({', '.join(filters)})" if filters else ""
            return f"{prefix} {len(self._visible_items)}/{len(self._all_items)} resources{suffix}."

        def _selected_item(self):
            row = self.resource_table.currentRow()
            if row < 0 or row >= len(self._visible_items):
                return None
            return self._visible_items[row]

        def _import_selected(self) -> None:
            item = self._selected_item()
            if item is None:
                self._set_status("Select a resource first.")
                return
            self._remember_current_definition_paths()

            def run():
                from . import ui as ui_helpers

                ui_helpers._run_definition_item(
                    item,
                    self.game_dir_edit.text(),
                    self.output_root_edit.text(),
                    import_arms=self.arms_check.isChecked(),
                    import_animations=self.animations_check.isChecked(),
                    import_collisions=self.collisions_check.isChecked(),
                    import_occlusion=self.occlusion_check.isChecked(),
                    import_lights=self.lights_check.isChecked(),
                    write_ase=self.ase_check.isChecked(),
                    write_3dp=self.project_check.isChecked(),
                    write_max=self.max_check.isChecked(),
                    copy_textures=self.textures_check.isChecked(),
                )

            self._run_busy(f"Importing {item.type} {item.name}...", run)

        def _import_batch(self) -> None:
            items = list(self._visible_items)
            if not items:
                self._set_status("No resources match the current filters.")
                return
            self._remember_current_definition_paths()

            def run():
                from . import ui as ui_helpers

                completed = 0
                total = len(items)
                for index, item in enumerate(items, start=1):
                    self._set_status(
                        f"Importing {index}/{total}: {item.type} {item.name}..."
                    )
                    QtWidgets.QApplication.processEvents()
                    if ui_helpers._run_definition_item(
                        item,
                        self.game_dir_edit.text(),
                        self.output_root_edit.text(),
                        import_arms=self.arms_check.isChecked(),
                        import_animations=self.animations_check.isChecked(),
                        import_collisions=self.collisions_check.isChecked(),
                        import_occlusion=self.occlusion_check.isChecked(),
                        import_lights=self.lights_check.isChecked(),
                        write_ase=self.ase_check.isChecked(),
                        write_3dp=self.project_check.isChecked(),
                        write_max=self.max_check.isChecked(),
                        copy_textures=self.textures_check.isChecked(),
                    ):
                        completed += 1
                self._set_status(
                    f"Filtered batch complete: {completed}/{total} resources -> {Path(self.output_root_edit.text().strip())}"
                )

            self._run_busy("Running filtered batch import...", run)

        def _import_loose(self) -> None:
            self._remember_current_loose_paths()

            def run():
                from . import ui as ui_helpers

                ui_helpers._run_loose_from_ui(
                    self._selected_loose_paths(),
                    self.asset_dir_edit.text(),
                    self.loose_output_edit.text(),
                    self.collisions_check.isChecked(),
                    self.occlusion_check.isChecked(),
                    self.lights_check.isChecked(),
                    self.ase_check.isChecked(),
                    self.project_check.isChecked(),
                    self.max_check.isChecked(),
                    self.textures_check.isChecked(),
                )

            self._run_busy("Importing loose .3di...", run)

        def _run_busy(self, message: str, callback) -> None:
            self._set_busy(True)
            self._set_status(message)
            QtWidgets.QApplication.setOverrideCursor(_qt_enum(QtCore.Qt, "WaitCursor", "CursorShape"))
            try:
                callback()
            except Exception as exc:
                self._set_status(f"Import failed: {exc}")
                QtWidgets.QMessageBox.critical(self, "OpenNova Importer", str(exc))
            finally:
                QtWidgets.QApplication.restoreOverrideCursor()
                self._set_busy(False)

        def _set_busy(self, busy: bool) -> None:
            self._busy = busy
            self.tabs.setEnabled(not busy)
            self.options_panel.setEnabled(not busy)
            self._update_actions()

        def _update_actions(self) -> None:
            write_ase = self.ase_check.isChecked()
            write_3dp = self.project_check.isChecked()
            write_max = self.max_check.isChecked()
            copy_textures = self.textures_check.isChecked()
            self.scan_button.setEnabled(not self._busy and bool(self.game_dir_edit.text().strip()))
            self.import_selected_button.setEnabled(
                not self._busy
                and can_import_definition(
                    has_selection=self._selected_item() is not None,
                    output_root=self.output_root_edit.text(),
                    write_ase=write_ase,
                    write_3dp=write_3dp,
                    write_max=write_max,
                    copy_textures=copy_textures,
                )
            )
            self.import_batch_button.setEnabled(
                not self._busy
                and can_import_batch(
                    visible_count=len(self._visible_items),
                    output_root=self.output_root_edit.text(),
                    write_ase=write_ase,
                    write_3dp=write_3dp,
                    write_max=write_max,
                    copy_textures=copy_textures,
                )
            )
            self.import_loose_button.setEnabled(
                not self._busy
                and can_import_loose(
                    self._selected_loose_paths(),
                    self.loose_output_edit.text(),
                    write_ase,
                    write_3dp,
                    write_max,
                    copy_textures,
                )
            )

        def _set_status(self, message: str) -> None:
            self.status_label.setText(message)

else:

    class OpenNovaImporterDialog:  # pragma: no cover
        pass
