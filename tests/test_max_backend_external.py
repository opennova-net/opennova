from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from opennova_jobs import ImportOptions, ImportRequest, ImportResult


def test_resolve_3dsmaxbatch_uses_env_override(monkeypatch, tmp_path: Path) -> None:
    exe = tmp_path / "3dsmaxbatch.exe"
    exe.write_text("", encoding="utf-8")
    monkeypatch.setenv("OPENNOVA_3DSMAXBATCH", str(exe))

    from opennova_max.discovery import resolve_3dsmaxbatch

    assert resolve_3dsmaxbatch() == exe


def test_resolve_3dsmaxbatch_ignores_missing_env_override(monkeypatch, tmp_path: Path) -> None:
    missing = tmp_path / "missing.exe"
    monkeypatch.setenv("OPENNOVA_3DSMAXBATCH", str(missing))

    from opennova_max.discovery import resolve_3dsmaxbatch

    assert resolve_3dsmaxbatch(search_roots=()) is None


def test_batch_json_round_trips_requests(tmp_path: Path) -> None:
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        output_stem="Shed",
        options=ImportOptions(
            write_blend=False,
            write_max=True,
            write_ase=True,
            write_3dp=False,
        ),
    )
    path = tmp_path / "request.json"

    from opennova_max.batch import read_batch_request, write_batch_request

    write_batch_request(path, [request])
    loaded = read_batch_request(path)

    assert len(loaded) == 1
    assert loaded[0].mode == request.mode
    assert loaded[0].threedi_path == request.threedi_path
    assert loaded[0].options.write_max is True
    assert loaded[0].options.write_ase is True
    assert loaded[0].options.write_blend is False


def test_batch_json_round_trips_results(tmp_path: Path) -> None:
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        output_stem="Shed",
        options=ImportOptions(write_blend=False, write_max=True, write_ase=False, write_3dp=False),
    )
    path = tmp_path / "result.json"

    from opennova_max.batch import read_batch_results, write_batch_results

    write_batch_results(
        path,
        [ImportResult.success(request, output_path=str(tmp_path / "out" / "Shed"), written_files=["Shed.max"])],
    )
    loaded = read_batch_results(path)

    assert len(loaded) == 1
    assert loaded[0].ok is True
    assert loaded[0].request.output_stem == "Shed"
    assert loaded[0].written_files == ["Shed.max"]


def test_batch_runner_invokes_3dsmaxbatch_with_env_ipc(monkeypatch, tmp_path: Path) -> None:
    exe = tmp_path / "3dsmaxbatch.exe"
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_blend=False, write_max=True, write_ase=True, write_3dp=False),
    )
    calls = []

    def fake_run(command, **kwargs):
        calls.append((command, kwargs))
        from opennova_max.batch import write_batch_results

        write_batch_results(
            Path(kwargs["env"]["OPENNOVA_MAX_BATCH_RESULT"]),
            [
                ImportResult.success(
                    request,
                    output_path=str(tmp_path / "out" / "Shed"),
                    written_files=[str(tmp_path / "out" / "Shed" / "Shed.max")],
                )
            ],
        )
        return SimpleNamespace(returncode=0, stdout="ok", stderr="")

    monkeypatch.setattr("opennova_max.runner.subprocess.run", fake_run)

    from opennova_max.runner import MaxBatchRunner

    runner = MaxBatchRunner(maxbatch_path=exe, timeout_seconds=3)
    results = runner.run([request])

    assert results[0].ok
    assert calls
    command, kwargs = calls[0]
    assert command[0] == str(exe)
    assert command[1].endswith("batch_entry.py")
    assert "OPENNOVA_MAX_BATCH_REQUEST" in kwargs["env"]
    assert "OPENNOVA_MAX_BATCH_RESULT" in kwargs["env"]
    assert kwargs["timeout"] == 3


def test_batch_runner_scrubs_qt_test_env_before_launch(monkeypatch, tmp_path: Path) -> None:
    exe = tmp_path / "3dsmaxbatch.exe"
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_blend=False, write_max=True, write_ase=True, write_3dp=False),
    )
    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setenv("QT_PLUGIN_PATH", str(tmp_path / "pyside-plugins"))
    calls = []

    def fake_run(command, **kwargs):
        calls.append((command, kwargs))
        from opennova_max.batch import write_batch_results

        write_batch_results(
            Path(kwargs["env"]["OPENNOVA_MAX_BATCH_RESULT"]),
            [ImportResult.success(request, output_path=str(tmp_path / "out" / "Shed"))],
        )
        return SimpleNamespace(returncode=0, stdout="ok", stderr="")

    monkeypatch.setattr("opennova_max.runner.subprocess.run", fake_run)

    from opennova_max.runner import MaxBatchRunner

    runner = MaxBatchRunner(maxbatch_path=exe)
    results = runner.run([request])

    assert results[0].ok
    env = calls[0][1]["env"]
    assert "QT_QPA_PLATFORM" not in env
    assert "QT_PLUGIN_PATH" not in env
    assert "OPENNOVA_MAX_BATCH_REQUEST" in env
    assert "OPENNOVA_MAX_BATCH_RESULT" in env


def test_batch_runner_reports_missing_3dsmaxbatch(tmp_path: Path) -> None:
    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        options=ImportOptions(write_blend=False, write_max=True, write_ase=True, write_3dp=False),
    )

    from opennova_max.runner import MaxBatchRunner

    runner = MaxBatchRunner(resolver=lambda: None)
    results = runner.run([request])

    assert len(results) == 1
    assert not results[0].ok
    assert "3dsmaxbatch.exe was not found" in results[0].error
