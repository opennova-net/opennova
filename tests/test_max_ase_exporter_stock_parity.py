"""Stock 3DI -> Max scene -> current-scene ASE parity tests."""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess

import pytest

from pyopennova.legacy_tools.stock_roundtrip import STOCK_THREEDI_FIXTURE_NAMES
from scripts.max_ase_exporter_stock_parity_runner import _ase_texts_match


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "max_ase_exporter_stock_parity_runner.py"


@dataclass(frozen=True)
class MaxAseParityBatchRun:
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


def _clean_process_text(text: str) -> str:
    return text.replace("\x00", "")


def _batch_log(run: MaxAseParityBatchRun) -> str:
    return (
        f"command: {' '.join(run.command)}\n"
        f"returncode: {run.returncode}\n"
        f"summary: {run.summary_path}\n"
        f"stdout:\n{run.stdout}\n"
        f"stderr:\n{run.stderr}\n"
    )


@pytest.fixture(scope="session")
def max_batch_exe() -> Path:
    exe = _find_3dsmaxbatch()
    if exe is None:
        pytest.skip("3dsmaxbatch.exe was not found; set OPENNOVA_3DSMAXBATCH")
    return exe


@pytest.fixture(scope="session")
def max_ase_parity_run(
    max_batch_exe: Path,
    tmp_path_factory: pytest.TempPathFactory,
) -> MaxAseParityBatchRun:
    output_root = tmp_path_factory.mktemp("max_ase_exporter_stock_parity")
    command = [str(max_batch_exe), str(RUNNER)]
    env = os.environ.copy()
    env["OPENNOVA_MAX_ASE_PARITY_OUTPUT_ROOT"] = str(output_root)
    env["OPENNOVA_MAX_ASE_PARITY_FIXTURES"] = ",".join(STOCK_THREEDI_FIXTURE_NAMES)

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

    return MaxAseParityBatchRun(
        command=tuple(command),
        returncode=returncode,
        stdout=stdout,
        stderr=stderr,
        output_root=output_root,
        summary_path=summary_path,
        summary=summary,
        summary_error=summary_error,
    )


def _summary_or_fail(run: MaxAseParityBatchRun) -> dict:
    if run.summary is None:
        pytest.fail(f"Max ASE parity runner did not produce a readable summary: {run.summary_error}\n{_batch_log(run)}")
    return run.summary


def _records_by_fixture(records: object) -> dict[str, dict]:
    if not isinstance(records, list):
        return {}
    return {
        record["fixture"]: record
        for record in records
        if isinstance(record, dict) and isinstance(record.get("fixture"), str)
    }


def test_max_ase_parity_comparator_accepts_tiny_float_drift() -> None:
    expected = '\t*MESH_VERTEXNORMAL 16\t0\t0.998603165\t0.0528360903\n'
    actual = '\t*MESH_VERTEXNORMAL 16\t0\t0.998603225\t0.0528360941\n'

    matches, report = _ase_texts_match(expected, actual)

    assert matches
    assert "numeric drift only" in report


def test_max_ase_parity_comparator_accepts_max_skin_single_weight_rounding() -> None:
    expected = "\t*MESH_WEIGHTSVERTEX 946\t6\t-1\t-1\t-1\t0.99989998\t0.00000000\t0.00000000\t0.00000000\n"
    actual = "\t*MESH_WEIGHTSVERTEX 946\t6\t-1\t-1\t-1\t1.00000000\t0.00000000\t0.00000000\t0.00000000\n"

    matches, report = _ase_texts_match(expected, actual)

    assert matches
    assert "numeric drift only" in report


def test_max_ase_parity_comparator_rejects_structural_differences() -> None:
    expected = '\t*MESH_VERTEXNORMAL 16\t0\t0.998603165\t0.0528360903\n'
    actual = '\t*MESH_VERTEXNORMAL 17\t0\t0.998603225\t0.0528360941\n'

    matches, report = _ase_texts_match(expected, actual)

    assert not matches
    assert "beyond tolerance" in report


@pytest.mark.parametrize("name", STOCK_THREEDI_FIXTURE_NAMES)
def test_stock_3di_imported_max_scene_exports_matching_ase(
    max_ase_parity_run: MaxAseParityBatchRun,
    name: str,
) -> None:
    """The Max current-scene exporter must match direct stock 3DI ASE output."""
    summary = _summary_or_fail(max_ase_parity_run)
    successes = _records_by_fixture(summary.get("successes"))
    failures = _records_by_fixture(summary.get("failures"))

    record = successes.get(name) or failures.get(name)
    if record is None:
        pytest.fail(f"Max ASE parity runner produced no record for {name}\n{_batch_log(max_ase_parity_run)}")

    if name in failures:
        trace_path = Path(str(record["trace"])) if "trace" in record else None
        detail = ""
        if trace_path and trace_path.is_file():
            detail = trace_path.read_text(encoding="utf-8", errors="replace")
        pytest.fail(
            f"Max ASE parity failed for {name}: {record.get('error', 'unknown error')}\n"
            f"{detail}\n{_batch_log(max_ase_parity_run)}"
        )

    direct = Path(str(record["direct"]))
    exported = Path(str(record["exported"]))
    report = Path(str(record["report"]))
    assert direct.is_file()
    assert exported.is_file()
    assert report.is_file()
    matches, comparison = _ase_texts_match(
        direct.read_text(encoding="utf-8", errors="replace"),
        exported.read_text(encoding="utf-8", errors="replace"),
    )
    assert matches, comparison + report.read_text(encoding="utf-8", errors="replace")
