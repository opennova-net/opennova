"""BlenderBackend — implements the importer's ImportBackend Protocol."""
from __future__ import annotations

import logging
import time

from opennova_jobs import ImportRequest, ImportResult, ScanResult
from opennova_qt_ui.backend import BackendCapabilities
from pyopennova.scan import scan_definitions

from .dispatcher import ImportDispatcher


_log = logging.getLogger(__name__)


class BlenderBackend:
    """ImportBackend impl that runs imports in a fresh bpy subprocess.

    Lazily constructs an ImportDispatcher with ``max_workers=1``. Each
    ``execute`` call submits one request and blocks on the future, yielding
    Max-equivalent sequential behavior.
    """

    def __init__(self) -> None:
        self._dispatcher: ImportDispatcher | None = None

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="Standalone",
            supports_blend=True,
            supports_max=False,
            supports_parallel=False,
        )

    def scan(self, base_dir: str) -> ScanResult:
        return scan_definitions(base_dir)

    def execute(self, request: ImportRequest) -> ImportResult:
        dispatcher = self._get_dispatcher()
        started = time.monotonic()
        try:
            return dispatcher.submit(request).result()
        except Exception as exc:  # noqa: BLE001 - surface any failure
            _log.error("BlenderBackend.execute failed on %s: %s", request.label, exc, exc_info=True)
            return ImportResult.failure(
                request,
                error=str(exc),
                output_path=request.likely_output_dir,
                elapsed_seconds=time.monotonic() - started,
            )

    def shutdown(self) -> None:
        if self._dispatcher is not None:
            self._dispatcher.close()
            self._dispatcher = None

    def _get_dispatcher(self) -> ImportDispatcher:
        if self._dispatcher is None:
            self._dispatcher = ImportDispatcher(max_workers=1)
        return self._dispatcher
