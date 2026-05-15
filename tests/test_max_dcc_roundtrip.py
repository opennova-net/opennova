"""Max DCC roundtrip gate.

Pytest runs outside 3ds Max, launches 3dsmaxbatch.exe, and asserts the
artifacts produced by the Max-hosted runner. The pymxs module is provided by
3ds Max's bundled Python, so ordinary uv Python must never import pymxs here.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "max_dcc_roundtrip_runner.py"

from pyopennova.legacy_tools.max_dcc_roundtrip import STOCK_FIXTURES


@dataclass(frozen=True)
class MaxDccBatchRun:
    command: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    output_root: Path
    summary_path: Path
    summary: dict | None
    summary_error: str | None = None


def _find_3dsmaxbatch() -> Path | None:
    env_path = os.environ.get("OPENNOVA_3DSMAXBATCH", "").strip().strip('"')
    if env_path:
        candidate = Path(env_path)
        if candidate.is_file():
            return candidate

    candidates: list[Path] = []
    for root in (Path(r"C:\Program Files\Autodesk"), Path(r"C:\Program Files (x86)\Autodesk")):
        if root.is_dir():
            candidates.extend(root.glob("3ds Max */3dsmaxbatch.exe"))

    for candidate in sorted(candidates, reverse=True):
        if candidate.is_file():
            return candidate
    return None


def _batch_log(run: MaxDccBatchRun) -> str:
    return (
        f"command: {' '.join(run.command)}\n"
        f"returncode: {run.returncode}\n"
        f"summary: {run.summary_path}\n"
        f"stdout:\n{run.stdout}\n"
        f"stderr:\n{run.stderr}\n"
    )


def _clean_process_text(text: str) -> str:
    return text.replace("\x00", "")


@pytest.fixture(scope="session")
def max_batch_exe() -> Path:
    exe = _find_3dsmaxbatch()
    if exe is None:
        pytest.skip("3dsmaxbatch.exe was not found; set OPENNOVA_3DSMAXBATCH")
    return exe


@pytest.fixture(scope="session")
def max_dcc_roundtrip_run(
    max_batch_exe: Path,
    tmp_path_factory: pytest.TempPathFactory,
) -> MaxDccBatchRun:
    output_root = tmp_path_factory.mktemp("max_dcc_roundtrip")
    command = [str(max_batch_exe), str(RUNNER)]
    env = os.environ.copy()
    env["OPENNOVA_MAX_DCC_OUTPUT_ROOT"] = str(output_root)
    env["OPENNOVA_MAX_DCC_FIXTURES"] = ",".join(STOCK_FIXTURES)

    try:
        process = subprocess.run(
            command,
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=60 * 30,
        )
        returncode = process.returncode
        stdout = _clean_process_text(process.stdout)
        stderr = _clean_process_text(process.stderr)
    except subprocess.TimeoutExpired as exc:
        returncode = -1
        stdout = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode(errors="replace")
        stderr = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode(errors="replace")
        stdout = _clean_process_text(stdout)
        stderr = _clean_process_text(stderr)
        stderr = f"{stderr}\nTimed out after {exc.timeout} seconds"

    summary_path = output_root / "summary.json"
    summary: dict | None = None
    summary_error: str | None = None
    if summary_path.is_file():
        try:
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            summary_error = f"{type(exc).__name__}: {exc}"
    else:
        summary_error = "summary.json was not produced"

    return MaxDccBatchRun(
        command=tuple(command),
        returncode=returncode,
        stdout=stdout,
        stderr=stderr,
        output_root=output_root,
        summary_path=summary_path,
        summary=summary,
        summary_error=summary_error,
    )


def _summary_or_fail(run: MaxDccBatchRun) -> dict:
    if run.summary is None:
        pytest.fail(f"Max DCC runner did not produce a readable summary: {run.summary_error}\n{_batch_log(run)}")
    return run.summary


def _records_by_fixture(records: object) -> dict[str, dict]:
    if not isinstance(records, list):
        return {}
    return {
        record["fixture"]: record
        for record in records
        if isinstance(record, dict) and isinstance(record.get("fixture"), str)
    }


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


@pytest.mark.parametrize("name", STOCK_FIXTURES)
def test_stock_3di_roundtrips_through_max_dcc(
    max_dcc_roundtrip_run: MaxDccBatchRun,
    name: str,
) -> None:
    """The Max DCC path must produce OED-compatible ASE and pass parity."""
    from tests.dcc_ase_assertions import assert_oed_render_mesh_names
    from tests.three_di_report_assertions import (
        STOCK_DCC_COMPARE_TOLERANCES,
        assert_tight_non_geometry_compare,
    )
    from pyopennova.three_di_policy_ffi import validate_geometry_chunks

    summary = _summary_or_fail(max_dcc_roundtrip_run)
    successes = _records_by_fixture(summary.get("successes"))
    failures = _records_by_fixture(summary.get("failures"))

    record = successes.get(name) or failures.get(name)
    if record is None:
        pytest.fail(f"Max DCC runner produced no record for {name}\n{_batch_log(max_dcc_roundtrip_run)}")

    ase_paths = tuple(Path(path) for path in record.get("ase_paths", ()))
    if ase_paths:
        assert_oed_render_mesh_names(ase_paths)

    if name in failures:
        report_path = Path(str(record["report"])) if "report" in record else None
        trace_path = Path(str(record["trace"])) if "trace" in record else None
        detail = ""
        if report_path and report_path.is_file():
            detail = report_path.read_text(encoding="utf-8", errors="replace")
        elif trace_path and trace_path.is_file():
            detail = trace_path.read_text(encoding="utf-8", errors="replace")
        pytest.fail(
            f"Max DCC roundtrip failed for {name}: {record.get('error', 'unknown error')}\n"
            f"{detail}\n{_batch_log(max_dcc_roundtrip_run)}"
        )

    output_path = Path(str(record["output"]))
    report_path = Path(str(record["report"]))
    assert output_path.is_file()
    assert report_path.is_file()

    geometry_report_path = max_dcc_roundtrip_run.output_root / f"{name}_geometry.json"
    geometry_report = validate_geometry_chunks(
        output_path,
        report_path=geometry_report_path,
    )
    report = _read_json(report_path)
    assert_tight_non_geometry_compare(
        report,
        report_path,
        allowed_tolerances=STOCK_DCC_COMPARE_TOLERANCES.get(name, set()),
    )
    assert not geometry_report["failed"], geometry_report_path.read_text(encoding="utf-8")
    assert not report["failed"], (
        f"Max DCC roundtrip compare failed for {name}: "
        f"{report_path.read_text(encoding='utf-8', errors='replace')}"
    )
