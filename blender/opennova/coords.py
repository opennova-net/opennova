"""Coordinate-space converters and 3x3 matrix helpers (DCC-agnostic).

Engine source coordinates are Y-up. Both Blender and 3ds Max are Z-up
right-handed, so the source-side transforms here apply unchanged for
either DCC. Inputs accept any 3-element sequence (tuple, list,
mathutils.Vector, pymxs Point3) since all of them support index access.
Outputs are tuples; DCC-side wrappers re-wrap into native types.
"""
from __future__ import annotations

import math
from typing import Sequence, Tuple

Vec3 = Tuple[float, float, float]
Mat3 = Tuple[Vec3, Vec3, Vec3]


# ---------------------------------------------------------------------------
# Coordinate-space converters
# ---------------------------------------------------------------------------


def render_space(pos: Sequence[float]) -> Vec3:
    """Engine render-mesh coords to Z-up RH.

    Engine is Y-up with negated-X on export, so the full mapping is
    (x, y, z) -> (-x, -z, y).
    """
    return (-pos[0], -pos[2], pos[1])


def inverse_render_space(pos: Sequence[float]) -> Vec3:
    """Z-up RH to engine render-mesh coords. Inverse of render_space."""
    return (-pos[0], pos[2], -pos[1])


def bone_space(pos: Sequence[float]) -> Vec3:
    """BAD bone position (Y-up, identity adm transform) to Z-up RH."""
    return (pos[0], -pos[2], pos[1])


def collision_space(pos: Sequence[float]) -> Vec3:
    """Engine collision coords (y, z, x order) to Z-up RH.

    Mapping: (x, y, z) -> (y, -x, z).
    """
    return (pos[1], -pos[0], pos[2])


# ---------------------------------------------------------------------------
# 3x3 rotation-matrix helpers
# ---------------------------------------------------------------------------


def conjugate_y_to_z(m: Sequence[Sequence[float]]) -> Mat3:
    """Conjugate a 3x3 rotation matrix from Y-up to Z-up.

    M_zup = S @ M_yup @ S^-1 where S maps (x, y, z) -> (x, -z, y).
    """
    return (
        ( m[0][0], -m[0][2],  m[0][1]),
        (-m[2][0],  m[2][2], -m[2][1]),
        ( m[1][0], -m[1][2],  m[1][1]),
    )


def orthonormalize(m: Sequence[Sequence[float]]) -> Mat3:
    """Gram-Schmidt orthonormalisation of a 3x3 matrix (row vectors)."""
    r0 = _normalize(_as_tuple(m[0]))
    r1 = _vec_sub(_as_tuple(m[1]), _vec_scale(r0, _vec_dot(m[1], r0)))
    r1 = _normalize(r1)
    r2 = _vec_sub(_as_tuple(m[2]), _vec_scale(r0, _vec_dot(m[2], r0)))
    r2 = _vec_sub(r2, _vec_scale(r1, _vec_dot(r2, r1)))
    r2 = _normalize(r2)
    return (r0, r1, r2)


# ---------------------------------------------------------------------------
# Internal vector helpers
# ---------------------------------------------------------------------------


def _as_tuple(v: Sequence[float]) -> Vec3:
    return (v[0], v[1], v[2])


def _vec_dot(a: Sequence[float], b: Sequence[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _vec_sub(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _vec_scale(a: Sequence[float], s: float) -> Vec3:
    return (a[0] * s, a[1] * s, a[2] * s)


def _normalize(v: Sequence[float]) -> Vec3:
    n = math.sqrt(_vec_dot(v, v))
    if n == 0.0:
        return (0.0, 0.0, 0.0)
    return (v[0] / n, v[1] / n, v[2] / n)
