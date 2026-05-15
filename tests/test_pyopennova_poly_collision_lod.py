"""Focused unit tests for derive_poly_collision_lod (.3dp inference).

Mirrors OED's ``WriteCDTA @ 0x456050`` (ModSuperOed.exe.i64) which
accumulates ``lod->subobjects[i].vertCount`` across the LOD chosen by
``g_ActiveLod``. ``g_ActiveLod`` is set from the .3dp's
``poly_collision_lod`` field, parsed at
``ImportWorkspace_Parse3daToken @ 0x408cf0`` (specifically the branch
at ``0x408f53`` that handles the ``poly_collision_lod`` token).

The 3DI binary doesn't carry ``poly_collision_lod`` directly, so we
recover it from count evidence: CDTA face count should match the render
LOD triangle count, with vertex count used only as a tie-breaker/fallback.
"""

from __future__ import annotations

import inspect
from pathlib import Path

import pytest

from pyopennova import threedi_ffi
from pyopennova.project_writer import derive_poly_collision_lod


def test_project_writer_exposes_collision_lod_without_bullet_alias() -> None:
    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.project_writer import write_3dp_from_3di3

    assert "collision_lod_index" in inspect.signature(write_3dp_from_3di3).parameters
    assert "bullet_lod_index" not in inspect.signature(write_3dp_from_3di3).parameters
    assert "collision_lod_index" in inspect.signature(write_host_neutral_outputs).parameters
    assert "bullet_lod_index" not in inspect.signature(write_host_neutral_outputs).parameters


def test_derive_picks_highest_matching_lod_for_akcrate() -> None:
    """akcrate has CDTA verts/faces matching both LOD3 and LOD4."""
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/akcrate/akcrate.3di")
    try:
        derived = derive_poly_collision_lod(ir)
    finally:
        threedi_ffi.free_model_3di3(ir)
    assert derived == 4


def test_derive_uses_face_count_for_beret() -> None:
    """Beret's CDTA verts do not match, but CDTA faces match LOD0 tris."""
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/Beret/Beret.3di")
    try:
        derived = derive_poly_collision_lod(ir)
    finally:
        threedi_ffi.free_model_3di3(ir)
    assert derived == 0


def test_derive_uses_face_count_for_armry01() -> None:
    """Armry01's CDTA faces match LOD1 even though vertex counts differ."""
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/Armry01/Armry01.3di")
    try:
        derived = derive_poly_collision_lod(ir)
    finally:
        threedi_ffi.free_model_3di3(ir)
    assert derived == 1


def test_derive_returns_zero_for_single_lod_mp5() -> None:
    """A single render LOD is the only possible poly collision source."""
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/mp5_1st/mp5_1st.3di")
    try:
        derived = derive_poly_collision_lod(ir)
    finally:
        threedi_ffi.free_model_3di3(ir)
    assert derived == 0


def test_derive_returns_minus_one_when_collision_is_empty() -> None:
    """Defensive: degenerate model with empty collision -> -1 (no inference).
    The caller (write_3dp_from_3di3) treats -1 as "leave the existing
    default" and writes ``poly_collision_lod 0``.
    """

    class _StubColl:
        vertex_count = 0

    class _StubLod:
        vertex_count = 100
        strip_count = 0
        strips = None

    class _StubIR:
        lod_count = 1
        lods = [_StubLod()]
        collision = [_StubColl()]

    assert derive_poly_collision_lod(_StubIR()) == -1


def test_write_3dp_uses_derived_value_when_no_override(tmp_path: Path) -> None:
    """End-to-end: write_3dp_from_3di3 without an explicit
    collision_lod_index uses the derived value, surfacing it as
    ``poly_collision_lod 4`` in the .3dp for akcrate (was always 0
    before).
    """
    from pyopennova.project_writer import write_3dp_from_3di3

    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/akcrate/akcrate.3di")
    try:
        out = tmp_path / "akcrate.3dp"
        write_3dp_from_3di3(ir, str(out))
    finally:
        threedi_ffi.free_model_3di3(ir)
    text = out.read_text()
    # The header line should be `    poly_collision_lod 4` per derivation.
    assert "poly_collision_lod 4" in text, (
        f"derived value not surfaced; .3dp header:\n"
        + "\n".join(text.splitlines()[:10])
    )


@pytest.mark.parametrize(
    ("fixture", "expected"),
    [
        ("Armry01", 1),
        ("Beret", 0),
        ("mp5_1st", 0),
    ],
)
def test_write_3dp_uses_face_count_or_single_lod_inference(
    fixture: str,
    expected: int,
    tmp_path: Path,
) -> None:
    from pyopennova.project_writer import write_3dp_from_3di3

    ir = threedi_ffi.read_model_3di3(f"fixtures/stock_3di/{fixture}/{fixture}.3di")
    try:
        out = tmp_path / f"{fixture}.3dp"
        write_3dp_from_3di3(ir, str(out))
    finally:
        threedi_ffi.free_model_3di3(ir)
    text = out.read_text()
    assert f"poly_collision_lod {expected}" in text


def test_write_3dp_explicit_override_wins(tmp_path: Path) -> None:
    """When a caller passes an explicit ``collision_lod_index``, the
    derived value is bypassed.
    """
    from pyopennova.project_writer import write_3dp_from_3di3

    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/akcrate/akcrate.3di")
    try:
        out = tmp_path / "akcrate_override.3dp"
        write_3dp_from_3di3(ir, str(out), collision_lod_index=2)
    finally:
        threedi_ffi.free_model_3di3(ir)
    text = out.read_text()
    assert "poly_collision_lod 2" in text
    assert "poly_collision_lod 4" not in text
