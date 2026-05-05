"""Host-neutral OpenNova project file writers."""
from __future__ import annotations

import logging
import os

log = logging.getLogger(__name__)


def write_3dp_from_ir(ir, tdp_path: str, bullet_lod_index: int = -1) -> tuple[str, str]:
    """Write sibling ``.3dp`` and ``.3da`` project files from a model IR.

    ``bullet_lod_index`` is the scene LOD that contains poly-collision
    geometry.  Passing ``-1`` leaves the project collision LOD unchanged.
    """
    from pyopennova.tdp_ffi import (
        TDP_MAX_LODS,
        free_tdp,
        tdp_from_ir,
        write_3da,
        write_tdp,
    )

    os.makedirs(os.path.dirname(os.path.abspath(tdp_path)), exist_ok=True)
    proj = tdp_from_ir(ir)
    try:
        if 0 <= bullet_lod_index < TDP_MAX_LODS:
            lod_slot = proj.lods[bullet_lod_index]
            name = ir.name.decode("utf-8", errors="replace").rstrip("\x00")
            lod_slot.scene_file = f"{name}_bullet.ase".encode("utf-8")[:63]
            lod_slot.attributes = proj.lods[0].attributes
            lod_slot.render_function = proj.lods[0].render_function
            proj.poly_collision_lod = bullet_lod_index

        write_tdp(tdp_path, proj)
        tda_path = os.path.splitext(tdp_path)[0] + ".3da"
        write_3da(tda_path, proj)
    finally:
        free_tdp(proj)

    log.info("Wrote 3DP: %s", tdp_path)
    log.info("Wrote 3DA: %s", tda_path)
    return tdp_path, tda_path
