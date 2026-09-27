# OpenNova 3DI: the Blender front end of opennova-3di.
#
# Export reads a model's parts (rig.py: PN## empties, or the BN## bones of the
# model's one armature), what sits on each (meshes of any name, `_## center`,
# `_## attach`, `_## hit` helpers, `UP<c>## <label>` user points, LP## lights,
# `<code>##-colonly` collision volumes, `<OB|OS|OP|OH>##-occonly` occlusion;
# the table heads export.py), writes the .o3d scene text
# (docs/threedi/o3d-scene-format.md) and hands it to the bundled opennova-3di
# CLI, which mints the .3di through the engine's own writer. Import runs the
# same CLI backwards (`opennova-3di scene`) and lays the .o3d out in the same
# shape (importer.py), so an imported model exports again. Nothing here
# encodes or decodes 3DI3: frames, quantization and chunk layout belong to the
# engine (formats/threedi/threedi_build.h). The properties below carry only
# what the scene cannot: LOD thresholds and types, part animation tracks and a
# part's stored parent, a skinned model's mesh part, a material's shader,
# generators and the texture rows its nodes cannot give, light generators, the
# bullet-face surface and flags, the collision LOD. What the engine derives
# (collision planes, seam flags, tangents, bounds) is never stored in the
# scene: export recomputes it from the meshes every time.

import collections
import importlib
import os
import sys

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                       IntProperty, PointerProperty, StringProperty)
from bpy_extras.io_utils import ExportHelper, ImportHelper
from mathutils import Matrix

# Blender re-runs this file when the extension is updated or scripts are
# reloaded, but keeps the submodules it imported before: reload them first so
# the property groups registered here and the code that reads them agree.
for _name in ("o3dtext", "rig", "materials", "export", "importer", "assembly", "animation", "anim_import",
              "weapon"):
    if f"{__name__}.{_name}" in sys.modules:
        importlib.reload(sys.modules[f"{__name__}.{_name}"])
from . import anim_import, animation, assembly, export, importer, materials, rig, weapon
from .rig import active_model
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

# A path property takes a blend-relative `//` path without a warning.
PATH_OPTIONS = {"PATH_SUPPORTS_BLEND_RELATIVE"}


# The engine's tables `opennova-3di catalog` prints: the CTRL register names,
# the generator style names {code: name}, the shader tags with their capability
# words [(tag, flags)], the 252 anim slots [(index, key)], the weapon actions
# that have a slot of their own [(suffix, slot index, key)] and the animation
# event bits [(mask, NAME)], each in the engine's order.
Catalog = collections.namedtuple("Catalog", "registers styles shaders slots actions triggers")


def read_catalog():
    """The catalog entry of the current executable, run once per executable
    (a new path or a rebuilt file reads again; a missing file is looked for
    again next time without running anything)."""
    path = cli_path()
    try:
        key = (path, os.path.getmtime(path))
    except OSError:
        return Catalog([], {}, [], [], [], []), f"opennova-3di not found at {path}"
    if key not in _catalog:
        table = Catalog([], {}, [], [], [], [])
        problem = None
        try:
            out = run_cli(None, ["catalog"], ExportError, timeout=10).stdout
        except ExportError as e:
            out, problem = "", f"opennova-3di catalog failed: {e}"
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "register":
                table.registers.append(parts[1])
            elif len(parts) == 3 and parts[0] == "style":
                table.styles[int(parts[1])] = parts[2]
            elif len(parts) == 3 and parts[0] == "shader":
                table.shaders.append((parts[1], int(parts[2], 16)))
            elif len(parts) == 3 and parts[0] == "animslot":
                table.slots.append((int(parts[1]), parts[2]))
            elif len(parts) == 4 and parts[0] == "weaponaction":
                table.actions.append((parts[1], int(parts[2]), parts[3]))
            elif len(parts) == 3 and parts[0] == "trigger":
                table.triggers.append((int(parts[1], 16), parts[2]))
        if not table.shaders and problem is None:
            problem = f"opennova-3di catalog ({path}) printed no shader table"
        _catalog.clear()
        _catalog[key] = (table, problem)
    return _catalog[key]


def catalog():
    """The engine's own tables (Catalog), read from `opennova-3di catalog`;
    no Python copy. Empty tables when it failed: catalog_error() says why."""
    return read_catalog()[0]


def catalog_error():
    """Why catalog() is empty, or None."""
    return read_catalog()[1]


def search_registers(self, context, edit_text):
    text = edit_text.upper()
    return [r for r in catalog()[0] if text in r]


def search_slots(self, context, edit_text):
    """The engine's 252 anim slot keys; a row names its slot by what follows
    the key's first five characters, and export refuses any other."""
    text = edit_text.lower()
    return [k for _, k in catalog().slots if text in k]


def search_shaders(self, context, edit_text):
    text = edit_text.upper()
    return [tag for tag, _ in catalog()[2] if text in tag]


def style_label(style):
    return catalog()[1].get(style, "?")


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
    return rig.is_model_root(ob) and ob is not self.id_data


def is_camera(self, ob):
    return ob.type == "CAMERA"


def update_mount(self, context):
    assembly.mount(self.id_data, context.scene)


def search_mount_points(self, context, edit_text):
    if self.mount_parent is None:
        return []
    text = edit_text.lower()
    return [label for label, _ in assembly.user_points(self.mount_parent) if text in label.lower()]


# What a part's parent is when the hierarchy cannot say it (rig.stored_parent):
# retail stores a part that names itself (Eturret's turret) and parts that name
# none (Excavatr's arm, Chair03X's pieces). Only an import sets the others.
PART_PARENTS = [
    ("HIERARCHY", "Hierarchy", "The PN## or bone above it (none: part 01)"),
    ("SELF", "Itself", "The part names itself as its parent"),
    ("NONE", "None", "The part names no parent (-1)"),
]


def part_parent_prop():
    return EnumProperty(name="Parent", items=PART_PARENTS, default="HIERARCHY",
                        description="The part's parent as the file stores it: the hierarchy's, or itself or none, "
                                    "which a hierarchy cannot say (retail Eturret, Excavatr, Chair03X)")


class O3DAdmVariant(bpy.types.PropertyGroup):
    action: PointerProperty(name="Clip", type=bpy.types.Action,
                            description="The Action this clip plays. Its file is named after the table and the "
                                        "row's slot (<table>_<slot code>.bad)")


class O3DAdmRow(bpy.types.PropertyGroup):
    # One .adm row: an anim slot and its clip ring, in the order the file
    # stores. The engine serves a ring from its LAST clip back, one step at
    # every play and loop wrap [orig: AnimMap_RegisterBoneNode @0x40c385;
    # AnimMap_AdvanceToNextAnim @0x40bdf0].
    key: StringProperty(name="Slot", default="anim_reset", search=search_slots,
                        description="The anim slot this row answers, one of the engine's 252 named by what follows "
                                    "the key's first five characters: anim_reset is the bind and the rig's rest "
                                    "pose, anim_wpn_<action> a weapon action's clips")
    variants: CollectionProperty(type=O3DAdmVariant)


class O3DWeaponEntry(bpy.types.PropertyGroup):
    # A weapon.def entry that plays the model's clips (retail's AK47AUTO and
    # AK47 share AKM_1st's), timed by the weapon FSM in its own fire mode.
    name: StringProperty(name="Entry", default="",
                         description="The weapon.def entry that plays these clips (WPN_AK47AUTO): 1 to 31 letters, "
                                     "digits, _ - or .")
    mode: EnumProperty(name="Fire mode", default="auto", items=[
        ("semi", "Semi-auto", "A fresh press from Idle; the preview measures the earliest repeat the game takes"),
        ("auto", "Automatic", "Hold the trigger to repeat through the recoil"),
        ("burst", "Burst", "Three rounds a press; the preview measures the rate within a burst")],
                       description="The mode the entry's FLAGS fire in (Merge into weapon.def checks it)")
    rpm: FloatProperty(name="Target RPM", default=600.0, min=1.0, max=1875.0, precision=1,
                       description="The rate this entry should fire at. The game fires on whole ticks, so the "
                                   "preview shows the rate it gets (auto and burst at most 1875, semi 1250)")


class O3DActionProps(bpy.types.PropertyGroup):
    # A clip's own rate and the one flag nothing derives, on its Action. Its
    # length is the Action's manual frame range, its loop the Action's Cyclic
    # setting, its translations whether a part moves off its rest offset;
    # everything else a .bad carries is derived on export
    # (formats/bad/bad_build.h).
    fps: IntProperty(name="Clip rate", default=30, min=1, max=255,
                     description="The frames a second the game plays this clip at; every retail clip ships 30. "
                                 "Blender plays it at the scene's rate")
    raw_flag_8: BoolProperty(name="Flag 8", default=False,
                             description="The clip flag bit 3, which 73 of the 477 retail clips carry (the viewmodel "
                                         "draw clips, the bikes) and nothing has been seen to read; carried as set")


class O3DObjectProps(bpy.types.PropertyGroup):
    # On a model root (the Empty above a model's LOD roots): one .3di.
    model_name: StringProperty(name="Model name", default="",
                               description="GHDR name, 15 chars max; empty: the output file's name")
    output_path: StringProperty(name="Output .3di", subtype="FILE_PATH", default="", options=PATH_OPTIONS,
                                description="The .3di export writes; empty: the model root's name beside the "
                                            ".blend (//<name>.3di). Its file name has at most 15 characters, as a "
                                            "game archive holds it")
    export_bullet_faces: BoolProperty(name="Generate bullet faces", default=True,
                                      description="The collision LOD's render triangles are also its bullet faces; "
                                                  "off for a model that needs none (first-person arms). Its "
                                                  "collision volumes still export")
    poly_collision_lod: IntProperty(name="Collision LOD", default=0, min=0,
                                    description="The render LOD whose part meshes also become the bullet faces (the OED "
                                                ".3dp poly_collision_lod); 0 = the most detailed")
    attach_points: EnumProperty(name="Attach points", default="PARTS", items=[
        ("PARTS", "One per part", "A CXLT row for every collision section after the root (every section on a "
                                  "skinned model), at its part's _attach Empty or its pivot: the table nearly every "
                                  "retail model stores"),
        ("HELPERS", "The attach helpers", "Exactly the collision LOD's _attach Empties, a row each in export order, "
                                          "none an empty table: a table one per part cannot say (M24_1st's rows in "
                                          "parent order, Chair03X's none), which import sets")])
    mesh_part: BoolProperty(name="Mesh part", default=False,
                            description="A skinned model keeps its skinned geometry on a part of its own after the "
                                        "bones, at its mesh's origin, whose collision section holds the bullet faces "
                                        "(the retail layout of US01 and ArmsG); off, on the root part, whose section "
                                        "holds them (Delta04, ArmGlovD)")
    # Display-only assembly (assembly.py); export never reads these.
    mount_parent: PointerProperty(name="Mount on", type=bpy.types.Object, poll=is_other_model, update=update_mount,
                                  description="Place this model on another model, as an ITEMS.DEF addeweap child "
                                              "(the M1A1's turret) sits on its parent (display only)")
    mount_point: StringProperty(name="At user point", default="", search=search_mount_points, update=update_mount,
                                description="The parent's user point (the addeweap row's name, e.g. ewep01); none or "
                                            "not found: the parent's root")
    # On a model root: its animations (the clip set its rig carries).
    adm_path: StringProperty(name="Output .adm", subtype="FILE_PATH", default="", options=PATH_OPTIONS,
                             description="Where Export Animations writes the clip table, and beside it each clip as "
                                         "<table>_<slot code>.bad (15 characters at most, as a retail archive "
                                         "holds). Empty: //<model name>.adm")
    rows: CollectionProperty(type=O3DAdmRow)
    head_bone: StringProperty(name="Head bone", default="",
                              description="On a rig with a Root bone: the bone whose height above Root is each "
                                          "frame's top (the body's capsule top). Empty: the one bone whose name ends "
                                          "in 'head'; with none, the top is the bottom (the hips' height)")
    # On a first-person gun's root: the weapon.def entries its clips time, and
    # the eyes the view positions are measured from (weapon.py).
    weapons: CollectionProperty(type=O3DWeaponEntry)
    hip_camera: PointerProperty(name="Hip view", type=bpy.types.Object, poll=is_camera,
                                description="A camera at the eye the gun is seen from at the hip: the entries' POS "
                                            "(the game looks along the model's forward from it)")
    aim_camera: PointerProperty(name="Aim view", type=bpy.types.Object, poll=is_camera,
                                description="A camera at the eye the gun is aimed from: the entries' TPOS")
    tracks: CollectionProperty(type=O3DTrack)
    panm_flags: IntProperty(name="PANM flags", default=-1,
                            description="The part's raw PANM flags word; -1 derives it from the tracks")
    part_parent: part_parent_prop()
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
    # A rig's part, on its BN## bone: its part animation, stored parent and,
    # on a skinned model, whether its section takes hits.
    hit_sphere: BoolProperty(name="Hit sphere", default=True,
                             description="On a skinned model, this bone's collision section has a hit sphere and "
                                         "box: its `_## hit` and `_## bounds` empties, else the ones the LOD 0 "
                                         "vertices it moves give. Off, the section stores none and no shot finds "
                                         "it, as some retail bones")
    tracks: CollectionProperty(type=O3DTrack)
    panm_flags: IntProperty(name="PANM flags", default=-1,
                            description="The part's raw PANM flags word; -1 derives it from the tracks")
    part_parent: part_parent_prop()
    frame: FloatVectorProperty(name="Track frame", subtype="MATRIX", size=(3, 3),
                               default=((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)),
                               description="The axes this part's tracks turn about (its PANM MTRX row), as a "
                                           "turn of the rig's axes (a mirror turns them the other way); the "
                                           "identity: the model's own (dM1A1's turret ring and wheels turn 90 "
                                           "degrees)")


class O3DTexture(bpy.types.PropertyGroup):
    # A texture row the material's nodes cannot say (materials.py): a
    # flipbook frame, row flags, a normal map file, a file Blender cannot
    # open. A slot the list gives is taken from the list, not the nodes.
    name: StringProperty(name="File", default="",
                         description="The texture's file name: printable ASCII, at most 16 characters, no folder; "
                                     "empty, a row that names no file (retail keeps some as placeholders). A file "
                                     "export writes (Write) is <name>.tga or <name>.mdt, one dot and at most 15 "
                                     "characters")
    slot: IntProperty(name="Slot", default=1, min=0, max=255,
                      description="1 the diffuse texture, 2 the detail texture (on the second UV map), 3 the "
                                  "normal map (4 a second normal map, which no retail model uses)")
    type: IntProperty(name="Type", default=0, min=0, max=255,
                      description="0 an image. 4 or 5 a normal map: an .mdt file holds the normals, a .tga file "
                                  "a height in its alpha that the game turns into normals")
    flags: IntProperty(name="Flags", default=0, min=0, max=255,
                       description="1: a flipbook frame (animated). 2: the render-state override: bind the batch's "
                                   "override texture in place of this row when one is pushed, which only the "
                                   "player preview does")
    frame: IntProperty(name="Frame", default=0, min=0, max=255, description="The row's flipbook frame")
    image: PointerProperty(name="Image", type=bpy.types.Image)
    write: BoolProperty(name="Write", default=True,
                        description="Write the image as a 32-bit TGA file under this name beside the .3di once it "
                                    "is built")


class O3DMaterialProps(bpy.types.PropertyGroup):
    # What Blender's material settings cannot say. Two-sided, the alpha test,
    # the alpha pass and the glow are Blender's own settings (materials.py).
    shader: StringProperty(name="Shader", default="", search=search_shaders, search_options={"SUGGESTION", "SORT"},
                           description="The engine shader, any in its table. Empty: export picks one by OED's rule "
                                       "for the material's textures, the one that draws its Blended render method, "
                                       "its Emission, a U or V generator and a normal map")
    order: IntProperty(name="Export order", default=-1, min=-1,
                       description="The material's index in the model; -1 sorts it after the ordered ones, in the "
                                   "order meshes first use it. A material in a mesh's slots that no face draws "
                                   "with exports only with an order (retail models keep such materials)")
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
                                              "passes through (CFAC flag 0x800; a two-sided material, Backface "
                                              "Culling off, overrides it)")
    face_other_flags: IntProperty(name="Other face flags", default=0, min=0,
                                  description="CFAC flag bits besides 1, 0x100 and 0x800 (OED wrote 2 and 0x400)")
    other_flags: IntProperty(name="Other flag bits", default=0, min=0, max=255,
                             description="Material flag bits besides the alpha test (1), its inversion (2) and "
                                         "two-sided (4), which Blender's settings give")
    # Glass and emissive follow the shader (every retail glass shader is glass,
    # every *_LUM one emissive 2); only the reflection colour is authored.
    reflect: FloatVectorProperty(name="Reflect", subtype="COLOR", size=4, default=(0, 0, 0, 0), min=0, max=1,
                                 description="Glass reflection colour (black: a glass shader's 128 grey)")
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
    write_textures: BoolProperty(name="Write textures", default=True)


class O3DPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__
    cli_path: StringProperty(name="opennova-3di", subtype="FILE_PATH", default="",
                             description="The opennova-3di executable the add-on runs; empty: the one bundled "
                                         "with it")

    def draw(self, context):
        self.layout.prop(self, "cli_path")
        if not self.cli_path:
            self.layout.label(text=f"Runs the bundled {bundled_cli_path()}")


def part_animation(context, bone):
    return context.bone.o3d if bone else context.object.o3d


class O3D_OT_add_track(bpy.types.Operator):
    bl_idname = "opennova_3di.add_track"
    bl_label = "Add Part Animation Track"
    bl_options = {"REGISTER", "UNDO"}
    bone: BoolProperty(options={"HIDDEN"})

    @classmethod
    def poll(cls, context):
        return context.object is not None

    def execute(self, context):
        part_animation(context, self.bone).tracks.add()
        return {"FINISHED"}


class O3D_OT_remove_track(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_track"
    bl_label = "Remove Track"
    bl_options = {"REGISTER", "UNDO"}
    index: IntProperty()
    bone: BoolProperty(options={"HIDDEN"})

    @classmethod
    def poll(cls, context):
        return context.object is not None

    def execute(self, context):
        part_animation(context, self.bone).tracks.remove(self.index)
        return {"FINISHED"}


class O3D_OT_add_texture(bpy.types.Operator):
    bl_idname = "opennova_3di.add_texture"
    bl_label = "Add Texture Row"
    bl_description = ("Add a texture row the material's nodes cannot give: a flipbook frame, a row with flags, a "
                      "normal map file, a file Blender cannot open")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return getattr(context, "material", None) is not None

    def execute(self, context):
        rows = context.material.o3d.textures
        t = rows.add()
        if len(rows) > 1:
            t.slot, t.flags, t.frame = rows[-2].slot, rows[-2].flags, rows[-2].frame + 1
        return {"FINISHED"}


class O3D_OT_remove_texture(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_texture"
    bl_label = "Remove Texture Row"
    bl_options = {"REGISTER", "UNDO"}
    index: IntProperty()

    @classmethod
    def poll(cls, context):
        return getattr(context, "material", None) is not None

    def execute(self, context):
        context.material.o3d.textures.remove(self.index)
        return {"FINISHED"}


def export_models(op, context, models):
    """Export the models as one run: two models that would write one file
    are refused before any is written."""
    wrote = []
    run = export.ExportRun()
    try:
        for model in models:
            run.claim(export.output_path(model), model)
    except ExportError as e:
        op.report({"ERROR"}, str(e))
        return {"CANCELLED"}
    run.paths.clear()
    for model in models:
        try:
            message, notes = export.export_model(context, model, run)
        except ExportError as e:
            op.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            op.report({"WARNING"}, f"{model.name}: {note}")
        wrote.append(message)
    op.report({"INFO"}, "; ".join(wrote))
    return {"FINISHED"}


class O3D_OT_export(bpy.types.Operator, ExportHelper):
    bl_idname = "opennova_3di.export"
    bl_label = "Export Model"
    bl_description = ("Write the active object's model as a NovaLogic .3di through opennova-3di, to its output "
                      "path")

    filename_ext = ".3di"
    filter_glob: StringProperty(default="*.3di", options={"HIDDEN"})
    browse: BoolProperty(options={"HIDDEN", "SKIP_SAVE"},
                         description="Choose the file first (File > Export); it becomes the model's output path")

    @classmethod
    def poll(cls, context):
        return active_model(context) is not None

    def invoke(self, context, event):
        if not self.browse:
            return self.execute(context)
        self.filepath = export.output_path(active_model(context))
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        model = active_model(context)
        if self.browse and self.filepath:
            model.o3d.output_path = bpy.path.relpath(self.filepath) if bpy.data.filepath else self.filepath
        return export_models(self, context, [model])


class O3D_OT_export_all(bpy.types.Operator):
    bl_idname = "opennova_3di.export_all"
    bl_label = "Export All Models"
    bl_description = "Write every model in the scene to its own .3di"

    def execute(self, context):
        models = rig.model_roots(context.scene)
        if not models:
            self.report({"ERROR"}, "the scene holds no model root (the Empty above a model's LOD roots)")
            return {"CANCELLED"}
        return export_models(self, context, models)


class O3D_OT_add_model(bpy.types.Operator):
    bl_idname = "opennova_3di.add_model"
    bl_label = "Add Model"
    bl_description = ("Add a model root with its LOD 0 root at the world origin, ready for its parts (Add Part from "
                      "Selection, or an armature's BN## bones)")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return context.mode == "OBJECT"

    def execute(self, context):
        model = bpy.data.objects.new("Model", None)
        model.empty_display_type = "CUBE"
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


class O3D_OT_add_lod(bpy.types.Operator):
    bl_idname = "opennova_3di.add_lod"
    bl_label = "Add LOD"
    bl_description = ("Add the active model's next LOD root, a coarser level of detail: its own PN## parts, or on a "
                      "skinned model meshes deforming with the model's rig")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return context.mode == "OBJECT" and active_model(context) is not None

    def execute(self, context):
        model = active_model(context)
        roots = [c for c in model.children if rig.is_lod_root(c)]
        index = max((rig.lod_index(c) for c in roots), default=-1) + 1
        lod = bpy.data.objects.new(f"{rig.clean_name(model.name)}_LOD{index}", None)
        lod.empty_display_type = "ARROWS"
        lod.empty_display_size = 0.5
        lod["_lod_index"] = index
        first = next((c for c in roots if rig.lod_index(c) == 0), None)
        lod.o3d.lod_type = first.o3d.lod_type if first is not None else "gnrc"
        for collection in model.users_collection:
            collection.objects.link(lod)
        lod.parent = model
        self.report({"INFO"}, f"{lod.name}: give each LOD but the last a threshold, falling from LOD 0")
        return {"FINISHED"}


class O3D_OT_add_part(bpy.types.Operator):
    bl_idname = "opennova_3di.add_part"
    bl_label = "Add Part from Selection"
    bl_description = ("Put the selected objects on a new PN## part: an empty at the active object's origin (its "
                      "pivot) under the part the active object sits on, else under PN01; a LOD's first part is "
                      "PN01 at the model origin")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        lod = rig.lod_of(ob) if ob is not None else None
        return context.mode == "OBJECT" and lod is not None and rig.own_rig(lod) is None

    def execute(self, context):
        active = context.active_object
        lod = rig.lod_of(active)
        parts = {}
        for ob in rig.descendants(lod):
            m = rig.PART_RE.match(rig.clean_name(ob.name))
            if m and ob.type == "EMPTY":
                parts[int(m.group(1)) - 1] = ob
        index = max(parts, default=-1) + 1
        if index > 98:
            self.report({"ERROR"}, f"{lod.name}: a part number has two digits (99 parts)")
            return {"CANCELLED"}
        above = rig.part_of(active)
        parent = parts.get(above, parts.get(0)) if index else lod
        part = bpy.data.objects.new(f"PN{index + 1:02d}", None)
        part.empty_display_type = "ARROWS"
        part.empty_display_size = 0.2
        for collection in lod.users_collection:
            collection.objects.link(part)
        pivot = lod.matrix_world.translation if index == 0 else active.matrix_world.translation
        part.parent = parent
        part.matrix_parent_inverse = parent.matrix_world.inverted_safe()
        part.matrix_basis = Matrix.Translation(pivot)
        context.view_layer.update()
        moved = 0
        for ob in context.selected_objects:
            if ob is part or ob.type == "EMPTY" and (rig.PART_RE.match(rig.clean_name(ob.name)) or
                                                    rig.is_lod_root(ob) or rig.is_model_root(ob)):
                continue
            if rig.lod_of(ob) is not lod and ob.parent is not None:
                continue
            world = ob.matrix_world.copy()
            ob.parent = part
            ob.matrix_parent_inverse = part.matrix_world.inverted_safe()
            ob.matrix_basis = world
            moved += 1
        self.report({"INFO"}, f"{part.name}: {moved} objects on it")
        return {"FINISHED"}


class O3D_OT_add_rig(bpy.types.Operator):
    bl_idname = "opennova_3di.add_rig"
    bl_label = "Add Animation Rig"
    bl_description = ("Turn the model's PN## parts into the BN## bones of an armature under its LOD 0 root, "
                      "everything on a part hung from its bone, so clips can animate the model")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        model = active_model(context)
        return context.mode == "OBJECT" and model is not None and rig.rig_of(model) is None

    def execute(self, context):
        model = active_model(context)
        try:
            arm, notes = rig.make_rig(context, model)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            self.report({"WARNING"}, note)
        self.report({"INFO"}, f"{model.name}: its parts are {arm.name}'s bones")
        return {"FINISHED"}


# The gun models a Deform with Rig of can pick, kept alive for Blender (an
# enum's items must outlive the call that made them).
_gun_items = []


def gun_models(self, context):
    """The scene's other models with a rig of their own."""
    model = active_model(context)
    _gun_items[:] = [(m.name, m.name, f"Deform with {m.name}'s rig") for m in rig.model_roots(context.scene)
                     if m is not model and rig.rig_of(m) is not None and rig.model_of(rig.rig_of(m)) is m]
    return _gun_items


class O3D_OT_share_rig(bpy.types.Operator):
    bl_idname = "opennova_3di.share_rig"
    bl_label = "Deform with Rig of"
    bl_description = ("Make the active arms model deform with a first-person gun's rig, as the game draws a gun's "
                      "arms with the gun's parts: its meshes deform with the gun's bones of the same BN## numbers, "
                      "its own rig goes and its root stands under the gun's")
    bl_options = {"REGISTER", "UNDO"}
    gun: EnumProperty(name="Gun", items=gun_models)

    @classmethod
    def poll(cls, context):
        model = active_model(context)
        arm = rig.rig_of(model) if model is not None else None
        return context.mode == "OBJECT" and arm is not None and rig.model_of(arm) is model

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        model = active_model(context)
        gun = bpy.data.objects.get(self.gun)
        if gun is None:
            self.report({"ERROR"}, "choose the gun whose rig the arms deform with")
            return {"CANCELLED"}
        try:
            notes = rig.share_rig(context, model, gun)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            self.report({"INFO"}, note)
        return {"FINISHED"}


class O3D_OT_number_parts(bpy.types.Operator):
    bl_idname = "opennova_3di.number_parts"
    bl_label = "Number Parts"
    bl_description = ("Number the rig's parts BN01, BN02, ... in hierarchy order, parents first, keeping each "
                      "bone's label: the bones already numbered and every other deforming bone but Root")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        model = active_model(context)
        arm = rig.rig_of(model) if model is not None else None
        return arm is not None and rig.model_of(arm) is model

    def execute(self, context):
        arm = rig.rig_of(active_model(context))
        mode = context.mode
        if mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            renamed = rig.number_parts(arm)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        finally:
            if mode == "EDIT_ARMATURE":
                bpy.ops.object.mode_set(mode="EDIT")
            elif mode == "POSE":
                bpy.ops.object.mode_set(mode="POSE")
        self.report({"INFO"}, f"{arm.name}: {renamed} bones renumbered")
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
        # One file that fails leaves nothing of itself behind and does not
        # stop the others; a first-person gun and its arms share one rig.
        for path, model, notes in importer.import_files(context, paths, self):
            if model is None:
                kind = "" if isinstance(notes, ImportFailed) else f"{type(notes).__name__}: "
                self.report({"ERROR"}, f"{os.path.basename(path)}: {kind}{notes}")
                continue
            for note in notes:
                self.report({"WARNING"}, f"{os.path.basename(path)}: {note}")
            models.append(model)
        if not models:
            return {"CANCELLED"}
        for ob in context.selected_objects:
            ob.select_set(False)
        models[-1].select_set(True)
        context.view_layer.objects.active = models[-1]
        self.report({"INFO"}, "imported " + ", ".join(m.name for m in models))
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


def clip_model(context):
    """The active model when it carries a clip set (a rig of its own)."""
    model = active_model(context)
    return model if model is not None and animation.clip_rig(model) is not None else None


class O3D_OT_export_anim(bpy.types.Operator):
    bl_idname = "opennova_3di.export_anim"
    bl_label = "Export Animations"
    bl_description = ("Write the active model's clip set as a NovaLogic .adm table and its .bad clips through "
                      "opennova-3di, and a first-person gun's weapon.def edits beside them")

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        return export_animation_sets(self, context, [clip_model(context)])


class O3D_OT_export_all_anim(bpy.types.Operator):
    bl_idname = "opennova_3di.export_all_anim"
    bl_label = "Export All Animations"
    bl_description = "Write every model's clip set to its own .adm"

    def execute(self, context):
        models = animation.models_with_rigs(context.scene)
        if not models:
            self.report({"ERROR"}, "the scene holds no model with a rig of its own (an Armature of BN## bones under "
                                   "its LOD 0 root)")
            return {"CANCELLED"}
        ready = []
        for model in models:
            gap = animation.clip_set_gap(model)
            if gap is None:
                ready.append(model)
            else:
                self.report({"WARNING"}, f"{model.name}: {gap}; skipped")
        if not ready:
            self.report({"ERROR"}, "no rigged model carries a clip set (a table row naming a clip)")
            return {"CANCELLED"}
        return export_animation_sets(self, context, ready)


class O3D_OT_import_anim(bpy.types.Operator, ImportHelper):
    bl_idname = "opennova_3di.import_anim"
    bl_label = "Import Animations"
    bl_description = ("Read a NovaLogic .adm clip table (or one .bad clip) onto the active model's rig through "
                      "opennova-3di; a model with PN## parts gets a rig of them first")
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".adm"
    filter_glob: StringProperty(default="*.adm;*.bad", options={"HIDDEN"})
    align_rest: BoolProperty(name="Rest pose from the reset clip", default=True,
                             description="On a rig that holds no clip yet, turn each rest bone onto the reset "
                                         "clip's bind, so a clip shows the pose the game draws. Heads, lengths, "
                                         "weights and what hangs from a bone do not move, so the model still "
                                         "exports the same model")

    @classmethod
    def poll(cls, context):
        return context.mode == "OBJECT"

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
    bl_description = "Add a table row: an anim slot and the clips that answer it"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        row = clip_model(context).o3d.rows.add()
        row.variants.add()
        return {"FINISHED"}


class O3D_OT_remove_row(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_row"
    bl_label = "Remove Row"
    bl_options = {"REGISTER", "UNDO"}
    index: IntProperty()

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        rows = clip_model(context).o3d.rows
        if not 0 <= self.index < len(rows):
            return {"CANCELLED"}
        rows.remove(self.index)
        return {"FINISHED"}


class O3D_OT_add_variant(bpy.types.Operator):
    bl_idname = "opennova_3di.add_variant"
    bl_label = "Add Clip"
    bl_description = "Add a clip to this row's ring (the game serves a ring from its last clip back)"
    bl_options = {"REGISTER", "UNDO"}
    row: IntProperty()

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        rows = clip_model(context).o3d.rows
        if not 0 <= self.row < len(rows):
            return {"CANCELLED"}
        rows[self.row].variants.add()
        return {"FINISHED"}


class O3D_OT_remove_variant(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_variant"
    bl_label = "Remove Clip"
    bl_options = {"REGISTER", "UNDO"}
    row: IntProperty()
    index: IntProperty()

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        rows = clip_model(context).o3d.rows
        if not 0 <= self.row < len(rows) or not 0 <= self.index < len(rows[self.row].variants):
            return {"CANCELLED"}
        rows[self.row].variants.remove(self.index)
        return {"FINISHED"}


def menu_import_anim(self, context):
    self.layout.operator(O3D_OT_import_anim.bl_idname, text="NovaLogic Animations (.adm, .bad)")


def edited_clip(context):
    """The rig playing an Action, and that Action, when the active object is
    a model's rig."""
    ob = getattr(context, "object", None)
    if ob is None or ob.type != "ARMATURE" or ob.animation_data is None or ob.animation_data.action is None:
        return None, None
    return ob, ob.animation_data.action


# The roles Assign Weapon Action offers, kept alive for Blender (an enum's
# items must outlive the call that made them).
_role_items = []


def weapon_roles(self, context):
    """Reset, then the weapon actions with a slot of their own (the catalog's
    weaponaction rows)."""
    _role_items[:] = [("reset", "Reset (anim_reset)", "The bind: the rig's rest pose")] + [
        (suffix, f"{suffix.capitalize()} ({key})", f"The clip of the {suffix} action, row {key}")
        for suffix, _, key in catalog().actions]
    return _role_items


class O3D_OT_assign_weapon_action(bpy.types.Operator):
    bl_idname = "opennova_3di.assign_weapon_action"
    bl_label = "Assign Weapon Action"
    bl_description = ("Make the Action the rig plays the clip of a weapon action's row (adding the row), with that "
                      "action's timing markers; its loop and other settings stay as they are")
    bl_options = {"REGISTER", "UNDO"}
    role: EnumProperty(name="Action", items=weapon_roles)

    @classmethod
    def poll(cls, context):
        arm, action = edited_clip(context)
        return action is not None and rig.model_of(arm) is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        arm, action = edited_clip(context)
        model = rig.model_of(arm)
        if arm.animation_data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode before assigning an Action")
            return {"CANCELLED"}
        try:
            weapon.clip_bounds(action)
            replaced = weapon.assign(model, action, self.role)
            weapon.initialize_markers(action, self.role)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        if replaced:
            self.report({"INFO"}, f"{action.name} replaces {', '.join(replaced)} on the row")
        if context.area and context.area.type == "DOPESHEET_EDITOR":
            context.space_data.show_pose_markers = True
        return {"FINISHED"}


class O3D_OT_timing_marker(bpy.types.Operator):
    bl_idname = "opennova_3di.timing_marker"
    bl_label = "Set Timing Marker"
    bl_description = "Place this Action-local marker at the current frame"
    bl_options = {"REGISTER", "UNDO"}
    name: EnumProperty(items=[(n, n[3:], "") for n in weapon.MARKERS])

    @classmethod
    def poll(cls, context):
        return edited_clip(context)[1] is not None

    def execute(self, context):
        arm, action = edited_clip(context)
        if arm.animation_data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode to place a marker in Action time")
            return {"CANCELLED"}
        weapon.set_marker(action, self.name, context.scene.frame_current)
        if context.area and context.area.type == "DOPESHEET_EDITOR":
            context.space_data.show_pose_markers = True
        return {"FINISHED"}


# The event bits Add Event Trigger offers, kept alive for Blender.
_trigger_items = []


def trigger_names(self, context):
    _trigger_items[:] = [(name, name, f"Event bit 0x{mask:x}") for mask, name in catalog().triggers]
    return _trigger_items


class O3D_OT_trigger_marker(bpy.types.Operator):
    bl_idname = "opennova_3di.trigger_marker"
    bl_label = "Add Event Trigger"
    bl_description = ("Mark the current frame with an animation event bit (a footstep, a fire row, a foley "
                      "sound): an Action-local marker named after the bit, which sets it on that frame's event")
    bl_options = {"REGISTER", "UNDO"}
    name: EnumProperty(name="Event", items=trigger_names)

    @classmethod
    def poll(cls, context):
        return edited_clip(context)[1] is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        arm, action = edited_clip(context)
        if arm.animation_data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode to place a marker in Action time")
            return {"CANCELLED"}
        frame = context.scene.frame_current
        if not any(m.name == self.name and m.frame == frame for m in action.pose_markers):
            action.pose_markers.new(self.name).frame = frame
        if context.area and context.area.type == "DOPESHEET_EDITOR":
            context.space_data.show_pose_markers = True
        return {"FINISHED"}


class O3D_OT_edit_clip(bpy.types.Operator):
    bl_idname = "opennova_3di.edit_clip"
    bl_label = "Edit Clip"
    bl_description = ("Play this clip on the model's rig through its slot, every channel it does not key at rest, "
                      "the timeline's preview range on its frames")
    bl_options = {"REGISTER", "UNDO"}
    row: IntProperty()
    variant: IntProperty()

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        model = clip_model(context)
        arm = animation.clip_rig(model)
        rows = model.o3d.rows
        if not 0 <= self.row < len(rows) or not 0 <= self.variant < len(rows[self.row].variants):
            return {"CANCELLED"}
        action = rows[self.row].variants[self.variant].action
        if action is None:
            return {"CANCELLED"}
        data = arm.animation_data or arm.animation_data_create()
        if data.use_tweak_mode:
            self.report({"ERROR"}, "leave NLA tweak mode before changing clips")
            return {"CANCELLED"}
        try:
            slot = animation.clip_slot(action, arm)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        # A clip shows what it keys over the rig's rest, as export reads it:
        # a channel the last clip keyed and this one does not goes back.
        animation.rest_pose(arm)
        data.action = action
        data.action_slot = slot
        start, end = animation.clip_range(action)
        scene = context.scene
        scene.use_preview_range = True
        scene.frame_preview_start, scene.frame_preview_end = start, max(start + 1, end)
        scene.frame_set(start)
        if arm.name in context.view_layer.objects:
            for ob in context.selected_objects:
                ob.select_set(False)
            arm.hide_set(False)
            arm.select_set(True)
            context.view_layer.objects.active = arm
        if context.screen is not None:
            for area in context.screen.areas:
                if area.type == "DOPESHEET_EDITOR":
                    area.spaces.active.mode = "ACTION"
                    area.spaces.active.show_pose_markers = True
        return {"FINISHED"}


class O3D_OT_add_weapon_entry(bpy.types.Operator):
    bl_idname = "opennova_3di.add_weapon_entry"
    bl_label = "Add Weapon Entry"
    bl_description = "Add a weapon.def entry these clips play for (one per fire mode that shares them)"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        clip_model(context).o3d.weapons.add()
        return {"FINISHED"}


class O3D_OT_remove_weapon_entry(bpy.types.Operator):
    bl_idname = "opennova_3di.remove_weapon_entry"
    bl_label = "Remove Weapon Entry"
    bl_options = {"REGISTER", "UNDO"}
    index: IntProperty()

    @classmethod
    def poll(cls, context):
        return clip_model(context) is not None

    def execute(self, context):
        entries = clip_model(context).o3d.weapons
        if not 0 <= self.index < len(entries):
            return {"CANCELLED"}
        entries.remove(self.index)
        return {"FINISHED"}


class O3D_OT_preview_weapon(bpy.types.Operator):
    bl_idname = "opennova_3di.preview_weapon"
    bl_label = "Preview Game Timing"
    bl_description = ("Measure the weapon entries' timing with the engine's weapon FSM: each entry's rate, the "
                      "delays its ACTION blocks get and how much of each clip the viewmodel shows")

    @classmethod
    def poll(cls, context):
        model = clip_model(context)
        return model is not None and len(model.o3d.weapons) > 0

    def execute(self, context):
        model = clip_model(context)
        try:
            lines, notes = weapon.preview(context, model)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            self.report({"WARNING"}, note)
        self.report({"INFO"}, lines[0][0] if lines else "no weapon entry")
        return {"FINISHED"}


class O3D_OT_merge_weapon_def(bpy.types.Operator, ImportHelper):
    bl_idname = "opennova_3di.merge_weapon_def"
    bl_label = "Merge into weapon.def"
    bl_description = ("Set the weapon entries' timing and view keys in a copy of a weapon.def, written as a new file "
                      "(opennova-3di weapon merge); every other byte of the def stays as it was")
    filename_ext = ".def"
    filter_glob: StringProperty(default="*.def", options={"HIDDEN"})
    output: StringProperty(name="Write to", subtype="FILE_PATH", default="",
                           description="The merged weapon.def; empty: <the def's name>_merged.def beside it")

    @classmethod
    def poll(cls, context):
        model = clip_model(context)
        return model is not None and len(model.o3d.weapons) > 0

    def execute(self, context):
        model = clip_model(context)
        source = bpy.path.abspath(self.filepath)
        stem, ext = os.path.splitext(source)
        target = bpy.path.abspath(self.output) if self.output else f"{stem}_merged{ext}"
        try:
            said, notes = weapon.merge(context, model, source, target)
        except ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for note in notes:
            self.report({"WARNING"}, note)
        self.report({"INFO"}, said)
        return {"FINISHED"}


def draw_weapon(layout, model):
    """A first-person gun's weapon.def side: its entries, its view cameras,
    the last preview while it is current, and what writes the edits."""
    p = model.o3d
    box = layout.box()
    box.label(text="Weapon", icon="MOD_PHYSICS")
    for i, entry in enumerate(p.weapons):
        row = box.row(align=True)
        row.prop(entry, "name", text="")
        row.prop(entry, "mode", text="")
        row.prop(entry, "rpm", text="RPM")
        row.operator("opennova_3di.remove_weapon_entry", text="", icon="X").index = i
    box.operator("opennova_3di.add_weapon_entry", icon="ADD")
    if not p.weapons:
        box.label(text="No entry: the clips export without weapon.def edits")
        return
    box.prop(p, "hip_camera")
    box.prop(p, "aim_camera")
    box.operator("opennova_3di.preview_weapon", icon="PLAY")
    preview = weapon.last_preview(model)
    if preview is not None:
        for text, warn in weapon.preview_lines(model, preview):
            box.label(text=text, icon="ERROR" if warn else "NONE")
    elif weapon._previews.get(model.name_full) is not None:
        box.label(text="The timing changed: preview again", icon="INFO")
    box.operator("opennova_3di.merge_weapon_def", icon="FILE_TEXT")
    box.label(text="Export Animations writes <table>_weapon_edits.txt")


def draw_animations(layout, model):
    """The model's clip set: the table's rows, what writes them, and a
    first-person gun's weapon side. A model borrowing another's rig (arms on
    a gun) has none, and one without a rig gets it from its parts (the model
    box's Add Animation Rig)."""
    p = model.o3d
    arm = rig.rig_of(model)
    if arm is not None and rig.model_of(arm) is not model:
        return
    box = layout.box()
    box.label(text="Animations", icon="ARMATURE_DATA")
    if arm is None:
        box.label(text="No rig: Add Animation Rig makes one of the parts")
        return
    box.prop(p, "adm_path")
    if not p.adm_path:
        box.label(text=f"Writes {animation.adm_default(model)}")
    try:
        grounded = animation.root_of(arm) is not None
    except ExportError:
        grounded = True  # two Root bones, which export names
    if grounded:
        box.prop_search(p, "head_bone", arm.data, "bones", text="Head")
    else:
        box.label(text="No Root bone: the clips pose the model in place (first person)")
    try:
        table = catalog().actions
    except Exception:  # noqa: BLE001 (a panel draws whatever the catalog holds)
        table = []
    for i, row in enumerate(p.rows):
        line = box.box()
        head = line.row()
        head.prop(row, "key", text="")
        suffix = weapon.action_of(row.key, table) if table else None
        if suffix is not None:
            head.label(text=suffix)
        head.operator("opennova_3di.add_variant", text="", icon="ADD").row = i
        head.operator("opennova_3di.remove_row", text="", icon="X").index = i
        if animation.slot_of(row.key.strip()) == "reset" and not any(v.action for v in row.variants):
            line.label(text="No clip: export writes one of the rest pose")
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
    draw_weapon(box, model)


class O3D_PT_action(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "DOPESHEET_EDITOR"
    bl_region_type = "UI"
    bl_category = "Action"

    @classmethod
    def poll(cls, context):
        return edited_clip(context)[1] is not None

    def draw(self, context):
        arm, action = edited_clip(context)
        col = self.layout.column()
        col.label(text=f"Clip {action.name}")
        model = rig.model_of(arm)
        rows = [r for r in model.o3d.rows if any(v.action == action for v in r.variants)] if model else []
        col.label(text=", ".join(r.key.strip() for r in rows) if rows else "On no row: not exported",
                  icon="NONE" if rows else "INFO")
        col.prop(action, "use_frame_range", text="Manual Frame Range")
        if action.use_frame_range:
            row = col.row(align=True)
            row.prop(action, "frame_start", text="Start")
            row.prop(action, "frame_end", text="End")
        else:
            start, end = animation.clip_range(action)
            col.label(text=f"Frames {start} to {end} (its keys)")
        col.prop(action, "use_cyclic", text="Loop (Cyclic)")
        col.prop(action.o3d, "fps")
        render = context.scene.render
        if abs(action.o3d.fps - render.fps / render.fps_base) > 1e-6:
            col.label(text=f"The scene plays at {render.fps / render.fps_base:g} fps", icon="INFO")
        box = col.box()
        box.label(text="Event triggers (markers)")
        names = {name for _, name in catalog().triggers}
        for marker in sorted((m for m in action.pose_markers if m.name in names), key=lambda m: m.frame):
            box.label(text=f"{marker.frame}: {marker.name}")
        box.operator("opennova_3di.trigger_marker", icon="MARKER_HLT")
        table = catalog().actions
        suffixes = {s for s in (weapon.action_of(r.key, table) for r in rows) if s is not None} if table else set()
        box = col.box()
        box.label(text="Weapon timing (markers)")
        box.operator("opennova_3di.assign_weapon_action")
        wanted = set()
        for suffix in suffixes:
            wanted |= weapon.TIMED_BY.get(suffix, {weapon.ACTIVE, weapon.READY})
        if suffixes & weapon.SWITCHES:
            box.label(text="Draw and holster pace themselves on the switch timer")
        for name in weapon.MARKERS:
            if name not in wanted:
                continue
            row = box.row()
            marker = action.pose_markers.get(name)
            if marker:
                row.prop(marker, "frame", text=name[3:])
            else:
                row.label(text=name[3:])
            row.operator("opennova_3di.timing_marker", text="At Playhead").name = name


class O3D_PT_action_raw(bpy.types.Panel):
    bl_label = "Raw"
    bl_space_type = "DOPESHEET_EDITOR"
    bl_region_type = "UI"
    bl_category = "Action"
    bl_parent_id = "O3D_PT_action"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return edited_clip(context)[1] is not None

    def draw(self, context):
        self.layout.prop(edited_clip(context)[1].o3d, "raw_flag_8")


def menu_import(self, context):
    self.layout.operator(O3D_OT_import.bl_idname, text="NovaLogic 3DI (.3di)")


def menu_export(self, context):
    self.layout.operator(O3D_OT_export.bl_idname, text="NovaLogic 3DI (.3di)").browse = True


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
        col.prop(p, "write_textures")
        if not os.path.isfile(cli_path(context)):
            col.label(text="opennova-3di not found: set it in the add-on's preferences", icon="ERROR")
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
    if not p.output_path:
        layout.label(text=f"Writes //{rig.clean_name(model.name)}.3di")
    layout.prop(p, "poly_collision_lod")
    layout.prop(p, "export_bullet_faces")
    if p.attach_points != "PARTS":
        layout.prop(p, "attach_points")
    arm = rig.rig_of(model)
    own = arm is not None and rig.model_of(arm) is model
    root = next((c for c in model.children if rig.is_lod_root(c) and rig.lod_index(c) == 0), None)
    skinned = root is not None and any(rig.skin_rig(ob) is not None for ob in rig.descendants(root))
    if skinned:
        layout.prop(p, "mesh_part")
    if arm is not None and not own:
        layout.label(text=f"Deforms with {arm.name} ({rig.model_of(arm).name})", icon="ARMATURE_DATA")
    layout.prop(p, "mount_parent")
    if p.mount_parent is not None:
        layout.prop(p, "mount_point")
    row = layout.row(align=True)
    row.operator("opennova_3di.add_lod", icon="ADD")
    if arm is None:
        row.operator("opennova_3di.add_part", icon="EMPTY_AXIS")
        layout.operator("opennova_3di.add_rig", icon="ARMATURE_DATA")
    elif own:
        row.operator("opennova_3di.number_parts", icon="LINENUMBERS_ON")
        if skinned:
            layout.operator_menu_enum("opennova_3di.share_rig", "gun", icon="ARMATURE_DATA")


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

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob is not None and (rig.is_model_root(ob) or rig.lod_of(ob) is not None)

    def draw(self, context):
        ob = context.object
        p = ob.o3d
        layout = self.layout
        name = rig.clean_name(ob.name)
        if rig.is_model_root(ob):
            draw_model(layout, ob)
            draw_animations(layout, ob)
            return
        if rig.is_lod_root(ob):
            layout.prop(ob, '["_lod_index"]', text="LOD index")
            layout.prop(p, "lod_threshold")
            layout.prop(p, "lod_type")
            return
        if ob.type == "EMPTY" and rig.PART_RE.match(name):
            draw_part_animation(layout, ob, False)
            return
        part = rig.part_of(ob)
        layout.label(text=f"On part {part + 1:02d}" if part is not None else "On no part", icon="EMPTY_AXIS")
        if (ob.type == "EMPTY" and export.POINT_RE.match(name)) or ob.type == "LIGHT" or \
                export.OCCLUSION_RE.search(name):
            layout.prop(p, "order")


def draw_part_animation(layout, holder, bone):
    """A part's animation (PANM): its tracks, a bone's track frame, and the raw
    words in a closed subpanel."""
    p = holder.o3d
    layout.label(text="Part animation (PANM)")
    if bone:
        layout.prop(p, "frame")
    header, body = layout.panel(f"o3d_part_raw_{'bone' if bone else 'object'}", default_closed=True)
    header.label(text="Stored as")
    if body is not None:
        body.prop(p, "panm_flags")
        body.prop(p, "part_parent")
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
            rig.bone_part(context.bone) is not None

    def draw(self, context):
        index = rig.bone_part(context.bone)
        row = self.layout.row()
        row.label(text=f"Part {index + 1:02d}", icon="BONE_DATA")
        row.prop(context.bone.o3d, "hit_sphere")
        draw_part_animation(self.layout, context.bone, True)


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
        layout.label(text="LP on its part; the colour is the start colour")
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
        header, body = layout.panel("o3d_light_raw", default_closed=True)
        header.label(text="Stored as")
        if body is not None:
            body.prop(p, "other_flags")


class O3D_PT_material(bpy.types.Panel):
    bl_label = "OpenNova 3DI"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        mat = context.material
        p = mat.o3d
        layout = self.layout
        layout.prop(p, "shader")
        tag = p.shader.strip()
        table = catalog()[2]
        if not table:
            layout.label(text=catalog_error() or "No shader table", icon="ERROR")
        elif tag:
            known = next((flags for name, flags in table if name.lower() == tag.lower()), None)
            traits = [label for bit, label in ((materials.FLAG_BLENDING, "blends"), (materials.FLAG_GLASS, "glass"),
                                               (materials.FLAG_EMISSIVE, "glows"),
                                               (materials.FLAG_NORMAL, "normal map"),
                                               (materials.FLAG_TANGENT, "tangents"),
                                               (materials.FLAG_UVGEN, "moving UVs"),
                                               (materials.FLAG_SKINNED, "skinned")) if (known or 0) & bit]
            layout.label(text=(", ".join(traits) if traits else "opaque") if known is not None else
                         "Not in the engine's shader table", icon="NONE" if known is not None else "ERROR")
        else:
            layout.label(text="Automatic: picked by the textures and the settings below")
        layout.prop(p, "order")
        box = layout.box()
        box.label(text="From Blender's material settings")
        box.label(text="Two-sided (Backface Culling off)" if materials.two_sided(mat) else
                  "One-sided (Backface Culling on)")
        test = materials.alpha_test(mat)
        box.label(text=("No alpha test (a Greater Than node on Alpha sets one)" if test is None else
                        f"Alpha test {'at most' if test[1] else 'above'} {test[0]} (the Math node on Alpha)"))
        box.label(text="Alpha pass (Render Method Blended)" if materials.blended(mat) else
                  "Opaque pass (Render Method Dithered)")
        box.label(text="Glows (Emission)" if materials.emission(mat) else "No glow (Emission off)")
        box = layout.box()
        box.label(text="Bullet faces")
        row = box.row()
        row.prop(p, "surface")
        row = box.row()
        row.prop(p, "face_never_hit")
        row.prop(p, "face_front_only")
        box.prop(p, "face_other_flags")
        row = layout.row()
        row.prop(p, "other_flags")
        layout.prop(p, "reflect")
        box = layout.box()
        box.label(text="Texture rows the nodes cannot give")
        box.label(text="(flipbook frames, row flags, a normal map file, a file Blender cannot open)")
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
        if {t.slot for t in p.textures} & {1, 2}:
            box.label(text="The slots listed here are taken from the list, not the nodes", icon="INFO")
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


CLASSES = (O3DTrack, O3DAdmVariant, O3DAdmRow, O3DWeaponEntry, O3DActionProps, O3DObjectProps, O3DBoneProps,
           O3DTexture, O3DMaterialProps, O3DLightProps, O3DSceneProps, O3DPreferences,
           O3D_OT_add_track, O3D_OT_remove_track, O3D_OT_add_texture, O3D_OT_remove_texture, O3D_OT_export,
           O3D_OT_export_all, O3D_OT_add_model, O3D_OT_add_lod, O3D_OT_add_part, O3D_OT_add_rig,
           O3D_OT_share_rig, O3D_OT_number_parts,
           O3D_OT_import,
           O3D_OT_export_anim, O3D_OT_export_all_anim, O3D_OT_import_anim, O3D_OT_add_row,
           O3D_OT_remove_row, O3D_OT_add_variant, O3D_OT_remove_variant,
           O3D_OT_assign_weapon_action, O3D_OT_timing_marker, O3D_OT_trigger_marker, O3D_OT_edit_clip,
           O3D_OT_add_weapon_entry, O3D_OT_remove_weapon_entry, O3D_OT_preview_weapon, O3D_OT_merge_weapon_def,
           O3D_PT_action, O3D_PT_action_raw, O3D_PT_scene, O3D_PT_object, O3D_PT_bone, O3D_PT_light,
           O3D_PT_material)


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
    bpy.types.TOPBAR_MT_file_export.append(menu_export)
    bpy.types.TOPBAR_MT_file_import.append(menu_import_anim)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import_anim)
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    del bpy.types.Scene.o3d
    del bpy.types.Light.o3d
    del bpy.types.Material.o3d
    del bpy.types.Action.o3d
    del bpy.types.Bone.o3d
    del bpy.types.Object.o3d
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
