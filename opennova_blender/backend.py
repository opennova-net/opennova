"""Blender-backed importer backend for the standalone Qt application."""
from __future__ import annotations

import logging
import time
from collections.abc import Iterable, Iterator

from opennova_jobs import ImportRequest, ImportResult, ScanResult
from opennova_qt_ui.backend import BackendCapabilities


_log = logging.getLogger(__name__)


class BlenderBackend:
    """Run imports in isolated bpy worker processes."""

    def __init__(self, max_workers: int | None = None) -> None:
        self._max_workers_override = max_workers
        self._dispatcher = None

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="Standalone",
            supports_blend=True,
            supports_parallel=True,
        )

    @property
    def max_workers(self) -> int:
        if self._dispatcher is not None:
            return self._dispatcher.max_workers
        if self._max_workers_override is not None:
            return self._max_workers_override
        from apps.importer.dispatcher import _default_max_workers

        return _default_max_workers()

    def scan(self, base_dir: str) -> ScanResult:
        from apps.importer.import_runner import scan_directory_result

        return scan_directory_result(base_dir)

    def execute(self, request: ImportRequest) -> ImportResult:
        started = time.monotonic()
        try:
            return self._get_dispatcher().submit(request).result()
        except Exception as exc:  # noqa: BLE001 - surface any failure
            _log.error("BlenderBackend.execute failed on %s: %s", request.label, exc, exc_info=True)
            return ImportResult.failure(
                request,
                error=str(exc),
                output_path=request.likely_output_dir,
                elapsed_seconds=time.monotonic() - started,
            )

    def execute_batch(
        self,
        requests: Iterable[ImportRequest],
    ) -> Iterator[ImportResult]:
        return self._get_dispatcher().submit_batch(requests)

    def shutdown(self) -> None:
        if self._dispatcher is not None:
            self._dispatcher.close()
            self._dispatcher = None

    def _get_dispatcher(self):
        if self._dispatcher is None:
            from apps.importer.dispatcher import ImportDispatcher

            self._dispatcher = ImportDispatcher(max_workers=self._max_workers_override)
        return self._dispatcher
