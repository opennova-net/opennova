from __future__ import annotations

from pathlib import Path

import pytest


def test_lw10_source_constant_matches_c_header() -> None:
    from pyopennova import threedi_ffi

    assert threedi_ffi.THREEDI_IR_SOURCE_LW10 == 5


def test_read_lw10_fixture_reports_source_format() -> None:
    from pyopennova import threedi_ffi

    root = Path(__file__).resolve().parents[1]
    fixture = root / "fixtures" / "threedi" / "lw" / "ARBLU.3DI"
    try:
        ir = threedi_ffi.read_model_ir(str(fixture))
    except (FileNotFoundError, OSError) as exc:
        pytest.skip(f"native library unavailable: {exc}")

    try:
        assert ir.source_format == threedi_ffi.THREEDI_IR_SOURCE_LW10
        assert ir.lod_count == 1
        assert ir.material_count == 1
    finally:
        threedi_ffi.free_model_ir(ir)
