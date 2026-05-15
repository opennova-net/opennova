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
    collision_lod_index: int | None = None,
) -> list[str]:
    """Write host-neutral game outputs from a 3DI3 model.

    This is the shared path for DCC hosts. Host-specific scene files such as
    ``.blend`` and ``.max`` remain in the host integrations.
    """
    if ir is None:
        raise ValueError("write_host_neutral_outputs requires a 3DI3 model")

    os.makedirs(output_dir, exist_ok=True)
    written: list[str] = []
    resolved_collision_lod = _collision_lod_index(
        ir,
        include_collisions,
        collision_lod_index,
    )

    if write_3dp:
        from pyopennova.project_writer import write_3dp_from_3di3

        written.extend(write_3dp_from_3di3(
            ir,
            os.path.join(output_dir, name + ".3dp"),
            collision_lod_index=resolved_collision_lod,
        ))

    if write_ase:
        from pyopennova.ase_writer_ffi import write_ase_files_from_3di3

        written.extend(write_ase_files_from_3di3(
            ir,
            os.path.join(output_dir, name + ".ase"),
            bad_file=bad_file,
            include_collisions=include_collisions,
            include_occlusion=include_occlusion,
            include_lights=include_lights,
        ))

    return written


def _collision_lod_index(ir, include_collisions: bool, explicit: int | None) -> int | None:
    """Resolve a project collision LOD without inventing an extra ASE slot."""
    if explicit is not None:
        lod_idx = int(explicit)
        if lod_idx < 0:
            return None
        lod_count = int(getattr(ir, "lod_count", 0))
        if lod_idx >= lod_count:
            raise ValueError(
                f"collision_lod_index {lod_idx} is not an existing LOD "
                f"for model with {lod_count} LOD(s)"
            )
        return lod_idx
    if not include_collisions:
        return None
    return None
