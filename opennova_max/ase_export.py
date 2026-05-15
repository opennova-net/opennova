"""3ds Max-side .ase export using the C++ writer chain.

Workflow:
  1. Read a stock 3DI into the IR (pyopennova.threedi_ffi)
  2. Convert each LOD's mesh data into FlatMeshArrays via the C++ chain
  3. Import each FlatMeshArray's meshes into Max via flat_mesh_bridge
  4. Optionally: artist edits in Max (no-op in headless roundtrip)
  5. Export Max meshes back to FlatMeshes via flat_mesh_bridge
  6. Write final .ase via pyopennova.ase_writer_ffi.write_ase_files_from_flat_meshes

Symmetric to opennova_blender/ase_export.py. pymxs import deferred to
function body so the module loads in non-Max environments.
"""
from __future__ import annotations

import ctypes
import warnings
from pathlib import Path
from typing import Any

from pyopennova._native import load_lib
from pyopennova.ase_writer_ffi import write_ase_files_from_flat_meshes
from pyopennova.flat_mesh_ffi import (
    FlatMesh,
    FlatMeshArray,
    FlatMeshBoneInfluence,
    FlatMeshFace,
    FlatMeshFaceCornerData,
    FlatMeshVertex,
    flat_meshes_from_3di3,
    free_array,
)
from opennova_max.flat_mesh_bridge import (
    attach_flat_mesh_weights_to_max_object,
    export_max_to_flat_mesh,
    import_flat_mesh_to_max,
    read_weights_from_max_object,
)


def _is_skinned_ir(ir) -> bool:
    """Returns True if the IR model is skinned (matches the C++
    is_skinned(ir) at libs/object/src/write_ase_files.cpp:104-106).

    Mirror of opennova_blender.ase_export._is_skinned_ir.
    """
    # THREEDI_MESH_SKINNED == 2 per libs/threedi/include/threedi/threedi_3di3.h:62-66
    from pyopennova.threedi_ffi import THREEDI_MESH_SKINNED
    try:
        return int(getattr(getattr(ir, "header", None), "mesh_type", 0)) == int(THREEDI_MESH_SKINNED)
    except (AttributeError, TypeError):
        return False


def _max_bone_count_from_ir(ir) -> int:
    """Return the number of bones the IR's matrix table declares.

    ``flat_meshes_from_3di3`` stores bone_index values into each
    FlatMeshBoneInfluence. The maximum bone_index across all influences is
    bounded by the IR's MTRX (bone matrix) count. We allocate one Dummy
    scene node per slot so ``attach_flat_mesh_weights_to_max_object`` can
    look up by FlatMesh bone_index directly.

    The IR wrapper (pyopennova.threedi_ffi) exposes ``matrix_count`` as a
    top-level property (not on header), reading ``self.mtrx.count``.
    """
    try:
        n = getattr(ir, "matrix_count", None)
        if n is not None:
            return int(n)
        mats = getattr(ir, "matrices", None)
        if mats is not None:
            return len(mats)
    except (AttributeError, TypeError):
        pass
    return 0


def _required_bone_node_count(flat_mesh: FlatMesh) -> int:
    """Return max referenced FlatMesh bone index + 1 for one mesh."""
    if not flat_mesh.vertex_influence_offsets:
        return 0
    vertex_count = int(flat_mesh.vertex_count)
    if vertex_count <= 0:
        return 0
    total_influences = int(flat_mesh.vertex_influence_offsets[vertex_count])
    max_index = -1
    for i in range(total_influences):
        max_index = max(max_index, int(flat_mesh.vertex_influences[i].bone_index))
    return max_index + 1


def export_ase_via_max(ir, primary_path: Any,
                         *,
                         include_collisions: bool = True,
                         include_occlusion: bool = True,
                         include_lights: bool = True) -> list[str]:
    """Headless Max-roundtripping .ase export.

    For each LOD of the IR: load FlatMeshes via C++, import each into Max
    as Editable_Mesh via the bridge, immediately export back via the bridge,
    then feed all per-LOD FlatMeshArrays into the C++ writer chain.

    For skinned IRs (mesh_type == THREEDI_MESH_SKINNED), each imported mesh
    gets a Skin modifier populated from the source FlatMesh's CSR weights;
    those weights are read back into the exported FlatMesh before the
    C-side writer consumes the array. Mirrors opennova_blender/ase_export.py.

    Returns list of .ase file paths written.

    Requires pymxs (3ds Max Python module). Raises RuntimeError via the
    bridge's _rt() helper if pymxs is unavailable.
    """
    import pymxs
    rt = pymxs.runtime

    per_lod_arrays: list[FlatMeshArray] = []
    max_nodes_to_cleanup: list[Any] = []
    max_bones_to_cleanup: list[Any] = []
    py_flat_meshes_to_keep: list[FlatMesh] = []

    skinned = _is_skinned_ir(ir)
    bone_nodes: list[Any] = []
    if skinned:
        # Pre-create one persistent Dummy node per bone slot. The Skin
        # modifier holds references to these; they must outlive the
        # per-mesh round-trip loop. They get rt.delete()'d in the finally.
        bone_count = _max_bone_count_from_ir(ir)
        for bi in range(bone_count):
            d = rt.Dummy()
            d.name = f"bone_{bi}"
            bone_nodes.append(d)
            max_bones_to_cleanup.append(d)

    try:
        for lod_idx in range(int(ir.lod_count)):
            src_arr = flat_meshes_from_3di3(
                ir,
                lod_idx,
                include_empty_parts=True,
                track_bone_data=True,
                preserve_source_indexing=True,
            )

            roundtripped: list[FlatMesh] = []
            for mi in range(src_arr.count):
                src_fm = src_arr.meshes[mi]
                name = src_fm.name.decode("utf-8", errors="replace").rstrip("\x00")
                if not name:
                    name = f"lod{lod_idx}_part{mi}"

                max_obj = import_flat_mesh_to_max(src_fm, name)
                max_nodes_to_cleanup.append(max_obj)

                # Skinning: attach Skin modifier from the source FlatMesh's
                # CSR weights BEFORE export_max_to_flat_mesh, so the Skin
                # modifier exists for read_weights_from_max_object below.
                if skinned:
                    while len(bone_nodes) < _required_bone_node_count(src_fm):
                        bi = len(bone_nodes)
                        d = rt.Dummy()
                        d.name = f"bone_{bi}"
                        bone_nodes.append(d)
                        max_bones_to_cleanup.append(d)
                    attach_flat_mesh_weights_to_max_object(
                        src_fm, max_obj, bone_nodes,
                    )

                exported_fm = export_max_to_flat_mesh(
                    max_obj, name, int(src_fm.part_index),
                    (float(src_fm.origin[0]),
                     float(src_fm.origin[1]),
                     float(src_fm.origin[2])),
                )

                # Propagate weights into the exported FlatMesh.
                if skinned:
                    offsets_arr, influences_arr = read_weights_from_max_object(
                        max_obj, exported_fm.vertex_count,
                    )
                    exported_fm.vertex_influence_offsets = ctypes.cast(
                        offsets_arr, ctypes.POINTER(ctypes.c_int32),
                    )
                    exported_fm.vertex_influences = ctypes.cast(
                        influences_arr, ctypes.POINTER(FlatMeshBoneInfluence),
                    )
                    # Keep the new arrays alive across the FFI call.
                    exported_fm._py_offsets = offsets_arr
                    exported_fm._py_influences = influences_arr

                roundtripped.append(exported_fm)
                py_flat_meshes_to_keep.append(exported_fm)

            free_array(src_arr)

            out_count = len(roundtripped)
            arr = FlatMeshArray()
            lib = load_lib()
            rc = lib.object_flat_mesh_array_alloc(out_count, ctypes.byref(arr))
            if rc != 0:
                raise RuntimeError(f"object_flat_mesh_array_alloc failed (rc={rc})")

            for mi, exported_fm in enumerate(roundtripped):
                ctypes.memmove(
                    ctypes.byref(arr.meshes[mi]),
                    ctypes.byref(exported_fm),
                    ctypes.sizeof(FlatMesh),
                )
            per_lod_arrays.append(arr)

        paths = write_ase_files_from_flat_meshes(
            per_lod_arrays, ir, primary_path,
            include_collisions=include_collisions,
            include_occlusion=include_occlusion,
            include_lights=include_lights,
        )
        return paths

    finally:
        # Null out per-slot pointer fields BEFORE free_array (cross-heap
        # protection — see opennova_blender/ase_export.py for context).
        for arr in per_lod_arrays:
            try:
                for mi in range(arr.count):
                    arr.meshes[mi].vertices = ctypes.POINTER(FlatMeshVertex)()
                    arr.meshes[mi].faces = ctypes.POINTER(FlatMeshFace)()
                    arr.meshes[mi].corners = ctypes.POINTER(FlatMeshFaceCornerData)()
                    arr.meshes[mi].material_id_set = ctypes.POINTER(ctypes.c_int32)()
                    arr.meshes[mi].vertex_influence_offsets = ctypes.POINTER(ctypes.c_int32)()
                    arr.meshes[mi].vertex_influences = ctypes.POINTER(FlatMeshBoneInfluence)()
                free_array(arr)
            except Exception as exc:
                warnings.warn(
                    f"free_array failed during ase_export cleanup: {exc}",
                    RuntimeWarning,
                    stacklevel=2,
                )
        # Remove skinned mesh nodes BEFORE bone Dummies. The Skin modifier
        # holds a reference to each bone node; removing the mesh first
        # releases that reference so the Dummy delete is clean.
        for node in max_nodes_to_cleanup:
            try:
                rt.delete(node)
            except Exception as exc:
                warnings.warn(
                    f"rt.delete failed during ase_export cleanup: {exc}",
                    RuntimeWarning,
                    stacklevel=2,
                )
        for bone in max_bones_to_cleanup:
            try:
                rt.delete(bone)
            except Exception as exc:
                warnings.warn(
                    f"rt.delete(bone) failed during ase_export cleanup: {exc}",
                    RuntimeWarning,
                    stacklevel=2,
                )
