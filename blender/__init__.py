"""
Blender addon for exporting Novalogic ASE files.
"""

from __future__ import annotations

bl_info = {
    "name": "OpenNova ASE Exporter",
    "author": "Taylor Finnell",
    "version": (0, 0, 2),
    "blender": (2, 80, 0),
    "location": "File > Export",
    "description": "Export Novalogic ASE files.",
    "warning": "",
    "doc_url": "",
    "category": "Import-Export",
}

import importlib

import bpy
from bpy.props import BoolProperty, FloatProperty, IntProperty, StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ExportHelper

# Hot-reload support
if "math_utils" in locals():
    importlib.reload(math_utils)
if "ase_exporter" in locals():
    importlib.reload(ase_exporter)
from . import ase_exporter
from . import math_utils


def _reload_modules():
    """Reload addon modules for development iteration."""
    importlib.reload(math_utils)
    importlib.reload(ase_exporter)


class EXPORT_OT_novalogic_ase(Operator, ExportHelper):
    """Export scene to Novalogic ASCII Scene Export (.ase) format."""

    bl_idname = "export_scene.novalogic_ase"
    bl_label = "Export Novalogic ASE"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".ase"
    filter_glob: StringProperty(default="*.ase", options={'HIDDEN'})

    include_normals: BoolProperty(
        name="Include Normals",
        description="Export vertex normals",
        default=True,
    )
    include_uvs: BoolProperty(
        name="Include UVs",
        description="Export texture coordinates",
        default=True,
    )
    include_bone_weights: BoolProperty(
        name="Include Bone Weights",
        description="Export skin weights for rigged meshes",
        default=True,
    )
    export_textures: BoolProperty(
        name="Export Textures",
        description="Save diffuse textures as TGA files next to the ASE",
        default=True,
    )
    scale: FloatProperty(
        name="Scale",
        description="Export scale multiplier",
        min=0.01,
        max=1000.0,
        soft_min=0.01,
        soft_max=100.0,
        default=1.0,
    )
    precision: IntProperty(
        name="Float Precision",
        description="Number of decimal places for float values",
        default=4,
        min=1,
        max=8,
    )

    def execute(self, context):
        try:
            _reload_modules()
            from .ase_exporter import AseExporter

            exporter = AseExporter()
            exporter.set_options(
                include_normals=self.include_normals,
                include_uvs=self.include_uvs,
                include_bone_weights=self.include_bone_weights,
                export_textures=self.export_textures,
                scale=self.scale,
                precision=self.precision,
            )
            if exporter.export_scene(context.scene, self.filepath):
                self.report({'INFO'}, f"Exported to {self.filepath}")
                return {'FINISHED'}
            self.report({'ERROR'}, "Export failed")
            return {'CANCELLED'}
        except Exception as exc:
            import traceback

            traceback.print_exc()
            self.report({'ERROR'}, f"Export failed: {exc}")
            return {'CANCELLED'}


def menu_func_export(self, context):
    self.layout.operator(EXPORT_OT_novalogic_ase.bl_idname, text="Novalogic ASE (.ase)")


classes = [
    EXPORT_OT_novalogic_ase,
]


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
