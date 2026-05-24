"""Qt entrypoint for the standalone OpenNova Importer."""
from __future__ import annotations

import logging
import sys

from opennova_blender import StandaloneBackend
from opennova_qt_ui import OpenNovaImporterDialog


log = logging.getLogger(__name__)


def run_gui() -> None:
    from PySide6 import QtWidgets

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication(sys.argv)
    backend = StandaloneBackend()
    try:
        dialog = OpenNovaImporterDialog(backend=backend, version=_version_string())
        dialog.show()
        app.exec()
    finally:
        backend.shutdown()


def _version_string() -> str:
    try:
        from importlib.metadata import version

        return version("opennova-tools")
    except Exception:
        return ""
