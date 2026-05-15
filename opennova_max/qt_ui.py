"""3ds Max shim around the host-agnostic OpenNova importer dialog.

The dialog body lives in `opennova_qt_ui.dialog`. This module composes a
MaxBackend (added in Task 4.6) and asks the shared dialog to display.
"""
from __future__ import annotations

from .version import get_version

# Re-export dialog helpers so existing callers using `opennova_max.qt_ui`
# continue to work without modification.
from opennova_qt_ui.dialog import (  # noqa: F401
    RESOURCE_COLUMNS,
    DEFAULT_SCENE_OPTIONS,
    TYPE_FILTERS,
    QT_BINDING,
    can_import_batch,
    can_import_definition,
    can_import_loose,
    dialog_paths_from_preferences,
    dialog_title,
    is_available,
    loose_paths_display,
    resource_table_row,
    writes_any_output,
    close_importer_dialog,
    OpenNovaImporterDialog,
)


def show_importer_dialog() -> bool:
    """Open the Qt importer dialog inside 3ds Max."""
    from opennova_qt_ui import show_importer_dialog as _show

    # MaxBackend is created in Task 4.6; for Task 4.4 we still hand a
    # placeholder to keep the dialog working. The dialog itself ignores
    # the backend during Task 4.4 — see the TODO markers in
    # opennova_qt_ui.dialog.
    backend = _build_backend()
    return _show(backend, version=get_version(), parent=_max_parent())


def _build_backend():
    """Build the Max backend. Returns an object whose surface matches the
    ImportBackend Protocol once Task 4.6 lands MaxBackend."""
    try:
        from .backend import MaxBackend
        return MaxBackend()
    except ImportError:
        return None


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
