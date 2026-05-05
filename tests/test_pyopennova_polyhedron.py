"""Unit tests for pyopennova.polyhedron (halfspace -> convex hull)."""
from __future__ import annotations

import math

import pytest

from pyopennova import polyhedron


def _unit_cube_halfspaces():
    """Six halfspaces of the unit cube [-1, 1]^3.

    Each row is ``(nx, ny, nz, d)`` with the inequality ``n . x + d <= 0``.
    For the +x face: ``x <= 1`` so ``x - 1 <= 0`` -> ``(1, 0, 0, -1)``.
    """
    return [
        ( 1.0,  0.0,  0.0, -1.0),
        (-1.0,  0.0,  0.0, -1.0),
        ( 0.0,  1.0,  0.0, -1.0),
        ( 0.0, -1.0,  0.0, -1.0),
        ( 0.0,  0.0,  1.0, -1.0),
        ( 0.0,  0.0, -1.0, -1.0),
    ]


def test_unit_cube_returns_eight_corner_vertices():
    pts, faces = polyhedron.compute_polyhedron_controlled(
        _unit_cube_halfspaces(),
        min_bound=(-1.0, -1.0, -1.0),
        max_bound=( 1.0,  1.0,  1.0),
    )
    assert pts is not None
    assert len(pts) == 8
    # Every corner of the unit cube must be present.
    expected = {
        (sx, sy, sz)
        for sx in (-1.0, 1.0)
        for sy in (-1.0, 1.0)
        for sz in (-1.0, 1.0)
    }
    actual = {tuple(round(c, 6) for c in p) for p in pts}
    assert actual == expected


def test_unit_cube_returns_six_quad_faces():
    _, faces = polyhedron.compute_polyhedron_controlled(
        _unit_cube_halfspaces(),
        min_bound=(-1.0, -1.0, -1.0),
        max_bound=( 1.0,  1.0,  1.0),
    )
    assert faces is not None
    assert len(faces) == 6
    assert all(len(f) == 4 for f in faces)


def test_unit_cube_face_winding_outward():
    pts, faces = polyhedron.compute_polyhedron_controlled(
        _unit_cube_halfspaces(),
        min_bound=(-1.0, -1.0, -1.0),
        max_bound=( 1.0,  1.0,  1.0),
    )
    # For each face, the winding-derived normal should agree with the
    # outward-pointing normal of the corresponding halfspace plane.
    for f in faces:
        p0 = pts[f[0]]
        p1 = pts[f[1]]
        p2 = pts[f[2]]
        e1 = (p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2])
        e2 = (p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2])
        n = (
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        )
        # Centroid of the face.
        cx = sum(pts[i][0] for i in f) / len(f)
        cy = sum(pts[i][1] for i in f) / len(f)
        cz = sum(pts[i][2] for i in f) / len(f)
        # Outward normal points away from origin (cube centred at origin).
        outward_dot = n[0] * cx + n[1] * cy + n[2] * cz
        assert outward_dot > 0.0


def test_degenerate_halfspaces_return_none():
    # Two parallel planes that don't bound anything plus a single third plane:
    # the triplet-intersection has no chance of building 4 hull points.
    halfspaces = [
        ( 1.0, 0.0, 0.0, -1.0),
        (-1.0, 0.0, 0.0, -1.0),
        ( 0.0, 1.0, 0.0, -1.0),
    ]
    pts, faces = polyhedron.compute_polyhedron_controlled(
        halfspaces, min_bound=(-1.0, -1.0, -1.0), max_bound=(1.0, 1.0, 1.0),
    )
    assert pts is None
    assert faces is None


def test_tetrahedron_returns_four_triangles():
    # Regular tetrahedron from four halfspaces.
    # Vertices at (1,1,1), (1,-1,-1), (-1,1,-1), (-1,-1,1).
    # Face opposite (1,1,1): plane through other three, outward normal (1,1,1)/sqrt(3).
    s = 1.0 / math.sqrt(3.0)
    halfspaces = [
        # Plane n . x <= -d for outward normal n through (-1,-1,1) etc.
        ( s,  s,  s, -s),
        ( s, -s, -s, -s),
        (-s,  s, -s, -s),
        (-s, -s,  s, -s),
    ]
    pts, faces = polyhedron.compute_polyhedron_controlled(
        halfspaces, min_bound=(-1.0, -1.0, -1.0), max_bound=(1.0, 1.0, 1.0),
    )
    assert pts is not None
    assert len(pts) == 4
    assert len(faces) == 4
    assert all(len(f) == 3 for f in faces)
