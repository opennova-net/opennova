"""Headless ModSuperOED runner used by acceptance tests."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

from pyopennova import threedi_ffi


EXPECTED_EXE_SIZE = 2_473_984
EXPECTED_EXE_SHA256 = "B3B07D1E4FF77EB7FAFBE4FFC97C182508460EBAA235C2B88F1CC0C64AAA8325"


@dataclass(frozen=True)
class ModSuperOEDPaths:
    tool_dir: Path
    injector: Path
    hook_dll: Path


@dataclass(frozen=True)
class LegacyExportResult:
    output_path: Path
    log_path: Path
    returncode: int
    stdout: str
    stderr: str


def default_paths(repo_root: str | Path | None = None) -> ModSuperOEDPaths:
    root = Path(repo_root) if repo_root is not None else Path(__file__).resolve().parents[2]
    tool_dir = Path(os.environ.get("OPENNOVA_MODSUPEROED_DIR", root / "third_party" / "ModSuperOED"))
    default_injectors = [
        root / "build-legacy-oed" / "tools" / "legacy_oed" / "modsuperoed" / "modsuperoed_injector.exe",
        root / "build" / "tools" / "legacy_oed" / "modsuperoed" / "modsuperoed_injector.exe",
    ]
    if "OPENNOVA_MODSUPEROED_INJECTOR" in os.environ:
        injector = Path(os.environ["OPENNOVA_MODSUPEROED_INJECTOR"])
    else:
        injector = next(
            (path for path in default_injectors if path.exists()),
            default_injectors[0],
        )
    hook_dll = Path(os.environ.get("OPENNOVA_MODSUPEROED_HOOK", injector.with_name("modsuperoed_hook.dll")))
    return ModSuperOEDPaths(tool_dir=tool_dir, injector=injector, hook_dll=hook_dll)


def verify_tool(paths: ModSuperOEDPaths, *, require_hash: bool = True) -> None:
    exe = paths.tool_dir / "ModSuperOed.exe"
    missing = [path for path in (exe, paths.injector, paths.hook_dll) if not path.exists()]
    if missing:
        raise FileNotFoundError("missing legacy tool artifact(s): " + ", ".join(str(p) for p in missing))
    size = exe.stat().st_size
    if size != EXPECTED_EXE_SIZE:
        raise RuntimeError(f"unexpected ModSuperOED size: {size} != {EXPECTED_EXE_SIZE}")
    if require_hash:
        digest = hashlib.sha256(exe.read_bytes()).hexdigest().upper()
        if digest != EXPECTED_EXE_SHA256:
            raise RuntimeError(f"unexpected ModSuperOED sha256: {digest}")


def _hook_log_text(log_path: Path) -> str:
    return log_path.read_text(errors="replace") if log_path.exists() else ""


def _raise_for_hook_errors(log_path: Path) -> None:
    log_text = _hook_log_text(log_path)
    if "HOOK_ERROR" in log_text:
        raise RuntimeError(f"ModSuperOED hook reported an error\nlog:\n{log_text}")


def _assert_clean_hook_exit(log_path: Path) -> None:
    log_text = _hook_log_text(log_path)
    if "ExitProcess(0)" not in log_text:
        raise RuntimeError(f"ModSuperOED hook did not report a clean ExitProcess(0)\nlog:\n{log_text}")


def _legacy_tool_env() -> dict[str, str]:
    env = os.environ.copy()
    for key in tuple(env):
        if key.upper().startswith("UV_"):
            env.pop(key, None)
    return env


def export_3di(
    project_path: str | Path,
    output_path: str | Path,
    *,
    paths: ModSuperOEDPaths | None = None,
    work_dir: str | Path | None = None,
    import_delay_ms: int = 5000,
    export_delay_ms: int = 5000,
    timeout_s: int = 60,
    validate: bool = True,
    output_title: str | None = None,
) -> LegacyExportResult:
    """Run ModSuperOED headlessly and export a `.3di` from a `.3dp` project."""
    paths = paths or default_paths()
    verify_tool(paths)

    project_path = Path(project_path).resolve()
    output_path = Path(output_path).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    run_dir = Path(work_dir) if work_dir is not None else Path(tempfile.mkdtemp(prefix="opennova_oed_"))
    run_dir.mkdir(parents=True, exist_ok=True)
    log_dir = run_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "oed_hook.log"
    cfg_path = run_dir / "injector.cfg"
    cfg_path.write_text(
        "\n".join(
            [
                f"dir={paths.tool_dir}",
                f"proj={project_path}",
                f"out={output_path}",
                *([f"title={output_title}"] if output_title else []),
                f"import_delay_ms={int(import_delay_ms)}",
                f"export_delay_ms={int(export_delay_ms)}",
                f"timeout_ms={int(timeout_s * 1000)}",
                f"log={log_dir}",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    completed = subprocess.run(
        [str(paths.injector), str(cfg_path)],
        cwd=str(run_dir),
        env=_legacy_tool_env(),
        text=True,
        capture_output=True,
        timeout=timeout_s + 10,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"ModSuperOED export failed with exit code {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}\n"
            f"log:\n{_hook_log_text(log_path)}"
        )
    _raise_for_hook_errors(log_path)
    _assert_clean_hook_exit(log_path)
    if not output_path.exists() or output_path.stat().st_size == 0:
        raise RuntimeError(f"ModSuperOED did not produce {output_path}")
    if validate:
        ir = threedi_ffi.read_model_3di3(str(output_path))
        threedi_ffi.free_model_3di3(ir)
    return LegacyExportResult(
        output_path=output_path,
        log_path=log_path,
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
    )
