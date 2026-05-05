"""Blender-side wrappers around the host-agnostic numerics in pyopennova.

The actual math lives in ``pyopennova.coords`` and
``pyopennova.polyhedron``. These wrappers just convert tuple results
back into ``mathutils`` types so the rest of the Blender addon code is
unchanged.
"""
from __future__ import annotations

from typing import Sequence

from mathutils import Matrix, Vector

from pyopennova import coords as _coords
from pyopennova import polyhedron as _polyhedron


def render_space(pos: Sequence[float]) -> Vector:
    return Vector(_coords.render_space(pos))


def inverse_render_space(pos: Sequence[float]) -> Vector:
    return Vector(_coords.inverse_render_space(pos))


def bone_space(pos: Sequence[float]) -> Vector:
    return Vector(_coords.bone_space(pos))


def collision_space(pos: Sequence[float]) -> Vector:
    return Vector(_coords.collision_space(pos))


def conjugate_y_to_z(m: Sequence[Sequence[float]]) -> Matrix:
    return Matrix(_coords.conjugate_y_to_z(m))


def orthonormalize(m: Sequence[Sequence[float]]) -> Matrix:
    return Matrix(_coords.orthonormalize(m))


def compute_polyhedron_controlled(
    halfspaces: Sequence[Sequence[float]],
    min_bound: Sequence[float],
    max_bound: Sequence[float],
) -> tuple[list[Vector], list[list[int]]] | tuple[None, None]:
    pts, faces = _polyhedron.compute_polyhedron_controlled(
        halfspaces,
        (min_bound[0], min_bound[1], min_bound[2]),
        (max_bound[0], max_bound[1], max_bound[2]),
    )
    if pts is None:
        return None, None
    return [Vector(p) for p in pts], faces
