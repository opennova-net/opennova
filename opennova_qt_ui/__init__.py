"""Host-agnostic Qt importer dialog.

The Protocol and the pure-Python helpers (`backend`, `filtering`,
`preferences`) are eagerly importable. The Qt dialog itself loads PySide6
lazily — importing this package does NOT pull in PySide6 unless a caller
actually touches `OpenNovaImporterDialog`, `show_importer_dialog`,
`close_importer_dialog`, or `is_available`. This keeps
`opennova_qt_ui.filtering` and `opennova_qt_ui.preferences` PySide-free
for headless callers and tests.
"""
from __future__ import annotations

from .backend import BackendCapabilities, ImportBackend


__all__ = [
    "BackendCapabilities",
    "ImportBackend",
    "OpenNovaImporterDialog",
    "close_importer_dialog",
    "is_available",
    "show_importer_dialog",
]


_LAZY_DIALOG_NAMES = {
    "OpenNovaImporterDialog",
    "close_importer_dialog",
    "is_available",
    "show_importer_dialog",
}


def __getattr__(name: str):
    if name in _LAZY_DIALOG_NAMES:
        from . import dialog as _dialog

        return getattr(_dialog, name)
    raise AttributeError(f"module 'opennova_qt_ui' has no attribute {name!r}")
