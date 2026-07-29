"""Shared module-level helpers for the scene_builder package.

_set_object_mode / _ensure_object_mode wrap Blender-5 mode changes with an
explicit operator context; _mtrx_to_center_rotation inverts the exporter's
MTRX swizzle chain. One module, so armature/meshes/overlays import them
without a cycle.
"""

from __future__ import annotations

import math

import bpy
from mathutils import Matrix


def _mtrx_to_center_rotation(mat_data):
    """Convert MTRX 4x4 matrix → Blender 3x3 rotation for a center point.

    Inverts build_matrix_from_axis (export_3di.cpp) + ASE parser swizzle chain.
    Returns a 4x4 Matrix or None if the matrix contains NaN (zero-axis sentinel).
    """
    m = [mat_data[i] for i in range(16)]
    if any(math.isnan(v) for v in m):
        return None
    ax0 = (m[10], -m[2],  m[6])
    ax1 = (-m[8],  m[0], -m[4])
    ax2 = (m[9],  -m[1],  m[5])
    return Matrix([
        [ ax1[1], -ax0[1],  ax2[1]],
        [-ax1[0],  ax0[0], -ax2[0]],
        [ ax1[2], -ax0[2],  ax2[2]],
    ]).to_4x4()


def _set_object_mode(obj, mode: str) -> None:
    """Set Blender mode with an explicit Blender 5 operator context."""
    if obj is None:
        raise ValueError("Cannot change Blender mode without an object")

    view_layer = bpy.context.view_layer
    view_layer.update()
    obj.select_set(True)
    view_layer.objects.active = obj

    with bpy.context.temp_override(
        object=obj,
        active_object=obj,
        selected_objects=[obj],
        selected_editable_objects=[obj],
    ):
        bpy.ops.object.mode_set(mode=mode)


def _ensure_object_mode() -> None:
    active = getattr(bpy.context, "active_object", None)
    if active and getattr(active, "mode", "OBJECT") != "OBJECT":
        _set_object_mode(active, "OBJECT")
