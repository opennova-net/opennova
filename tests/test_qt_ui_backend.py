"""Tests for the ImportBackend Protocol surface."""
from __future__ import annotations

from opennova_jobs import ImportRequest, ImportResult, ScanResult


def test_protocol_accepts_blender_backend_shape() -> None:
    from opennova_blender.backend import BlenderBackend
    from opennova_qt_ui.backend import ImportBackend

    backend = BlenderBackend()
    # Protocol checks happen by structure; we use isinstance with
    # runtime_checkable to assert.
    assert isinstance(backend, ImportBackend)


def test_capabilities_dataclass_has_expected_fields() -> None:
    from opennova_qt_ui.backend import BackendCapabilities

    caps = BackendCapabilities(
        name="X",
        supports_blend=True,
        supports_max=False,
        supports_parallel=False,
    )
    assert caps.name == "X"
    assert caps.supports_blend is True
    assert caps.supports_max is False
    assert caps.supports_parallel is False
