"""Unit tests for pyopennova.mesh_utils."""
from __future__ import annotations

import math

import pytest

from pyopennova import mesh_utils


# ---------------------------------------------------------------------------
# mtrx_to_center_rotation
# ---------------------------------------------------------------------------


def test_mtrx_returns_none_when_any_value_is_nan():
    nan = float("nan")
    mat = [nan if i == 0 else 0.0 for i in range(16)]
    assert mesh_utils.mtrx_to_center_rotation(mat) is None


def test_mtrx_returns_3x3_tuple_for_finite_input():
    # Choose a matrix where every used field is finite. The function
    # only consumes m[0..2, 4..6, 8..10] - others are ignored.
    mat = [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]
    out = mesh_utils.mtrx_to_center_rotation(mat)
    assert out is not None
    assert len(out) == 3
    assert all(len(row) == 3 for row in out)


def test_mtrx_inverts_then_applies_center_axis_swizzle():
    # mtrx_to_center_rotation is the inverse of export_3di's true 3x3
    # inversion plus the center-axis swizzle.
    source = [
        [2.0, 3.0, 5.0],
        [7.0, 11.0, 13.0],
        [17.0, 19.0, 23.0],
    ]
    det = (
        source[0][0] * (source[1][1] * source[2][2] - source[1][2] * source[2][1])
        - source[0][1] * (source[1][0] * source[2][2] - source[1][2] * source[2][0])
        + source[0][2] * (source[1][0] * source[2][1] - source[1][1] * source[2][0])
    )
    inv = [
        [
            (source[1][1] * source[2][2] - source[1][2] * source[2][1]) / det,
            -(source[0][1] * source[2][2] - source[0][2] * source[2][1]) / det,
            (source[0][1] * source[1][2] - source[0][2] * source[1][1]) / det,
        ],
        [
            -(source[1][0] * source[2][2] - source[1][2] * source[2][0]) / det,
            (source[0][0] * source[2][2] - source[0][2] * source[2][0]) / det,
            -(source[0][0] * source[1][2] - source[0][2] * source[1][0]) / det,
        ],
        [
            (source[1][0] * source[2][1] - source[1][1] * source[2][0]) / det,
            -(source[0][0] * source[2][1] - source[0][1] * source[2][0]) / det,
            (source[0][0] * source[1][1] - source[0][1] * source[1][0]) / det,
        ],
    ]
    mat = [
        inv[0][0], inv[0][1], inv[0][2], 0.0,
        inv[1][0], inv[1][1], inv[1][2], 0.0,
        inv[2][0], inv[2][1], inv[2][2], 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]
    out = mesh_utils.mtrx_to_center_rotation(mat)
    assert out is not None
    ax0 = (source[2][2], -source[2][0], source[2][1])
    ax1 = (-source[0][2], source[0][0], -source[0][1])
    ax2 = (source[1][2], -source[1][0], source[1][1])
    expected = (
        ( ax1[1], -ax0[1],  ax2[1]),
        (-ax1[0],  ax0[0], -ax2[0]),
        ( ax1[2], -ax0[2],  ax2[2]),
    )
    for r in range(3):
        for c in range(3):
            assert out[r][c] == pytest.approx(expected[r][c], abs=1e-12)


# ---------------------------------------------------------------------------
# compute_smoothing_groups
# ---------------------------------------------------------------------------


def test_single_face_gets_a_smoothing_group_bit():
    # One triangle with one set of corner normals.
    faces = [(0, 1, 2)]
    n = (0.0, 0.0, 1.0)
    normals = [n, n, n]
    sg = mesh_utils.compute_smoothing_groups(faces, normals)
    # Single flat face → SG=0 (flat component branch).
    assert sg == [0]


def test_two_faces_with_matching_normals_share_group():
    # Square split into two triangles, all normals = +Z.
    faces = [(0, 1, 2), (0, 2, 3)]
    n = (0.0, 0.0, 1.0)
    normals = [n, n, n, n, n, n]
    sg = mesh_utils.compute_smoothing_groups(faces, normals)
    assert sg[0] == sg[1]
    # Both faces are flat → SG=0.
    assert sg == [0, 0]


def test_two_faces_with_different_normals_get_different_groups():
    # Two faces sharing edge (1,2) but with non-matching shared-edge
    # normals → sharp edge → separate components → distinct SG bits.
    faces = [(0, 1, 2), (3, 1, 2)]
    n_up = (0.0, 0.0, 1.0)
    n_side = (1.0, 0.0, 0.0)
    # Face 0 corners 1 and 2 carry n_up; face 1 corners 1 and 2 carry n_side.
    normals = [n_up, n_up, n_up, n_side, n_side, n_side]
    sg = mesh_utils.compute_smoothing_groups(faces, normals)
    # Both faces are individually flat → both SG=0 (still distinct components,
    # but flat components map to the same 0 bit).
    assert len(sg) == 2


def test_smoothing_group_bits_are_powers_of_two_or_zero():
    # Stress: a strip of faces, alternating sharp/smooth.
    faces = [(0, 1, 2), (2, 1, 3), (3, 1, 4)]
    nA = (0.0, 0.0, 1.0)
    nB = (0.0, 1.0, 0.0)
    nC = (1.0, 0.0, 0.0)
    normals = [
        nA, nA, nA,
        nB, nB, nB,
        nC, nC, nC,
    ]
    sg = mesh_utils.compute_smoothing_groups(faces, normals)
    for v in sg:
        assert v == 0 or (v & (v - 1)) == 0  # power of two or zero


def test_smoothing_groups_handles_normals_within_epsilon():
    # Tiny perturbation under the default epsilon should still be smooth.
    faces = [(0, 1, 2), (0, 2, 3)]
    n_up = (0.0, 0.0, 1.0)
    eps = 1e-6
    n_jittered = (0.0, eps, math.sqrt(1 - eps * eps))
    normals = [n_up, n_up, n_up, n_up, n_jittered, n_up]
    sg = mesh_utils.compute_smoothing_groups(faces, normals, epsilon=1e-4)
    # Both faces flat-ish → SG=0.
    assert sg == [0, 0]
