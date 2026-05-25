from __future__ import annotations

from opennova_jobs import ImportOptions, ImportRequest, ImportResult, ScanResult


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
        supports_max=False,
        supports_parallel=True,
    )
    assert caps.name == "Standalone"
    assert caps.supports_blend is True
    assert caps.supports_max is False
    assert caps.supports_parallel is True


class _FakeBackend:
    def capabilities(self):
        from opennova_qt_ui.backend import BackendCapabilities

        return BackendCapabilities(
            name="Fake",
            supports_blend=True,
            supports_max=True,
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


class _RecordingBlenderBackend:
    def __init__(self) -> None:
        self.requests = []
        self.closed = False

    def capabilities(self):
        from opennova_qt_ui.backend import BackendCapabilities

        return BackendCapabilities(
            name="Blender",
            supports_blend=True,
            supports_max=False,
            supports_parallel=True,
        )

    def scan(self, base_dir: str) -> ScanResult:
        return ScanResult(ok=True, items=[])

    def execute(self, request: ImportRequest) -> ImportResult:
        self.requests.append(request)
        return ImportResult.success(request, output_path=request.likely_output_dir)

    def shutdown(self) -> None:
        self.closed = True


class _RecordingMaxRunner:
    available = True

    def __init__(self) -> None:
        self.requests = []
        self.run_calls = []

    def run(self, requests):
        requests = list(requests)
        self.run_calls.append(requests)
        self.requests.extend(requests)
        return [
            ImportResult.success(request, output_path=request.likely_output_dir)
            for request in requests
        ]


def test_standalone_backend_splits_max_outputs_and_prefers_blender_for_ase(tmp_path) -> None:
    from opennova_blender.backend import StandaloneBackend

    blender = _RecordingBlenderBackend()
    max_runner = _RecordingMaxRunner()
    backend = StandaloneBackend(blender_backend=blender, max_runner=max_runner)
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_blend=True, write_3dp=True, write_ase=True, write_max=True),
    )

    result = backend.execute(request)

    assert result.ok
    assert len(blender.requests) == 1
    assert len(max_runner.requests) == 1
    blender_options = blender.requests[0].options
    max_options = max_runner.requests[0].options
    assert blender_options.write_blend
    assert blender_options.write_3dp
    assert blender_options.write_ase
    assert not blender_options.write_max
    assert not max_options.write_blend
    assert not max_options.write_3dp
    assert not max_options.write_ase
    assert max_options.write_max


def test_standalone_backend_lets_max_own_ase_when_no_blend(tmp_path) -> None:
    from opennova_blender.backend import StandaloneBackend

    blender = _RecordingBlenderBackend()
    max_runner = _RecordingMaxRunner()
    backend = StandaloneBackend(blender_backend=blender, max_runner=max_runner)
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_blend=False, write_3dp=False, write_ase=True, write_max=True),
    )

    result = backend.execute(request)

    assert result.ok
    assert blender.requests == []
    assert len(max_runner.requests) == 1
    max_options = max_runner.requests[0].options
    assert max_options.write_ase
    assert max_options.write_max


def test_standalone_backend_groups_max_batch_requests(tmp_path) -> None:
    from opennova_blender.backend import StandaloneBackend

    blender = _RecordingBlenderBackend()
    max_runner = _RecordingMaxRunner()
    backend = StandaloneBackend(blender_backend=blender, max_runner=max_runner)
    requests = [
        ImportRequest.for_loose(
            threedi_path=str(tmp_path / f"Shed_{index}.3di"),
            output_root=str(tmp_path / "out"),
            output_stem=f"Shed_{index}",
            options=ImportOptions(write_blend=False, write_3dp=False, write_ase=True, write_max=True),
        )
        for index in range(2)
    ]

    results = backend.execute_batch(requests)

    assert [result.ok for result in results] == [True, True]
    assert len(max_runner.run_calls) == 1
    assert len(max_runner.run_calls[0]) == 2


def test_standalone_backend_advertises_max_when_runner_and_import_are_available() -> None:
    from opennova_blender.backend import StandaloneBackend

    backend = StandaloneBackend(
        blender_backend=_RecordingBlenderBackend(),
        max_runner=_RecordingMaxRunner(),
    )
    assert backend.capabilities().supports_max is True


def test_standalone_backend_hides_max_when_runner_is_unavailable() -> None:
    from opennova_blender.backend import StandaloneBackend

    class MissingMaxRunner(_RecordingMaxRunner):
        available = False

    backend = StandaloneBackend(
        blender_backend=_RecordingBlenderBackend(),
        max_runner=MissingMaxRunner(),
    )

    assert backend.capabilities().supports_max is False
