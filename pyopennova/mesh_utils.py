"""Pure-Python mesh utilities (DCC-agnostic).

Moved out of ``apps/importer/scene_builder.py`` so the same algorithms
can run from a 3ds Max scene builder without pulling in Blender.
"""
from __future__ import annotations

import math
from collections import defaultdict, deque
from typing import Sequence, Tuple

Vec3 = Tuple[float, float, float]
Mat3 = Tuple[Vec3, Vec3, Vec3]


def mtrx_to_center_rotation(mat_data: Sequence[float]) -> Mat3 | None:
    """Convert MTRX 16-float buffer to a 3x3 rotation tuple.

    Inverts ``build_matrix_from_axis`` (export_3di.cpp) plus the ASE
    parser swizzle chain. Returns ``None`` if the matrix contains NaN
    (zero-axis sentinel from the exporter). The returned 3x3 is consumed
    by the ASE writer's ``_tm`` helper, which transposes it into the
    ``TM_ROW0``/``TM_ROW1``/``TM_ROW2`` values ModSuperOED expects.
    """
    m = [mat_data[i] for i in range(16)]
    if any(math.isnan(v) for v in m):
        return None

    g00, g01, g02 = m[0], m[1], m[2]
    g10, g11, g12 = m[4], m[5], m[6]
    g20, g21, g22 = m[8], m[9], m[10]
    c00 = g11 * g22 - g12 * g21
    c01 = -(g10 * g22 - g12 * g20)
    c02 = g10 * g21 - g11 * g20
    c10 = -(g01 * g22 - g02 * g21)
    c11 = g00 * g22 - g02 * g20
    c12 = -(g00 * g21 - g01 * g20)
    c20 = g01 * g12 - g02 * g11
    c21 = -(g00 * g12 - g02 * g10)
    c22 = g00 * g11 - g01 * g10
    det = g00 * c00 + g01 * c01 + g02 * c02

    m00, m01, m02 = c00 / det, c10 / det, c20 / det
    m10, m11, m12 = c01 / det, c11 / det, c21 / det
    m20, m21, m22 = c02 / det, c12 / det, c22 / det

    ax0 = (m22, -m20, m21)
    ax1 = (-m02, m00, -m01)
    ax2 = (m12, -m10, m11)
    return (
        ( ax1[1], -ax0[1],  ax2[1]),
        (-ax1[0],  ax0[0], -ax2[0]),
        ( ax1[2], -ax0[2],  ax2[2]),
    )


def compute_smoothing_groups(
    faces: Sequence[Sequence[int]],
    normals: Sequence[Sequence[float]],
    epsilon: float = 1e-4,
) -> list[int]:
    """Compute smoothing-group bitmasks from per-loop normals.

    Compares normals at shared edges to classify them as smooth or
    sharp, flood-fills connected smooth regions, then greedy-colours
    the component-adjacency graph so adjacent components never share a
    smoothing-group bit (avoids false smoothing from bit collisions).

    ``faces`` is a sequence of 3-tuples of vertex indices. ``normals``
    is a flat sequence indexed as ``normals[face_index * 3 + corner]``.
    Returns one bitmask per face.
    """
    edge_faces: dict[tuple[int, int], list[int]] = defaultdict(list)
    for fi, (v0, v1, v2) in enumerate(faces):
        for a, b in ((v0, v1), (v1, v2), (v2, v0)):
            edge_faces[(min(a, b), max(a, b))].append(fi)

    eps_sq = epsilon * epsilon
    smooth_adj: dict[int, set[int]] = defaultdict(set)
    for (ea, eb), flist in edge_faces.items():
        if len(flist) != 2:
            continue
        fi_a, fi_b = flist[0], flist[1]
        fa, fb = faces[fi_a], faces[fi_b]
        smooth = True
        for sv in (ea, eb):
            na = normals[fi_a * 3 + list(fa).index(sv)]
            nb = normals[fi_b * 3 + list(fb).index(sv)]
            dx = na[0] - nb[0]
            dy = na[1] - nb[1]
            dz = na[2] - nb[2]
            if dx * dx + dy * dy + dz * dz > eps_sq:
                smooth = False
                break
        if smooth:
            smooth_adj[fi_a].add(fi_b)
            smooth_adj[fi_b].add(fi_a)

    comp = [-1] * len(faces)
    cid = 0
    for fi in range(len(faces)):
        if comp[fi] >= 0:
            continue
        queue = deque([fi])
        while queue:
            f = queue.popleft()
            if comp[f] >= 0:
                continue
            comp[f] = cid
            for adj in smooth_adj.get(f, ()):
                if comp[adj] < 0:
                    queue.append(adj)
        cid += 1

    # Detect flat components: all per-loop normals within the component
    # are identical. Flat components get SG=0 (no smoothing) because the
    # original model likely used SG=0 for co-planar faces, which matters
    # for vertex dedup in compute_smoothed_vectors.
    comp_varies = [False] * cid
    comp_ref: list[Sequence[float] | None] = [None] * cid
    for fi in range(len(faces)):
        c = comp[fi]
        if c < 0 or comp_varies[c]:
            continue
        for j in range(3):
            n = normals[fi * 3 + j]
            if comp_ref[c] is None:
                comp_ref[c] = n
            else:
                ref = comp_ref[c]
                assert ref is not None
                dx = n[0] - ref[0]
                dy = n[1] - ref[1]
                dz = n[2] - ref[2]
                if dx * dx + dy * dy + dz * dz > eps_sq:
                    comp_varies[c] = True
                    break
    flat_comps = {c for c in range(cid) if not comp_varies[c]}

    comp_adj: dict[int, set[int]] = defaultdict(set)
    for (ea, eb), flist in edge_faces.items():
        if len(flist) != 2:
            continue
        fi_a, fi_b = flist[0], flist[1]
        ca, cb = comp[fi_a], comp[fi_b]
        if ca != cb and ca >= 0 and cb >= 0:
            comp_adj[ca].add(cb)
            comp_adj[cb].add(ca)

    comp_color: dict[int, int] = {}
    for c in range(cid):
        if c in flat_comps:
            comp_color[c] = -1
            continue
        used = set()
        for neighbor in comp_adj.get(c, ()):
            if neighbor in comp_color and comp_color[neighbor] >= 0:
                used.add(comp_color[neighbor])
        color = 0
        while color in used:
            color += 1
        comp_color[c] = color

    result: list[int] = []
    for fi in range(len(faces)):
        c = comp[fi]
        if c < 0:
            result.append(0)
        else:
            color = comp_color.get(c, 0)
            result.append(0 if color < 0 else (1 << (color % 31)))
    return result
