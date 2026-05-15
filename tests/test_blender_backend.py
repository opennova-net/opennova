"""Tests for opennova_blender.backend.BlenderBackend.

Uses a patched ImportDispatcher so we never spawn a real subprocess.
"""
from __future__ import annotations

from concurrent.futures import Future
from unittest.mock import MagicMock, patch

import pytest

from opennova_jobs import ImportOptions, ImportRequest, ImportResult


def _make_request(tmp_path) -> ImportRequest:
    return ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_blend=True),
    )


def test_capabilities_reports_blender_features() -> None:
    from opennova_blender.backend import BlenderBackend

    backend = BlenderBackend()
    caps = backend.capabilities()

    assert caps.name == "Standalone"
    assert caps.supports_blend is True
    assert caps.supports_max is False
    assert caps.supports_parallel is False


def test_scan_delegates_to_pyopennova() -> None:
    from opennova_blender.backend import BlenderBackend
    from opennova_jobs import ScanResult

    sentinel = ScanResult(ok=True, items=[])
    with patch("opennova_blender.backend.scan_definitions", return_value=sentinel) as m:
        backend = BlenderBackend()
        result = backend.scan("/some/dir")

    m.assert_called_once_with("/some/dir")
    assert result is sentinel


def test_execute_submits_to_dispatcher_and_blocks(tmp_path) -> None:
    from opennova_blender import backend as backend_mod
    from opennova_blender.backend import BlenderBackend

    request = _make_request(tmp_path)
    canned = ImportResult.success(request, message="ok")

    fake_future: Future[ImportResult] = Future()
    fake_future.set_result(canned)
    fake_dispatcher = MagicMock()
    fake_dispatcher.submit.return_value = fake_future

    with patch.object(backend_mod, "ImportDispatcher", return_value=fake_dispatcher):
        backend = BlenderBackend()
        result = backend.execute(request)

    assert result is canned
    fake_dispatcher.submit.assert_called_once_with(request)


def test_execute_surfaces_subprocess_crash_as_failure(tmp_path) -> None:
    from opennova_blender import backend as backend_mod
    from opennova_blender.backend import BlenderBackend

    request = _make_request(tmp_path)
    fake_future: Future[ImportResult] = Future()
    fake_future.set_exception(RuntimeError("worker crashed"))

    fake_dispatcher = MagicMock()
    fake_dispatcher.submit.return_value = fake_future

    with patch.object(backend_mod, "ImportDispatcher", return_value=fake_dispatcher):
        backend = BlenderBackend()
        result = backend.execute(request)

    assert not result.ok
    assert "worker crashed" in result.error


def test_shutdown_closes_dispatcher() -> None:
    from opennova_blender import backend as backend_mod
    from opennova_blender.backend import BlenderBackend

    fake_dispatcher = MagicMock()
    with patch.object(backend_mod, "ImportDispatcher", return_value=fake_dispatcher):
        backend = BlenderBackend()
        backend._dispatcher = fake_dispatcher  # force-init for test
        backend.shutdown()

    fake_dispatcher.close.assert_called_once()
