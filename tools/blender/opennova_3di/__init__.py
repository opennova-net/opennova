# OpenNova 3DI Exporter: the Blender front end of opennova-3di.
#
# The add-on reads a scene laid out by the NovaLogic ASE/OED object-naming
# convention (LOD roots with `_lod_index`, PN## parts, "## Mesh<n>" meshes,
# `_## center`, `~PPx attach`, `UP<c>## <label>` user points,
# `<code>##-colonly` collision volumes, `Material_<i>_<SHADER>`; the full
# table heads export.py), writes the .o3d scene text
# (docs/threedi/o3d-scene-format.md) and hands it to the bundled opennova-3di
# CLI, which mints the .3di through the engine's own writer. Nothing here
# encodes 3DI3: frames, quantization and chunk layout belong to the engine
# (formats/threedi/threedi_build.h). The properties below carry only what a
# name cannot: LOD thresholds, part animation tracks, material generators
# and flags, the bullet-face surface type, the collision LOD.

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, IntProperty,
                       PointerProperty, StringProperty)

from . import export

SHADERS = [
    ("FF_ST_OP", "Opaque (FF_ST_OP)", "Fixed-function single texture, opaque"),
    ("FF_ST_AB", "Alpha blend (FF_ST_AB)", "Fixed-function single texture, alpha blended"),
    ("FF_ST_AD", "Additive (FF_ST_AD)", "Fixed-function single texture, additive"),
    ("FF_ST_OP_LUM", "Luminous (FF_ST_OP_LUM)", "Opaque, unlit / emissive"),
    ("FF_ST_AB_LUM", "Luminous blend (FF_ST_AB_LUM)", "Alpha blended, unlit / emissive"),
    ("FF_ST_AD_LUM", "Luminous additive (FF_ST_AD_LUM)", "Additive, unlit / emissive (glows, flames)"),
    ("FFP_GLASS", "Glass (FFP_GLASS)", "Reflective glass, no texture"),
]

TRACK_TARGETS = [
    ("rotx", "Rotate X", "Rotation about the part's forward axis"),
    ("roty", "Rotate Y", "Rotation about the part's left axis"),
    ("rotz", "Rotate Z", "Rotation about the part's up axis"),
    ("scalex", "Scale X", ""),
    ("scaley", "Scale Y", ""),
    ("scalez", "Scale Z", ""),
    ("trans", "Translate", "Translation along one axis"),
]

TRACK_STYLES = [
    ("113", "Control register", "Driven by an engine register (rotor, gear, doors, ...)"),
    ("32", "Rotate CW", "Constant spin"),
    ("33", "Rotate CCW", "Constant spin"),
    ("50", "Sine wave", ""),
    ("52", "Saw wave", ""),
    ("16", "Slide", ""),
    ("24", "Set", ""),
]

# The CTRL registers a vehicle or prop animates from (threedi_ctrl_catalog.h).
REGISTERS = [(n, n, "") for n in (
    "HELO_ROTOR", "HELO_TAILROTOR", "HELO_GEAR", "HELO_GEARB", "HELO_REAR_GEAR", "HELO_GEARDOORS",
    "HELO_BAYDOORS", "HELO_PCANOPY", "HELO_CPCANOPY", "HELO_PILOTYAW", "HELO_PILOTPITCH", "HELO_GUNYAW",
    "HELO_GUNPITCH", "HEAT_GLOW", "EWEAP_GUNYAW", "EWEAP_GUNPITCH", "WEAP_SPIN", "VEHICLE_WHEELS",
    "VEHICLE_STEERING", "VEHICLE_SPEED", "VEHICLE_GUNYAW", "VEHICLE_GUNPITCH", "VEHICLE_SPECIAL1",
    "VEHICLE_SPECIAL2", "OBJECT_DESTROY", "DOOR_00", "DOOR_01", "DOOR_02", "DOOR_03", "LIGHTSWITCH0",
    "FLICKER", "SWING", "TEX_TEAM")]

GEN_STYLES = [
    ("0", "None", ""),
    ("50", "Sine wave", ""),
    ("52", "Saw wave", ""),
    ("53", "Inverse saw", ""),
    ("113", "Control register", ""),
]


class O3DTrack(bpy.types.PropertyGroup):
    target: EnumProperty(name="Target", items=TRACK_TARGETS, default="rotx")
    style: EnumProperty(name="Style", items=TRACK_STYLES, default="113")
    register: EnumProperty(name="Register", items=REGISTERS, default="HELO_ROTOR")
    rate: FloatProperty(name="Rate", default=0.0, description="Rate (units per second, 8.8 fixed)")
    start: FloatProperty(name="Start", default=0.0, description="Degrees for rotations, units otherwise")
    end: FloatProperty(name="End", default=359.0, description="Degrees for rotations, units otherwise")
    axis: EnumProperty(name="Axis", items=[("1", "X", ""), ("2", "Y", ""), ("3", "Z", "")], default="3")


class O3DObjectProps(bpy.types.PropertyGroup):
    tracks: CollectionProperty(type=O3DTrack)
    lod_threshold: IntProperty(name="LOD threshold", default=0, min=0,
                               description="On a LOD root: the projected radius above which this LOD draws "
                                           "(0 = the coarsest)")


class O3DMaterialProps(bpy.types.PropertyGroup):
    shader: EnumProperty(name="Shader", items=SHADERS, default="FF_ST_OP")
    # The bullet-mesh face material on COLLISION meshes: the impact effect is
    # the ammo effects-table row material + 4 (metal = 14 -> "metal").
    surface: EnumProperty(name="Collision surface", items=[
        ("14", "Metal (14)", ""), ("15", "Glass (15)", ""), ("18", "Heavy metal (18)", ""),
        ("13", "Wood (13)", ""), ("12", "Stone (12)", ""), ("16", "Cloth (16)", ""), ("17", "Foliage (17)", ""),
        ("1", "Object (1)", ""),
    ], default="14")
    alpha_test: BoolProperty(name="Alpha test", default=False)
    two_sided: BoolProperty(name="Two sided", default=False)
    texture_name: StringProperty(name="Texture file", default="",
                                 description="Override the exported texture name (16 chars max, .tga)")
    rgb_style: EnumProperty(name="RGB gen", items=GEN_STYLES, default="0")
    rgb_register: EnumProperty(name="Register", items=REGISTERS, default="HEAT_GLOW")
    rgb_rate: FloatProperty(name="Rate", default=1.0)
    rgb_start: bpy.props.FloatVectorProperty(name="Start", subtype="COLOR", size=3, default=(1, 1, 1), min=0, max=1)
    rgb_end: bpy.props.FloatVectorProperty(name="End", subtype="COLOR", size=3, default=(1, 1, 1), min=0, max=1)
    alpha_style: EnumProperty(name="Alpha gen", items=GEN_STYLES, default="0")
    alpha_register: EnumProperty(name="Register", items=REGISTERS, default="HEAT_GLOW")
    alpha_rate: FloatProperty(name="Rate", default=1.0)
    alpha_start: IntProperty(name="Start", default=0)
    alpha_end: IntProperty(name="End", default=255)
    u_style: EnumProperty(name="U scroll", items=GEN_STYLES + [("32", "Constant", "")], default="0")
    u_rate: FloatProperty(name="U rate", default=0.0)
    v_style: EnumProperty(name="V scroll", items=GEN_STYLES + [("32", "Constant", "")], default="0")
    v_rate: FloatProperty(name="V rate", default=0.0)


class O3DSceneProps(bpy.types.PropertyGroup):
    model_name: StringProperty(name="Model name", default="", description="GHDR name, 15 chars max")
    output_path: StringProperty(name="Output .3di", subtype="FILE_PATH", default="//model.3di")
    forward: EnumProperty(name="Forward", items=[
        ("-Y", "-Y (Blender front)", "The model faces Blender's front view"),
        ("X", "+X", "The model faces +X"),
    ], default="-Y")
    cli_path: StringProperty(name="opennova-3di", subtype="FILE_PATH", default="",
                             description="Override the bundled CLI")
    write_textures: BoolProperty(name="Write textures", default=True)
    poly_collision_lod: IntProperty(name="Collision LOD", default=0, min=0,
                                    description="The render LOD whose part meshes also become the bullet faces (the OED "
                                                ".3dp poly_collision_lod); 0 = the most detailed")


class O3D_OT_add_track(bpy.types.Operator):
    bl_idname = "opennova_3di.add_track"
    bl_label = "Add Part Animation Track"

    def execute(self, context):
        context.object.o3d.tracks.add()
        return {"FINISHED"}


class O3D_OT_remove_track(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_track"
    bl_label = "Remove Track"
    index: IntProperty()

    def execute(self, context):
        context.object.o3d.tracks.remove(self.index)
        return {"FINISHED"}


class O3D_OT_export(bpy.types.Operator):
    bl_idname = "opennova_3di.export"
    bl_label = "Export .3di"
    bl_description = "Write the scene as a NovaLogic .3di through opennova-3di"

    def execute(self, context):
        try:
            message = export.export_scene(context)
        except export.ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, message)
        return {"FINISHED"}


class O3D_PT_scene(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "OpenNova"

    def draw(self, context):
        p = context.scene.o3d
        col = self.layout.column()
        col.prop(p, "model_name")
        col.prop(p, "output_path")
        col.prop(p, "forward")
        col.prop(p, "write_textures")
        col.prop(p, "poly_collision_lod")
        col.prop(p, "cli_path")
        col.operator("opennova_3di.export", icon="EXPORT")


class O3D_PT_object(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    def draw(self, context):
        ob = context.object
        p = ob.o3d
        layout = self.layout
        if ob.type != "EMPTY":
            return
        if "_lod_index" in ob:
            layout.prop(p, "lod_threshold")
            return
        if not export.PART_RE.match(export.clean_name(ob.name)):
            return
        layout.label(text="Part animation (PANM)")
        for i, t in enumerate(p.tracks):
            box = layout.box()
            row = box.row()
            row.prop(t, "target")
            row.operator("opennova_3di.remove_track", text="", icon="X").index = i
            box.prop(t, "style")
            if t.style == "113":
                box.prop(t, "register")
            row = box.row()
            row.prop(t, "rate")
            row.prop(t, "start")
            row.prop(t, "end")
            if t.target == "trans":
                box.prop(t, "axis")
        layout.operator("opennova_3di.add_track", icon="ADD")


class O3D_PT_material(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        p = context.material.o3d
        layout = self.layout
        layout.prop(p, "shader")
        layout.prop(p, "surface")
        row = layout.row()
        row.prop(p, "alpha_test")
        row.prop(p, "two_sided")
        layout.prop(p, "texture_name")
        box = layout.box()
        box.prop(p, "rgb_style")
        if p.rgb_style != "0":
            if p.rgb_style == "113":
                box.prop(p, "rgb_register")
            box.prop(p, "rgb_rate")
            box.prop(p, "rgb_start")
            box.prop(p, "rgb_end")
        box = layout.box()
        box.prop(p, "alpha_style")
        if p.alpha_style != "0":
            if p.alpha_style == "113":
                box.prop(p, "alpha_register")
            box.prop(p, "alpha_rate")
            row = box.row()
            row.prop(p, "alpha_start")
            row.prop(p, "alpha_end")
        box = layout.box()
        row = box.row()
        row.prop(p, "u_style")
        row.prop(p, "u_rate")
        row = box.row()
        row.prop(p, "v_style")
        row.prop(p, "v_rate")


CLASSES = (O3DTrack, O3DObjectProps, O3DMaterialProps, O3DSceneProps, O3D_OT_add_track,
           O3D_OT_remove_track, O3D_OT_export, O3D_PT_scene, O3D_PT_object, O3D_PT_material)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.Object.o3d = PointerProperty(type=O3DObjectProps)
    bpy.types.Material.o3d = PointerProperty(type=O3DMaterialProps)
    bpy.types.Scene.o3d = PointerProperty(type=O3DSceneProps)


def unregister():
    del bpy.types.Scene.o3d
    del bpy.types.Material.o3d
    del bpy.types.Object.o3d
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
