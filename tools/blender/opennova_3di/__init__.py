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

import importlib
import json
import os
import sys

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                       IntProperty, PointerProperty, StringProperty)
from bpy_extras.io_utils import ImportHelper

# Blender re-runs this file when the extension is updated or scripts are
# reloaded, but keeps the submodules it imported before: reload them first so
# the property groups registered here and the code that reads them agree.
for _name in ("o3dtext", "materials", "export", "importer", "assembly", "animation", "anim_import", "weapon"):
    if f"{__name__}.{_name}" in sys.modules:
        importlib.reload(sys.modules[f"{__name__}.{_name}"])
from . import anim_import, animation, assembly, export, importer, materials, weapon
from .export import active_model
from .o3dtext import CTRL_REFERENCE_THRESHOLD, ExportError, ImportFailed, bundled_cli_path, cli_path, run_cli

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

# The catalog of the executable it was read from: (path, modified time) ->
# (tables, the reason they are empty or None).
_catalog = {}

# Blender 4.5 lets a path property take a blend-relative `//` path without
# warning on every assignment; 4.2 to 4.4 have no such option.
PATH_OPTIONS = {"ANIMATABLE", "PATH_SUPPORTS_BLEND_RELATIVE"} \
    if "is_path_supports_blend_relative" in bpy.types.Property.bl_rna.properties else {"ANIMATABLE"}


def read_catalog():
    """The catalog entry of the current executable, run once per executable
    (a new path or a rebuilt file reads again; a missing file is looked for
    again next time without running anything)."""
    path = cli_path()
    try:
        key = (path, os.path.getmtime(path))
    except OSError:
        return ([], {}, [], [], []), f"opennova-3di not found at {path}"
    if key not in _catalog:
        registers, styles, shaders, slots, triggers = [], {}, [], [], []
        problem = None
        try:
            out = run_cli(None, ["catalog"], ExportError, timeout=10).stdout
        except ExportError as e:
            out, problem = "", f"opennova-3di catalog failed: {e}"
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "register":
                registers.append(parts[1])
            elif len(parts) == 3 and parts[0] == "style":
                styles[int(parts[1])] = parts[2]
            elif len(parts) == 3 and parts[0] == "shader":
                shaders.append((parts[1], int(parts[2], 16)))
            elif len(parts) == 2 and parts[0] == "animslot":
                slots.append(parts[1])
            elif len(parts) == 3 and parts[0] == "trigger":
                triggers.append((int(parts[1], 16), parts[2]))
        if not shaders and problem is None:
            problem = f"opennova-3di catalog ({path}) printed no shader table"
        _catalog.clear()
        _catalog[key] = ((registers, styles, shaders, slots, triggers), problem)
    return _catalog[key]


def catalog():
    """The CTRL register names, generator style names, shader tags (with their
    capability words, in the engine's table order), anim slot keys and animation
    event bits, read from `opennova-3di catalog` (the engine's own tables; no
    Python copy). Empty tables when it failed: catalog_error() says why."""
    return read_catalog()[0]


def catalog_error():
    """Why catalog() is empty, or None."""
    return read_catalog()[1]


def search_registers(self, context, edit_text):
    text = edit_text.upper()
    return [r for r in catalog()[0] if text in r]


def search_slots(self, context, edit_text):
    """The anim slot keys the engine itself names. Retail's namespace is far
    wider (the JOX corpus authors 240), so the field takes any key: a row names
    its slot by what follows the key's first five characters."""
    text = edit_text.lower()
    return [k for k in catalog()[3] if text in k]


def search_shaders(self, context, edit_text):
    text = edit_text.upper()
    return [tag for tag, _ in catalog()[2] if text in tag]


def style_label(style):
    return catalog()[1].get(style, "?")


def get_shader(self):
    """A material's shader tag is its name's, Material_<i>_<SHADER> (the
    ASE/OED convention); empty when the name carries none, and export then
    takes the default for its texture count (materials.default_shader)."""
    m = materials.MATERIAL_RE.match(export.clean_name(self.id_data.name))
    return m.group(2) if m else ""


def set_shader(self, value):
    """Choosing a shader renames the material Material_<i>_<SHADER>, keeping
    its export index i (or taking the next free one)."""
    mat = self.id_data
    value = value.strip()
    if any(c.isspace() for c in value):
        return
    m = materials.MATERIAL_RE.match(export.clean_name(mat.name))
    if m:
        index = int(m.group(1))
    else:
        used = [int(x.group(1)) for x in (materials.MATERIAL_RE.match(export.clean_name(o.name))
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


def is_other_model(self, ob):
    return export.is_model_root(ob) and ob is not self.id_data


def is_rig_model(self, ob):
    return is_other_model(self, ob) and not assembly.armatures(ob)


def update_drive(self, context):
    assembly.drive(self.id_data, self.drive_rig)


def update_mount(self, context):
    assembly.mount(self.id_data, context.scene)


def search_mount_points(self, context, edit_text):
    if self.mount_parent is None:
        return []
    text = edit_text.lower()
    return [label for label, _ in assembly.user_points(self.mount_parent) if text in label.lower()]


class O3DAdmVariant(bpy.types.PropertyGroup):
    action: PointerProperty(name="Clip", type=bpy.types.Action,
                            description="The clip this variant plays (its name is the .bad file stem)")


class O3DAdmRow(bpy.types.PropertyGroup):
    # One .adm row: an anim slot and its clip ring, in the order the file
    # stores. The engine serves a row from its LAST variant back
    # [orig: AnimMap_RegisterBoneNode @0x40C2D0].
    key: StringProperty(name="Slot", default="anim_reset", search=search_slots,
                        description="The anim slot this row answers, named by what follows the key's "
                                    "first five characters (anim_reset is the rig's bind and rest pose, "
                                    "and a table needs it); retail authors far more keys than the "
                                    "engine names, so any anim_<name> is allowed")
    variants: CollectionProperty(type=O3DAdmVariant)
    weapon_role: EnumProperty(name="Weapon action", items=weapon.ROLES, default="NONE")
    weapon_sound: StringProperty(name="Start sound", description="SOUNDSET reference at action entry")
    weapon_end_sound: StringProperty(name="End sound", description="SOUNDSETEND reference at active completion")
    weapon_particle: StringProperty(name="Particle", description="Effect reference; recoil emits it at its decision tick")
    weapon_userpoint: StringProperty(name="Effect point", description="PARTICLEUSERPOINT on the model")


class O3DActionProps(bpy.types.PropertyGroup):
    # A clip's own header, on its Action. Everything else a .bad carries is
    # derived on export (formats/bad/bad_build.h).
    fps: FloatProperty(name="Clip rate", default=30.0, min=1.0, max=255.0,
                       description="The clip's own frame rate; every retail clip ships 30")
    frames: IntProperty(name="Frames", default=0, min=0,
                        description="The clip's length in frames; 0 takes the Action's own keyed "
                                    "range. A longer one holds the Action's last pose over the "
                                    "extra frames")
    loop: BoolProperty(name="Loop", default=True, description="The clip repeats (flag 1)")
    translation: BoolProperty(name="Translations", default=False,
                              description="Carry each bone's per-frame displacement as well as its "
                                          "rotation (flag 2): a bolt, a magazine, a rig that slides")
    raw_flag_8: BoolProperty(name="Flag 8", default=False,
                             description="The clip flag bit 3, which 73 of the 477 retail clips "
                                         "carry (viewmodel draw clips, the bikes) and nothing has "
                                         "witnessed; carried, not read")


class O3DObjectProps(bpy.types.PropertyGroup):
    # On a model root (the Empty above a model's LOD roots): one .3di.
    model_name: StringProperty(name="Model name", default="",
                               description="GHDR name, 15 chars max; empty: the output file's name")
    output_path: StringProperty(name="Output .3di", subtype="FILE_PATH", default="//model.3di", options=PATH_OPTIONS)
    export_bullet_faces: BoolProperty(name="Generate bullet faces", default=True,
                                      description="The collision LOD's render triangles are also its bullet faces; "
                                                  "off for a model that needs none (first-person arms). Its "
                                                  "collision volumes still export")
    poly_collision_lod: IntProperty(name="Collision LOD", default=0, min=0,
                                    description="The render LOD whose part meshes also become the bullet faces (the OED "
                                                ".3dp poly_collision_lod); 0 = the most detailed")
    # Display-only assembly (assembly.py); export never reads these.
    drive_rig: PointerProperty(name="Bones follow", type=bpy.types.Object, poll=is_rig_model, update=update_drive,
                               description="A skinned model's bones follow this model's parts of the same index, as "
                                           "retail draws first-person arms with the gun's part matrices (display only)")
    mount_parent: PointerProperty(name="Mount on", type=bpy.types.Object, poll=is_other_model, update=update_mount,
                                  description="Place this model on another model, as an ITEMS.DEF addeweap child "
                                              "(the M1A1's turret) sits on its parent (display only)")
    mount_point: StringProperty(name="At user point", default="", search=search_mount_points, update=update_mount,
                                description="The parent's user point (the addeweap row's name, e.g. ewep01); none or "
                                            "not found: the parent's root")
    # On a model root: its animations (the clip set its rig carries).
    adm_path: StringProperty(name="Output .adm", subtype="FILE_PATH", default="", options=PATH_OPTIONS,
                             description="Where Export Animations writes the clip table; every clip "
                                         "it names is written beside it as <clip>.bad. Empty: "
                                         "//<model name>.adm")
    rows: CollectionProperty(type=O3DAdmRow)
    clip_prefix: StringProperty(name="Clip file prefix", default="",
                                 description="Prefix exported BAD filenames and ADM references while keeping "
                                             "Action names and engine row names unchanged (e.g. rifle_)")
    weapon_enabled: BoolProperty(name="Export weapon actions", default=False,
                                 description="Write marker-derived weapon.def ACTION blocks beside the animations")
    weapon_mode: EnumProperty(name="Fire mode", default="semi", items=[
        ("semi", "Semi-auto", "A fresh press from Idle; preview measures the earliest accepted repeat"),
        ("auto", "Automatic", "Hold the trigger to repeat through recoil"),
        ("burst", "Three-round burst", "Preview the within-burst firing cadence")])
    weapon_cadence: EnumProperty(name="Cadence from", default="MARKER", items=[
        ("MARKER", "Ready marker", "The Fire Action's Shot-to-Ready interval requests the firing cycle"),
        ("RPM", "Target RPM", "Target RPM is authoritative; the Fire Ready marker is unused")])
    weapon_rpm: FloatProperty(name="Target RPM", default=600, min=0.5, max=3750, precision=2,
                              description="Requested rate; the native FSM reports the achievable integer-tick rate")
    weapon_preview: StringProperty(options={"HIDDEN"}, description="Disposable preview cache; never an export input")
    head_bone: StringProperty(name="Head bone", default="",
                              description="The rig bone whose height above the ground is each frame's "
                                          "top (the body's capsule top). Empty: the one bone whose name "
                                          "ends in 'head'; with none, the top is the bottom (the hips' "
                                          "height), as a first-person rig's is")
    # On a skinned model's Armature: the clip's per-frame event word. Key it to
    # place footsteps, fire and foley (opennova-3di catalog lists the bits).
    anim_trigger: IntProperty(name="Trigger", default=0,
                              description="The animation event bits this frame fires: 1 and 2 the "
                                          "left and right footstep, 4 8 and 16 the ammo rows, "
                                          "0x20..0x400 the six foley sounds. The word is 32 bits, "
                                          "so one with bit 31 set shows negative (a version 0 "
                                          "clip's 0xffffffff is -1)")
    tracks: CollectionProperty(type=O3DTrack)
    panm_flags: IntProperty(name="PANM flags", default=-1,
                            description="The part's raw PANM flags word; -1 derives it from the tracks")
    lod_threshold: IntProperty(name="LOD threshold", default=0, min=0,
                               description="On a LOD root: the projected radius in pixels above which this "
                                           "LOD draws (0 = the coarsest; Armry01's run 200, 60, 20, 0)")
    lod_type: StringProperty(name="LOD type", default="gnrc", maxlen=4,
                             description="On a LOD root: the RMDL model type (gnrc, bldg, door, veh0)")
    order: IntProperty(name="Export order", default=-1,
                       description="On a user point, light or occlusion mesh: its record index in the model "
                                   "(retail scans seats and effect points in this order); -1 sorts it after the "
                                   "ordered ones by name")


class O3DBoneProps(bpy.types.PropertyGroup):
    # A skinned model's part animation, on its BN## bone.
    tracks: CollectionProperty(type=O3DTrack)
    panm_flags: IntProperty(name="PANM flags", default=-1,
                            description="The part's raw PANM flags word; -1 derives it from the tracks")
    frame: FloatVectorProperty(name="Track frame", subtype="EULER", size=3, default=(0.0, 0.0, 0.0),
                               description="The axes this part's tracks turn about (its PANM MTRX row), as a "
                                           "rotation of the model's axes; zero: the model's own (dM1A1's turret "
                                           "ring and wheels turn 90 degrees)")


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
    # sided: OED derived both from one render attribute. A projectile's face
    # test skips a 0x100 face, and one with 0x800 but not 1 stops only a
    # bullet crossing it from the front [orig: Physics_RaycastAgainstBoneCollision
    # @ 0x4e4cb0, the 0x800 test @ 0x4e5139; runtime/world/collision_query.cpp].
    face_never_hit: BoolProperty(name="Bullets pass", default=False,
                                 description="Bullets never hit these faces (CFAC flag 0x100; retail rotor blades)")
    face_front_only: BoolProperty(name="Front only", default=False,
                                  description="Bullets hit these faces only from the front; one coming from behind "
                                              "passes through (CFAC flag 0x800; Two sided overrides it)")
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
    forward: EnumProperty(name="Forward", items=[
        ("-Y", "-Y (Blender front)", "The model faces Blender's front view"),
        ("X", "+X", "The model faces +X"),
    ], default="-Y")
    cli_path: StringProperty(name="3DI executable", subtype="FILE_PATH", default=bundled_cli_path(),
                             options=PATH_OPTIONS,
                             description="Uses the bundled opennova-3di automatically. Choose a different executable "
                                         "to override it; clearing this field also uses the bundled executable")
    write_textures: BoolProperty(name="Write textures", default=True)


def part_animation(context, bone):
    return context.bone.o3d if bone else context.object.o3d


class O3D_OT_add_track(bpy.types.Operator):
    bl_idname = "opennova_3di.add_track"
    bl_label = "Add Part Animation Track"
    bone: BoolProperty(options={"HIDDEN"})

    def execute(self, context):
        part_animation(context, self.bone).tracks.add()
        return {"FINISHED"}


class O3D_OT_remove_track(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_track"
    bl_label = "Remove Track"
    index: IntProperty()
    bone: BoolProperty(options={"HIDDEN"})

    def execute(self, context):
        part_animation(context, self.bone).tracks.remove(self.index)
        return {"FINISHED"}


class O3D_OT_add_texture(bpy.types.Operator):
    bl_idname = "opennova_3di.add_texture"
    bl_label = "Add Texture"

    @classmethod
    def poll(cls, context):
        return getattr(context, "material", None) is not None

    def execute(self, context):
        t = context.material.o3d.textures.add()
        t.slot = 1 if len(context.material.o3d.textures) == 1 else 2
        return {"FINISHED"}


class O3D_OT_remove_texture(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_texture"
    bl_label = "Remove Texture"
    index: IntProperty()

    @classmethod
    def poll(cls, context):
        return getattr(context, "material", None) is not None

    def execute(self, context):
        context.material.o3d.textures.remove(self.index)
        return {"FINISHED"}


def export_models(op, context, models):
    wrote = []
    for model in models:
        try:
            message, notes = export.export_model(context, model)
        except ExportError as e:
            op.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            op.report({"WARNING"}, f"{model.name}: {note}")
        wrote.append(message)
    op.report({"INFO"}, "; ".join(wrote))
    return {"FINISHED"}


class O3D_OT_export(bpy.types.Operator):
    bl_idname = "opennova_3di.export"
    bl_label = "Export Model"
    bl_description = "Write the active object's model as a NovaLogic .3di through opennova-3di"

    def execute(self, context):
        model = active_model(context)
        if model is None:
            self.report({"ERROR"}, "select an object of the model to export (a model root is the Empty above its "
                                   "LOD roots; Add Model makes one)")
            return {"CANCELLED"}
        return export_models(self, context, [model])


class O3D_OT_export_all(bpy.types.Operator):
    bl_idname = "opennova_3di.export_all"
    bl_label = "Export All Models"
    bl_description = "Write every model in the scene to its own .3di"

    def execute(self, context):
        models = export.model_roots(context.scene)
        if not models:
            self.report({"ERROR"}, "the scene holds no model root (the Empty above a model's LOD roots)")
            return {"CANCELLED"}
        return export_models(self, context, models)


class O3D_OT_add_model(bpy.types.Operator):
    bl_idname = "opennova_3di.add_model"
    bl_label = "Add Model"
    bl_description = "Add a model root with its LOD 0 root, ready for PN## parts"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        model = bpy.data.objects.new("Model", None)
        model.empty_display_type = "CUBE"
        model.o3d.output_path = f"//{model.name}.3di"
        lod = bpy.data.objects.new(f"{model.name}_LOD0", None)
        lod.empty_display_type = "ARROWS"
        lod.empty_display_size = 0.5
        lod["_lod_index"] = 0
        lod.parent = model
        for ob in (model, lod):
            context.collection.objects.link(ob)
        for ob in context.selected_objects:
            ob.select_set(False)
        model.select_set(True)
        context.view_layer.objects.active = model
        return {"FINISHED"}


class O3D_OT_import(bpy.types.Operator, ImportHelper):
    bl_idname = "opennova_3di.import_3di"
    bl_label = "Import .3di"
    bl_description = ("Read NovaLogic .3di models (textures beside them) into the scene through opennova-3di, "
                      "each under a model root of its own")
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
        models = []
        for path in paths:
            # One file that fails leaves nothing of itself behind and does not
            # stop the others.
            try:
                model, notes = importer.import_file(context, path, self)
            except Exception as e:  # noqa: BLE001 (reported per file)
                kind = "" if isinstance(e, ImportFailed) else f"{type(e).__name__}: "
                self.report({"ERROR"}, f"{os.path.basename(path)}: {kind}{e}")
                continue
            for note in notes:
                self.report({"WARNING"}, f"{os.path.basename(path)}: {note}")
            models.append(model)
        if not models:
            return {"CANCELLED"}
        # Models imported together that the game draws together: a skinned
        # model whose bones are another's parts (arms on a first-person gun).
        pairs = assembly.pair_imported(context, models)
        for ob in context.selected_objects:
            ob.select_set(False)
        models[-1].select_set(True)
        context.view_layer.objects.active = models[-1]
        self.report({"INFO"}, "imported " + ", ".join(m.name for m in models) +
                    "".join(f"; {skin.name} follows {rig.name}" for skin, rig in pairs))
        return {"FINISHED"}


def export_animation_sets(op, context, models):
    wrote = []
    for model in models:
        try:
            message, notes = animation.export_animations(context, model)
        except ExportError as e:
            op.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            op.report({"WARNING"}, f"{model.name}: {note}")
        wrote.append(message)
    op.report({"INFO"}, "; ".join(wrote))
    return {"FINISHED"}


class O3D_OT_export_anim(bpy.types.Operator):
    bl_idname = "opennova_3di.export_anim"
    bl_label = "Export Animations"
    bl_description = ("Write the active model's clip set as a NovaLogic .adm table and its .bad clips "
                      "through opennova-3di")

    def execute(self, context):
        model = active_model(context)
        if model is None:
            self.report({"ERROR"}, "select an object of the model whose animations to export")
            return {"CANCELLED"}
        return export_animation_sets(self, context, [model])


class O3D_OT_export_all_anim(bpy.types.Operator):
    bl_idname = "opennova_3di.export_all_anim"
    bl_label = "Export All Animations"
    bl_description = "Write every rigged model's clip set to its own .adm"

    def execute(self, context):
        models = animation.models_with_rigs(context.scene)
        if not models:
            self.report({"ERROR"}, "the scene holds no rigged model (a skinned model's LOD 0 carries "
                                   "its BN## armature)")
            return {"CANCELLED"}
        # A rig with no clip set of its own (the arms beside a first-person
        # gun, whose set the gun carries) is skipped, not an error.
        ready = []
        for model in models:
            gap = animation.clip_set_gap(model)
            if gap is None:
                ready.append(model)
            else:
                self.report({"WARNING"}, f"{model.name}: {gap}; skipped")
        if not ready:
            self.report({"ERROR"}, "no rigged model carries a clip set (a table row and a clip)")
            return {"CANCELLED"}
        return export_animation_sets(self, context, ready)


class O3D_OT_import_anim(bpy.types.Operator, ImportHelper):
    bl_idname = "opennova_3di.import_anim"
    bl_label = "Import Animations"
    bl_description = ("Read a NovaLogic .adm clip table (or one .bad clip) onto the active model's rig "
                      "through opennova-3di")
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".adm"
    filter_glob: StringProperty(default="*.adm;*.bad", options={"HIDDEN"})
    align_rest: BoolProperty(name="Rest pose from the reset clip", default=True,
                             description="On a rig that holds no clip yet, turn each rest bone onto "
                                         "the reset clip's bind, so a clip shows the pose the game "
                                         "draws. Heads, lengths and weights do not move, so the "
                                         "model still exports the same model")

    def execute(self, context):
        model = active_model(context)
        if model is None:
            self.report({"ERROR"}, "select an object of the model whose rig these clips animate")
            return {"CANCELLED"}
        try:
            message, notes = anim_import.import_file(context, self.filepath, model, self)
        except (ImportFailed, ExportError) as e:
            self.report({"ERROR"}, f"{os.path.basename(self.filepath)}: {e}")
            return {"CANCELLED"}
        for note in notes:
            self.report({"WARNING"}, f"{os.path.basename(self.filepath)}: {note}")
        self.report({"INFO"}, f"imported {message}")
        return {"FINISHED"}


class O3D_OT_add_row(bpy.types.Operator):
    bl_idname = "opennova_3di.add_row"
    bl_label = "Add Row"
    bl_description = "Add a .adm row: an anim slot and the clip ring that answers it"

    def execute(self, context):
        model = active_model(context)
        if model is None:
            return {"CANCELLED"}
        row = model.o3d.rows.add()
        row.variants.add()
        return {"FINISHED"}


class O3D_OT_remove_row(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_row"
    bl_label = "Remove Row"
    index: IntProperty()

    def execute(self, context):
        active_model(context).o3d.rows.remove(self.index)
        return {"FINISHED"}


class O3D_OT_add_variant(bpy.types.Operator):
    bl_idname = "opennova_3di.add_variant"
    bl_label = "Add Variant"
    bl_description = "Add a clip to this row's ring (the engine serves a row from its last back)"
    row: IntProperty()

    def execute(self, context):
        active_model(context).o3d.rows[self.row].variants.add()
        return {"FINISHED"}


class O3D_OT_remove_variant(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_variant"
    bl_label = "Remove Variant"
    row: IntProperty()
    index: IntProperty()

    def execute(self, context):
        active_model(context).o3d.rows[self.row].variants.remove(self.index)
        return {"FINISHED"}


def menu_import_anim(self, context):
    self.layout.operator(O3D_OT_import_anim.bl_idname, text="NovaLogic Animations (.adm, .bad)")


class O3D_OT_assign_weapon_action(bpy.types.Operator):
    bl_idname = "opennova_3di.assign_weapon_action"
    bl_label = "Assign Weapon Action"
    bl_description = "Keep the active Action on an NLA track and bind it to a weapon animation slot"
    bl_options = {"REGISTER", "UNDO"}
    role: EnumProperty(name="Role", items=[("RESET", "Bind / Reset", "The rig's authored bind pose")] + weapon.ROLES[1:])

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        model = active_model(context)
        rig = animation.rig_of(model) if model else None
        if rig is None or rig.animation_data is None or rig.animation_data.action is None:
            self.report({"ERROR"}, "select a model's rig with an active Action")
            return {"CANCELLED"}
        if rig.animation_data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode before assigning an Action")
            return {"CANCELLED"}
        action = rig.animation_data.action
        try:
            weapon.clip_bounds(action)
            weapon.assign_action(model, action, self.role)
            # Retail's weapon clips are one-shots, its idle holds too (M4's
            # m4_1i / m4_1i2: flags 0x0 / 0x2); the idle ACTION replays them.
            action.o3d.loop = False
            if self.role != "RESET":
                weapon.initialize_markers(action, self.role)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        if context.area and context.area.type == "DOPESHEET_EDITOR":
            context.space_data.show_pose_markers = True
        return {"FINISHED"}


class O3D_OT_timing_marker(bpy.types.Operator):
    bl_idname = "opennova_3di.timing_marker"
    bl_label = "Set Timing Marker"
    bl_description = "Place this Action-local marker at the current frame"
    bl_options = {"REGISTER", "UNDO"}
    name: EnumProperty(items=[(n, n[3:], "") for n in weapon.MARKERS])

    def execute(self, context):
        ob = context.object
        action = ob.animation_data.action if ob and ob.animation_data else None
        if action is None:
            return {"CANCELLED"}
        if ob.animation_data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode to place a marker in Action time")
            return {"CANCELLED"}
        weapon.set_marker(action, self.name, context.scene.frame_current)
        if context.area and context.area.type == "DOPESHEET_EDITOR":
            context.space_data.show_pose_markers = True
        return {"FINISHED"}


class O3D_OT_edit_clip(bpy.types.Operator):
    bl_idname = "opennova_3di.edit_clip"
    bl_label = "Edit Clip"
    bl_description = "Activate this clip on its rig and show its Action-local markers"
    bl_options = {"REGISTER", "UNDO"}
    row: IntProperty()
    variant: IntProperty()

    def execute(self, context):
        model = active_model(context)
        rig = animation.rig_of(model) if model else None
        if rig is None:
            return {"CANCELLED"}
        action = model.o3d.rows[self.row].variants[self.variant].action
        if action is None:
            return {"CANCELLED"}
        if rig.animation_data and rig.animation_data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode before changing clips")
            return {"CANCELLED"}
        start, end, fps = weapon.clip_bounds(action)
        for ob in context.selected_objects:
            ob.select_set(False)
        rig.hide_set(False)
        rig.select_set(True)
        context.view_layer.objects.active = rig
        rig.animation_data_create()
        rig.animation_data.use_nla = False
        rig.animation_data.action = action
        strip = next((s for a,s in animation.clip_strips(rig) if a == action), None)
        if hasattr(rig.animation_data, "action_slot"):
            rig.animation_data.action_slot = strip.action_slot if strip else next(iter(action.slots), None)
        context.scene.frame_start, context.scene.frame_end = start, end
        context.scene.render.fps, context.scene.render.fps_base = fps, 1.0
        context.scene.frame_set(start)
        for area in context.screen.areas:
            if area.type == "DOPESHEET_EDITOR":
                area.spaces.active.mode = "ACTION"
                area.spaces.active.show_pose_markers = True
        return {"FINISHED"}


class O3D_OT_preview_weapon(bpy.types.Operator):
    bl_idname = "opennova_3di.preview_weapon"
    bl_label = "Preview Game Timing"
    bl_description = "Run the authored action windows through the engine's weapon FSM"

    def execute(self, context):
        model = active_model(context)
        if model is None:
            return {"CANCELLED"}
        try:
            preview, snippet = weapon.compile_timing(context, model)
        except ExportError as e:
            model.o3d.weapon_preview = ""
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        model.o3d.weapon_preview = json.dumps(preview)
        name = f"{model.name} - Weapon Timing"
        report = bpy.data.texts.get(name) or bpy.data.texts.new(name)
        report.clear()
        report.write(snippet + "\n// Native FSM trace (tick zero is each scenario's entry):\n")
        for e in preview["events"]:
            report.write(f"// {e['scenario']:12} tick {e['tick']:5} {e['action']:12} {e['kind']:16} clip {e['clip_seconds']:.6f}s\n")
        self.report({"INFO"}, f"{preview['rpm']:.2f} RPM, {preview['cycle_ticks']} ticks/shot. Full trace: {name}")
        return {"FINISHED"}


def draw_weapon(layout, model):
    p = model.o3d
    layout.prop(p, "weapon_enabled")
    if not p.weapon_enabled:
        return
    box = layout.box()
    box.label(text="Weapon timing")
    box.prop(p, "weapon_mode")
    box.prop(p, "weapon_cadence")
    if p.weapon_cadence == "RPM":
        box.prop(p, "weapon_rpm")
        box.label(text="Fire Ready marker is unused")
    box.operator("opennova_3di.preview_weapon", icon="PLAY")
    if p.weapon_preview:
        try:
            preview = json.loads(p.weapon_preview)
            fresh = preview["signature"] == weapon.signature(weapon.request_text(model))
            if not fresh:
                box.label(text="Timing changed: preview again", icon="ERROR")
            else:
                box.label(text=f"Actual: {preview['rpm']:.2f} RPM / {preview['cycle_ticks']} ticks")
                box.label(text=f"Requested: {preview['requested_rpm']:.2f} RPM")
                if abs(preview['rpm'] - preview['requested_rpm']) > 0.01:
                    box.label(text="Rounded to whole game ticks", icon="INFO")
                for row in preview["rows"]:
                    box.label(text=f"{row['role']}: delaystart {row['delaystart']}, delayend {row['delayend']}")
        except (ExportError, ValueError, KeyError):
            box.label(text="Timing changed: preview again", icon="ERROR")
    box.label(text="Exports <table>_weapon_actions.txt")
    box.label(text="Reload ammo applies on entry; switches have a fixed timer")


def draw_animations(layout, model):
    """The model's clip set: the table's rows, and what writes them."""
    p = model.o3d
    rig = animation.rig_of(model)
    box = layout.box()
    box.label(text="Animations", icon="ARMATURE_DATA")
    if rig is None:
        box.label(text="No rig: a skinned model's LOD 0 carries its BN## armature")
        return
    box.prop(p, "adm_path")
    box.prop(p, "clip_prefix")
    if not p.adm_path:
        box.label(text=f"Writes {animation.adm_default(model)}")
    clips = animation.clip_actions(rig)
    box.label(text=f"{len(clips)} clips on {rig.name} (its NLA tracks)")
    box.prop_search(p, "head_bone", rig.data, "bones", text="Head")
    if not p.head_bone:
        heads = animation.head_candidates(rig)
        box.label(text=f"Head: {heads[0].name} (its name ends in 'head')" if len(heads) == 1 else
                  "No head: each top is the bottom" if not heads else "Several bones end in 'head': choose one")
    box.label(text="Root bone: the ground; hips BN01; top: the head's height")
    draw_weapon(box, model)
    for i, row in enumerate(p.rows):
        line = box.box()
        head = line.row()
        head.prop(row, "key", text="")
        head.operator("opennova_3di.add_variant", text="", icon="ADD").row = i
        head.operator("opennova_3di.remove_row", text="", icon="X").index = i
        if p.weapon_enabled:
            line.prop(row, "weapon_role")
            if row.weapon_role != "NONE":
                for prop in ("weapon_sound", "weapon_end_sound", "weapon_particle", "weapon_userpoint"):
                    line.prop(row, prop)
        for v, variant in enumerate(row.variants):
            entry = line.row()
            entry.prop(variant, "action", text="")
            edit = entry.operator("opennova_3di.edit_clip", text="", icon="ACTION")
            edit.row, edit.variant = i, v
            drop = entry.operator("opennova_3di.remove_variant", text="", icon="X")
            drop.row = i
            drop.index = v
    box.operator("opennova_3di.add_row", icon="ADD")
    box.operator("opennova_3di.export_anim", icon="EXPORT")


class O3D_PT_action(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "DOPESHEET_EDITOR"
    bl_region_type = "UI"
    bl_category = "Action"

    @classmethod
    def poll(cls, context):
        ob = getattr(context, "object", None)
        return (ob is not None and ob.type == "ARMATURE" and ob.animation_data is not None and
                ob.animation_data.action is not None)

    def draw(self, context):
        action = context.object.animation_data.action
        col = self.layout.column()
        col.label(text=f"Clip {action.name}")
        col.prop(action.o3d, "fps")
        col.prop(action.o3d, "frames")
        col.prop(action.o3d, "loop")
        col.prop(action.o3d, "translation")
        col.prop(action.o3d, "raw_flag_8")
        col.prop(context.object.o3d, "anim_trigger")
        col.label(text="Key the trigger per frame: 1/2 footsteps, 4/8/16 fire, 0x20+ foley")
        box = col.box()
        box.label(text="Weapon timing (Action-local markers)")
        box.operator("opennova_3di.assign_weapon_action")
        model = active_model(context)
        roles = {r.weapon_role for r in model.o3d.rows if r.weapon_role != "NONE" and
                 any(v.action == action for v in r.variants)} if model else set()
        names = {weapon.READY} if roles - weapon.FIXED_ROLES else set()
        if roles & weapon.FIXED_ROLES:
            box.label(text="Fixed engine switch timer; inspect Preview Game Timing")
        if "fire" in roles:
            names.add(weapon.SHOT)
        if "recoil" in roles:
            names.add(weapon.EJECT)
        if roles - {"fire", "recoil"} - weapon.IDLE_ROLES - weapon.FIXED_ROLES:
            names.add(weapon.ACTIVE)
        for name in weapon.MARKERS:
            if name not in names:
                continue
            row = box.row()
            marker = action.pose_markers.get(name)
            if marker:
                row.prop(marker, "frame", text=name[3:])
            else:
                row.label(text=name[3:])
            row.operator("opennova_3di.timing_marker", text="At Playhead").name = name
        if model and roles:
            draw_weapon(box, model)


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
        row = col.row()
        row.operator("opennova_3di.import_3di", icon="IMPORT")
        row.operator("opennova_3di.import_anim", icon="IMPORT")
        row.operator("opennova_3di.add_model", icon="ADD")
        col.separator()
        col.prop(p, "forward")
        col.prop(p, "write_textures")
        col.prop(p, "cli_path")
        if not p.is_property_set("cli_path") or not p.cli_path:
            col.label(text="Bundled executable (automatic)")
        model = active_model(context)
        box = col.box()
        if model is None:
            box.label(text="No model: select one of its objects")
        else:
            box.label(text=f"Model {model.name}", icon="OBJECT_DATA")
            draw_model(box, model)
            box.operator("opennova_3di.export", icon="EXPORT")
            draw_animations(box, model)
        col.operator("opennova_3di.export_all", icon="EXPORT")
        col.operator("opennova_3di.export_all_anim", icon="EXPORT")


def draw_model(layout, model):
    p = model.o3d
    layout.prop(p, "model_name")
    layout.prop(p, "output_path")
    layout.prop(p, "poly_collision_lod")
    layout.prop(p, "export_bullet_faces")
    if assembly.armatures(model):
        layout.prop(p, "drive_rig")
    layout.prop(p, "mount_parent")
    if p.mount_parent is not None:
        layout.prop(p, "mount_point")


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
        if export.is_model_root(ob):
            draw_model(layout, ob)
            draw_animations(layout, ob)
            return
        if "_lod_index" in ob:
            layout.prop(p, "lod_threshold")
            layout.prop(p, "lod_type")
            return
        if not export.PART_RE.match(name):
            return
        draw_part_animation(layout, p, False)


def draw_part_animation(layout, p, bone):
    layout.label(text="Part animation (PANM)")
    if bone:
        layout.prop(p, "frame")
    layout.prop(p, "panm_flags")
    for i, t in enumerate(p.tracks):
        box = layout.box()
        row = box.row()
        row.prop(t, "target")
        op = row.operator("opennova_3di.remove_track", text="", icon="X")
        op.index = i
        op.bone = bone
        draw_style(box, t, "style", "register")
        if t.style <= CTRL_REFERENCE_THRESHOLD:
            box.prop(t, "param")
        row = box.row()
        row.prop(t, "rate")
        row.prop(t, "start")
        row.prop(t, "end")
        if t.target == "trans":
            box.prop(t, "axis")
    layout.operator("opennova_3di.add_track", icon="ADD").bone = bone


class O3D_PT_bone(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "bone"

    @classmethod
    def poll(cls, context):
        return getattr(context, "bone", None) is not None and \
            export.BONE_RE.match(export.clean_name(context.bone.name)) is not None

    def draw(self, context):
        draw_part_animation(self.layout, context.bone.o3d, True)


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
        table = catalog()[2]
        if not table:
            layout.label(text=catalog_error() or "No shader table", icon="ERROR")
        elif tag:
            known = next((flags for name, flags in table if name.lower() == tag.lower()), None)
            traits = [label for bit, label in ((materials.FLAG_BLENDING, "alpha pass"), (materials.FLAG_GLASS, "glass"),
                                               (materials.FLAG_EMISSIVE, "emissive"),
                                               (materials.FLAG_TANGENT, "tangents"),
                                               (materials.FLAG_SKINNED, "skinned")) if (known or 0) & bit]
            layout.label(text=(", ".join(traits) if traits else "opaque") if known is not None else
                         "Not in the engine's shader table", icon="NONE" if known is not None else "ERROR")
        else:
            layout.label(text="No shader in the name: export picks one by texture count")
        box = layout.box()
        box.label(text="Bullet faces")
        row = box.row()
        row.prop(p, "surface")
        row = box.row()
        row.prop(p, "face_never_hit")
        row.prop(p, "face_front_only")
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
        box.label(text="Textures (none: the image wired to Base Color, slot 1)")
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


CLASSES = (O3DTrack, O3DAdmVariant, O3DAdmRow, O3DActionProps, O3DObjectProps, O3DBoneProps, O3DTexture,
           O3DMaterialProps, O3DLightProps, O3DSceneProps,
           O3D_OT_add_track, O3D_OT_remove_track, O3D_OT_add_texture, O3D_OT_remove_texture, O3D_OT_export,
           O3D_OT_export_all, O3D_OT_add_model, O3D_OT_import,
           O3D_OT_export_anim, O3D_OT_export_all_anim, O3D_OT_import_anim, O3D_OT_add_row,
           O3D_OT_remove_row, O3D_OT_add_variant, O3D_OT_remove_variant,
           O3D_OT_assign_weapon_action, O3D_OT_timing_marker, O3D_OT_edit_clip, O3D_OT_preview_weapon,
           O3D_PT_action, O3D_PT_scene, O3D_PT_object, O3D_PT_bone, O3D_PT_light, O3D_PT_material)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.Object.o3d = PointerProperty(type=O3DObjectProps)
    bpy.types.Bone.o3d = PointerProperty(type=O3DBoneProps)
    bpy.types.Action.o3d = PointerProperty(type=O3DActionProps)
    bpy.types.Material.o3d = PointerProperty(type=O3DMaterialProps)
    bpy.types.Light.o3d = PointerProperty(type=O3DLightProps)
    bpy.types.Scene.o3d = PointerProperty(type=O3DSceneProps)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    bpy.types.TOPBAR_MT_file_import.append(menu_import_anim)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import_anim)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    del bpy.types.Scene.o3d
    del bpy.types.Light.o3d
    del bpy.types.Material.o3d
    del bpy.types.Action.o3d
    del bpy.types.Bone.o3d
    del bpy.types.Object.o3d
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
