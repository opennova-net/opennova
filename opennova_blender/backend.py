"""Standalone import backends for the DCC-agnostic Qt importer."""
from __future__ import annotations

import logging
import time

from opennova_jobs import ImportOptions, ImportRequest, ImportResult, ScanResult
from opennova_qt_ui.backend import BackendCapabilities


_log = logging.getLogger(__name__)


class BlenderBackend:
    """ImportBackend implementation using the existing bpy subprocess pool."""

    def __init__(self, max_workers: int | None = None) -> None:
        self._max_workers_override = max_workers
        self._dispatcher = None

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="Standalone",
            supports_blend=True,
            supports_max=False,
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

    def shutdown(self) -> None:
        if self._dispatcher is not None:
            self._dispatcher.close()
            self._dispatcher = None

    def _get_dispatcher(self):
        if self._dispatcher is None:
            from apps.importer.dispatcher import ImportDispatcher

            self._dispatcher = ImportDispatcher(max_workers=self._max_workers_override)
        return self._dispatcher


class StandaloneBackend:
    """Composite backend for bundled Blender plus optional external 3ds Max."""

    def __init__(self, blender_backend=None, max_runner=None) -> None:
        self._blender = blender_backend if blender_backend is not None else BlenderBackend()
        if max_runner is None:
            from opennova_max.runner import MaxBatchRunner

            max_runner = MaxBatchRunner()
        self._max_runner = max_runner

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="Standalone",
            supports_blend=True,
            supports_max=(
                bool(getattr(self._max_runner, "available", False))
                and _max_scene_import_available()
            ),
            supports_parallel=True,
        )

    @property
    def max_workers(self) -> int:
        return getattr(self._blender, "max_workers", 1)

    def scan(self, base_dir: str) -> ScanResult:
        return self._blender.scan(base_dir)

    def execute(self, request: ImportRequest) -> ImportResult:
        return next(iter(self.execute_batch([request])))

    def execute_batch(self, requests):
        # type: (list[ImportRequest]) -> list[ImportResult]
        requests = list(requests)
        partial_results = [[] for _request in requests]
        max_jobs = []

        for index, request in enumerate(requests):
            blender_request = _request_for_blender(request)
            if blender_request is not None:
                partial_results[index].append(self._blender.execute(blender_request))

            max_request = _request_for_max(request)
            if max_request is not None:
                max_jobs.append((index, max_request))

        if max_jobs:
            if not bool(getattr(self._max_runner, "available", False)):
                for index, max_request in max_jobs:
                    partial_results[index].append(_missing_max_result(max_request))
            else:
                max_results = self._max_runner.run([max_request for _index, max_request in max_jobs])
                for offset, (index, max_request) in enumerate(max_jobs):
                    if offset < len(max_results):
                        partial_results[index].append(max_results[offset])
                    else:
                        partial_results[index].append(
                            ImportResult.failure(
                                max_request,
                                error="3ds Max batch runner did not return a result.",
                                output_path=max_request.likely_output_dir,
                            )
                        )

        merged = []
        for request, results in zip(requests, partial_results):
            if not results:
                merged.append(
                    ImportResult.failure(
                        request,
                        error="Select at least one file to write.",
                        output_path=request.likely_output_dir,
                    )
                )
            else:
                merged.append(_merge_results(request, results))
        return merged

    def shutdown(self) -> None:
        self._blender.shutdown()


def _request_for_blender(request: ImportRequest):
    # type: (ImportRequest) -> ImportRequest | None
    options = request.options
    owns_ase = options.ase_export_owner() == "blender"
    if not (options.write_blend or options.write_3dp or owns_ase or options.write_glb or options.write_fbx):
        return None
    return _copy_request_with_options(
        request,
        ImportOptions(
            import_animations=options.import_animations,
            import_collisions=options.import_collisions,
            import_occlusion=options.import_occlusion,
            import_lights=options.import_lights,
            import_arms=options.import_arms,
            write_blend=options.write_blend,
            write_3dp=options.write_3dp,
            write_ase=owns_ase,
            write_glb=options.write_glb,
            write_fbx=options.write_fbx,
            write_max=False,
        ),
    )


def _request_for_max(request: ImportRequest):
    # type: (ImportRequest) -> ImportRequest | None
    options = request.options
    owns_ase = options.ase_export_owner() == "max"
    if not (options.write_max or owns_ase):
        return None
    return _copy_request_with_options(
        request,
        ImportOptions(
            import_animations=options.import_animations,
            import_collisions=options.import_collisions,
            import_occlusion=options.import_occlusion,
            import_lights=options.import_lights,
            import_arms=options.import_arms,
            write_blend=False,
            write_3dp=False,
            write_ase=owns_ase,
            write_glb=False,
            write_fbx=False,
            write_max=options.write_max,
        ),
    )


def _copy_request_with_options(request: ImportRequest, options: ImportOptions) -> ImportRequest:
    return ImportRequest(
        mode=request.mode,
        output_root=request.output_root,
        options=options,
        base_dir=request.base_dir,
        item_name=request.item_name,
        item_type=request.item_type,
        threedi_path=request.threedi_path,
        output_stem=request.output_stem,
    )


def _merge_results(request: ImportRequest, results) -> ImportResult:
    written_files = []
    warnings = []
    elapsed_seconds = 0.0
    errors = []
    for result in results:
        written_files.extend(result.written_files)
        warnings.extend(result.warnings)
        elapsed_seconds += result.elapsed_seconds
        if not result.ok:
            errors.append(result.error or result.message)

    if errors:
        return ImportResult.failure(
            request,
            error=" ".join(error for error in errors if error),
            output_path=request.likely_output_dir,
            written_files=written_files,
            warnings=warnings,
            elapsed_seconds=elapsed_seconds,
        )
    return ImportResult.success(
        request,
        output_path=request.likely_output_dir,
        written_files=written_files,
        warnings=warnings,
        elapsed_seconds=elapsed_seconds,
    )


def _missing_max_result(request: ImportRequest) -> ImportResult:
    return ImportResult.failure(
        request,
        error=(
            "3dsmaxbatch.exe was not found. Set OPENNOVA_3DSMAXBATCH or install "
            "Autodesk 3ds Max to write .max output."
        ),
        output_path=request.likely_output_dir,
    )


def _max_scene_import_available() -> bool:
    try:
        from opennova_max.import_runner import SUPPORTS_SCENE_IMPORT
    except Exception:
        return False
    return bool(SUPPORTS_SCENE_IMPORT)
