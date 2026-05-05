"""Unit tests for pyopennova.mesh_primitives."""
from __future__ import annotations

import math

import pytest

from pyopennova import mesh_primitives


# ---------------------------------------------------------------------------
# Cube
# ---------------------------------------------------------------------------


def test_cube_has_8_verts_and_6_quads():
    verts, faces = mesh_primitives.cube_mesh(1.0)
    assert len(verts) == 8
    assert len(faces) == 6
    assert all(len(f) == 4 for f in faces)


def test_cube_corners_are_at_signed_half_size():
    verts, _ = mesh_primitives.cube_mesh(2.0)
    actual = {tuple(round(c, 6) for c in v) for v in verts}
    expected = {
        (sx, sy, sz)
        for sx in (-1.0, 1.0)
        for sy in (-1.0, 1.0)
        for sz in (-1.0, 1.0)
    }
    assert actual == expected


def test_cube_face_indices_are_in_range():
    verts, faces = mesh_primitives.cube_mesh(1.0)
    n = len(verts)
    for f in faces:
        assert all(0 <= idx < n for idx in f)


# ---------------------------------------------------------------------------
# Direction arrow
# ---------------------------------------------------------------------------


def test_arrow_default_segs_produces_expected_vertex_count():
    # 2 shaft rings * 6 + 6 cone-base + 1 tip + 1 bottom cap centre
    # + 1 cone cap centre = 21.
    verts, _ = mesh_primitives.direction_arrow_mesh(length=1.0, radius=0.05)
    assert len(verts) == 21


def test_arrow_total_length_matches_input():
    verts, _ = mesh_primitives.direction_arrow_mesh(length=2.0, radius=0.05)
    max_z = max(v[2] for v in verts)
    assert max_z == pytest.approx(2.0, abs=1e-9)


def test_arrow_base_at_origin_z():
    verts, _ = mesh_primitives.direction_arrow_mesh(length=1.0, radius=0.05)
    min_z = min(v[2] for v in verts)
    assert min_z == pytest.approx(0.0, abs=1e-9)


def test_arrow_face_indices_are_in_range():
    verts, faces = mesh_primitives.direction_arrow_mesh(length=1.0, radius=0.05)
    n = len(verts)
    for f in faces:
        assert all(0 <= idx < n for idx in f)


def test_arrow_segments_scale_face_count():
    _, faces6 = mesh_primitives.direction_arrow_mesh(1.0, 0.05, segs=6)
    _, faces12 = mesh_primitives.direction_arrow_mesh(1.0, 0.05, segs=12)
    # Doubling segs should double the face count proportionally.
    assert len(faces12) > len(faces6)
    # Face count is 2*segs (shaft) + segs (cone tri) + segs (bot cap) + segs (cone cap)
    # = 5 * segs.
    assert len(faces6) == 5 * 6
    assert len(faces12) == 5 * 12


def test_shaft_top_ring_has_correct_radius():
    verts, _ = mesh_primitives.direction_arrow_mesh(length=1.0, radius=0.1, segs=6)
    # Top ring is verts[6:12] (second ring of shaft).
    for v in verts[6:12]:
        r = math.sqrt(v[0] ** 2 + v[1] ** 2)
        assert r == pytest.approx(0.1, abs=1e-9)
