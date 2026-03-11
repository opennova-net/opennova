"""
Blender addon for exporting Novalogic ASE and animation files.
"""

from __future__ import annotations

bl_info = {
    "name": "Novalogic ASE & Anim Exporter",
    "author": "Taylor Finnell",
    "version": (0, 0, 1),
    "blender": (2, 80, 0),
    "location": "File > Import-Export",
    "description": "Export ASE files and Novalogic ADM/BAD animations; import Mixamo FBX for animation workflow",
    "warning": "",
    "doc_url": "",
    "category": "Import-Export",
}

import os
import bpy
import importlib
from bpy.props import (
    StringProperty, BoolProperty, FloatProperty, IntProperty,
    CollectionProperty,
)
from bpy.types import Operator, PropertyGroup
from bpy_extras.io_utils import ExportHelper, ImportHelper

# Hot-reload support
if "math_utils" in locals():
    importlib.reload(math_utils)
if "ase_exporter" in locals():
    importlib.reload(ase_exporter)
if "anim_exporter" in locals():
    importlib.reload(anim_exporter)
if "mixamo_importer" in locals():
    importlib.reload(mixamo_importer)
from . import math_utils
from . import ase_exporter
from . import anim_exporter
from . import mixamo_importer


def _reload_modules():
    """Reload all addon modules for development iteration."""
    importlib.reload(math_utils)
    importlib.reload(ase_exporter)
    importlib.reload(anim_exporter)
    importlib.reload(mixamo_importer)


# ==========================================================================
# Animation clip property group (used by EXPORT_OT_novalogic_anims)
# ==========================================================================

class NovalogicAnimClipItem(PropertyGroup):
    action_name: StringProperty(name="Action")
    include: BoolProperty(name="Include", default=True)
    loop: BoolProperty(name="Loop", default=True)
    translation: BoolProperty(name="Translation", default=True)
    is_reset: BoolProperty(name="Reset", default=False)
    filename: StringProperty(name="Filename", description="BAD filename (without extension)")


# ==========================================================================
# ASE Export operator
# ==========================================================================

class EXPORT_OT_novalogic_ase(Operator, ExportHelper):
    """Export scene to Novalogic ASCII Scene Export (.ase) format"""
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
        min=0.01, max=1000.0,
        soft_min=0.01, soft_max=100.0,
        default=1.0,
    )
    precision: IntProperty(
        name="Float Precision",
        description="Number of decimal places for float values",
        default=4, min=1, max=8,
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
            result = exporter.export_scene(context.scene, self.filepath)
            if result:
                self.report({'INFO'}, f"Exported to {self.filepath}")
                return {'FINISHED'}
            else:
                self.report({'ERROR'}, "Export failed")
                return {'CANCELLED'}
        except Exception as e:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"Export failed: {str(e)}")
            return {'CANCELLED'}


class EXPORT_OT_novalogic_anims(Operator, ExportHelper):
    """Export Novalogic ADM + BAD animations from armature NLA actions"""
    bl_idname = "export_scene.novalogic_anims"
    bl_label = "Export Novalogic Anims"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".adm"
    filter_glob: StringProperty(default="*.adm", options={'HIDDEN'})

    clip_items: CollectionProperty(type=NovalogicAnimClipItem)

    def invoke(self, context, event):
        _reload_modules()
        from .anim_exporter import build_clip_items_for_armature

        arm = build_clip_items_for_armature(self)
        if arm is None:
            self.report({'ERROR'}, "Select or activate an armature first")
            return {'CANCELLED'}
        if len(self.clip_items) == 0:
            self.report({'ERROR'}, "No NLA strip actions found on the armature")
            return {'CANCELLED'}

        if not self.filepath:
            self.filepath = f"{arm.name}.adm"

        return ExportHelper.invoke(self, context, event)

    def draw(self, context):
        layout = self.layout
        layout.label(text="Clip Flags")
        box = layout.box()
        row = box.row(align=True)
        row.label(text="Clip")
        row.label(text="Filename")
        row.label(text="Include")
        row.label(text="Loop")
        row.label(text="Trans")
        row.label(text="Reset")

        for item in self.clip_items:
            row = box.row(align=True)
            row.label(text=item.action_name)
            row.prop(item, "filename", text="")
            row.prop(item, "include", text="")
            row.prop(item, "loop", text="")
            row.prop(item, "translation", text="")
            row.prop(item, "is_reset", text="")

    def execute(self, context):
        try:
            _reload_modules()
            from .anim_exporter import (
                NovalogicAnimExporter,
                _get_active_armature,
                _sanitize_name,
                _is_valid_adm_anim_key,
                _ClipData,
            )
        except Exception as e:
            self.report({'ERROR'}, f"Failed to load exporter modules: {e}")
            return {'CANCELLED'}

        arm = _get_active_armature(context)
        if arm is None:
            self.report({'ERROR'}, "Select or activate an armature first")
            return {'CANCELLED'}

        selected = [c for c in self.clip_items if c.include]
        if not selected:
            self.report({'ERROR'}, "No clips selected for export")
            return {'CANCELLED'}

        reset_selected = [c for c in selected if c.is_reset]
        if len(reset_selected) == 0:
            self.report({'ERROR'}, "Select exactly one reset clip")
            return {'CANCELLED'}
        if len(reset_selected) > 1:
            self.report({'ERROR'}, "Multiple reset clips selected")
            return {'CANCELLED'}

        action_map = {a.name: a for a in bpy.data.actions}
        clips = []
        invalid_adm_keys = []
        for item in selected:
            action = action_map.get(item.action_name)
            if action is None:
                self.report({'WARNING'}, f"Skipping missing action: {item.action_name}")
                continue
            if not item.is_reset and not _is_valid_adm_anim_key(item.action_name):
                invalid_adm_keys.append(item.action_name)
                continue
            start = int(action.frame_start)
            end = int(action.frame_end)
            if end < start:
                self.report({'WARNING'}, f"Skipping short action: {item.action_name}")
                continue
            flags = 0
            if item.loop:
                flags |= 0x01
            if item.translation:
                flags |= 0x02
            bad_name = item.filename if item.filename else _sanitize_name(item.action_name)
            clips.append(
                _ClipData(
                    action=action,
                    action_name=item.action_name,
                    bad_name=bad_name,
                    start_frame=start,
                    end_frame=end,
                    is_reset=item.is_reset,
                    flags=flags,
                )
            )

        if invalid_adm_keys:
            invalid_adm_keys.sort()
            self.report(
                {'ERROR'},
                "Invalid ADM keys (must match anim_*): "
                + ", ".join(invalid_adm_keys),
            )
            return {'CANCELLED'}

        if not clips:
            self.report({'ERROR'}, "No valid clips to export")
            return {'CANCELLED'}

        out_adm = bpy.path.ensure_ext(self.filepath, ".adm")
        out_dir = os.path.dirname(out_adm) or "."
        os.makedirs(out_dir, exist_ok=True)

        exporter = NovalogicAnimExporter(context, arm)
        reset_clip = next((c for c in clips if c.is_reset), None)
        if reset_clip is None:
            self.report({'ERROR'}, "No reset clip selected")
            return {'CANCELLED'}
        exporter.configure_from_reset_clip(reset_clip)

        old_frame = context.scene.frame_current
        old_ad = arm.animation_data
        old_action = old_ad.action if old_ad else None
        old_use_nla = old_ad.use_nla if old_ad else True
        try:
            for clip in clips:
                bad_path = os.path.join(out_dir, f"{clip.bad_name}.bad")
                exporter.write_bad(bad_path, clip)
            exporter.write_adm(out_adm, clips)
        except Exception as e:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"Animation export failed: {e}")
            return {'CANCELLED'}
        finally:
            context.scene.frame_set(old_frame)
            if old_ad:
                old_ad.action = old_action
                old_ad.use_nla = old_use_nla

        self.report({'INFO'}, f"Exported {len(clips)} BAD clip(s) + {os.path.basename(out_adm)}")
        return {'FINISHED'}


# ==========================================================================
# Mixamo FBX Import operator (File > Import — animation workflow only)
# ==========================================================================

class IMPORT_OT_mixamo_novalogic(Operator, ImportHelper):
    """Import Mixamo FBX files for Novalogic pipeline"""
    bl_idname = "import_scene.mixamo_novalogic"
    bl_label = "Import Mixamo FBX for Novalogic"
    bl_options = {'REGISTER', 'UNDO'}

    filter_glob: StringProperty(default="*.fbx", options={'HIDDEN'})
    files: CollectionProperty(type=bpy.types.OperatorFileListElement)

    strip_prefix: StringProperty(
        name="Bone Name Prefix",
        description="Prefix to strip from Mixamo bone names",
        default="mixamorig:",
    )
    create_root_motion: BoolProperty(
        name="Create Root Motion Bone",
        description="Add root_motion bone as parent of BN01",
        default=True,
    )
    default_looped: BoolProperty(
        name="Default Looped",
        description="Set looped flag on imported animations",
        default=True,
    )
    default_translation: BoolProperty(
        name="Default Translation",
        description="Set translation flag on imported animations",
        default=True,
    )

    def execute(self, context):
        try:
            _reload_modules()
            from .mixamo_importer import import_mixamo

            directory = os.path.dirname(self.filepath)
            filenames = [f.name for f in self.files]
            if not filenames:
                filenames = [os.path.basename(self.filepath)]

            import_mixamo(
                directory=directory,
                files=filenames,
                strip_prefix=self.strip_prefix,
                create_root_motion=self.create_root_motion,
                default_looped=self.default_looped,
                default_translation=self.default_translation,
            )
            self.report({'INFO'}, f"Imported {len(filenames)} Mixamo FBX file(s)")
            return {'FINISHED'}
        except Exception as e:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"Mixamo import failed: {e}")
            return {'CANCELLED'}


# ==========================================================================
# Registration
# ==========================================================================

def menu_func_import(self, context):
    self.layout.operator(IMPORT_OT_mixamo_novalogic.bl_idname, text="Mixamo FBX for Novalogic (.fbx)")


def menu_func_export(self, context):
    self.layout.operator(EXPORT_OT_novalogic_ase.bl_idname, text="Novalogic ASE (.ase)")
    self.layout.operator(EXPORT_OT_novalogic_anims.bl_idname, text="Novalogic Anims (.adm + .bad)")


classes = [
    NovalogicAnimClipItem,
    IMPORT_OT_mixamo_novalogic,
    EXPORT_OT_novalogic_ase,
    EXPORT_OT_novalogic_anims,
]


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
