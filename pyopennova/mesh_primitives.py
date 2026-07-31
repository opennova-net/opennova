"""Pure-Python mesh primitive generators (DCC-agnostic).

Each function returns ``(verts, faces)`` as plain Python tuples. DCC
wrappers feed the result into their own mesh API (``mesh.from_pydata``
in Blender, ``pymxs.runtime.mesh()`` in 3ds Max).
"""
from __future__ import annotations

import math
from typing import Tuple

Vec3 = Tuple[float, float, float]


def cube_mesh(size: float) -> tuple[list[Vec3], list[tuple[int, int, int, int]]]:
    """Origin-centred axis-aligned cube of edge length ``size``.

    Returns 8 vertices and 6 quad faces (matches ``bmesh.ops.create_cube``
    topology so the Blender side stays byte-equivalent on round-trip).
    """
    h = size * 0.5
    verts: list[Vec3] = [
        (-h, -h, -h),
        ( h, -h, -h),
        ( h,  h, -h),
        (-h,  h, -h),
        (-h, -h,  h),
        ( h, -h,  h),
        ( h,  h,  h),
        (-h,  h,  h),
    ]
    faces: list[tuple[int, int, int, int]] = [
        (0, 1, 2, 3),  # -Z
        (4, 7, 6, 5),  # +Z
        (0, 4, 5, 1),  # -Y
        (2, 6, 7, 3),  # +Y
        (1, 5, 6, 2),  # +X
        (0, 3, 7, 4),  # -X
    ]
    return verts, faces


def direction_arrow_mesh(
    length: float,
    radius: float,
    segs: int = 6,
) -> tuple[list[Vec3], list[tuple[int, ...]]]:
    """Thin arrow pointing along local +Z.

    Shaft runs from origin to ``(0, 0, length * 0.7)``, then a cone tip
    extends to ``(0, 0, length)``. Used to indicate the local Z-axis of
    a userpoint marker. Returns triangle faces.
    """
    shaft_r = radius
    tip_r = radius * 2.5
    shaft_len = length * 0.7
    tip_len = length * 0.3

    verts: list[Vec3] = []
    faces: list[tuple[int, ...]] = []

    # Shaft: two rings (bottom z=0, top z=shaft_len).
    for ring_z in (0.0, shaft_len):
        for i in range(segs):
            angle = 2.0 * math.pi * i / segs
            verts.append((math.cos(angle) * shaft_r, math.sin(angle) * shaft_r, ring_z))
    # Shaft side quads (triangulated).
    for i in range(segs):
        i1 = i
        i2 = (i + 1) % segs
        i3 = i2 + segs
        i4 = i + segs
        faces.append((i1, i2, i3))
        faces.append((i1, i3, i4))

    # Cone base ring at shaft top, wider radius.
    base_start = len(verts)
    for i in range(segs):
        angle = 2.0 * math.pi * i / segs
        verts.append((math.cos(angle) * tip_r, math.sin(angle) * tip_r, shaft_len))
    # Cone tip vertex.
    tip_idx = len(verts)
    verts.append((0.0, 0.0, shaft_len + tip_len))
    for i in range(segs):
        i1 = base_start + i
        i2 = base_start + (i + 1) % segs
        faces.append((i1, i2, tip_idx))

    # Bottom cap (closes the shaft base).
    bot_center = len(verts)
    verts.append((0.0, 0.0, 0.0))
    for i in range(segs):
        i1 = (i + 1) % segs
        i2 = i
        faces.append((bot_center, i1, i2))

    # Cone base cap (ring between shaft top and cone base).
    cone_cap_center = len(verts)
    verts.append((0.0, 0.0, shaft_len))
    for i in range(segs):
        i1 = base_start + i
        i2 = base_start + (i + 1) % segs
        faces.append((cone_cap_center, i2, i1))

    return verts, faces
