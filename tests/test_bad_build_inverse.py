"""Pure-math inverse identities for pyopennova.bad_build (no native lib needed).

Each export-side helper must invert the corresponding import-side function in
pyopennova.animation_build / pyopennova.coords so a .bad round-trips through the
DCC import->export path.
"""
from __future__ import annotations

from pyopennova import bad_build, coords
from pyopennova.animation_build import (
    bad_channel_to_zup_quat,
    mat_mul,
    quat_normalize,
    quat_to_matrix,
    quat_xyzw_to_matrix_rows,
)
from pyopennova.bad_ffi import BadQuaternion


def _approx(a, b, eps=1e-5):
    return all(abs(float(x) - float(y)) <= eps for x, y in zip(a, b))


def _approx_rows(a, b, eps=1e-5):
    return all(_approx(a[r], b[r], eps) for r in range(3))


def _norm_xyzw(xyzw):
    w, x, y, z = quat_normalize((xyzw[3], xyzw[0], xyzw[1], xyzw[2]))
    return (x, y, z, w)


_QUATS = [
    (0.0, 0.0, 0.0, 1.0),
    (0.1, 0.2, 0.3, 0.9),
    (0.4, 0.0, 0.0, 0.9),
    (0.2, -0.3, 0.1, 0.92),
    (-0.3, 0.3, -0.3, 0.85),
]


def test_inverse_bone_space():
    for v in [(1.0, 2.0, 3.0), (-4.0, 5.0, -6.0), (0.0, 0.0, 0.0), (7.0, -8.0, 9.0)]:
        assert _approx(bad_build.inverse_bone_space(coords.bone_space(v)), v)


def test_zup_quat_to_bad_xyzw_inverse():
    for xyzw in _QUATS:
        n = _norm_xyzw(xyzw)
        rq = BadQuaternion(n[0], n[1], n[2], n[3])
        zup = bad_channel_to_zup_quat(rq)
        assert _approx(bad_build.zup_quat_to_bad_xyzw(zup), n)


def test_inverse_conjugate_y_to_z():
    for q in _QUATS:
        R = quat_to_matrix(quat_normalize((q[3], q[0], q[1], q[2])))
        m = coords.conjugate_y_to_z(R)
        assert _approx_rows(bad_build.inverse_conjugate_y_to_z(m), R)


def _max_forward_rows(q_xyzw):
    return mat_mul(
        mat_mul(bad_build._MAX_GLOBAL_MATRIX_ROWS, quat_xyzw_to_matrix_rows(q_xyzw)),
        bad_build._MAX_GLOBAL_CORRECTION_ROWS,
    )


def test_bad_rows_from_max_world_rows_inverse():
    for q in _QUATS:
        n = _norm_xyzw(q)
        bad_rows = bad_build.bad_rows_from_max_world_rows(_max_forward_rows(n))
        assert _approx_rows(bad_rows, quat_xyzw_to_matrix_rows(n))


def test_bad_xyzw_from_max_world_rows_inverse():
    for q in _QUATS:
        n = _norm_xyzw(q)
        back = bad_build.bad_xyzw_from_max_world_rows(_max_forward_rows(n))
        dot = sum(back[i] * n[i] for i in range(4))
        assert abs(abs(dot) - 1.0) <= 1e-4  # sign-agnostic


def test_dedupe_quat_sign():
    prev = (0.0, 0.0, 0.0, 1.0)
    assert bad_build.dedupe_quat_sign(prev, (0.0, 0.0, 0.0, -1.0)) == (0.0, 0.0, 0.0, 1.0)
    assert _approx(bad_build.dedupe_quat_sign(prev, (0.1, 0.0, 0.0, 0.99)), (0.1, 0.0, 0.0, 0.99))


def test_max_constants_match_import_side():
    try:
        from opennova_max import animation as max_anim
    except Exception as exc:  # pragma: no cover - opennova_max optional in CI
        import pytest

        pytest.skip(f"opennova_max not importable: {exc}")
    assert bad_build._MAX_GLOBAL_MATRIX_ROWS == max_anim._MAX_GLOBAL_MATRIX_ROWS
    assert bad_build._MAX_GLOBAL_CORRECTION_ROWS == max_anim._MAX_GLOBAL_CORRECTION_ROWS
