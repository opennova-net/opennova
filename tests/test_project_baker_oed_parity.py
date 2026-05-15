from __future__ import annotations

from pathlib import Path

import pytest


GENERATED_SOURCE_COMPARE_TOLERANCES = {
    "akcrate": {("USRP", "numeric-tolerated")},
    "mp5_1st": {("MTRX", "fp-tolerated")},
    "US01": {("USRP", "numeric-tolerated")},
}


def test_project_baker_exposes_native_project_export_binding() -> None:
    from pyopennova import object_bake_ffi

    assert hasattr(object_bake_ffi, "export_project")


def test_existing_project_fixture_bakes_through_native_project_export(tmp_path: Path) -> None:
    from pyopennova import threedi_ffi
    from pyopennova.legacy_tools.project_baker import bake_jo_project
    from pyopennova.three_di_policy_ffi import validate_geometry_chunks

    project_path = Path("fixtures/3dp/Bird1/Bird1.3dp")
    output_path = tmp_path / "Bird1_opennova.3di"

    bake_jo_project(project_path, output_path, model_name="Bird1")

    ir = threedi_ffi.read_model_3di3(str(output_path))
    threedi_ffi.free_model_3di3(ir)
    geometry_report_path = tmp_path / "Bird1_geometry.json"
    geometry_report = validate_geometry_chunks(output_path, report_path=geometry_report_path)
    assert not geometry_report["failed"], geometry_report_path.read_text(encoding="utf-8")


@pytest.mark.parametrize("name", ("akcrate", "mp5_1st", "US01"))
def test_generated_source_matches_oed_for_non_geometry_chunks(
    tmp_path: Path,
    name: str,
) -> None:
    from pyopennova.legacy_tools import modsuperoed
    from pyopennova.legacy_tools.project_baker import bake_jo_project
    from pyopennova.legacy_tools.stock_roundtrip import (
        discover_stock_3di_fixtures,
        generate_source_from_stock_3di,
    )
    from pyopennova.three_di_policy_ffi import (
        OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        compare_files,
        validate_geometry_chunks,
    )
    from tests.three_di_report_assertions import assert_tight_non_geometry_compare

    fixture = discover_stock_3di_fixtures(names=[name])[0]
    source = generate_source_from_stock_3di(fixture, tmp_path / "source")

    opennova_output = tmp_path / f"{name}_opennova.3di"
    bake_jo_project(source.project_path, opennova_output, model_name=name)

    oed_export = modsuperoed.export_3di(
        source.project_path,
        tmp_path / f"{name}_oed.3di",
        work_dir=tmp_path / "modsuperoed_work",
        output_title=name,
        timeout_s=240,
    )

    report_path = tmp_path / f"{name}_oed_vs_opennova.json"
    report = compare_files(
        oed_export.output_path,
        opennova_output,
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        report_path=report_path,
    )
    assert_tight_non_geometry_compare(
        report,
        report_path,
        allowed_tolerances=GENERATED_SOURCE_COMPARE_TOLERANCES.get(name, set()),
    )
    opennova_geometry_report_path = tmp_path / f"{name}_opennova_geometry.json"
    opennova_geometry_report = validate_geometry_chunks(
        opennova_output,
        report_path=opennova_geometry_report_path,
    )
    oed_geometry_report_path = tmp_path / f"{name}_oed_geometry.json"
    oed_geometry_report = validate_geometry_chunks(
        oed_export.output_path,
        report_path=oed_geometry_report_path,
    )
    assert not opennova_geometry_report["failed"], opennova_geometry_report_path.read_text(encoding="utf-8")
    assert not oed_geometry_report["failed"], oed_geometry_report_path.read_text(encoding="utf-8")
    assert not report["failed"], report_path.read_text(encoding="utf-8")
