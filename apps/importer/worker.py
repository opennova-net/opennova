"""Worker entry point for subprocess-isolated imports.

Each worker process is born, handles exactly one ``ImportRequest``, and
exits. The pool replaces it with a fresh process for the next request, so
``bpy`` state cannot accumulate across imports - the entire class of
"second-import" crashes the previous in-process design fought is gone by
construction.

Called as the target of ``concurrent.futures.ProcessPoolExecutor.submit``.
The pool's ``initializer`` wires this worker's logging into a
``multiprocessing.Queue`` provided by the parent so log records flow back to
the main process's existing handlers.
"""
from __future__ import annotations

import logging
import logging.handlers
import multiprocessing as mp
import time
from pathlib import Path

from .jobs import (
    IMPORT_MODE_DEF,
    IMPORT_MODE_LOOSE,
    ImportRequest,
    ImportResult,
    validate_import_request,
)


_log = logging.getLogger(__name__)


def init_worker(log_queue: "mp.Queue") -> None:
    """Pool initializer: route this worker's logs to the parent's queue."""
    root = logging.getLogger()
    for handler in list(root.handlers):
        root.removeHandler(handler)
    root.addHandler(logging.handlers.QueueHandler(log_queue))
    root.setLevel(logging.DEBUG)


def run_one(request: ImportRequest) -> ImportResult:
    """Run a single import in this fresh subprocess and return the result."""
    started_at = time.monotonic()
    errors = validate_import_request(request)
    if errors:
        return ImportResult.failure(
            request,
            error=" ".join(errors),
            elapsed_seconds=time.monotonic() - started_at,
        )

    output_path = request.likely_output_dir
    try:
        if _can_run_loose_source_without_blender(request):
            from .source_generator import generate_source_from_3di

            output_path = request.loose_output_dir
            generate_source_from_3di(
                request.threedi_path,
                output_path,
                output_stem=request.display_name,
                import_collisions=request.options.import_collisions,
                import_occlusion=request.options.import_occlusion,
                import_lights=request.options.import_lights,
                write_3dp=request.options.write_3dp,
                write_ase=request.options.write_ase,
            )
            ok = True
        else:
            from . import bpy_session
            bpy_session.init_headless()

            if request.mode == IMPORT_MODE_DEF:
                from .import_runner import resolve_definition_output_stem, run_import
                output_stem = request.output_stem or resolve_definition_output_stem(
                    request.base_dir,
                    request.item_name,
                    request.item_type,
                )
                if output_stem:
                    output_path = str(Path(request.output_root) / output_stem)
                ok = run_import(
                    base_dir=request.base_dir,
                    item_name=request.item_name,
                    item_type=request.item_type,
                    output_dir=request.output_root,
                    **request.options.as_def_kwargs(),
                )
            elif request.mode == IMPORT_MODE_LOOSE:
                from .import_runner import run_loose_import
                output_path = request.loose_output_dir
                ok = run_loose_import(
                    threedi_path=request.threedi_path,
                    output_dir=output_path,
                    output_stem=request.display_name,
                    asset_base_dir=request.base_dir or None,
                    reset_scene=False,
                    **request.options.as_loose_kwargs(),
                )
            else:
                return ImportResult.failure(
                    request,
                    error=f"Unknown import mode: {request.mode}",
                    output_path=output_path,
                    elapsed_seconds=time.monotonic() - started_at,
                )
    except Exception as exc:
        _log.error("worker failed on %s: %s", request.label, exc, exc_info=True)
        return ImportResult.failure(
            request,
            error=str(exc),
            output_path=output_path,
            elapsed_seconds=time.monotonic() - started_at,
        )

    if ok:
        return ImportResult.success(
            request,
            message=f"Imported {request.label}.",
            output_path=output_path,
            written_files=_written_files(output_path),
            elapsed_seconds=time.monotonic() - started_at,
        )
    return ImportResult.failure(
        request,
        error="Importer did not produce a scene. Check onimport.log for the importer-side error.",
        output_path=output_path,
        written_files=_written_files(output_path),
        elapsed_seconds=time.monotonic() - started_at,
    )


def _written_files(output_path: str) -> list[str]:
    if not output_path:
        return []
    path = Path(output_path)
    if not path.is_dir():
        return []
    return sorted(str(child) for child in path.iterdir() if child.is_file())


def _can_run_loose_source_without_blender(request: ImportRequest) -> bool:
    options = request.options
    return (
        request.mode == IMPORT_MODE_LOOSE
        and not options.write_blend
        and not options.write_glb
        and not options.write_fbx
        and (options.write_3dp or options.write_ase)
    )
