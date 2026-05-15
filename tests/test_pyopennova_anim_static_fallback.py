"""Focused tests for the .3dp anim_normaltex / anim_diffusetex fallback to
static normaltex / diffusetex when a material has BOTH animated frames
in some slots AND a static texture in another slot.

OED's pipeline (witnessed in IDA against ModSuperOed.exe.i64):
- ImportWorkspace_Parse3daToken @ 0x40927d copies "normaltex[0] <name>" to
  the start of mat->anim_textures (the union'd struct that doubles as
  the static normal[0].path field).
- WriteMTRL @ 0x453470, when rattrib & 0x100 (animated flag) is set,
  branches to AppendTextureSlot @ 0x453300 which iterates anim_normal[0]
  frames. AppendTextureSlot collapses identical-name frames to ONE
  entry; otherwise emits one entry per frame.
- WriteMTRL when not animated reads &slots[i].anim_textures (the same
  storage) verbatim as the slot 3 name.

So if a regen .3dp emits ``rattrib 256`` (animated) plus ``normaltex[0]
"A_Beret.mdt"`` plus ``anim_normaltex[0] N "0"`` (all-zero anim frames),
OED takes the animated path, sees three "0"s, collapses to ONE MTRL
entry literally named "0" -- losing the static name. The fix is to
populate ``anim_normaltex[0]`` with the static name when the material
has a static normal AND animated diffuse frames.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

import pytest

from pyopennova import threedi_ffi
from pyopennova.project_writer import write_3dp_from_3di3


def _read_3dp_lines_for_material(project_path: Path, mat_index: int) -> list[str]:
    txt = project_path.read_text(errors="replace")
    out: list[str] = []
    in_target = False
    for line in txt.split("\n"):
        s = line.strip()
        if s == f"material {mat_index}":
            in_target = True
            continue
        if in_target:
            if s.startswith("material ") and s != f"material {mat_index}":
                break
            out.append(s)
    return out


def test_beret_anim_normaltex_falls_back_to_static_name(tmp_path: Path) -> None:
    """Beret mat[1] has animated diffuse (3 frames: A_BeretG/B/R.tga) plus
    a static normal (A_Beret.mdt). The .3dp must emit
    ``anim_normaltex[0] N "A_Beret.mdt"`` for every animation frame so
    OED's AppendTextureSlot collapses to a single MTRL entry with the
    correct name.
    """
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/Beret/Beret.3di")
    try:
        out = tmp_path / "Beret.3dp"
        write_3dp_from_3di3(ir, str(out))
    finally:
        threedi_ffi.free_model_3di3(ir)

    lines = _read_3dp_lines_for_material(out, 1)
    static_lines = [s for s in lines if s.startswith("normaltex[0]")]
    anim_lines = [s for s in lines if s.startswith("anim_normaltex[0]")]

    assert static_lines, f"no normaltex[0] line in mat[1]; saw: {lines[:20]}"
    assert 'A_Beret.mdt' in static_lines[0], (
        f"static normaltex[0] should be A_Beret.mdt; got: {static_lines[0]!r}"
    )

    assert anim_lines, f"no anim_normaltex[0] lines in mat[1]; saw: {lines[:20]}"
    for line in anim_lines:
        assert 'A_Beret.mdt' in line, (
            f"anim_normaltex[0] should fall back to static name; got: {line!r}"
        )


def test_anim_diffusetex_falls_back_to_static_when_first_frame_empty(
    tmp_path: Path,
) -> None:
    """Mirror case for diffuse: if a material has a static diffuse name
    but anim_diffusetex frames are empty, fill them with the static name.
    Already partially covered by line 752 (which copies the first anim
    frame back to d0_path for the static field), but we also want the
    INVERSE direction handled symmetrically with the normal-slot fix.

    For Beret mat[1] there ARE anim_diffuse frames (A_BeretG/B/R), so
    the static fallback is unused here. Just verify all anim_diffusetex
    lines have non-zero names (the merge from textures[] + animation
    frames already populates them).
    """
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/Beret/Beret.3di")
    try:
        out = tmp_path / "Beret.3dp"
        write_3dp_from_3di3(ir, str(out))
    finally:
        threedi_ffi.free_model_3di3(ir)
    lines = _read_3dp_lines_for_material(out, 1)
    diff_lines = [s for s in lines if s.startswith("anim_diffusetex[0]")]
    assert diff_lines, "no anim_diffusetex[0] lines"
    # Each line: anim_diffusetex[0] <frame> "<name>" <flag>
    expected = ["A_BeretG.tga", "A_BeretB.tga", "A_BeretR.tga"]
    for i, want in enumerate(expected):
        matched = [l for l in diff_lines if f' {i} "' in l]
        assert matched, f"missing frame {i} of anim_diffusetex[0]"
        assert want in matched[0], (
            f"frame {i}: expected {want!r} in {matched[0]!r}"
        )


def test_anim_fallback_keeps_zero_when_no_static_name(tmp_path: Path) -> None:
    """If the material has no static name (n0_path = "0") AND no animated
    frames for that slot, the fill should still emit "0" as before.
    Verified via mat[0] (VS_SKBUMPPHONGOBJ): it has fully-animated normal
    in slot 0 (A_R_mHd1.MDT etc.) -- no static fallback needed and no
    "0" entries.
    """
    ir = threedi_ffi.read_model_3di3("fixtures/stock_3di/Beret/Beret.3di")
    try:
        out = tmp_path / "Beret.3dp"
        write_3dp_from_3di3(ir, str(out))
    finally:
        threedi_ffi.free_model_3di3(ir)
    lines = _read_3dp_lines_for_material(out, 0)
    # mat[0] has no static normaltex[1] and no animated normal[1] frames,
    # so anim_normaltex[1] should be all "0"
    anim_n1 = [s for s in lines if s.startswith("anim_normaltex[1]")]
    assert anim_n1, "expected anim_normaltex[1] lines for mat[0]"
    for line in anim_n1:
        assert ' "0" ' in line, (
            f"anim_normaltex[1] for mat[0] should still be '0' "
            f"(no static fallback); got: {line!r}"
        )
