"""Coordinate-space, matrix, and polyhedron math helpers for the Blender addon.

Extracted from scene_builder.py so the heavy numerics live in one place.
"""

from __future__ import annotations

import math
from itertools import combinations

from mathutils import Matrix, Vector

# ---------------------------------------------------------------------------
# Coordinate-space converters (return Vector)
# ---------------------------------------------------------------------------


def render_space(pos: Vector) -> Vector:
    """Convert render mesh coords to Blender space.

    Engine coords are Y-up. Blender is Z-up (+X=right, +Y=forward, +Z=up).
    Input is negated-X from the engine, so the full mapping is:
    (-x, -z, y)
    """
    return Vector((-pos[0], -pos[2], pos[1]))


def inverse_render_space(pos: Vector) -> Vector:
    """Convert Blender space back to engine render coords.

    Inverse of render_space: Blender (bx, by, bz) -> engine (-bx, bz, -by)
    """
    return Vector((-pos[0], pos[2], -pos[1]))


def bone_space(pos: Vector) -> Vector:
    """Convert bone position from Y-up to Blender Z-up.

    adm_transform_position is identity, so raw BAD coords are Y-up.
    """
    return Vector((pos[0], -pos[2], pos[1]))


def collision_space(pos: Vector) -> Vector:
    """Convert collision coords to Blender space.

    Engine collision order is (y, z, x), remapped to Blender Z-up: (y, -x, z).
    """
    return Vector((pos[1], -pos[0], pos[2]))


# ---------------------------------------------------------------------------
# 3x3 matrix helpers
# ---------------------------------------------------------------------------


def conjugate_y_to_z(m: Matrix) -> Matrix:
    """Conjugate a 3x3 rotation matrix from Y-up to Blender Z-up.

    M_blender = S @ M_yup @ S^-1 where S maps (x,y,z)->(x,-z,y).
    """
    return Matrix((
        ( m[0][0], -m[0][2],  m[0][1]),
        (-m[2][0],  m[2][2], -m[2][1]),
        ( m[1][0], -m[1][2],  m[1][1]),
    ))


def orthonormalize(m: Matrix) -> Matrix:
    """Gram-Schmidt orthonormalization of a 3x3 matrix (row vectors)."""
    r0 = Vector(m[0]).normalized()
    r1 = Vector(m[1]) - r0 * Vector(m[1]).dot(r0)
    r1.normalize()
    r2 = Vector(m[2]) - r0 * Vector(m[2]).dot(r0)
    r2 -= r1 * r2.dot(r1)
    r2.normalize()
    return Matrix((r0, r1, r2))


# ---------------------------------------------------------------------------
# Halfspace / polyhedron helpers for collision volumes
# ---------------------------------------------------------------------------


def compute_polyhedron_controlled(
    halfspaces: list[list[float]],
    min_bound: Vector,
    max_bound: Vector,
) -> tuple[list[Vector], list[list[int]]] | tuple[None, None]:
    """Compute convex hull from halfspace planes using plane-triplet intersection.

    Intersect every combination of three planes, keep vertices that satisfy
    all halfspaces, then build a convex hull. Falls back to an axis-aligned
    box from min/max bounds when planes don't produce enough vertices.
    """
    epsilon = 1e-6
    hs_count = len(halfspaces)
    # Each halfspace row is [nx, ny, nz, d] representing n . x + d <= 0
    # Convert to n . x <= -d form for intersection test
    normals = [Vector(hs[:3]) for hs in halfspaces]
    offsets = [-hs[3] for hs in halfspaces]

    hull_points: list[Vector] = []

    for a, b, c in combinations(range(hs_count), 3):
        n1, n2, n3 = normals[a], normals[b], normals[c]
        d1, d2, d3 = offsets[a], offsets[b], offsets[c]

        cross23 = n2.cross(n3)
        denom = n1.dot(cross23)
        if abs(denom) < epsilon:
            continue

        point = (cross23 * d1 + n3.cross(n1) * d2 + n1.cross(n2) * d3) * (1.0 / denom)

        # Check point is inside all halfspaces
        inside = True
        for h in range(hs_count):
            if normals[h].dot(point) > offsets[h] + epsilon:
                inside = False
                break

        if inside:
            hull_points.append(point)

    if len(hull_points) >= 4:
        # Deduplicate close points
        unique: list[Vector] = [hull_points[0]]
        for p in hull_points[1:]:
            if all((p - u).length > epsilon for u in unique):
                unique.append(p)
        hull_points = unique

    if len(hull_points) >= 4:
        faces = _build_faces_from_halfspaces(hull_points, normals, offsets, epsilon)
        if faces:
            return hull_points, faces

    # Not enough hull points -- caller handles the fallback box
    return None, None


def _build_faces_from_halfspaces(
    pts: list[Vector],
    normals: list[Vector],
    offsets: list[float],
    epsilon: float,
) -> list[list[int]]:
    """Build polygonal faces by grouping hull vertices onto halfspace planes.

    For each halfspace plane, finds which hull vertices lie on it,
    projects them into the plane's 2D coordinate system, and sorts
    by angle to get correct winding order (one polygon per halfspace plane).
    """
    faces: list[list[int]] = []
    for i in range(len(normals)):
        n = normals[i]
        d = offsets[i]

        # Find vertices on this plane
        on_plane: list[int] = []
        for vi in range(len(pts)):
            if abs(n.dot(pts[vi]) - d) < epsilon * 10:
                on_plane.append(vi)

        if len(on_plane) < 3:
            continue

        # Build local 2D basis on the plane
        if abs(n.x) < 0.9:
            ref = Vector((1.0, 0.0, 0.0))
        else:
            ref = Vector((0.0, 1.0, 0.0))
        u = n.cross(ref)
        u.normalize()
        v = n.cross(u)

        # Project face vertices and compute centroid
        coords_2d = []
        for vi in on_plane:
            p = pts[vi]
            coords_2d.append((p.dot(u), p.dot(v)))

        cx = sum(c[0] for c in coords_2d) / len(coords_2d)
        cy = sum(c[1] for c in coords_2d) / len(coords_2d)

        # Sort by angle around centroid
        angles = []
        for j, (px, py) in enumerate(coords_2d):
            angles.append(math.atan2(py - cy, px - cx))

        sorted_indices = [on_plane[j] for j in sorted(range(len(on_plane)), key=lambda j: angles[j])]

        # Verify winding: face normal should point same direction as halfspace normal
        p0 = pts[sorted_indices[0]]
        p1 = pts[sorted_indices[1]]
        p2 = pts[sorted_indices[2]]
        face_normal = (p1 - p0).cross(p2 - p0)
        if face_normal.dot(n) < 0:
            sorted_indices.reverse()

        faces.append(sorted_indices)

    return faces
