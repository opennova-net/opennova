"""Run Max DCC roundtrip fixtures under 3dsmaxbatch.exe.

This script intentionally does not depend on pytest inside Max's bundled
Python. It exits nonzero when any fixture fails and writes tracebacks under
the output directory.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import traceback
from pathlib import Path


WORKTREE = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT_ROOT = WORKTREE / "build-max-dcc"


def _emit(line: str) -> None:
    print(line, flush=True)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-root",
        default=os.environ.get("OPENNOVA_MAX_DCC_OUTPUT_ROOT", str(DEFAULT_OUTPUT_ROOT)),
        help="Directory for Max DCC outputs and reports.",
    )
    parser.add_argument(
        "--fixture",
        action="append",
        dest="fixtures",
        help="Fixture name to run. May be provided multiple times.",
    )
    parser.add_argument(
        "--timeout-s",
        type=int,
        default=int(os.environ.get("OPENNOVA_MAX_DCC_TIMEOUT_S", "240")),
        help="ModSuperOED export timeout per fixture.",
    )
    return parser.parse_args()


def _env_fixtures() -> tuple[str, ...]:
    value = os.environ.get("OPENNOVA_MAX_DCC_FIXTURES", "")
    return tuple(part.strip() for part in value.split(",") if part.strip())


def _result_record(result: object) -> dict[str, object]:
    return {
        "fixture": result.fixture.name,
        "source_dir": str(result.source.directory),
        "project": str(result.source.project_path),
        "ase_paths": [str(path) for path in result.source.ase_paths],
        "output": str(result.modsuperoed_output),
        "log": str(result.log_path),
        "report": str(result.report_path),
    }


def main() -> int:
    args = _parse_args()
    sys.path.insert(0, str(WORKTREE))

    import pyopennova.legacy_tools.max_dcc_roundtrip as max_dcc_roundtrip

    output_root = Path(args.output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    trace_dir = output_root / "traces"
    trace_dir.mkdir(parents=True, exist_ok=True)

    fixtures = tuple(args.fixtures or _env_fixtures() or max_dcc_roundtrip.STOCK_FIXTURES)
    _emit("OPENNOVA_MAX_DCC_START")
    _emit(f"OPENNOVA_MAX_DCC_OUTPUT_ROOT {output_root}")
    _emit(f"OPENNOVA_MAX_DCC_FIXTURES {','.join(fixtures)}")

    failures: list[dict[str, object]] = []
    successes: list[dict[str, object]] = []
    for name in fixtures:
        _emit(f"OPENNOVA_MAX_DCC_FIXTURE_START {name}")
        try:
            result = max_dcc_roundtrip.run_fixture(
                name,
                output_root / name,
                timeout_s=args.timeout_s,
            )
            record = _result_record(result)
            if result.report.get("failed"):
                failures.append(
                    {
                        **record,
                        "fixture": name,
                        "error": "native compare failed",
                        "report": str(result.report_path),
                    }
                )
                _emit(
                    "OPENNOVA_MAX_DCC_FIXTURE_FAIL "
                    f"{name} report={result.report_path}"
                )
                continue
            successes.append(record)
            _emit(
                "OPENNOVA_MAX_DCC_FIXTURE_OK "
                f"{name} output={result.modsuperoed_output} report={result.report_path}"
            )
        except BaseException as exc:
            trace_path = trace_dir / f"{name}_trace.txt"
            trace_path.write_text(
                "".join(traceback.format_exception(type(exc), exc, exc.__traceback__)),
                encoding="utf-8",
            )
            failures.append(
                {
                    "fixture": name,
                    "error": f"{type(exc).__name__}: {exc}",
                    "trace": str(trace_path),
                }
            )
            _emit(
                "OPENNOVA_MAX_DCC_FIXTURE_FAIL "
                f"{name} trace={trace_path}: {type(exc).__name__}: {exc}"
            )

    summary_path = output_root / "summary.json"
    summary_path.write_text(
        json.dumps(
            {
                "failed": bool(failures),
                "fixtures": fixtures,
                "successes": successes,
                "failures": failures,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    _emit(f"OPENNOVA_MAX_DCC_SUMMARY {summary_path}")
    _emit(f"OPENNOVA_MAX_DCC_RESULT {not failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
