"""DCC-agnostic Qt importer UI.

Importing this package exposes pure backend types without importing PySide6.
Qt widgets load lazily when callers request the dialog entry points.
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

_DIALOG_EXPORTS = {
    "OpenNovaImporterDialog",
    "close_importer_dialog",
    "is_available",
    "show_importer_dialog",
}


def __getattr__(name: str):
    if name in _DIALOG_EXPORTS:
        from . import dialog as _dialog

        return getattr(_dialog, name)
    raise AttributeError(f"module 'opennova_qt_ui' has no attribute {name!r}")
