"""
Blender addon for importing Novalogic .3di/.bad/.adm files and exporting ASE.
Unified from the import pipeline (.scratch/onblender) and the mature ASE exporter.
"""

from __future__ import annotations

bl_info = {
    "name": "Novalogic 3DI Importer & ASE Exporter",
    "author": "Taylor Finnell",
    "version": (0, 0, 1),
    "blender": (2, 80, 0),
    "location": "File > Import-Export",
    "description": "Import Novalogic 3D models and Export ASE files",
    "warning": "",
    "doc_url": "",
    "category": "Import-Export",
}

import os
import bpy
import importlib
from bpy.props import (
    StringProperty, BoolProperty, FloatProperty, IntProperty,
    EnumProperty, CollectionProperty,
)
from bpy.types import Operator, Panel, PropertyGroup, UIList
from bpy_extras.io_utils import ExportHelper, ImportHelper

# Hot-reload support
if "math_utils" in locals():
    importlib.reload(math_utils)
if "blender_importer" in locals():
    importlib.reload(blender_importer)
if "scene_builder" in locals():
    importlib.reload(scene_builder)
if "ase_exporter" in locals():
    importlib.reload(ase_exporter)
if "anim_exporter" in locals():
    importlib.reload(anim_exporter)
if "mixamo_importer" in locals():
    importlib.reload(mixamo_importer)

from . import math_utils
from . import blender_importer
from . import scene_builder
from . import ase_exporter
from . import anim_exporter
from . import mixamo_importer


def _reload_modules():
    """Reload all addon modules for development iteration."""
    importlib.reload(math_utils)
    importlib.reload(blender_importer)
    importlib.reload(scene_builder)
    importlib.reload(ase_exporter)
    importlib.reload(anim_exporter)
    importlib.reload(mixamo_importer)


# ==========================================================================
# Import UI (from .scratch/onblender)
# ==========================================================================

class NovalogicWeaponItem(PropertyGroup):
    """Property group for weapon/item entries"""
    name: StringProperty(name="Name")
    item_type: StringProperty(name="Type")
    has_model: BoolProperty(name="Has Model", default=False)
    has_animations: BoolProperty(name="Has Animations", default=False)
    context_data: StringProperty(name="Context Data")
    graphic_name: StringProperty(name="Graphic")


class NovalogicAnimClipItem(PropertyGroup):
    action_name: StringProperty(name="Action")
    include: BoolProperty(name="Include", default=True)
    loop: BoolProperty(name="Loop", default=True)
    translation: BoolProperty(name="Translation", default=True)
    is_reset: BoolProperty(name="Reset", default=False)
    filename: StringProperty(name="Filename", description="BAD filename (without extension)")



class NovalogicPanelProperties(PropertyGroup):
    """Properties for the Novalogic panel"""
    resource_directory: StringProperty(
        name="Resource Directory",
        description="Path to Novalogic resource files",
        subtype='DIR_PATH',
        default="",
    )
    weapon_items: CollectionProperty(type=NovalogicWeaponItem)
    selected_item_index: IntProperty(name="Selected Item", default=-1)
    search_filter: StringProperty(
        name="Search",
        description="Filter items by name",
        default="",
    )
    import_main_model: BoolProperty(name="Main Model", default=True)
    import_arms_model: BoolProperty(name="Arms Model", default=True)
    import_animations: BoolProperty(name="Animations", default=True)
    import_collisions: BoolProperty(name="Collisions", default=True)
    import_occlusion: BoolProperty(name="Occlusion", default=True)
    import_lights: BoolProperty(name="Lights", default=True)
    output_directory: StringProperty(
        name="Output Directory",
        subtype='DIR_PATH',
    )
    active_tab: EnumProperty(
        items=[
            ('ITEMS', "Items", "Show items"),
            ('WEAPONS', "Weapons", "Show weapons"),
        ],
        default='ITEMS',
    )


class VIEW3D_PT_novalogic_panel(Panel):
    """OpenNova Importer Panel in 3D View sidebar"""
    bl_label = "OpenNova Importer"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "OpenNova Importer"

    def draw(self, context):
        layout = self.layout
        props = context.scene.novalogic_props

        layout.prop(props, "resource_directory")
        layout.prop(props, "output_directory")
        layout.operator("novalogic.scan_directory", text="Scan Directory", icon='FILE_REFRESH')
        layout.separator()

        if props.weapon_items:
            row = layout.row(align=True)
            row.prop(props, "active_tab", expand=True)

            tab_label = "Weapons" if props.active_tab == 'WEAPONS' else "Items"
            layout.label(text=f"Available {tab_label}:")
            layout.prop(props, "search_filter", icon='VIEWZOOM')
            row = layout.row()
            row.template_list(
                "NOVALOGIC_UL_weapon_list", "",
                props, "weapon_items",
                props, "selected_item_index",
                rows=8,
            )

            if 0 <= props.selected_item_index < len(props.weapon_items):
                selected = props.weapon_items[props.selected_item_index]
                expected_type = "weapon" if props.active_tab == 'WEAPONS' else "item"
                if selected.item_type == expected_type:
                    layout.separator()
                    layout.label(text=f"Import Options for {selected.name}:")
                    box = layout.box()
                    if selected.has_model:
                        box.prop(props, "import_main_model")
                        if selected.item_type == "weapon":
                            box.prop(props, "import_arms_model")
                    if selected.has_animations:
                        box.prop(props, "import_animations")
                    box.prop(props, "import_collisions")
                    box.prop(props, "import_occlusion")
                    box.prop(props, "import_lights")
                    layout.operator("novalogic.import_selected", text="Import Selected", icon='IMPORT')
        else:
            layout.label(text="No items found. Select directory and scan.")

        # Dependencies — only show when something is wrong
        try:
            from .dependencies import check_native_library
            native_ok, native_info = check_native_library()
        except Exception as e:
            native_ok, native_info = False, str(e)

        if not native_ok:
            layout.separator()
            dep_box = layout.box()
            dep_box.label(text="Native library: missing", icon='ERROR')
            dep_box.label(text=f"  {native_info}", icon='BLANK1')



class VIEW3D_PT_opennova_oed_panel(Panel):
    """OpenNova OED Panel in 3D View sidebar"""
    bl_label = "OpenNova OED"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "OpenNova OED"

    def draw(self, context):
        layout = self.layout
        layout.label(text="Coming soon.")


class NOVALOGIC_UL_weapon_list(UIList):
    """UI List for weapons and items"""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname):
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            row = layout.row(align=True)
            label = item.name
            if item.graphic_name:
                label = f"{item.name} ({item.graphic_name})"
            if item.item_type == 'weapon':
                row.label(text=label, icon='TOOL_SETTINGS')
            else:
                row.label(text=label, icon='OBJECT_DATA')
            sub = row.row(align=True)
            sub.alignment = 'RIGHT'
            if item.has_model:
                sub.label(text="", icon='MESH_DATA')
            if item.has_animations:
                sub.label(text="", icon='ANIM_DATA')
        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            layout.label(text=item.name)

    def filter_items(self, context, data, propname):
        items = getattr(data, propname)
        props = context.scene.novalogic_props
        search = props.search_filter.lower()
        tab_type = "weapon" if props.active_tab == 'WEAPONS' else "item"
        flags = []
        for item in items:
            if item.item_type != tab_type:
                flags.append(0)
            elif search and search not in item.name.lower() and search not in item.graphic_name.lower():
                flags.append(0)
            else:
                flags.append(self.bitflag_filter_item)
        return flags, []


class NOVALOGIC_OT_scan_directory(Operator):
    """Scan directory for weapons and items"""
    bl_idname = "novalogic.scan_directory"
    bl_label = "Scan Directory"

    def execute(self, context):
        props = context.scene.novalogic_props
        if not props.resource_directory:
            self.report({'ERROR'}, "Please select a directory")
            return {'CANCELLED'}
        try:
            props.weapon_items.clear()
            from .blender_importer import scan_and_parse_directory
            weapons, items = scan_and_parse_directory(props.resource_directory)

            for w in weapons:
                item = props.weapon_items.add()
                item.name = w.name
                item.item_type = "weapon"
                item.has_model = bool(w.graphic1.main)
                item.has_animations = bool(w.anim_adm)
                item.context_data = w.name
                item.graphic_name = w.graphic1.main if w.graphic1.main else ""

            for gi in items:
                item = props.weapon_items.add()
                item.name = gi.name
                item.item_type = "item"
                item.has_model = bool(gi.graphic_us)
                item.has_animations = bool(gi.anim_def)
                item.context_data = gi.name
                item.graphic_name = gi.graphic_us if gi.graphic_us else ""

            self.report({'INFO'}, f"Found {len(weapons)} weapons and {len(items)} items")
        except Exception as e:
            self.report({'ERROR'}, f"Error scanning: {e}")
            return {'CANCELLED'}
        return {'FINISHED'}


class NOVALOGIC_OT_import_selected(Operator):
    """Import the selected weapon/item"""
    bl_idname = "novalogic.import_selected"
    bl_label = "Import Selected"

    def execute(self, context):
        props = context.scene.novalogic_props
        if props.selected_item_index < 0 or props.selected_item_index >= len(props.weapon_items):
            self.report({'ERROR'}, "No item selected")
            return {'CANCELLED'}

        selected = props.weapon_items[props.selected_item_index]

        if not props.output_directory:
            self.report({'ERROR'}, "Output directory must be set")
            return {'CANCELLED'}

        try:
            _reload_modules()
            from .blender_importer import import_basic_model

            output_dir = props.output_directory

            if props.import_main_model and selected.has_model:
                result = import_basic_model(
                    base_dir=props.resource_directory,
                    item_name=selected.name,
                    item_type=selected.item_type,
                    import_arms=props.import_arms_model,
                    import_animations=props.import_animations and selected.has_animations,
                    import_collisions=props.import_collisions,
                    import_occlusion=props.import_occlusion,
                    import_lights=props.import_lights,
                    output_dir=output_dir,
                )
                if result:
                    self.report({'INFO'}, f"Imported {selected.name}")
                else:
                    self.report({'ERROR'}, f"Failed to import {selected.name}: no model files found")
                    return {'CANCELLED'}
            else:
                self.report({'WARNING'}, "No model to import")
        except Exception as e:
            import traceback
            traceback.print_exc()
            msg = str(e)
            if len(msg) > 200:
                msg = msg[:200] + "..."
            self.report({'ERROR'}, f"Import failed: {msg}")
            return {'CANCELLED'}

        # Auto-frame the NLA editor view on all strips
        for area in bpy.context.screen.areas:
            if area.type == 'NLA_EDITOR':
                for region in area.regions:
                    if region.type == 'WINDOW':
                        with bpy.context.temp_override(area=area, region=region):
                            bpy.ops.nla.view_all()
                        break
                break

        return {'FINISHED'}


# ==========================================================================
# Loose .3di Import operator (File > Import) — disabled, kept for reference
# ==========================================================================

class IMPORT_OT_novalogic_3di(Operator, ImportHelper):
    """Import a Novalogic 3DI model file"""
    bl_idname = "import_scene.novalogic_3di"
    bl_label = "Import Novalogic 3DI"
    bl_options = {'REGISTER', 'UNDO'}

    filter_glob: StringProperty(default="*.3di", options={'HIDDEN'})
    import_collisions: BoolProperty(name="Collisions", default=True)
    import_occlusion: BoolProperty(name="Occlusion", default=True)
    import_lights: BoolProperty(name="Lights", default=True)
    export_project: BoolProperty(
        name="Export project files",
        default=False,
    )
    output_directory: StringProperty(
        name="Output Directory",
        subtype='DIR_PATH',
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "import_collisions")
        layout.prop(self, "import_occlusion")
        layout.prop(self, "import_lights")
        layout.prop(self, "export_project")
        if self.export_project:
            layout.prop(self, "output_directory")

    def execute(self, context):
        if self.export_project and not self.output_directory:
            self.report({'ERROR'}, "Output directory must be set when exporting project files")
            return {'CANCELLED'}

        try:
            _reload_modules()
            from .blender_importer import import_loose_3di

            output_dir = self.output_directory if self.export_project else ""

            if import_loose_3di(self.filepath,
                                import_collisions=self.import_collisions,
                                import_occlusion=self.import_occlusion,
                                import_lights=self.import_lights,
                                output_dir=output_dir):
                self.report({'INFO'}, f"Imported {os.path.basename(self.filepath)}")
                return {'FINISHED'}
            else:
                self.report({'ERROR'}, "Import returned no result")
                return {'CANCELLED'}
        except Exception as e:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"Import failed: {e}")
            return {'CANCELLED'}


# ==========================================================================
# ASE Export operator (kept from mature blender/ version)
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
# Mixamo FBX Import operator (File > Import)
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
    NovalogicWeaponItem,
    NovalogicAnimClipItem,
    NovalogicPanelProperties,
    NOVALOGIC_UL_weapon_list,
    VIEW3D_PT_novalogic_panel,
    VIEW3D_PT_opennova_oed_panel,
    NOVALOGIC_OT_scan_directory,
    NOVALOGIC_OT_import_selected,
    IMPORT_OT_mixamo_novalogic,
    EXPORT_OT_novalogic_ase,
    EXPORT_OT_novalogic_anims,
]


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.novalogic_props = bpy.props.PointerProperty(type=NovalogicPanelProperties)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    del bpy.types.Scene.novalogic_props
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
