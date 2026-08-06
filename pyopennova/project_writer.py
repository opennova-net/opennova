"""DCC-neutral OpenNova project file writers."""
from __future__ import annotations

import logging
import os

log = logging.getLogger(__name__)


def derive_poly_collision_lod(model) -> int:
    """Recover the .3dp ``poly_collision_lod`` value from a ``Threedi3di3`` model.

    Stock 3DI files do not store this build-time setting directly, but it
    is recoverable: OED's ``WriteCDTA @ 0x456050`` (in ModSuperOed.exe.i64)
    accumulates ``lod->subobjects[i].vertCount`` across the LOD selected by
    ``g_ActiveLod``, which is set from the .3dp's ``poly_collision_lod``
    field (imported by the original OED workspace parser). So the LOD whose vertex count equals
    ``CDTA.CMDL`` counts identify the collision LOD.

    Heuristic:
      1. If only one render LOD exists, return 0.
      2. Prefer LODs whose triangle count equals CDTA's face count.
      3. Break face-count ties with an exact vertex-count match, then
         by highest LOD index.
      4. If no face count matches, fall back to exact vertex-count matches
         and choose the highest LOD index.

    Returns ``-1`` when the 3DI3 model has no collision data, no LODs, or no
    exact count evidence.
    """
    coll = _collision_model(model)
    if coll is None:
        return -1
    target_face_count = int(coll.face_count)
    target_vertex_count = int(coll.vertex_count)
    if target_face_count <= 0 or target_vertex_count <= 0:
        return -1
    lod_count = int(model.lod_count)
    if lod_count == 0:
        return -1
    if lod_count == 1:
        return 0

    face_matches: list[int] = []
    for i in range(lod_count):
        if _lod_triangle_count(model.lods[i]) == target_face_count:
            face_matches.append(i)
    if face_matches:
        vertex_face_matches = [
            i for i in face_matches
            if target_vertex_count > 0 and int(model.lods[i].vertices.count) == target_vertex_count
        ]
        return (vertex_face_matches or face_matches)[-1]

    matches: list[int] = []
    for i in range(lod_count):
        if int(model.lods[i].vertices.count) == target_vertex_count:
            matches.append(i)
    if matches:
        return matches[-1]
    return -1


def _collision_model(model):
    coll = model.collision
    if not coll:
        return None
    return coll[0]


def _lod_triangle_count(lod) -> int:
    strip_count = int(lod.strip_count)
    strips = lod.strips
    if strips and strip_count > 0:
        return sum(int(strips[i].num_triangles) for i in range(strip_count))
    return int(lod.indices.count) // 3


def write_3dp_from_3di3(
    model,
    tdp_path: str,
    collision_lod_index: int | None = None,
) -> tuple[str]:
    """Write a ``.3dp`` object workspace project file from a 3DI3 model.

    ``collision_lod_index`` is an existing referenced LOD that supplies
    poly-collision geometry. Passing ``None`` triggers
    :func:`derive_poly_collision_lod` to recover the value from the 3DI3 model's
    collision data; pass an explicit integer to override.

    """
    from pyopennova.tdp_ffi import (
        TDP_MAX_LODS,
        free_tdp,
        tdp_from_3di,
        write_tdp,
    )

    os.makedirs(os.path.dirname(os.path.abspath(tdp_path)), exist_ok=True)
    proj = tdp_from_3di(model)
    try:
        if collision_lod_index is None:
            # No explicit override: recover poly_collision_lod from CDTA count
            # evidence by matching against existing render LODs.
            # Returns -1 when not inferable; treat that as "leave default".
            derived = derive_poly_collision_lod(model)
            if derived >= 0:
                collision_lod_index = derived
        if collision_lod_index is not None and int(collision_lod_index) >= 0:
            lod_idx = int(collision_lod_index)
            lod_count = int(model.lod_count)
            if lod_idx >= min(TDP_MAX_LODS, lod_count):
                raise ValueError(
                    f"collision_lod_index {lod_idx} is not an existing LOD "
                    f"for model with {lod_count} LOD(s)"
                )
            proj.poly_collision_lod = lod_idx

        write_tdp(tdp_path, proj)
    finally:
        free_tdp(proj)

    log.info("Wrote 3DP: %s", tdp_path)
    return (tdp_path,)
