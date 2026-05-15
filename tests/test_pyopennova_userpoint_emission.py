"""Focused unit tests for the .ase userpoint emission used by ThreediAseWriter.

OED's USRP write path (witnessed in IDA):
- ConvertToInternal @ 0x4265cc reads each ``aseObj->tm_row`` (set by
  ase_parser's swizzle ``(-y, x, z)`` on .ase TM_ROW0..3) into
  ``lod->userPoints[i].axis``.
- WriteUSRP @ 0x452c10 writes ``axis.m[2][0..2] * 65536`` to USRP's
  rot_x/y/z fields.
- ConvertToInternal @ 0x4268b3 bubble-sorts userpoints in a single pass
  (i, j) where the swap condition is::

      v56 = subObj_i < subObj_j
         || type_i > type_j
         || stricmp(name_i, name_j) > 0

  This produces an OED stable order of (subObj DESC, type ASC, name
  ASC). Note IDA's inline comment is misleading on subObj/type
  directions.

The reader-side inverse (the 3DI3 reader:308):
  ir.direction[0] = USRP.rot_y
  ir.direction[1] = USRP.rot_z
  ir.direction[2] = USRP.rot_x

So to round-trip stock's USRP rot via our regen:
  desired USRP rot = (ir.direction[2], ir.direction[0], ir.direction[1])
  required ASE TM_ROW2 (pre-parser-swizzle) = (USRP.rot_y, -USRP.rot_x,
                                               USRP.rot_z)
                                          = (ir.direction[0],
                                             -ir.direction[2],
                                             ir.direction[1])
"""

from __future__ import annotations

import pytest

from pyopennova.userpoint_emit import (
    _userpoint_tm,
    _userpoint_emit_order,
)


def test_userpoint_tm_row2_inverts_parser_swizzle_and_oed_usrp_write() -> None:
    """For each direction d, emitted TM_ROW2 must equal the normalized
    ``(d[0], -d[2], d[1])`` so that:
      - ASE parser swizzle (-y, x, z) yields tm_row[2] = (d[2], d[0], d[1])
      - OED's ConvertToInternal NormalizeVec3 (line 807) keeps it unit
      - WriteUSRP @ 0x452c10 writes USRP rot = (d[2], d[0], d[1])
      - the 3DI3 reader loader recovers
        ir.direction = (d[0], d[1], d[2])
    """
    import math

    def _normalize(v):
        m = math.sqrt(sum(c * c for c in v))
        return tuple(c / m for c in v) if m > 1e-12 else (0.0, 0.0, 0.0)

    cases = [
        (0.0, 0.0, 1.0),                  # Beret LOOK
        (0.0, 1.0, 0.0),                  # Beret NVG
        (-0.0707, -0.8593, -0.5065),      # mp5_1st bcasing
    ]
    for direction in cases:
        expected_row2 = _normalize((direction[0], -direction[2], direction[1]))
        tm = _userpoint_tm((0.0, 0.0, 0.0), direction)
        for axis in range(3):
            assert tm[2][axis] == pytest.approx(expected_row2[axis], abs=1e-6), (
                f"direction={direction}: TM_ROW2[{axis}] expected "
                f"{expected_row2[axis]}, got {tm[2][axis]}"
            )


def test_userpoint_tm_rows0_and_1_are_orthonormal_to_row2() -> None:
    """Other rows must be a valid orthonormal basis (so the .ase parser
    doesn't reject the matrix). Specific values aren't asserted -- OED
    only reads ROW2 for USRP -- but length-1 + perpendicular constraints
    keep the matrix well-formed.
    """
    direction = (-0.0707, -0.8593, -0.5065)
    tm = _userpoint_tm((1.0, 2.0, 3.0), direction)
    rows = [tm[0], tm[1], tm[2]]
    for r in rows:
        length = (r[0] ** 2 + r[1] ** 2 + r[2] ** 2) ** 0.5
        assert length == pytest.approx(1.0, abs=1e-5), f"row {r} not unit-length"
    for i in range(3):
        for j in range(i + 1, 3):
            d = rows[i][0] * rows[j][0] + rows[i][1] * rows[j][1] + rows[i][2] * rows[j][2]
            assert abs(d) < 1e-5, f"rows {i} and {j} not orthogonal (dot={d})"
    assert tm[3] == (1.0, 2.0, 3.0)


def test_userpoint_emit_order_pre_applies_oed_sort_for_beret() -> None:
    """Beret has 3DI3 userpoints [LOOK (S), NVG (G)] with the same subObj.
    OED-sort applied to that source order swaps (type LOOK > NVG triggers
    the comparator) -> [NVG, LOOK]. Emitting in [NVG, LOOK] order, OED
    applies its sort once at bake time and lands back on [LOOK, NVG]
    via the stricmp swap (a 2-cycle of OED's non-transitive sort). The
    result matches the 3DI3 model / stock USRP order.
    """
    ir_userpoints = [
        {"subobj": 14, "type": ord("S"), "name": "LOOK"},
        {"subobj": 14, "type": ord("G"), "name": "NVG"},
    ]
    emit_order = _userpoint_emit_order(ir_userpoints)
    assert emit_order == [1, 0], (
        f"expected [1, 0] (= OED-sort applied to source order), got {emit_order}"
    )


def test_userpoint_emit_order_identity_for_mp5_1st() -> None:
    """mp5_1st has 3DI3 userpoints [bcasing, bullet, MFLASH01] all type=S
    same subObj. OED's bubble sort on this exact order produces no
    swaps (transitive case), so emit_order is the identity.
    """
    ir_userpoints = [
        {"subobj": 37, "type": ord("S"), "name": "bcasing"},
        {"subobj": 37, "type": ord("S"), "name": "bullet"},
        {"subobj": 37, "type": ord("S"), "name": "MFLASH01"},
    ]
    emit_order = _userpoint_emit_order(ir_userpoints)
    assert emit_order == [0, 1, 2], f"expected identity, got {emit_order}"


def test_userpoint_emit_order_returns_valid_permutation_for_degenerate_input() -> None:
    """Degenerate same-everything case still returns a valid permutation
    (no crash, no duplicate or missing index).
    """
    ir_userpoints = [
        {"subobj": 0, "type": ord("S"), "name": "x"},
        {"subobj": 0, "type": ord("S"), "name": "x"},
    ]
    emit_order = _userpoint_emit_order(ir_userpoints)
    assert sorted(emit_order) == [0, 1]


def test_userpoint_emit_order_handles_large_n_quickly() -> None:
    """Real fixtures may have many userpoints (Dblkhwk1: 15). The
    algorithm must be O(n^2), not O(n!) brute force.
    """
    import time
    ir_userpoints = [
        {"subobj": i % 5, "type": ord("S") if i % 2 else ord("A"),
         "name": f"point_{i:03d}"}
        for i in range(40)
    ]
    t0 = time.time()
    emit_order = _userpoint_emit_order(ir_userpoints)
    elapsed = time.time() - t0
    assert sorted(emit_order) == list(range(40))
    assert elapsed < 0.1, f"O(n!) regression: {elapsed:.2f}s for 40 points"
