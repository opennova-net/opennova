"""Reverse roundtrip: stock .3di → our .3dp/.ase → ModSuperOED → .3di matches source.

The forward smoke (test_modsuperoed_automation.py::test_external_modsuperoed_smoke)
validates the OED automation hook by running the tracked CharModel.3dp/.ase through
ModSuperOED and checking the output matches CharModel.3di. This file is the reverse:
take a canonical .3di, regenerate the project files with our native writer, push them back
through OED, and assert the resulting .3di matches the source.

It exercises the canonical Threedi3di3 reader, the native ASE writer, and the
3dp/3da writer without loading Blender.

Add a fixture by dropping a stock .3di into fixtures/threedi/stock_jo/ and appending
its filename to FIXTURES.
"""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
STOCK_JO = ROOT / "fixtures" / "threedi" / "stock_jo"

FIXTURES: list[str] = [
    "wtrfall.3di",
    "Wcrate5.3di",
    "Lstrng1.3di",
    "Cbunker1.3di",
    "Armry01.3di",
    "DT801.3di",
]


@pytest.mark.skipif(not sys.platform.startswith("win"), reason="ModSuperOED is Windows-only")
@pytest.mark.parametrize("source_name", FIXTURES, ids=lambda n: Path(n).stem)
def test_ase_3dp_produces_correct_3di_via_original_oed_tool(
    source_name: str,
    tmp_path: Path,
) -> None:
    from apps import modsuperoed
    from apps.importer.source_generator import generate_source_from_3di
    from blender.opennova.three_di_policy_ffi import (
        OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        compare_files,
        validate_geometry_chunks,
    )

    tool_dir_env = os.environ.get("OPENNOVA_MODSUPEROED_DIR")
    if not tool_dir_env:
        pytest.skip("OPENNOVA_MODSUPEROED_DIR is not set")
    tool_dir = Path(tool_dir_env)

    source_3di = STOCK_JO / source_name
    paths_template = modsuperoed.resolve_paths(tool_dir=tool_dir, repo_root=ROOT)
    exe_path = tool_dir / "ModSuperOed.exe"
    required = [source_3di, exe_path, paths_template.injector, paths_template.hook_dll]
    missing = [p for p in required if not p.is_file()]
    if missing:
        pytest.skip("missing roundtrip artifacts: " + ", ".join(str(p) for p in missing))

    stem = source_3di.stem

    generated_root = tmp_path / "generated"
    generated_dir = generated_root / stem
    generated = generate_source_from_3di(
        source_3di,
        generated_dir,
        output_stem=stem,
        write_3dp=True,
        write_ase=True,
    )
    assert generated.project_path.is_file()
    assert generated.ase_paths
    assert generated.ase_paths[0].is_file()

    paths = modsuperoed.ModSuperOEDPaths(
        tool_dir=generated_dir,
        injector=paths_template.injector,
        hook_dll=paths_template.hook_dll,
        exe=exe_path,
    )
    regenerated = tmp_path / f"{stem}.regenerated.3di"
    compare_report_path = tmp_path / f"{stem}.compare.json"
    geometry_report_path = tmp_path / f"{stem}.geometry.json"
    try:
        export_result = modsuperoed.export_3di(
            project_path=generated_dir / f"{stem}.3dp",
            output_path=regenerated,
            paths=paths,
            work_dir=tmp_path / "oed-work",
            timeout_s=120,
            output_title=stem,
        )

        assert export_result.returncode == 0
        assert regenerated.is_file()
        assert regenerated.stat().st_size > 0
        compare_report = compare_files(
            source_3di,
            regenerated,
            policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
            report_path=compare_report_path,
        )
        assert compare_report["status"] == 0, _format_policy_report(compare_report)
        geometry_report = validate_geometry_chunks(
            regenerated,
            report_path=geometry_report_path,
        )
        assert geometry_report["status"] == 0, _format_policy_report(geometry_report)
        assert "ExitProcess(0)" in export_result.log_path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        _preserve_failure_artifacts(
            stem=stem,
            source_3di=source_3di,
            generated_dir=generated_dir,
            regenerated=regenerated,
            oed_work_dir=tmp_path / "oed-work",
            report_paths=(compare_report_path, geometry_report_path),
        )
        raise


def _preserve_failure_artifacts(
    *,
    stem: str,
    source_3di: Path,
    generated_dir: Path,
    regenerated: Path,
    oed_work_dir: Path,
    report_paths: tuple[Path, ...] = (),
) -> None:
    artifact_root = os.environ.get("OPENNOVA_ROUNDTRIP_ARTIFACT_DIR")
    if not artifact_root:
        return

    out_dir = Path(artifact_root) / stem
    out_dir.mkdir(parents=True, exist_ok=True)
    if source_3di.is_file():
        shutil.copy2(source_3di, out_dir / source_3di.name)
    if generated_dir.is_dir():
        shutil.copytree(generated_dir, out_dir / "generated", dirs_exist_ok=True)
    if regenerated.is_file():
        shutil.copy2(regenerated, out_dir / regenerated.name)
    for report_path in report_paths:
        if report_path.is_file():
            shutil.copy2(report_path, out_dir / report_path.name)
    if oed_work_dir.is_dir():
        shutil.copytree(oed_work_dir, out_dir / "oed-work", dirs_exist_ok=True)


def _format_policy_report(report: dict) -> str:
    return "\n".join(
        f"{event.get('path')}: {event.get('status')} - {event.get('reason')}"
        for event in report.get("events", [])
    )
