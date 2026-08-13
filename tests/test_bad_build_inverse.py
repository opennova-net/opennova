"""Pure-math inverse identities for pyopennova.bad_build (no native lib needed).

Each export-side helper must invert the corresponding import-side function in
pyopennova.animation_build / pyopennova.coords so a .bad round-trips through the
Blender import/export path.
"""
from __future__ import annotations

from pyopennova import bad_build, coords
from pyopennova.animation_build import (
    bad_channel_to_zup_quat,
    quat_normalize,
    quat_to_matrix,
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


def test_dedupe_quat_sign():
    prev = (0.0, 0.0, 0.0, 1.0)
    assert bad_build.dedupe_quat_sign(prev, (0.0, 0.0, 0.0, -1.0)) == (0.0, 0.0, 0.0, 1.0)
    assert _approx(bad_build.dedupe_quat_sign(prev, (0.1, 0.0, 0.0, 0.99)), (0.1, 0.0, 0.0, 0.99))
