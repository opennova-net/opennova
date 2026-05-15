"""Phase C prep gate: C++ object_ir_to_flat_meshes_v2 must produce
byte-equivalent FlatMesh data to Python flatten_lod for all 5 stock fixtures.

NOTE: Since v2, C++ fm->vertices stores WORLD-SPACE (absolute render_space)
positions rather than part-local positions, matching the double-precision path
used in the ASE writer gate. When comparing against Python's flatten_lod
(which stores part-local positions), we add the part origin to each Python
vertex before comparing.
"""
from __future__ import annotations

import ctypes

import pytest

from pyopennova import threedi_ffi, coords
from pyopennova.flat_mesh_ffi import flat_meshes_from_3di3, free_array
from pyopennova.legacy_tools.stock_roundtrip import (
    STOCK_THREEDI_FIXTURE_NAMES,
    discover_stock_3di_fixtures,
)
from pyopennova.mesh_build import flatten_lod


@pytest.mark.parametrize("name", STOCK_THREEDI_FIXTURE_NAMES)
def test_ir_to_flat_meshes_matches_python_flatten_lod(name: str) -> None:
    fx = discover_stock_3di_fixtures(names=[name])[0]
    model = threedi_ffi.read_model_3di3(str(fx.reference_3di_path))
    cpp_arr = None
    try:
        for lod_idx in range(int(model.lod_count)):
            is_skinned = int(getattr(model, "mesh_type", 0)) == 3
            py_meshes = flatten_lod(
                model, lod_idx,
                include_empty_parts=False,
                track_bone_data=is_skinned,
                preserve_source_indexing=True,
            )
            cpp_arr = flat_meshes_from_3di3(
                model, lod_idx,
                include_empty_parts=False,
                track_bone_data=is_skinned,
                preserve_source_indexing=True,
            )
            # flat_meshes_from_3di3 returns a FlatMeshArray ctypes struct directly
            assert int(cpp_arr.count) == len(py_meshes), \
                f"{name} LOD {lod_idx}: part count {int(cpp_arr.count)} != {len(py_meshes)}"
            lod = model.lods[lod_idx]
            for i in range(int(cpp_arr.count)):
                cm = cpp_arr.meshes[i]
                pm = py_meshes[i]
                cm_vc = int(cm.vertex_count)
                cm_fc = int(cm.face_count)
                assert cm_vc == len(pm.vertices), \
                    f"{name} LOD {lod_idx} part {i}: vert count {cm_vc} != {len(pm.vertices)}"
                assert cm_fc == len(pm.faces), \
                    f"{name} LOD {lod_idx} part {i}: face count {cm_fc} != {len(pm.faces)}"
                # C++ stores world-space positions; Python stores part-local.
                # Compute world-space for Python by adding part origin (in double,
                # matching the Python _vec_add path that lived in ase_from_3di3
                # (deleted Phase D 2026-05-17; logic preserved in libs/object C++ port)).
                part_idx = int(cm.part_index)
                ro = lod.render_objects[part_idx]
                origin = coords.render_space(ro.abs)  # double precision
                # Position check: compare C++ world-space vs Python world-space
                for v in range(cm_vc):
                    cv = cm.vertices[v]
                    pv = pm.vertices[v]
                    # Python world-space = render_space(origin) + local
                    py_world = (float(origin[0]) + float(pv[0]),
                                float(origin[1]) + float(pv[1]),
                                float(origin[2]) + float(pv[2]))
                    for c, attr in enumerate(("x", "y", "z")):
                        cval = float(getattr(cv, attr))
                        pval = ctypes.c_float(py_world[c]).value  # truncate to f32 as Python does
                        assert abs(cval - pval) < 1e-6, \
                            f"{name} LOD {lod_idx} part {i} vert {v}.{attr}: C={cval} P={pval}"
                # Face check (v[0..2], material_id, smoothing)
                for f in range(cm_fc):
                    cf = cm.faces[f]
                    pf = pm.faces[f]
                    for c in range(3):
                        assert int(cf.v[c]) == int(pf[c]), \
                            f"{name} LOD {lod_idx} part {i} face {f} corner {c}: C={int(cf.v[c])} P={int(pf[c])}"
                    assert int(cf.material_id) == int(pm.face_material_ids[f]), \
                        f"{name} LOD {lod_idx} part {i} face {f} material: C={int(cf.material_id)} P={int(pm.face_material_ids[f])}"
                    assert int(cf.smoothing_group_mask) == int(pm.smoothing_groups[f]), \
                        f"{name} LOD {lod_idx} part {i} face {f} smoothing: C={int(cf.smoothing_group_mask):#x} P={int(pm.smoothing_groups[f]):#x}"
                # material_id_set check
                cms = [int(cpp_arr.meshes[i].material_id_set[j]) for j in range(int(cpp_arr.meshes[i].material_id_set_count))]
                assert cms == list(pm.material_id_set), \
                    f"{name} LOD {lod_idx} part {i}: material_id_set {cms} != {pm.material_id_set}"
                # Per-corner UVs check
                for f in range(cm_fc):
                    for corner in range(3):
                        cd = cm.corners[f * 3 + corner]
                        p_uv = pm.face_uvs0[f * 3 + corner]
                        assert abs(float(cd.u0) - float(p_uv[0])) < 1e-5, \
                            f"{name} LOD {lod_idx} part {i} face {f} corner {corner} u0: C={float(cd.u0)} P={float(p_uv[0])}"
                        assert abs(float(cd.v0) - float(p_uv[1])) < 1e-5, \
                            f"{name} LOD {lod_idx} part {i} face {f} corner {corner} v0: C={float(cd.v0)} P={float(p_uv[1])}"
            free_array(cpp_arr)
            cpp_arr = None
    finally:
        if cpp_arr is not None:
            free_array(cpp_arr)
        threedi_ffi.free_model_3di3(model)
