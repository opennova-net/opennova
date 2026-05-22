"""Headless ModSuperOED automation runner."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


EXPECTED_EXE_SIZE = 2_473_984
EXPECTED_EXE_SHA256 = "B3B07D1E4FF77EB7FAFBE4FFC97C182508460EBAA235C2B88F1CC0C64AAA8325"


@dataclass(frozen=True)
class ModSuperOEDPaths:
    tool_dir: Path
    injector: Path
    hook_dll: Path


@dataclass(frozen=True)
class ModSuperOEDExportResult:
    output_path: Path
    log_path: Path
    returncode: int
    stdout: str
    stderr: str


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _native_dir(build_dir: Path) -> Path:
    return build_dir / "tools" / "modsuperoed"


def _default_build_dir(root: Path) -> Path:
    candidates = (root / "build-modsuperoed", root / "build")
    for candidate in candidates:
        if (_native_dir(candidate) / "modsuperoed_injector.exe").is_file():
            return candidate
    return candidates[0]


def resolve_paths(
    *,
    tool_dir: str | Path | None = None,
    repo_root: str | Path | None = None,
    build_dir: str | Path | None = None,
) -> ModSuperOEDPaths:
    root = Path(repo_root) if repo_root is not None else _repo_root()
    tool_value = tool_dir if tool_dir is not None else os.environ.get("OPENNOVA_MODSUPEROED_DIR")
    if not tool_value:
        raise RuntimeError("Set OPENNOVA_MODSUPEROED_DIR or pass tool_dir.")

    build_root = Path(build_dir) if build_dir is not None else _default_build_dir(root)
    native_dir = _native_dir(build_root)
    return ModSuperOEDPaths(
        tool_dir=Path(tool_value),
        injector=native_dir / "modsuperoed_injector.exe",
        hook_dll=native_dir / "modsuperoed_hook.dll",
    )


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def verify_tool(paths: ModSuperOEDPaths, *, require_hash: bool = True) -> None:
    exe = paths.tool_dir / "ModSuperOed.exe"
    missing = [path for path in (exe, paths.injector, paths.hook_dll) if not path.is_file()]
    if missing:
        raise FileNotFoundError(
            "missing ModSuperOED automation artifact(s): "
            + ", ".join(str(path) for path in missing)
        )

    size = exe.stat().st_size
    if size != EXPECTED_EXE_SIZE:
        raise RuntimeError(f"unexpected ModSuperOED size: {size} != {EXPECTED_EXE_SIZE}")

    if require_hash:
        digest = _sha256_file(exe)
        if digest != EXPECTED_EXE_SHA256:
            raise RuntimeError(f"unexpected ModSuperOED sha256: {digest}")


def _hook_log_text(log_path: Path) -> str:
    return log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""


def _raise_for_hook_errors(log_path: Path) -> None:
    log_text = _hook_log_text(log_path)
    if "HOOK_ERROR" in log_text:
        raise RuntimeError(f"ModSuperOED hook reported an error\nlog:\n{log_text}")


def _assert_clean_hook_exit(log_path: Path) -> None:
    log_text = _hook_log_text(log_path)
    if "ExitProcess(0)" not in log_text:
        raise RuntimeError(f"ModSuperOED hook did not report a clean ExitProcess(0)\nlog:\n{log_text}")


def export_3di(
    project_path: str | Path,
    output_path: str | Path,
    *,
    paths: ModSuperOEDPaths | None = None,
    tool_dir: str | Path | None = None,
    work_dir: str | Path | None = None,
    import_delay_ms: int = 5000,
    export_delay_ms: int = 5000,
    timeout_s: int = 60,
    output_title: str | None = None,
    require_hash: bool = True,
) -> ModSuperOEDExportResult:
    paths = paths or resolve_paths(tool_dir=tool_dir)
    verify_tool(paths, require_hash=require_hash)

    project = Path(project_path).resolve()
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    run_dir = Path(work_dir) if work_dir is not None else Path(tempfile.mkdtemp(prefix="opennova_oed_"))
    run_dir.mkdir(parents=True, exist_ok=True)
    log_dir = run_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "oed_hook.log"
    cfg_path = run_dir / "injector.cfg"

    cfg_lines = [
        f"dir={paths.tool_dir}",
        f"proj={project}",
        f"out={output}",
    ]
    if output_title:
        cfg_lines.append(f"title={output_title}")
    cfg_lines.extend(
        [
            f"import_delay_ms={int(import_delay_ms)}",
            f"export_delay_ms={int(export_delay_ms)}",
            f"timeout_ms={int(timeout_s * 1000)}",
            f"log={log_dir}",
        ]
    )
    cfg_path.write_text("\n".join(cfg_lines) + "\n", encoding="utf-8")

    output.unlink(missing_ok=True)

    completed = subprocess.run(
        [str(paths.injector), str(cfg_path)],
        cwd=str(run_dir),
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
    if not output.exists() or output.stat().st_size == 0:
        raise RuntimeError(f"ModSuperOED did not produce {output}")

    return ModSuperOEDExportResult(
        output_path=output,
        log_path=log_path,
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
    )
