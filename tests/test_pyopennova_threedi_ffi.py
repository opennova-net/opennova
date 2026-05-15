from __future__ import annotations

from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SAMPLE_3DI3 = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"
SAMPLE_GP = ROOT / "fixtures" / "threedi" / "gp" / "wcrate5.3di"


def test_read_model_3di3_reads_stock_3di3_fixture() -> None:
    from pyopennova.threedi_ffi import free_model_3di3, read_model_3di3

    model = read_model_3di3(str(SAMPLE_3DI3))
    try:
        assert model.version == 259
        assert model.lod_count > 0
        assert model.header.name.rstrip(b"\x00") == b"Shed.3di"
    finally:
        free_model_3di3(model)


def test_read_chunk_tree_3di3_uses_native_reader() -> None:
    from pyopennova.threedi_ffi import read_chunk_tree_3di3

    root = read_chunk_tree_3di3(str(SAMPLE_3DI3))

    assert root.offset == 8
    assert root.is_parent
    assert root.children


def test_decode_transform_3di3_resolves_control_register_name() -> None:
    from pyopennova.threedi_ffi import (
        ThreediControlRegister,
        ThreediCtrl,
        ThreediTransform,
        decode_transform_3di3,
    )

    registers = (ThreediControlRegister * 1)()
    registers[0].name = b"CTRL_TEST"
    ctrl = ThreediCtrl(count=1, record_size=25, registers=registers)
    transform = ThreediTransform(
        control=113,
        control_param=0,
        rate=512,
        start=16384,
        end=-16384,
    )

    decoded = decode_transform_3di3(transform, is_rotation=True, ctrl=ctrl)

    assert decoded.rate == 2.0
    assert decoded.phase == 0.0
    assert decoded.start == 360.0
    assert decoded.end == -360.0
    assert decoded.ctrl_reg_name == b"CTRL_TEST"


def test_read_model_3di3_rejects_gp_magic(tmp_path: Path) -> None:
    from pyopennova.threedi_ffi import read_model_3di3

    gp_path = tmp_path / "old_gp.3di"
    gp_path.write_bytes(b"GPP\x02" + b"\x00" * 64)

    with pytest.raises(RuntimeError, match="Only 3DI3 .3di files are supported"):
        read_model_3di3(str(gp_path))


def test_read_model_auto_reads_legacy_gp_fixture() -> None:
    from pyopennova.threedi_ffi import free_model_3di3, read_model

    model = read_model(str(SAMPLE_GP))
    try:
        assert model.version == 259
        assert model.header.name.rstrip(b"\x00") == b"wcrate5"
        assert model.lod_count > 0
        assert model.material_count > 0
        assert model.lods[0].vertex_count > 0
        assert model.lods[0].index_count > 0
    finally:
        free_model_3di3(model)
