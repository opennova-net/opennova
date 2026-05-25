"""JSON IPC helpers for grouped 3ds Max batch imports."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable, List, Mapping

from opennova_jobs import ImportOptions, ImportRequest, ImportResult


def write_batch_request(path, requests):
    # type: (Path, Iterable[ImportRequest]) -> None
    data = {"requests": [_request_to_mapping(request) for request in requests]}
    _write_json(path, data)


def read_batch_request(path):
    # type: (Path) -> List[ImportRequest]
    data = _read_json(path)
    return [_request_from_mapping(item) for item in data.get("requests", [])]


def write_batch_results(path, results):
    # type: (Path, Iterable[ImportResult]) -> None
    data = {"results": [_result_to_mapping(result) for result in results]}
    _write_json(path, data)


def read_batch_results(path):
    # type: (Path) -> List[ImportResult]
    data = _read_json(path)
    return [_result_from_mapping(item) for item in data.get("results", [])]


def _write_json(path, data):
    # type: (Path, Mapping[str, object]) -> None
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")


def _read_json(path):
    # type: (Path) -> Mapping[str, object]
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _options_to_mapping(options):
    # type: (ImportOptions) -> dict
    return {
        "import_animations": options.import_animations,
        "import_collisions": options.import_collisions,
        "import_occlusion": options.import_occlusion,
        "import_lights": options.import_lights,
        "import_arms": options.import_arms,
        "write_blend": options.write_blend,
        "write_3dp": options.write_3dp,
        "write_ase": options.write_ase,
        "write_max": options.write_max,
    }


def _options_from_mapping(data):
    # type: (Mapping[str, object]) -> ImportOptions
    defaults = ImportOptions()
    values = {}
    for field in _options_to_mapping(defaults):
        values[field] = bool(data.get(field, getattr(defaults, field)))
    return ImportOptions(**values)


def _request_to_mapping(request):
    # type: (ImportRequest) -> dict
    return {
        "mode": request.mode,
        "output_root": request.output_root,
        "base_dir": request.base_dir,
        "item_name": request.item_name,
        "item_type": request.item_type,
        "threedi_path": request.threedi_path,
        "output_stem": request.output_stem,
        "options": _options_to_mapping(request.options),
    }


def _request_from_mapping(data):
    # type: (Mapping[str, object]) -> ImportRequest
    return ImportRequest(
        mode=str(data.get("mode", "")),
        output_root=str(data.get("output_root", "")),
        base_dir=str(data.get("base_dir", "")),
        item_name=str(data.get("item_name", "")),
        item_type=str(data.get("item_type", "")),
        threedi_path=str(data.get("threedi_path", "")),
        output_stem=str(data.get("output_stem", "")),
        options=_options_from_mapping(data.get("options", {})),
    )


def _result_to_mapping(result):
    # type: (ImportResult) -> dict
    return {
        "request": _request_to_mapping(result.request),
        "ok": result.ok,
        "message": result.message,
        "output_path": result.output_path,
        "error": result.error,
        "written_files": list(result.written_files),
        "warnings": list(result.warnings),
        "elapsed_seconds": result.elapsed_seconds,
    }


def _result_from_mapping(data):
    # type: (Mapping[str, object]) -> ImportResult
    return ImportResult(
        request=_request_from_mapping(data.get("request", {})),
        ok=bool(data.get("ok", False)),
        message=str(data.get("message", "")),
        output_path=str(data.get("output_path", "")),
        error=str(data.get("error", "")),
        written_files=list(data.get("written_files", [])),
        warnings=list(data.get("warnings", [])),
        elapsed_seconds=float(data.get("elapsed_seconds", 0.0)),
    )
