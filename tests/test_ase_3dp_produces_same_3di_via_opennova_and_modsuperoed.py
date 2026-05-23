"""Head-to-head bake parity: identical generated source through OED and OpenNova."""
from __future__ import annotations

import os
import re
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

_RELAXED_GEOMETRY_PATHS = (
    re.compile(r"^CDTA(?:/|$)"),
    re.compile(r"^RDTA/RLOD\[\d+\]/(?:VERT|INDX|STRP)$"),
)


@pytest.mark.skipif(not sys.platform.startswith("win"), reason="ModSuperOED is Windows-only")
@pytest.mark.parametrize("source_name", FIXTURES, ids=lambda n: Path(n).stem)
def test_same_generated_ase_3dp_bakes_to_matching_3di_in_opennova_and_modsuperoed(
    source_name: str,
    tmp_path: Path,
) -> None:
    from apps import modsuperoed
    from apps.importer.source_generator import generate_source_from_3di
    from blender.opennova.bake_ffi import bake_project_export
    from blender.opennova.three_di_policy_ffi import (
        OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        compare_files,
        validate_geometry_chunks,
    )

    source_3di = STOCK_JO / source_name
    paths_template = modsuperoed.resolve_paths(repo_root=ROOT)
    tool_dir = paths_template.tool_dir
    exe_path = tool_dir / "ModSuperOed.exe"
    required = [source_3di, exe_path, paths_template.injector, paths_template.hook_dll]
    missing = [p for p in required if not p.is_file()]
    if missing:
        raise FileNotFoundError(
            "missing head-to-head bake artifacts: " + ", ".join(str(p) for p in missing)
        )

    stem = source_3di.stem
    generated_dir = tmp_path / "generated" / stem
    oed_output = tmp_path / f"{stem}.modsuperoed.3di"
    opennova_output = tmp_path / f"{stem}.opennova.3di"
    relaxed_report_path = tmp_path / f"{stem}.opennova-vs-oed.relaxed.json"
    strict_report_path = tmp_path / f"{stem}.opennova-vs-oed.strict.json"
    oed_geometry_report_path = tmp_path / f"{stem}.modsuperoed.geometry.json"
    opennova_geometry_report_path = tmp_path / f"{stem}.opennova.geometry.json"

    try:
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
        export_result = modsuperoed.export_3di(
            project_path=generated.project_path,
            output_path=oed_output,
            paths=paths,
            work_dir=tmp_path / "oed-work",
            timeout_s=120,
            output_title=stem,
        )
        assert export_result.returncode == 0
        assert oed_output.is_file()
        assert oed_output.stat().st_size > 0

        bake_project_export(
            generated.project_path,
            opennova_output,
            model_name=stem,
        )
        assert opennova_output.is_file()
        assert opennova_output.stat().st_size > 0

        relaxed_report = compare_files(
            oed_output,
            opennova_output,
            policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
            report_path=relaxed_report_path,
        )
        assert relaxed_report["status"] == 0, _format_policy_report(relaxed_report)

        strict_report = compare_files(
            oed_output,
            opennova_output,
            report_path=strict_report_path,
        )
        _assert_only_relaxed_geometry_differs(strict_report)

        oed_geometry_report = validate_geometry_chunks(
            oed_output,
            report_path=oed_geometry_report_path,
        )
        assert oed_geometry_report["status"] == 0, _format_policy_report(oed_geometry_report)

        opennova_geometry_report = validate_geometry_chunks(
            opennova_output,
            report_path=opennova_geometry_report_path,
        )
        assert opennova_geometry_report["status"] == 0, _format_policy_report(opennova_geometry_report)

        assert "ExitProcess(0)" in export_result.log_path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        _preserve_failure_artifacts(
            stem=stem,
            source_3di=source_3di,
            generated_dir=generated_dir,
            oed_output=oed_output,
            opennova_output=opennova_output,
            oed_work_dir=tmp_path / "oed-work",
            report_paths=(
                relaxed_report_path,
                strict_report_path,
                oed_geometry_report_path,
                opennova_geometry_report_path,
            ),
        )
        raise


def _assert_only_relaxed_geometry_differs(report: dict) -> None:
    if report["status"] == 0:
        return
    failures = [
        event
        for event in report.get("events", [])
        if event.get("status") == "fail"
    ]
    assert failures, _format_policy_report(report)
    outside_relaxed_geometry = [
        event
        for event in failures
        if not _is_relaxed_geometry_path(str(event.get("path", "")))
    ]
    assert not outside_relaxed_geometry, _format_policy_report(
        {"events": outside_relaxed_geometry}
    )


def _is_relaxed_geometry_path(path: str) -> bool:
    return any(pattern.match(path) for pattern in _RELAXED_GEOMETRY_PATHS)


def _preserve_failure_artifacts(
    *,
    stem: str,
    source_3di: Path,
    generated_dir: Path,
    oed_output: Path,
    opennova_output: Path,
    oed_work_dir: Path,
    report_paths: tuple[Path, ...] = (),
) -> None:
    artifact_root = os.environ.get("OPENNOVA_ROUNDTRIP_ARTIFACT_DIR")
    if not artifact_root:
        return

    out_dir = Path(artifact_root) / f"{stem}-opennova-vs-modsuperoed"
    out_dir.mkdir(parents=True, exist_ok=True)
    if source_3di.is_file():
        shutil.copy2(source_3di, out_dir / source_3di.name)
    if generated_dir.is_dir():
        shutil.copytree(generated_dir, out_dir / "generated", dirs_exist_ok=True)
    if oed_output.is_file():
        shutil.copy2(oed_output, out_dir / oed_output.name)
    if opennova_output.is_file():
        shutil.copy2(opennova_output, out_dir / opennova_output.name)
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
