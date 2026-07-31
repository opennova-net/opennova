"""Halfspace-intersection convex-hull builder for collision volumes.

DCC-agnostic: takes and returns plain tuples. DCC wrappers convert to
mathutils.Vector / pymxs Point3 at the call site.
"""
from __future__ import annotations

import math
from itertools import combinations
from typing import Sequence, Tuple

Vec3 = Tuple[float, float, float]


def compute_polyhedron_controlled(
    halfspaces: Sequence[Sequence[float]],
    min_bound: Sequence[float],
    max_bound: Sequence[float],
) -> tuple[list[Vec3], list[list[int]]] | tuple[None, None]:
    """Convex hull from halfspace planes.

    Each halfspace is a 4-tuple ``(nx, ny, nz, d)`` representing the
    inequality ``n . x + d <= 0``. Intersect every triplet of planes,
    keep vertices satisfying all halfspaces, then build a face polygon
    per plane.

    Returns ``(vertices, faces)`` on success, or ``(None, None)`` if
    fewer than 4 hull points were found (caller falls back to an
    axis-aligned box from ``min_bound`` / ``max_bound``).
    """
    epsilon = 1e-6

    normals: list[Vec3] = [(hs[0], hs[1], hs[2]) for hs in halfspaces]
    offsets: list[float] = [-hs[3] for hs in halfspaces]
    hs_count = len(halfspaces)

    hull_points: list[Vec3] = []

    for a, b, c in combinations(range(hs_count), 3):
        n1, n2, n3 = normals[a], normals[b], normals[c]
        d1, d2, d3 = offsets[a], offsets[b], offsets[c]

        cross23 = _cross(n2, n3)
        denom = _dot(n1, cross23)
        if abs(denom) < epsilon:
            continue

        # point = (cross23 * d1 + cross(n3,n1) * d2 + cross(n1,n2) * d3) / denom
        cross31 = _cross(n3, n1)
        cross12 = _cross(n1, n2)
        scaled_sum = _add(
            _add(_scale(cross23, d1), _scale(cross31, d2)),
            _scale(cross12, d3),
        )
        point = _scale(scaled_sum, 1.0 / denom)

        inside = True
        for h in range(hs_count):
            if _dot(normals[h], point) > offsets[h] + epsilon:
                inside = False
                break

        if inside:
            hull_points.append(point)

    if len(hull_points) >= 4:
        unique: list[Vec3] = [hull_points[0]]
        for p in hull_points[1:]:
            if all(_dist_sq(p, u) > epsilon * epsilon for u in unique):
                unique.append(p)
        hull_points = unique

    if len(hull_points) < 4:
        # Caller handles the fallback box from min_bound / max_bound.
        del min_bound, max_bound
        return None, None

    faces = _build_faces_from_halfspaces(hull_points, normals, offsets, epsilon)
    if not faces:
        return None, None

    return hull_points, faces


def _build_faces_from_halfspaces(
    pts: list[Vec3],
    normals: list[Vec3],
    offsets: list[float],
    epsilon: float,
) -> list[list[int]]:
    """Group hull vertices by halfspace plane and emit one wound polygon per plane."""
    faces: list[list[int]] = []
    plane_eps = epsilon * 10.0

    for i, n in enumerate(normals):
        d = offsets[i]

        on_plane: list[int] = [
            vi for vi, p in enumerate(pts) if abs(_dot(n, p) - d) < plane_eps
        ]
        if len(on_plane) < 3:
            continue

        # Local 2D basis on the plane.
        ref: Vec3 = (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)
        u = _normalize(_cross(n, ref))
        v = _cross(n, u)

        coords_2d: list[tuple[float, float]] = [
            (_dot(pts[vi], u), _dot(pts[vi], v)) for vi in on_plane
        ]

        cx = sum(c[0] for c in coords_2d) / len(coords_2d)
        cy = sum(c[1] for c in coords_2d) / len(coords_2d)

        angles = [math.atan2(py - cy, px - cx) for px, py in coords_2d]
        order = sorted(range(len(on_plane)), key=lambda j: angles[j])
        winding = [on_plane[j] for j in order]

        # Verify winding: face normal must agree with the halfspace normal.
        p0, p1, p2 = pts[winding[0]], pts[winding[1]], pts[winding[2]]
        face_normal = _cross(_sub(p1, p0), _sub(p2, p0))
        if _dot(face_normal, n) < 0:
            winding.reverse()

        faces.append(winding)

    return faces


# ---------------------------------------------------------------------------
# Internal vector helpers (kept private to this module)
# ---------------------------------------------------------------------------


def _dot(a: Sequence[float], b: Sequence[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _add(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _sub(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _scale(a: Sequence[float], s: float) -> Vec3:
    return (a[0] * s, a[1] * s, a[2] * s)


def _dist_sq(a: Sequence[float], b: Sequence[float]) -> float:
    dx = a[0] - b[0]
    dy = a[1] - b[1]
    dz = a[2] - b[2]
    return dx * dx + dy * dy + dz * dz


def _normalize(v: Sequence[float]) -> Vec3:
    n = math.sqrt(_dot(v, v))
    if n == 0.0:
        return (0.0, 0.0, 0.0)
    return (v[0] / n, v[1] / n, v[2] / n)
