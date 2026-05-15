"""USRP userpoint TM-row emission + OED bubble-sort emit order.

Extracted from ase_from_3di3.py during Phase D migration. Pure functions;
no dependencies on the rest of the ASE writer.

IDA-pinned behavior:
- USRP rotation: WriteUSRP @ 0x452c10 (ModSuperOED) reads axis.m[2][0..2] * 65536.
- ASE parser swizzle: TM_ROW2 emitted as (-y, x, z) of the desired userpoint
  axis. The pre-parser-swizzle row we emit must be (axis[0], -axis[2], axis[1]).
- Emit-order bubble-sort: ConvertToInternal @ 0x4268b3 sorts via a single
  pass with comparator (subObj DESC, type ASC, name ASC). We pre-apply this
  sort so OED's in-place sort lands on the stock USRP order.

Tests in tests/test_pyopennova_userpoint_emission.py pin these.
"""
from __future__ import annotations

import math
from typing import Sequence


def _vec_len_sq(v: Sequence[float]) -> float:
    return float(v[0]) * float(v[0]) + float(v[1]) * float(v[1]) + float(v[2]) * float(v[2])


def _vec_normalize(v: Sequence[float]) -> tuple[float, float, float]:
    length = math.sqrt(_vec_len_sq(v))
    if length <= 1e-12:
        return (0.0, 0.0, 0.0)
    return (float(v[0]) / length, float(v[1]) / length, float(v[2]) / length)


def _vec_cross(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (
        float(a[1]) * float(b[2]) - float(a[2]) * float(b[1]),
        float(a[2]) * float(b[0]) - float(a[0]) * float(b[2]),
        float(a[0]) * float(b[1]) - float(a[1]) * float(b[0]),
    )


def _vec_dot(a: Sequence[float], b: Sequence[float]) -> float:
    return float(a[0]) * float(b[0]) + float(a[1]) * float(b[1]) + float(a[2]) * float(b[2])


def _userpoint_tm(
    translation: Sequence[float],
    direction: Sequence[float],
) -> tuple[tuple[float, float, float], ...]:
    """Build a 4-row .ase TM for a userpoint object whose ROW2 inverts
    the ase_parser swizzle ``(-y, x, z)`` and OED's USRP rot write
    (``WriteUSRP @ 0x452c10`` reads ``axis.m[2][0..2]``).

    The reader-side inverse (the 3DI3 reader
    line 308) maps ``ir.direction = (USRP.rot_y, USRP.rot_z, USRP.rot_x)``,
    so the only TM_ROW2 that survives the round-trip is::

        TM_ROW2 = (direction[0], -direction[2], direction[1])

    ROW0 / ROW1 are filled with an arbitrary orthonormal basis (OED
    only consumes ROW2 for USRP) so the .ase parser still validates
    the matrix.
    """
    z_axis = (
        float(direction[0]),
        -float(direction[2]),
        float(direction[1]),
    )
    z_axis = _vec_normalize(z_axis)
    if _vec_len_sq(z_axis) <= 1e-12:
        z_axis = (0.0, 0.0, 1.0)
    world_up = (0.0, 0.0, 1.0)
    if abs(_vec_dot(z_axis, world_up)) > 0.999:
        world_up = (0.0, 1.0, 0.0)
    x_axis = _vec_normalize(_vec_cross(world_up, z_axis))
    y_axis = _vec_normalize(_vec_cross(z_axis, x_axis))
    return (
        x_axis,
        y_axis,
        z_axis,
        (float(translation[0]), float(translation[1]), float(translation[2])),
    )


def _userpoint_emit_order(ir_userpoints: Sequence[dict]) -> list[int]:
    """Return the permutation of ``ir_userpoints`` indices to emit into
    the .ase such that OED's bubble-sort
    (``ConvertToInternal @ 0x4268b3``) produces the 3DI3 model (= stock USRP)
    order.

    OED's sort applies once to the .ase userpoint iteration order:

        for i in 0..n-1: for j in i+1..n:
          swap if  subObj_i < subObj_j  ||  type_i > type_j
                || stricmp(name_i, name_j) > 0

    Strategy: pre-apply OED's sort to the 3DI3 model userpoint order ourselves
    and emit the .ase in that result. When OED runs its sort on our
    pre-sorted input it either:
      - converges immediately (transitive case; most fixtures): the
        emit order IS the 3DI3 model order and OED leaves it alone.
      - completes a 2-cycle (Beret-type N=2 with type+name conflict):
        applying OED's sort once gives the reverse of the stock order; OED applies
        it again, lands back on the original order.

    O(n^2) and deterministic. For pathological inputs that don't form
    a 1- or 2-cycle (rare; would need >2 conflicting criteria across
    >2 elements), the emit order may diverge from stock by some swap;
    accepted as regen-loss in those cases.
    """
    n = len(ir_userpoints)
    if n <= 1:
        return list(range(n))

    idx = list(range(n))
    arr = [ir_userpoints[p] for p in idx]
    for i in range(n - 1):
        for j in range(i + 1, n):
            a, b = arr[i], arr[j]
            cond = (
                int(a["subobj"]) < int(b["subobj"])
                or int(a["type"]) > int(b["type"])
                or str(a["name"]).casefold() > str(b["name"]).casefold()
            )
            if cond:
                arr[i], arr[j] = arr[j], arr[i]
                idx[i], idx[j] = idx[j], idx[i]
    return idx
