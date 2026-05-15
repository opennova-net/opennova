"""Blender-side .ase export using the C++ writer chain.

Workflow:
  1. Read a stock 3DI into the IR (pyopennova.threedi_ffi)
  2. Convert each LOD's mesh data into FlatMeshArrays via the C++ chain
     (pyopennova.flat_mesh_ffi.flat_meshes_from_3di3)
  3. Import each FlatMeshArray's meshes into bpy via flat_mesh_bridge
  4. Optionally: artist edits in Blender (no-op in headless roundtrip)
  5. Export bpy meshes back to FlatMeshes via flat_mesh_bridge
  6. Write final .ase via pyopennova.ase_writer_ffi.write_ase_files_from_flat_meshes
     (which combines edited meshes + IR side-data)

This module exposes a single high-level function `export_ase_via_blender(ir, primary_path, ...)`
that orchestrates the entire workflow with no actual artist edit step.

bpy import is deferred into function bodies so the module loads in non-bpy envs.
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
from opennova_blender.flat_mesh_bridge import (
    attach_flat_mesh_weights_to_bpy_object,
    export_bpy_to_flat_mesh,
    import_flat_mesh_to_bpy,
    read_weights_from_bpy_object,
)


def _is_skinned_ir(ir) -> bool:
    """Returns True if the IR model is skinned (matches the C++
    is_skinned(ir) at libs/object/src/write_ase_files.cpp:104-106).
    """
    # THREEDI_MESH_SKINNED == 2 per libs/threedi/include/threedi/threedi_3di3.h:62-66
    # (THREEDI_MESH_INVALID=0, THREEDI_MESH_BASIC=1, THREEDI_MESH_SKINNED=2).
    from pyopennova.threedi_ffi import THREEDI_MESH_SKINNED
    try:
        return int(getattr(getattr(ir, "header", None), "mesh_type", 0)) == int(THREEDI_MESH_SKINNED)
    except (AttributeError, TypeError):
        return False


def export_ase_via_blender(ir, primary_path: Any,
                              *,
                              include_collisions: bool = True,
                              include_occlusion: bool = True,
                              include_lights: bool = True) -> list[str]:
    """Headless Blender-roundtripping .ase export.

    For each LOD of the IR: load FlatMeshes via C++, push to bpy via the
    bridge, immediately pull back via the bridge, then feed all per-LOD
    FlatMeshArrays into the C++ writer chain. The bpy round-trip exercises
    the bridge marshaling.

    Returns list of .ase file paths written.

    Requires bpy (Blender Python module). Raises ImportError if unavailable.
    """
    import bpy

    per_lod_arrays: list[FlatMeshArray] = []
    bpy_meshes_to_cleanup: list[Any] = []
    bpy_objects_to_cleanup: list[Any] = []
    py_flat_meshes_to_keep: list[FlatMesh] = []  # keep _py_* refs alive

    try:
        for lod_idx in range(int(ir.lod_count)):
            # 1. Build source FlatMeshes from IR via C++.
            src_arr = flat_meshes_from_3di3(
                ir,
                lod_idx,
                include_empty_parts=True,
                track_bone_data=True,
                preserve_source_indexing=True,
            )

            # 2. Push to bpy + pull back, accumulating into a new FlatMeshArray.
            # We hold (bm, fm) pairs so we can reconstruct the round-tripped
            # FlatMeshArray with one entry per source mesh.
            roundtripped: list[FlatMesh] = []
            skinned = _is_skinned_ir(ir)
            for mi in range(src_arr.count):
                src_fm = src_arr.meshes[mi]
                name = src_fm.name.decode("utf-8", errors="replace").rstrip("\x00")
                if not name:
                    name = f"lod{lod_idx}_part{mi}"
                bm = bpy.data.meshes.new(name)
                import_flat_mesh_to_bpy(src_fm, bm)
                bpy_meshes_to_cleanup.append(bm)

                # Skinning: wrap in an Object so vertex_groups can live on it,
                # attach the FlatMesh's weights, then read back into exported_fm.
                bpy_obj = None
                if skinned:
                    bpy_obj = bpy.data.objects.new(name + "_obj", bm)
                    attach_flat_mesh_weights_to_bpy_object(src_fm, bpy_obj)
                    bpy_objects_to_cleanup.append(bpy_obj)

                exported_fm = export_bpy_to_flat_mesh(
                    bm,
                    name,
                    int(src_fm.part_index),
                    (float(src_fm.origin[0]),
                     float(src_fm.origin[1]),
                     float(src_fm.origin[2])),
                )

                # Propagate weights into the exported FlatMesh.
                if bpy_obj is not None:
                    offsets_arr, influences_arr = read_weights_from_bpy_object(
                        bpy_obj, exported_fm.vertex_count,
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

            # 3. Free the source FlatMeshArray (we now hold round-tripped versions).
            free_array(src_arr)

            # 4. Build a C-side FlatMeshArray populated with the round-tripped meshes.
            out_count = len(roundtripped)
            arr = FlatMeshArray()
            lib = load_lib()
            rc = lib.object_flat_mesh_array_alloc(out_count, ctypes.byref(arr))
            if rc != 0:
                raise RuntimeError(f"object_flat_mesh_array_alloc failed (rc={rc})")

            # 5. Bytewise copy each exported FlatMesh into the C-allocated array slot.
            # The Python-owned _py_* arrays (held in py_flat_meshes_to_keep) keep
            # the underlying buffers alive for the duration of the C call.
            for mi, exported_fm in enumerate(roundtripped):
                ctypes.memmove(
                    ctypes.byref(arr.meshes[mi]),
                    ctypes.byref(exported_fm),
                    ctypes.sizeof(FlatMesh),
                )

            per_lod_arrays.append(arr)

        # 6. Write .ase files via the new C++ entry.
        paths = write_ase_files_from_flat_meshes(
            per_lod_arrays, ir, primary_path,
            include_collisions=include_collisions,
            include_occlusion=include_occlusion,
            include_lights=include_lights,
        )
        return paths

    finally:
        # Free each C-allocated FlatMeshArray. The Python _py_* attrs on the
        # exported FlatMeshes will get GC'd as py_flat_meshes_to_keep goes out
        # of scope; that's fine -- we've already done the C-side write.
        # Null out each slot's per-mesh pointer fields BEFORE calling
        # free_array. The pointers reference Python-owned ctypes arrays
        # (stashed as _py_* attrs on the exported FlatMeshes); calling
        # std::free on them via the C side would be a cross-heap free
        # (Python uses a different allocator than the DLL's CRT on
        # Windows MSVC) -> heap corruption / segfault.
        #
        # By nulling first, object_flat_mesh_array_free calls
        # std::free(nullptr) for each per-mesh buffer (a no-op), and
        # only frees the C-allocated meshes container itself.
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
        # Remove the Object wrappers BEFORE the underlying Meshes; the Objects
        # hold a user-count reference to the Mesh, so removing meshes first
        # would fail with users > 0.
        for bpy_obj in bpy_objects_to_cleanup:
            try:
                bpy.data.objects.remove(bpy_obj)
            except Exception as exc:
                warnings.warn(
                    f"bpy.data.objects.remove failed during ase_export cleanup: {exc}",
                    RuntimeWarning,
                    stacklevel=2,
                )
        # Remove the temporary bpy meshes (the meshes themselves; the
        # _tri_tmp meshes stashed in exported_fm._py_tri_mesh will GC with
        # py_flat_meshes_to_keep going out of scope).
        for bm in bpy_meshes_to_cleanup:
            try:
                bpy.data.meshes.remove(bm)
            except Exception as exc:
                warnings.warn(
                    f"bpy.data.meshes.remove failed during ase_export cleanup: {exc}",
                    RuntimeWarning,
                    stacklevel=2,
                )
        for fm in py_flat_meshes_to_keep:
            try:
                bpy.data.meshes.remove(fm._py_tri_mesh)
            except Exception as exc:
                warnings.warn(
                    f"bpy.data.meshes.remove(_py_tri_mesh) failed during ase_export cleanup: {exc}",
                    RuntimeWarning,
                    stacklevel=2,
                )
