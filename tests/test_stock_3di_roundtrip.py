from __future__ import annotations

from pathlib import Path

import pytest

from pyopennova.legacy_tools.stock_roundtrip import (
    STOCK_THREEDI_FIXTURE_NAMES,
    discover_stock_3di_fixtures,
    generate_source_from_stock_3di,
    roundtrip_stock_3di_with_modsuperoed,
    write_stock_roundtrip_report,
)


def test_modsuperoed_runner_strips_uv_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    from pyopennova.legacy_tools.modsuperoed import _legacy_tool_env

    monkeypatch.setenv("UV_CACHE_DIR", "C:/tmp/uv-cache")
    monkeypatch.setenv("UV_PROJECT_ENVIRONMENT", "C:/tmp/env")

    env = _legacy_tool_env()

    assert "UV_CACHE_DIR" not in env
    assert "UV_PROJECT_ENVIRONMENT" not in env


def test_stock_fixture_catalog_is_complete() -> None:
    assert STOCK_THREEDI_FIXTURE_NAMES == ("akcrate", "Armry01", "Beret", "mp5_1st", "US01")
    fixtures = discover_stock_3di_fixtures()
    assert tuple(fixture.name for fixture in fixtures) == STOCK_THREEDI_FIXTURE_NAMES
    for fixture in fixtures:
        assert fixture.reference_3di_path.is_file()


def test_generate_source_from_stock_3di_writes_project_and_scenes(tmp_path: Path) -> None:
    fixture = discover_stock_3di_fixtures(names=["akcrate"])[0]

    source = generate_source_from_stock_3di(fixture, tmp_path / "source")

    assert source.project_path.is_file()
    assert source.ase_paths
    for ase_path in source.ase_paths:
        assert ase_path.is_file()


def test_generate_source_from_stock_3di_preserves_control_registers(tmp_path: Path) -> None:
    fixture = discover_stock_3di_fixtures(names=["Armry01", "Beret"])

    armory_source = generate_source_from_stock_3di(fixture[0], tmp_path / "armory")
    beret_source = generate_source_from_stock_3di(fixture[1], tmp_path / "beret")

    armory_text = armory_source.project_path.read_text(encoding="utf-8", errors="replace")
    beret_text = beret_source.project_path.read_text(encoding="utf-8", errors="replace")
    assert "FLICKER" in armory_text
    assert "TEX_CAMO1" in beret_text
    assert "TEX_CAMO2" in beret_text


def test_generate_source_from_stock_3di_preserves_material_rattrib_flags(tmp_path: Path) -> None:
    fixture = discover_stock_3di_fixtures(names=["Armry01"])[0]

    source = generate_source_from_stock_3di(fixture, tmp_path / "armory")

    text = source.project_path.read_text(encoding="utf-8", errors="replace")
    mat9 = text.split("    material 9", 1)[1].split("    material 10", 1)[0]
    assert "rattrib          1" in mat9


@pytest.mark.parametrize("name", STOCK_THREEDI_FIXTURE_NAMES)
def test_stock_3di_roundtrips_through_modsuperoed(name: str, tmp_path: Path) -> None:
    from tests.three_di_report_assertions import (
        STOCK_DCC_COMPARE_TOLERANCES,
        assert_tight_non_geometry_compare,
    )
    from pyopennova.three_di_policy_ffi import validate_geometry_chunks

    fixture = discover_stock_3di_fixtures(names=[name])[0]

    result = roundtrip_stock_3di_with_modsuperoed(
        fixture,
        tmp_path / name,
        timeout_s=240,
    )

    write_stock_roundtrip_report(result, tmp_path / f"{name}_roundtrip.json")
    geometry_report_path = tmp_path / f"{name}_geometry.json"
    geometry_report = validate_geometry_chunks(
        result.modsuperoed_output,
        report_path=geometry_report_path,
    )
    assert_tight_non_geometry_compare(
        result.report,
        result.report_path,
        allowed_tolerances=STOCK_DCC_COMPARE_TOLERANCES.get(name, set()),
    )
    assert not geometry_report["failed"], geometry_report_path.read_text(encoding="utf-8")
    assert not result.report["failed"], result.report_path.read_text(encoding="utf-8")
