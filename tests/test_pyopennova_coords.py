"""Unit tests for pyopennova.coords (host-agnostic axis math)."""
from __future__ import annotations

import math

import pytest

from pyopennova import coords


# ---------------------------------------------------------------------------
# Coordinate-space converters
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "fn, in_xyz, out_xyz",
    [
        (coords.render_space,         (1.0,  2.0,  3.0), (-1.0, -3.0,  2.0)),
        (coords.render_space,         (-4.0, 5.0, -6.0), ( 4.0,  6.0,  5.0)),
        (coords.inverse_render_space, (1.0,  2.0,  3.0), (-1.0,  3.0, -2.0)),
        (coords.bone_space,           (1.0,  2.0,  3.0), ( 1.0, -3.0,  2.0)),
        (coords.collision_space,      (1.0,  2.0,  3.0), ( 2.0, -1.0,  3.0)),
    ],
)
def test_axis_swizzles(fn, in_xyz, out_xyz):
    assert fn(in_xyz) == pytest.approx(out_xyz)


def test_render_space_round_trips_through_inverse():
    src = (1.0, 2.0, 3.0)
    assert coords.inverse_render_space(coords.render_space(src)) == pytest.approx(src)


def test_inputs_accept_lists_and_tuples():
    # Anything sequence-like with [] indexing should work.
    assert coords.bone_space([1.0, 2.0, 3.0]) == coords.bone_space((1.0, 2.0, 3.0))


def test_render_space_returns_tuple_not_input_type():
    out = coords.render_space([1.0, 2.0, 3.0])
    assert isinstance(out, tuple)


# ---------------------------------------------------------------------------
# 3x3 matrix helpers
# ---------------------------------------------------------------------------


def test_conjugate_y_to_z_identity_is_identity():
    identity = (
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    )
    out = coords.conjugate_y_to_z(identity)
    for i in range(3):
        assert out[i] == pytest.approx(identity[i])


def test_conjugate_y_to_z_90deg_y_rotation_becomes_90deg_z_rotation():
    # 90deg about Y in source space (Y-up): (x,y,z) -> (z, y, -x)
    rot_y90 = (
        (0.0, 0.0, 1.0),
        (0.0, 1.0, 0.0),
        (-1.0, 0.0, 0.0),
    )
    # After conjugation S @ R @ S^-1 with S: (x,y,z)->(x,-z,y), the result
    # should rotate about Z (the new up axis).
    out = coords.conjugate_y_to_z(rot_y90)
    # Apply to (1,0,0): expect (0,1,0) for a 90deg z-rotation.
    x, y, z = 1.0, 0.0, 0.0
    rx = out[0][0] * x + out[0][1] * y + out[0][2] * z
    ry = out[1][0] * x + out[1][1] * y + out[1][2] * z
    rz = out[2][0] * x + out[2][1] * y + out[2][2] * z
    assert (rx, ry, rz) == pytest.approx((0.0, 1.0, 0.0), abs=1e-9)


def test_orthonormalize_identity_is_identity():
    identity = (
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    )
    out = coords.orthonormalize(identity)
    for i in range(3):
        for j in range(3):
            assert out[i][j] == pytest.approx(identity[i][j])


def test_orthonormalize_recovers_orthonormal_from_skewed_input():
    # Skewed but linearly independent rows.
    m = (
        (2.0, 0.0, 0.0),
        (1.0, 1.0, 0.0),
        (0.5, 0.5, 1.0),
    )
    out = coords.orthonormalize(m)

    def length(v):
        return math.sqrt(sum(c * c for c in v))

    def dot(a, b):
        return sum(ai * bi for ai, bi in zip(a, b))

    for row in out:
        assert length(row) == pytest.approx(1.0, abs=1e-9)
    assert dot(out[0], out[1]) == pytest.approx(0.0, abs=1e-9)
    assert dot(out[0], out[2]) == pytest.approx(0.0, abs=1e-9)
    assert dot(out[1], out[2]) == pytest.approx(0.0, abs=1e-9)


def test_orthonormalize_zero_row_returns_zero_vector():
    m = (
        (0.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    )
    out = coords.orthonormalize(m)
    assert out[0] == (0.0, 0.0, 0.0)
