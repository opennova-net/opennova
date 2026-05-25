"""Helpers for writing material descriptors into ASE FFI structs."""
from __future__ import annotations

from pyopennova.materials import MaterialDescriptor, ase_texture_names


def populate_ase_submaterial(
    sub,
    desc: MaterialDescriptor,
    *,
    used_tex_names: dict[str, str],
) -> None:
    """Write one material descriptor into an allocated ASE submaterial."""
    sub.name = desc.name.encode("utf-8")[:31]

    diffuse, detail = ase_texture_names(desc, used_tex_names)
    if diffuse:
        sub.maps[0].value = diffuse.encode("utf-8")[:31]
    if detail:
        sub.maps[1].value = detail.encode("utf-8")[:31]

    if desc.alpha_test and diffuse:
        sub.maps[2].value = diffuse.encode("utf-8")[:31]

    sub.uv_u_tiling[0] = float(desc.effective_u_tiling)
    sub.uv_v_tiling[0] = float(desc.effective_v_tiling)
    sub.uv_u_tiling[1] = float(desc.effective_u1_tiling)
    sub.uv_v_tiling[1] = float(desc.effective_v1_tiling)

    if desc.glass and any(desc.reflect_color[:3]):
        sub.ambient[0] = float(desc.reflect_color[0])
        sub.ambient[1] = float(desc.reflect_color[1])
        sub.ambient[2] = float(desc.reflect_color[2])
    else:
        sub.ambient[0] = sub.ambient[1] = sub.ambient[2] = 0.0588

    sub.diffuse[0] = sub.diffuse[1] = sub.diffuse[2] = 0.5882
    sub.specular[0] = sub.specular[1] = sub.specular[2] = 0.9
    sub.shine = 0.1
    sub.shine_strength = 0.0
    sub.transparency = 0.0
    sub.wiresize = 1.0

    if desc.two_sided:
        sub.extra_flags |= 1

    sub.shading = 1 if (
        desc.phong_shader or desc.shader_family in ("phong", "environment", "glass")
    ) else 0
