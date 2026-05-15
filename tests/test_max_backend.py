"""Tests for opennova_max.backend.MaxBackend.

Verifies the backend translates request/result correctly without needing
a running 3ds Max instance.
"""
from __future__ import annotations

from unittest.mock import patch

from opennova_jobs import ImportOptions, ImportRequest, ImportResult


def _request(tmp_path) -> ImportRequest:
    return ImportRequest.for_definition(
        base_dir=str(tmp_path / "game"),
        item_name="M16A2",
        item_type="weapon",
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_max=True),
    )


def test_capabilities_reports_max_features() -> None:
    from opennova_max.backend import MaxBackend

    caps = MaxBackend().capabilities()
    assert caps.name == "3ds Max"
    assert caps.supports_blend is False
    assert caps.supports_max is True
    assert caps.supports_parallel is False


def test_scan_delegates_to_pyopennova() -> None:
    from opennova_jobs import ScanResult
    from opennova_max.backend import MaxBackend

    sentinel = ScanResult(ok=True, items=[])
    with patch("opennova_max.backend.scan_definitions", return_value=sentinel) as m:
        result = MaxBackend().scan("/some/dir")
    m.assert_called_once_with("/some/dir")
    assert result is sentinel


def test_execute_delegates_to_in_process_runner(tmp_path) -> None:
    from opennova_max import backend as backend_mod
    from opennova_max.backend import MaxBackend

    request = _request(tmp_path)
    canned = ImportResult.success(request, message="ok")

    with patch.object(backend_mod, "execute_import_request", return_value=canned) as m:
        result = MaxBackend().execute(request)

    m.assert_called_once_with(request)
    assert result is canned


def test_execute_surfaces_exceptions_as_failure(tmp_path) -> None:
    from opennova_max import backend as backend_mod
    from opennova_max.backend import MaxBackend

    request = _request(tmp_path)

    with patch.object(backend_mod, "execute_import_request", side_effect=RuntimeError("boom")):
        result = MaxBackend().execute(request)

    assert not result.ok
    assert "boom" in result.error
