"""Max DCC source generation plus original-OED roundtrip harness."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from pyopennova.legacy_tools import modsuperoed
from pyopennova.legacy_tools.stock_roundtrip import (
    GeneratedStockSource,
    STOCK_THREEDI_FIXTURE_NAMES,
    StockThreediFixture,
    discover_stock_3di_fixtures,
    generate_source_via_max,
    reference_model_stem,
)
from pyopennova.three_di_policy_ffi import (
    OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    compare_files,
)


STOCK_FIXTURES: tuple[str, ...] = STOCK_THREEDI_FIXTURE_NAMES


@dataclass(frozen=True)
class MaxDccRoundtripResult:
    fixture: StockThreediFixture
    source: GeneratedStockSource
    modsuperoed_output: Path
    log_path: Path
    report: dict
    report_path: Path


def run_fixture(
    name: str,
    output_dir: str | Path,
    *,
    timeout_s: int = 240,
) -> MaxDccRoundtripResult:
    """Run one stock fixture through Max ASE generation and original OED."""
    fixture = discover_stock_3di_fixtures(names=[name])[0]
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    source = generate_source_via_max(fixture, out_dir / "source")
    output_title = reference_model_stem(fixture)
    output_path = out_dir / f"{output_title}.3di"
    export = modsuperoed.export_3di(
        source.project_path,
        output_path,
        work_dir=out_dir / "modsuperoed_work",
        output_title=output_title,
        timeout_s=timeout_s,
    )

    report_path = out_dir / f"{name}_max_dcc_compare.json"
    report = compare_files(
        fixture.reference_3di_path,
        export.output_path,
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        report_path=report_path,
    )
    return MaxDccRoundtripResult(
        fixture=fixture,
        source=source,
        modsuperoed_output=export.output_path,
        log_path=export.log_path,
        report=report,
        report_path=report_path,
    )


def run_all(
    output_root: str | Path,
    *,
    names: Iterable[str] = STOCK_FIXTURES,
    timeout_s: int = 240,
) -> list[MaxDccRoundtripResult]:
    root = Path(output_root)
    root.mkdir(parents=True, exist_ok=True)
    return [
        run_fixture(name, root / name, timeout_s=timeout_s)
        for name in names
    ]
