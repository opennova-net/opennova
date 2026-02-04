"""Mesh primitive generators for scene markers."""

from __future__ import annotations

import math

import bmesh


def create_cube_mesh(mesh_data, size):
    """Fill *mesh_data* with a cube centered at the origin.

    Uses bmesh.ops.create_cube — 8 shared vertices, 6 quad faces.
    """
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=size)
    bm.to_mesh(mesh_data)
    bm.free()


def create_direction_arrow_mesh(mesh_data, length, radius):
    """Fill mesh_data with a thin arrow pointing along local +Z.

    The shaft runs from origin to (0, 0, length*0.7), then a cone tip
    extends to (0, 0, length). Indicates the userpoint's local Z-axis.
    """
    segs = 6
    shaft_r = radius
    tip_r = radius * 2.5
    shaft_len = length * 0.7
    tip_len = length * 0.3

    verts = []
    faces = []

    # Shaft: two rings of vertices (bottom at z=0, top at z=shaft_len)
    for ring_z in (0.0, shaft_len):
        for i in range(segs):
            angle = 2 * math.pi * i / segs
            verts.append((math.cos(angle) * shaft_r, math.sin(angle) * shaft_r, ring_z))
    # Shaft quads (triangulated)
    for i in range(segs):
        i1 = i
        i2 = (i + 1) % segs
        i3 = i2 + segs
        i4 = i + segs
        faces.append((i1, i2, i3))
        faces.append((i1, i3, i4))

    # Cone base ring at z=shaft_len with tip_r
    base_start = len(verts)
    for i in range(segs):
        angle = 2 * math.pi * i / segs
        verts.append((math.cos(angle) * tip_r, math.sin(angle) * tip_r, shaft_len))
    # Cone tip vertex
    tip_idx = len(verts)
    verts.append((0.0, 0.0, shaft_len + tip_len))
    # Cone faces
    for i in range(segs):
        i1 = base_start + i
        i2 = base_start + (i + 1) % segs
        faces.append((i1, i2, tip_idx))

    # Bottom cap (close the shaft base)
    bot_center = len(verts)
    verts.append((0.0, 0.0, 0.0))
    for i in range(segs):
        i1 = (i + 1) % segs
        i2 = i
        faces.append((bot_center, i1, i2))

    # Cone base cap (ring between shaft top and cone base)
    cone_cap_center = len(verts)
    verts.append((0.0, 0.0, shaft_len))
    for i in range(segs):
        i1 = base_start + i
        i2 = base_start + (i + 1) % segs
        faces.append((cone_cap_center, i2, i1))

    mesh_data.from_pydata(verts, [], faces)
    mesh_data.update()
