"""Blender-side wrappers for the pure mesh-primitive generators.

The geometry math lives in ``pyopennova.mesh_primitives``. These shims
just feed the (verts, faces) result into a Blender ``Mesh`` datablock.
"""
from __future__ import annotations

from pyopennova.mesh_primitives import cube_mesh, direction_arrow_mesh


def create_cube_mesh(mesh_data, size):
    """Fill ``mesh_data`` with an origin-centred cube of edge length ``size``."""
    verts, faces = cube_mesh(size)
    mesh_data.from_pydata(verts, [], faces)
    mesh_data.update()


def create_direction_arrow_mesh(mesh_data, length, radius):
    """Fill ``mesh_data`` with a thin arrow pointing along local +Z."""
    verts, faces = direction_arrow_mesh(length, radius)
    mesh_data.from_pydata(verts, [], faces)
    mesh_data.update()
