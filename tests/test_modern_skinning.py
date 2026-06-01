from __future__ import annotations

from pathlib import Path

from pyopennova.bad_ffi import free_bad, parse_bad
from pyopennova.mesh_build import build_skin_bone_remap, flatten_lod
from pyopennova.threedi_ffi import free_model_ir, read_model_ir


FIXTURE = Path(__file__).resolve().parents[1] / "fixtures" / "modern_skin" / "aidid"


def test_aidid_extra_skinned_part_remaps_to_nearest_animated_parent() -> None:
    ir = read_model_ir(str(FIXTURE / "Aidid.3di"))
    bad = parse_bad(str(FIXTURE / "E_RESET.BAD"))
    try:
        meshes = flatten_lod(ir, 0, track_bone_data=True)
        referenced_bones = {
            bone_idx
            for mesh in meshes
            for vertex_bones in mesh.vertex_bone_data
            for bone_idx, _weight in vertex_bones
        }

        remap = build_skin_bone_remap(ir.lods[0], animated_bone_count=bad.num_bones)

        assert bad.num_bones == 19
        assert 19 in referenced_bones
        assert remap[18] == 18
        assert remap[19] == 14
        assert max(remap[bone_idx] for bone_idx in referenced_bones) < bad.num_bones
    finally:
        free_bad(bad)
        free_model_ir(ir)
