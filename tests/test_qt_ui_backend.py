from __future__ import annotations

from opennova_jobs import ImportRequest, ImportResult, ScanResult


def test_protocol_accepts_blender_backend_shape() -> None:
    from opennova_blender.backend import BlenderBackend
    from opennova_qt_ui.backend import ImportBackend

    backend = BlenderBackend()
    try:
        assert isinstance(backend, ImportBackend)
    finally:
        backend.shutdown()


def test_capabilities_dataclass_has_expected_fields() -> None:
    from opennova_qt_ui.backend import BackendCapabilities

    caps = BackendCapabilities(
        name="Standalone",
        supports_blend=True,
        supports_parallel=True,
    )
    assert caps.name == "Standalone"
    assert caps.supports_blend is True
    assert caps.supports_parallel is True


class _FakeBackend:
    def capabilities(self):
        from opennova_qt_ui.backend import BackendCapabilities

        return BackendCapabilities(
            name="Fake",
            supports_blend=True,
            supports_parallel=False,
        )

    def scan(self, base_dir: str) -> ScanResult:
        return ScanResult(ok=True, items=[])

    def execute(self, request: ImportRequest) -> ImportResult:
        return ImportResult.success(request)

    def shutdown(self) -> None:
        pass


def test_protocol_accepts_duck_typed_backend() -> None:
    from opennova_qt_ui.backend import ImportBackend

    assert isinstance(_FakeBackend(), ImportBackend)


def test_blender_backend_max_workers_is_lazy(monkeypatch) -> None:
    from opennova_blender.backend import BlenderBackend

    def fail_if_constructed(*_args, **_kwargs):
        raise AssertionError("max_workers should not construct the process pool")

    monkeypatch.setattr("apps.importer.dispatcher.ImportDispatcher", fail_if_constructed)
    monkeypatch.setattr("apps.importer.dispatcher._default_max_workers", lambda: 3)

    backend = BlenderBackend()
    assert backend.max_workers == 3


def test_blender_backend_execute_batch_uses_dispatcher(tmp_path) -> None:
    from opennova_blender.backend import BlenderBackend

    requests = [
        ImportRequest.for_loose(
            threedi_path=str(tmp_path / f"Shed_{index}.3di"),
            output_root=str(tmp_path / "out"),
        )
        for index in range(2)
    ]

    class FakeDispatcher:
        max_workers = 2

        def submit_batch(self, submitted):
            submitted = list(submitted)
            assert submitted == requests
            return iter(ImportResult.success(request) for request in submitted)

        def close(self) -> None:
            pass

    backend = BlenderBackend()
    backend._dispatcher = FakeDispatcher()

    assert [result.ok for result in backend.execute_batch(requests)] == [True, True]
