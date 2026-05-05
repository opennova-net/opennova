"""Host-neutral OpenNova output writers shared by DCC integrations."""
from __future__ import annotations

import os


def write_host_neutral_outputs(
    ir,
    output_dir: str,
    name: str,
    *,
    write_ase: bool = True,
    write_3dp: bool = True,
    include_collisions: bool = True,
    include_occlusion: bool = True,
    include_lights: bool = True,
    bad_file=None,
    bullet_lod_index: int | None = None,
) -> list[str]:
    """Write host-neutral game outputs from a model IR.

    This is the shared path for DCC hosts. Host-specific scene files such as
    ``.blend`` and ``.max`` remain in the host integrations.
    """
    if ir is None:
        raise ValueError("write_host_neutral_outputs requires a model IR")

    os.makedirs(output_dir, exist_ok=True)
    written: list[str] = []
    resolved_bullet_lod = _bullet_lod_index(ir, include_collisions, bullet_lod_index)

    if write_3dp:
        from pyopennova.project_writer import write_3dp_from_ir

        written.extend(write_3dp_from_ir(
            ir,
            os.path.join(output_dir, name + ".3dp"),
            bullet_lod_index=resolved_bullet_lod,
        ))

    if write_ase:
        from pyopennova.ase_from_ir import write_ase_from_ir

        written.extend(write_ase_from_ir(
            ir,
            os.path.join(output_dir, name + ".ase"),
            include_collisions=include_collisions,
            include_occlusion=include_occlusion,
            include_lights=include_lights,
            bad_file=bad_file,
        ))

    return written


def _bullet_lod_index(ir, include_collisions: bool, explicit: int | None) -> int:
    if explicit is not None:
        return int(explicit)
    if not include_collisions:
        return -1
    coll = getattr(ir, "collision", None)
    if not coll:
        return -1
    try:
        contents = coll.contents
    except ValueError:
        return -1
    if int(getattr(contents, "object_count", 0)) <= 0:
        return -1
    if int(getattr(contents, "vertex_count", 0)) <= 0:
        return -1
    return int(getattr(ir, "lod_count", 0))
