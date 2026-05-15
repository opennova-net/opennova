"""Blender DCC roundtrip gate.

For each stock fixture: load 3DI, generate ASE via the Blender bridge, drive
ModSuperOED to re-export 3DI, and compare against the original stock model
with the strict roundtrip tolerance set.

Requires bpy. Skips cleanly if bpy is not installed.
"""
from __future__ import annotations

from pathlib import Path

import pytest


bpy = pytest.importorskip("bpy", reason="bpy is not installed in this environment")


# Local runs skip via bpy importorskip; bpy-equipped sessions validate that
# Blender-generated ASE is accepted by original OED and roundtrips without
# comparator failures.
PHASE_F_FIXTURES = ("akcrate", "Armry01", "Beret", "mp5_1st", "US01")


@pytest.mark.parametrize("name", PHASE_F_FIXTURES)
def test_stock_3di_roundtrips_through_blender_dcc(tmp_path: Path, name: str) -> None:
    """The Blender DCC path must produce OED-compatible ASE and pass parity."""
    from tests.dcc_ase_assertions import assert_oed_render_mesh_names
    from tests.three_di_report_assertions import (
        STOCK_DCC_COMPARE_TOLERANCES,
        assert_tight_non_geometry_compare,
    )
    from pyopennova.legacy_tools.stock_roundtrip import (
        discover_stock_3di_fixtures,
        generate_source_via_blender,
        reference_model_stem,
    )
    from pyopennova.legacy_tools import modsuperoed
    from pyopennova.three_di_policy_ffi import (
        OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        compare_files,
        validate_geometry_chunks,
    )

    fixtures = {f.name: f for f in discover_stock_3di_fixtures([name])}
    fixture = fixtures[name]

    source = generate_source_via_blender(fixture, tmp_path / "source")
    assert_oed_render_mesh_names(source.ase_paths)

    output_path = tmp_path / f"{name}_out.3di"
    export = modsuperoed.export_3di(
        source.project_path,
        output_path,
        work_dir=tmp_path / "modsuperoed_work",
        output_title=reference_model_stem(fixture),
    )

    report_path = tmp_path / f"{name}_blender_dcc_compare.json"
    report = compare_files(
        fixture.reference_3di_path,
        export.output_path,
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        report_path=report_path,
    )
    geometry_report_path = tmp_path / f"{name}_blender_dcc_geometry.json"
    geometry_report = validate_geometry_chunks(
        export.output_path,
        report_path=geometry_report_path,
    )
    assert_tight_non_geometry_compare(
        report,
        report_path,
        allowed_tolerances=STOCK_DCC_COMPARE_TOLERANCES.get(name, set()),
    )

    assert not geometry_report["failed"], geometry_report_path.read_text(encoding="utf-8")
    assert not report["failed"], (
        f"Blender DCC roundtrip failed for {name}: {report_path.read_text(encoding='utf-8')}"
    )
