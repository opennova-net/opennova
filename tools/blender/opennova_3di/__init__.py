# OpenNova 3DI: the Blender front end of opennova-3di.
#
# Export reads a scene laid out by the NovaLogic ASE/OED object-naming
# convention (LOD roots with `_lod_index`, PN## parts, "## Mesh<n>" meshes,
# `_## center`, `~PPx attach`, `UP<c>## <label>` user points, LP## lights,
# `<code>##-colonly` collision volumes, `<OB|OS|OP|OH>##-occonly` occlusion,
# `Material_<i>_<SHADER>`; the full table heads export.py), writes the .o3d
# scene text (docs/threedi/o3d-scene-format.md) and hands it to the bundled
# opennova-3di CLI, which mints the .3di through the engine's own writer.
# Import runs the same CLI backwards (`opennova-3di scene`) and lays the .o3d
# out by the same convention (importer.py), so an imported model exports
# again. Nothing here encodes or decodes 3DI3: frames, quantization and chunk
# layout belong to the engine (formats/threedi/threedi_build.h). The
# properties below carry only what a name cannot: LOD thresholds and types,
# part animation tracks, material flags, textures and generators, light
# generators, the bullet-face surface and flags, the collision LOD. What the
# engine derives (collision planes, seam flags, tangents, bounds) is never
# stored in the scene: export recomputes it from the meshes every time.

import os
import subprocess

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                       IntProperty, PointerProperty, StringProperty)
from bpy_extras.io_utils import ImportHelper

from . import export, importer

# The seven PANM tracks, labelled by the axis retail turns them about
# (threedi_panm_matrices.cpp: rotation_x turns about the model's up axis,
# rotation_y about its side axis, rotation_z about its forward axis; a
# helicopter's main rotor is rotx, its tail rotor roty, as on Dblkhwk1).
TRACK_TARGETS = [
    ("rotx", "Rotate about up (rotx)", "Yaw: a main rotor, a turret"),
    ("roty", "Rotate about side (roty)", "Pitch: a tail rotor, a wheel, a gun barrel"),
    ("rotz", "Rotate about forward (rotz)", "Roll"),
    ("scalex", "Scale side (scalex)", ""),
    ("scaley", "Scale up (scaley)", ""),
    ("scalez", "Scale forward (scalez)", ""),
    ("trans", "Translate", "Translation along one axis"),
]

# Styles above 0x70 carry a CTRL register (the loader's structural rule,
# threedi_panm_parameter_is_ctrl_reference); the others carry a phase.
CTRL_REFERENCE_THRESHOLD = 0x70

_catalog = None


def bundled_cli_path():
    return os.path.join(os.path.dirname(__file__), "bin", "opennova-3di.exe")


def cli_path(context=None):
    scene = (context or bpy.context).scene
    custom = scene.o3d.cli_path if scene is not None else ""
    if custom:
        return bpy.path.abspath(custom)
    return bundled_cli_path()


def catalog():
    """The CTRL register names, generator style names and shader tags (with
    their capability words, in the engine's table order), read once from
    `opennova-3di catalog` (the engine's own tables; no Python copy)."""
    global _catalog
    if _catalog is None:
        registers, styles, shaders = [], {}, []
        try:
            out = subprocess.run([cli_path(), "catalog"], capture_output=True, text=True, timeout=10).stdout
        except (OSError, subprocess.SubprocessError):
            out = ""
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "register":
                registers.append(parts[1])
            elif len(parts) == 3 and parts[0] == "style":
                styles[int(parts[1])] = parts[2]
            elif len(parts) == 3 and parts[0] == "shader":
                shaders.append((parts[1], int(parts[2], 16)))
        if not shaders:
            return registers, styles, shaders  # no CLI yet: read it again next time
        _catalog = (registers, styles, shaders)
    return _catalog


def search_registers(self, context, edit_text):
    text = edit_text.upper()
    return [r for r in catalog()[0] if text in r]


def search_shaders(self, context, edit_text):
    text = edit_text.upper()
    return [tag for tag, _ in catalog()[2] if text in tag]


def style_label(style):
    return catalog()[1].get(style, "?")


def get_shader(self):
    """A material's shader tag is its name's, Material_<i>_<SHADER> (the
    ASE/OED convention); empty when the name carries none, and export then
    takes the default for its texture count (export.default_shader)."""
    m = export.MATERIAL_RE.match(export.clean_name(self.id_data.name))
    return m.group(2) if m else ""


def set_shader(self, value):
    """Choosing a shader renames the material Material_<i>_<SHADER>, keeping
    its export index i (or taking the next free one)."""
    mat = self.id_data
    value = value.strip()
    if any(c.isspace() for c in value):
        return
    m = export.MATERIAL_RE.match(export.clean_name(mat.name))
    if m:
        index = int(m.group(1))
    else:
        used = [int(x.group(1)) for x in (export.MATERIAL_RE.match(export.clean_name(o.name))
                                          for o in bpy.data.materials) if x]
        index = max(used) + 1 if used else 0
    mat.name = f"Material_{index}_{value}" if value else f"Material_{index}"


def register_prop(name="Register"):
    return StringProperty(name=name, default="", search=search_registers,
                          description="CTRL register (styles above 112), checked by opennova-3di")


class O3DTrack(bpy.types.PropertyGroup):
    target: EnumProperty(name="Target", items=TRACK_TARGETS, default="rotx")
    style: IntProperty(name="Style", default=113, min=0, max=255,
                       description="Generator style (113 control register, 32/33 spin, 50 sine, ...)")
    register: register_prop()
    param: IntProperty(name="Phase", default=0, min=0, max=255, description="Styles up to 112: the phase byte")
    rate: FloatProperty(name="Rate", default=0.0, description="Rate (units per second, 8.8 fixed)")
    start: FloatProperty(name="Start", default=0.0, description="Degrees for rotations, units otherwise")
    end: FloatProperty(name="End", default=359.0, description="Degrees for rotations, units otherwise")
    axis: EnumProperty(name="Axis", items=[("1", "Side", "Model X"), ("2", "Up", "Model Y"), ("3", "Forward", "Model Z")],
                       default="3")


class O3DObjectProps(bpy.types.PropertyGroup):
    tracks: CollectionProperty(type=O3DTrack)
    panm_flags: IntProperty(name="PANM flags", default=-1,
                            description="The part's raw PANM flags word; -1 derives it from the tracks")
    lod_threshold: IntProperty(name="LOD threshold", default=0, min=0,
                               description="On a LOD root: the projected radius above which this LOD draws "
                                           "(0 = the coarsest)")
    lod_type: StringProperty(name="LOD type", default="gnrc", maxlen=4,
                             description="On a LOD root: the RMDL model type (gnrc, bldg, door, veh0)")
    order: IntProperty(name="Export order", default=-1,
                       description="On a user point, light or occlusion mesh: its record index in the model "
                                   "(retail scans seats and effect points in this order); -1 sorts it after the "
                                   "ordered ones by name")


class O3DTexture(bpy.types.PropertyGroup):
    name: StringProperty(name="File", default="", maxlen=16, description="Texture file name (16 characters max)")
    slot: IntProperty(name="Slot", default=1, min=0, max=255, description="1 diffuse, 2 detail, 3/4 normal")
    type: IntProperty(name="Type", default=0, min=0, max=255, description="0 diffuse, 4 MDT normal, 5 TGA-alpha normal")
    flags: IntProperty(name="Flags", default=0, min=0, max=255, description="1 animated, 2 clamped")
    frame: IntProperty(name="Frame", default=0, min=0, max=255)
    image: PointerProperty(name="Image", type=bpy.types.Image)
    write: BoolProperty(name="Write TGA", default=True,
                        description="Write the image as a 32-bit TGA next to the .3di on export")


class O3DMaterialProps(bpy.types.PropertyGroup):
    shader: StringProperty(name="Shader", get=get_shader, set=set_shader, search=search_shaders,
                           search_options={"SUGGESTION", "SORT"},
                           description="The shader tag, any in the engine's table. The material's name carries it "
                                       "(Material_<i>_<SHADER>), so choosing one renames the material")
    # The bullet-mesh face material on COLLISION meshes: the impact effect is
    # the ammo effects-table row material + 4 (metal = 14 -> "metal").
    surface: IntProperty(name="Collision surface", default=14, min=0, max=255,
                         description="Bullet-face poly type: 14 metal, 15 glass, 18 heavy metal, 13 wood, "
                                     "12 stone, 16 cloth, 17 foliage, 1 object")
    # The bullet faces' CFAC flags besides "both sides" (1), which follows Two
    # sided: OED derived both from one render attribute.
    face_never_hit: BoolProperty(name="Bullets pass", default=False,
                                 description="Bullets never hit these faces (CFAC flag 0x100; retail rotor blades)")
    face_double_sided: BoolProperty(name="Hit from behind", default=False,
                                    description="Bullets hit these faces from either side (CFAC flag 0x800)")
    face_other_flags: IntProperty(name="Other face flags", default=0, min=0,
                                  description="CFAC flag bits besides 1, 0x100 and 0x800 (OED wrote 2 and 0x400)")
    alpha_test: BoolProperty(name="Alpha test", default=False)
    alpha_test_value: IntProperty(name="Threshold", default=128, min=0, max=255)
    two_sided: BoolProperty(name="Two sided", default=False)
    other_flags: IntProperty(name="Other flag bits", default=0, min=0, max=255,
                             description="Material flag bits besides alpha test (1) and two sided (4)")
    # Glass and emissive follow the shader (every retail glass shader is glass,
    # every *_LUM one emissive 2); only the reflection colour is authored.
    reflect: FloatVectorProperty(name="Reflect", subtype="COLOR", size=4, default=(0, 0, 0, 0), min=0, max=1,
                                 description="Glass reflection colour (black: a glass shader's 128 grey)")
    alpha_strips: BoolProperty(name="Alpha strips", default=False,
                               description="Draw in the alpha pass even when the shader is not a blended one")
    textures: CollectionProperty(type=O3DTexture)
    rgb_style: IntProperty(name="RGB gen", default=0, min=0, max=255)
    rgb_register: register_prop()
    rgb_rate: FloatProperty(name="Rate", default=1.0)
    rgb_phase: FloatProperty(name="Phase", default=0.0)
    rgb_start: FloatVectorProperty(name="Start", subtype="COLOR", size=3, default=(1, 1, 1), min=0, max=1)
    rgb_end: FloatVectorProperty(name="End", subtype="COLOR", size=3, default=(1, 1, 1), min=0, max=1)
    alpha_style: IntProperty(name="Alpha gen", default=0, min=0, max=255)
    alpha_register: register_prop()
    alpha_rate: FloatProperty(name="Rate", default=1.0)
    alpha_phase: FloatProperty(name="Phase", default=0.0)
    alpha_start: IntProperty(name="Start", default=0)
    alpha_end: IntProperty(name="End", default=255)
    u_style: IntProperty(name="U gen", default=0, min=0, max=255)
    u_register: register_prop()
    u_rate: FloatProperty(name="U rate", default=0.0)
    u_phase: FloatProperty(name="U phase", default=0.0)
    u_start: FloatProperty(name="U start", default=0.0)
    u_end: FloatProperty(name="U end", default=1.0)
    v_style: IntProperty(name="V gen", default=0, min=0, max=255)
    v_register: register_prop()
    v_rate: FloatProperty(name="V rate", default=0.0)
    v_phase: FloatProperty(name="V phase", default=0.0)
    v_start: FloatProperty(name="V start", default=0.0)
    v_end: FloatProperty(name="V end", default=1.0)
    anim_frames: IntProperty(name="Frames", default=0, min=0, max=255, description="Texture flipbook frames")
    anim_type: IntProperty(name="Anim type", default=0, min=0, max=255, description="0 time, 1 control register")
    anim_time: IntProperty(name="Frame time", default=0, description="Per-frame time")
    anim_register: register_prop()


class O3DLightProps(bpy.types.PropertyGroup):
    style: IntProperty(name="Style", default=24, min=0, max=255,
                       description="Colour generator style (24 set, 55 smooth random flicker, 113 register)")
    register: register_prop()
    rate: FloatProperty(name="Rate", default=0.0)
    phase: FloatProperty(name="Phase", default=0.0)
    color_end: FloatVectorProperty(name="End colour", subtype="COLOR", size=3, default=(0, 0, 0), min=0, max=1)
    atten_start: FloatProperty(name="Atten start", default=0.0, min=0.0)
    atten_end: FloatProperty(name="Atten end", default=4.0, min=0.0, description="The light's radius in game")
    disable_corona: BoolProperty(name="No corona", default=False)
    disable_terrain: BoolProperty(name="No terrain light", default=False)
    disable_objects: BoolProperty(name="No object light", default=False)
    other_flags: IntProperty(name="Other flag bits", default=0, min=0, max=255,
                             description="LGHT flag bits besides 1/2/4 and the spot bit 8 (retail sets 0x40)")


class O3DSceneProps(bpy.types.PropertyGroup):
    model_name: StringProperty(name="Model name", default="", description="GHDR name, 15 chars max")
    output_path: StringProperty(name="Output .3di", subtype="FILE_PATH", default="//model.3di")
    forward: EnumProperty(name="Forward", items=[
        ("-Y", "-Y (Blender front)", "The model faces Blender's front view"),
        ("X", "+X", "The model faces +X"),
    ], default="-Y")
    cli_path: StringProperty(name="3DI executable", subtype="FILE_PATH", default=bundled_cli_path(),
                             description="Uses the bundled opennova-3di automatically. Choose a different executable "
                                         "to override it; clearing this field also uses the bundled executable")
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


class O3D_OT_add_texture(bpy.types.Operator):
    bl_idname = "opennova_3di.add_texture"
    bl_label = "Add Texture"

    def execute(self, context):
        t = context.material.o3d.textures.add()
        t.slot = 1 if len(context.material.o3d.textures) == 1 else 2
        return {"FINISHED"}


class O3D_OT_remove_texture(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_texture"
    bl_label = "Remove Texture"
    index: IntProperty()

    def execute(self, context):
        context.material.o3d.textures.remove(self.index)
        return {"FINISHED"}


class O3D_OT_export(bpy.types.Operator):
    bl_idname = "opennova_3di.export"
    bl_label = "Export .3di"
    bl_description = "Write the scene as a NovaLogic .3di through opennova-3di"

    def execute(self, context):
        try:
            message, notes = export.export_scene(context)
        except export.ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            self.report({"WARNING"}, note)
        self.report({"INFO"}, message)
        return {"FINISHED"}


class O3D_OT_import(bpy.types.Operator, ImportHelper):
    bl_idname = "opennova_3di.import_3di"
    bl_label = "Import .3di"
    bl_description = "Read NovaLogic .3di models (textures beside them) into new scenes through opennova-3di"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".3di"
    filter_glob: StringProperty(default="*.3di", options={"HIDDEN"})
    files: CollectionProperty(type=bpy.types.OperatorFileListElement, options={"HIDDEN", "SKIP_SAVE"})
    directory: StringProperty(subtype="DIR_PATH", options={"HIDDEN", "SKIP_SAVE"})
    import_collision: BoolProperty(name="Collision volumes", default=True)
    import_occlusion: BoolProperty(name="Occlusion", default=True)
    import_lights: BoolProperty(name="Lights", default=True)

    def execute(self, context):
        paths = [os.path.join(self.directory, f.name) for f in self.files if f.name] or [self.filepath]
        imported = []
        for path in paths:
            try:
                scene, notes = importer.import_file(context, path, self)
            except importer.ImportFailed as e:
                self.report({"ERROR"}, f"{os.path.basename(path)}: {e}")
                return {"CANCELLED"}
            for note in notes:
                self.report({"WARNING"}, f"{os.path.basename(path)}: {note}")
            imported.append(scene.name)
        self.report({"INFO"}, "imported " + ", ".join(imported))
        return {"FINISHED"}


def menu_import(self, context):
    self.layout.operator(O3D_OT_import.bl_idname, text="NovaLogic 3DI (.3di)")


class O3D_PT_scene(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "OpenNova"

    def draw(self, context):
        p = context.scene.o3d
        col = self.layout.column()
        col.operator("opennova_3di.import_3di", icon="IMPORT")
        col.separator()
        col.prop(p, "model_name")
        col.prop(p, "output_path")
        col.prop(p, "forward")
        col.prop(p, "write_textures")
        col.prop(p, "poly_collision_lod")
        col.prop(p, "cli_path")
        if not p.is_property_set("cli_path") or not p.cli_path:
            col.label(text="Bundled executable (automatic)")
        col.operator("opennova_3di.export", icon="EXPORT")


def draw_style(layout, holder, style_attr, register_attr=None):
    row = layout.row()
    row.prop(holder, style_attr)
    style = getattr(holder, style_attr)
    row.label(text=style_label(style))
    if register_attr is not None and style > CTRL_REFERENCE_THRESHOLD:
        layout.prop(holder, register_attr)


class O3D_PT_object(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    def draw(self, context):
        ob = context.object
        p = ob.o3d
        layout = self.layout
        name = export.clean_name(ob.name)
        if (ob.type == "EMPTY" and export.POINT_RE.match(name)) or ob.type == "LIGHT" or \
                export.OCCLUSION_RE.search(name):
            layout.prop(p, "order")
            return
        if ob.type != "EMPTY":
            return
        if "_lod_index" in ob:
            layout.prop(p, "lod_threshold")
            layout.prop(p, "lod_type")
            return
        if not export.PART_RE.match(name):
            return
        layout.label(text="Part animation (PANM)")
        layout.prop(p, "panm_flags")
        for i, t in enumerate(p.tracks):
            box = layout.box()
            row = box.row()
            row.prop(t, "target")
            row.operator("opennova_3di.remove_track", text="", icon="X").index = i
            draw_style(box, t, "style", "register")
            if t.style <= CTRL_REFERENCE_THRESHOLD:
                box.prop(t, "param")
            row = box.row()
            row.prop(t, "rate")
            row.prop(t, "start")
            row.prop(t, "end")
            if t.target == "trans":
                box.prop(t, "axis")
        layout.operator("opennova_3di.add_track", icon="ADD")


class O3D_PT_light(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        return context.light is not None

    def draw(self, context):
        p = context.light.o3d
        layout = self.layout
        layout.label(text="Named LP## (## = owning part); colour = the start colour")
        draw_style(layout, p, "style", "register")
        row = layout.row()
        row.prop(p, "rate")
        if p.style <= CTRL_REFERENCE_THRESHOLD:
            row.prop(p, "phase")
        layout.prop(p, "color_end")
        row = layout.row()
        row.prop(p, "atten_start")
        row.prop(p, "atten_end")
        row = layout.row()
        row.prop(p, "disable_corona")
        row.prop(p, "disable_terrain")
        row.prop(p, "disable_objects")
        layout.prop(p, "other_flags")


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
        tag = p.shader
        if tag:
            caps = export.shader_flags(tag)
            known = any(name.lower() == tag.lower() for name, _ in catalog()[2])
            traits = [label for bit, label in ((export.FLAG_BLENDING, "alpha pass"), (export.FLAG_GLASS, "glass"),
                                               (export.FLAG_EMISSIVE, "emissive"), (0x8000, "tangents"),
                                               (export.FLAG_SKINNED, "skinned")) if caps & bit]
            layout.label(text=(", ".join(traits) if traits else "opaque") if known else
                         "Not in the engine's shader table", icon="NONE" if known else "ERROR")
        else:
            layout.label(text="No shader in the name: export picks one by texture count")
        box = layout.box()
        box.label(text="Bullet faces")
        row = box.row()
        row.prop(p, "surface")
        row = box.row()
        row.prop(p, "face_never_hit")
        row.prop(p, "face_double_sided")
        box.prop(p, "face_other_flags")
        row = layout.row()
        row.prop(p, "alpha_test")
        if p.alpha_test:
            row.prop(p, "alpha_test_value")
        row.prop(p, "two_sided")
        row = layout.row()
        row.prop(p, "alpha_strips")
        row.prop(p, "other_flags")
        layout.prop(p, "reflect")
        box = layout.box()
        box.label(text="Textures (empty: the first image node, slot 1)")
        for i, t in enumerate(p.textures):
            row = box.row()
            row.prop(t, "name", text="")
            row.prop(t, "slot")
            row.operator("opennova_3di.remove_texture", text="", icon="X").index = i
            row = box.row()
            row.prop(t, "image", text="")
            row.prop(t, "write")
            row = box.row()
            row.prop(t, "type")
            row.prop(t, "flags")
            row.prop(t, "frame")
        box.operator("opennova_3di.add_texture", icon="ADD")
        row = layout.row()
        row.prop(p, "anim_frames")
        row.prop(p, "anim_type")
        if p.anim_type == 1:
            row.prop(p, "anim_register")
        else:
            row.prop(p, "anim_time")
        box = layout.box()
        draw_style(box, p, "rgb_style", "rgb_register")
        if p.rgb_style != 0:
            row = box.row()
            row.prop(p, "rgb_rate")
            if p.rgb_style <= CTRL_REFERENCE_THRESHOLD:
                row.prop(p, "rgb_phase")
            box.prop(p, "rgb_start")
            box.prop(p, "rgb_end")
        box = layout.box()
        draw_style(box, p, "alpha_style", "alpha_register")
        if p.alpha_style != 0:
            row = box.row()
            row.prop(p, "alpha_rate")
            if p.alpha_style <= CTRL_REFERENCE_THRESHOLD:
                row.prop(p, "alpha_phase")
            row = box.row()
            row.prop(p, "alpha_start")
            row.prop(p, "alpha_end")
        for axis in ("u", "v"):
            box = layout.box()
            draw_style(box, p, f"{axis}_style", f"{axis}_register")
            if getattr(p, f"{axis}_style") != 0:
                row = box.row()
                row.prop(p, f"{axis}_rate")
                if getattr(p, f"{axis}_style") <= CTRL_REFERENCE_THRESHOLD:
                    row.prop(p, f"{axis}_phase")
                row = box.row()
                row.prop(p, f"{axis}_start")
                row.prop(p, f"{axis}_end")


CLASSES = (O3DTrack, O3DObjectProps, O3DTexture, O3DMaterialProps, O3DLightProps, O3DSceneProps,
           O3D_OT_add_track, O3D_OT_remove_track, O3D_OT_add_texture, O3D_OT_remove_texture, O3D_OT_export,
           O3D_OT_import, O3D_PT_scene, O3D_PT_object, O3D_PT_light, O3D_PT_material)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.Object.o3d = PointerProperty(type=O3DObjectProps)
    bpy.types.Material.o3d = PointerProperty(type=O3DMaterialProps)
    bpy.types.Light.o3d = PointerProperty(type=O3DLightProps)
    bpy.types.Scene.o3d = PointerProperty(type=O3DSceneProps)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    del bpy.types.Scene.o3d
    del bpy.types.Light.o3d
    del bpy.types.Material.o3d
    del bpy.types.Object.o3d
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
