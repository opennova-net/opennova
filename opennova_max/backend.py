"""MaxBackend — runs imports in-process inside 3ds Max."""
from __future__ import annotations

import logging
import time

from opennova_jobs import ImportRequest, ImportResult, ScanResult
from opennova_qt_ui.backend import BackendCapabilities
from pyopennova.scan import scan_definitions

from .import_runner import execute_import_request


_log = logging.getLogger(__name__)


class MaxBackend:
    """ImportBackend impl that runs in-process inside 3ds Max.

    No subprocess pool, no process isolation. Each request is executed
    synchronously via opennova_max.import_runner.execute_import_request.
    """

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="3ds Max",
            supports_blend=False,
            supports_max=True,
            supports_parallel=False,
        )

    def scan(self, base_dir: str) -> ScanResult:
        return scan_definitions(base_dir)

    def execute(self, request: ImportRequest) -> ImportResult:
        started = time.monotonic()
        try:
            return execute_import_request(request)
        except Exception as exc:  # noqa: BLE001 - surface any failure
            _log.error("MaxBackend.execute failed on %s: %s", request.label, exc, exc_info=True)
            return ImportResult.failure(
                request,
                error=str(exc),
                output_path=request.likely_output_dir,
                elapsed_seconds=time.monotonic() - started,
            )

    def shutdown(self) -> None:
        pass
