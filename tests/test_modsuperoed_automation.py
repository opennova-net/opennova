from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def _write_file(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _write_sparse_file(path: Path, size: int) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as file:
        file.seek(size - 1)
        file.write(b"\0")
    return path


def _make_paths(tmp_path: Path):
    from apps import modsuperoed

    tool_dir = tmp_path / "tool"
    build_dir = tmp_path / "build-modsuperoed"
    tool_dir.mkdir()
    native_dir = build_dir / "tools" / "modsuperoed"
    native_dir.mkdir(parents=True)
    return modsuperoed.ModSuperOEDPaths(
        tool_dir=tool_dir,
        injector=native_dir / "modsuperoed_injector.exe",
        hook_dll=native_dir / "modsuperoed_hook.dll",
    )


def _make_tool_artifacts(paths) -> None:
    from apps import modsuperoed

    _write_sparse_file(paths.tool_dir / "ModSuperOed.exe", modsuperoed.EXPECTED_EXE_SIZE)
    _write_file(paths.injector)
    _write_file(paths.hook_dll)


def test_resolve_paths_uses_external_tool_dir_and_build_dir(tmp_path: Path) -> None:
    from apps import modsuperoed

    tool_dir = tmp_path / "external" / "ModSuperOED"
    build_dir = tmp_path / "build-modsuperoed"

    paths = modsuperoed.resolve_paths(
        tool_dir=tool_dir,
        repo_root=tmp_path,
        build_dir=build_dir,
    )

    assert paths.tool_dir == tool_dir
    assert paths.injector == build_dir / "tools" / "modsuperoed" / "modsuperoed_injector.exe"
    assert paths.hook_dll == build_dir / "tools" / "modsuperoed" / "modsuperoed_hook.dll"


def test_resolve_paths_uses_environment_tool_dir(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    tool_dir = tmp_path / "env-tool"
    monkeypatch.setenv("OPENNOVA_MODSUPEROED_DIR", str(tool_dir))

    paths = modsuperoed.resolve_paths(repo_root=tmp_path)

    assert paths.tool_dir == tool_dir


def test_verify_tool_reports_missing_artifacts(tmp_path: Path) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _write_sparse_file(paths.tool_dir / "ModSuperOed.exe", modsuperoed.EXPECTED_EXE_SIZE)

    with pytest.raises(FileNotFoundError) as excinfo:
        modsuperoed.verify_tool(paths)

    message = str(excinfo.value)
    assert "modsuperoed_injector.exe" in message
    assert "modsuperoed_hook.dll" in message


def test_verify_tool_checks_size_before_hash(tmp_path: Path) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _write_file(paths.tool_dir / "ModSuperOed.exe", b"short")
    _write_file(paths.injector)
    _write_file(paths.hook_dll)

    with pytest.raises(RuntimeError, match="unexpected ModSuperOED size"):
        modsuperoed.verify_tool(paths)


def test_verify_tool_can_skip_hash_for_synthetic_artifacts(tmp_path: Path) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)

    modsuperoed.verify_tool(paths, require_hash=False)


def test_export_3di_writes_config_and_returns_clean_result(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = tmp_path / "out" / "CharModel.3di"
    calls: list[tuple[list[str], Path]] = []

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        calls.append((list(args), cfg_path))
        cfg_text = cfg_path.read_text(encoding="utf-8")
        assert f"dir={paths.tool_dir}" in cfg_text
        assert f"proj={project.resolve()}" in cfg_text
        assert f"out={output.resolve()}" in cfg_text
        assert "title=CharModel" in cfg_text
        assert "import_delay_ms=12" in cfg_text
        assert "export_delay_ms=34" in cfg_text
        assert "timeout_ms=56000" in cfg_text
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b"3di")
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "ModSuperOED hook loaded\nExitProcess(0)\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 0, "stdout text", "stderr text")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    result = modsuperoed.export_3di(
        project,
        output,
        paths=paths,
        work_dir=tmp_path / "work",
        import_delay_ms=12,
        export_delay_ms=34,
        timeout_s=56,
        output_title="CharModel",
        require_hash=False,
    )

    assert calls == [([str(paths.injector), str(tmp_path / "work" / "injector.cfg")], tmp_path / "work" / "injector.cfg")]
    assert result.output_path == output.resolve()
    assert result.log_path == tmp_path / "work" / "logs" / "oed_hook.log"
    assert result.returncode == 0
    assert result.stdout == "stdout text"
    assert result.stderr == "stderr text"


def test_export_3di_reports_subprocess_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = tmp_path / "out" / "CharModel.3di"

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "hook log text",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 5, "stdout text", "stderr text")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    with pytest.raises(RuntimeError) as excinfo:
        modsuperoed.export_3di(
            project,
            output,
            paths=paths,
            work_dir=tmp_path / "work",
            require_hash=False,
        )

    message = str(excinfo.value)
    assert "exit code 5" in message
    assert "stdout text" in message
    assert "stderr text" in message
    assert "hook log text" in message


def test_export_3di_rejects_hook_errors(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = tmp_path / "out" / "CharModel.3di"

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b"3di")
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "HOOK_ERROR unused-material prompt detected\nExitProcess(0)\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    with pytest.raises(RuntimeError, match="hook reported an error"):
        modsuperoed.export_3di(
            project,
            output,
            paths=paths,
            work_dir=tmp_path / "work",
            require_hash=False,
        )


def test_export_3di_accepts_dismissed_prompt(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = tmp_path / "out" / "CharModel.3di"

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b"3di")
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "HOOK_DISMISSED unused-material prompt detected; dismissing with IDNO\n"
            "ExitProcess(0)\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    result = modsuperoed.export_3di(
        project,
        output,
        paths=paths,
        work_dir=tmp_path / "work",
        require_hash=False,
    )

    assert result.returncode == 0
    assert output.read_bytes() == b"3di"


def test_export_3di_unlinks_stale_output_before_subprocess(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = _write_file(tmp_path / "out" / "CharModel.3di", b"stale")

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        assert not output.exists(), "stale output should have been unlinked before subprocess"
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "ExitProcess(0)\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    with pytest.raises(RuntimeError, match="did not produce"):
        modsuperoed.export_3di(
            project,
            output,
            paths=paths,
            work_dir=tmp_path / "work",
            require_hash=False,
        )


def test_export_3di_requires_clean_hook_exit(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = tmp_path / "out" / "CharModel.3di"

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b"3di")
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "Export3di returned\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    with pytest.raises(RuntimeError, match="did not report a clean ExitProcess"):
        modsuperoed.export_3di(
            project,
            output,
            paths=paths,
            work_dir=tmp_path / "work",
            require_hash=False,
        )


def test_export_3di_requires_nonempty_output(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from apps import modsuperoed

    paths = _make_paths(tmp_path)
    _make_tool_artifacts(paths)
    project = _write_file(tmp_path / "source" / "CharModel.3dp", b"project")
    output = tmp_path / "out" / "CharModel.3di"

    def fake_run(args, cwd, text, capture_output, timeout, check):
        cfg_path = Path(args[1])
        (cfg_path.parent / "logs" / "oed_hook.log").write_text(
            "ExitProcess(0)\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(modsuperoed.subprocess, "run", fake_run)

    with pytest.raises(RuntimeError, match="did not produce"):
        modsuperoed.export_3di(
            project,
            output,
            paths=paths,
            work_dir=tmp_path / "work",
            require_hash=False,
        )


@pytest.mark.skipif(not sys.platform.startswith("win"), reason="ModSuperOED is Windows-only")
def test_external_modsuperoed_smoke(tmp_path: Path) -> None:
    from apps import modsuperoed
    from blender.opennova.threedi_compare_ffi import compare_3di3_chunks

    tool_dir_env = os.environ.get("OPENNOVA_MODSUPEROED_DIR")
    if not tool_dir_env:
        pytest.skip("OPENNOVA_MODSUPEROED_DIR is not set")

    tool_dir = Path(tool_dir_env)
    paths = modsuperoed.resolve_paths(tool_dir=tool_dir, repo_root=ROOT)
    project = tool_dir / "CharModel.3dp"
    expected = tool_dir / "CharModel.3di"
    missing = [
        path
        for path in (
            tool_dir / "ModSuperOed.exe",
            project,
            expected,
            paths.injector,
            paths.hook_dll,
        )
        if not path.is_file()
    ]
    if missing:
        pytest.skip("ModSuperOED smoke artifacts missing: " + ", ".join(str(path) for path in missing))

    output = tmp_path / "CharModel.3di"
    result = modsuperoed.export_3di(
        project,
        output,
        paths=paths,
        work_dir=tmp_path / "modsuperoed-work",
        timeout_s=120,
        output_title="CharModel",
    )

    assert result.returncode == 0
    assert output.is_file()
    assert output.stat().st_size > 0
    compare_3di3_chunks(expected, output, "GHDR,USRP,INFO,CTRL,MTRL,OCCL,LGHT,MTRX,RDTA,CDTA")
    assert "ExitProcess(0)" in result.log_path.read_text(encoding="utf-8", errors="replace")
