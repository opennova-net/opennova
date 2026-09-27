"""Run with Blender --background --factory-startup --python this.py -- CLI OUT.

Constructs and exports everything from scratch, without an importer or assets.
"""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import bpy

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/blender"))
import opennova_3di as addon
from opennova_3di import animation, export, materials, weapon, importer
from opennova_3di.o3dtext import ExportError

cli, destination = sys.argv[sys.argv.index("--") + 1:]
out = Path(destination).resolve()
# A fresh directory: the clip-set checks below see only this run's files.
shutil.rmtree(out, ignore_errors=True)
out.mkdir(parents=True)
addon.register()
# Texture byte conversion preserves the original rounding, clipping and BGRA
# layout while using bulk image access for large authored textures.
pixel_image = bpy.data.images.new("authored_pixels", width=3, height=1, float_buffer=True)
pixel_image.pixels[:] = [-0.1, 0.5, 1.1, 1, 0.5/255, 1.5/255, 2.5/255, 0.25, 0.125, 0.875, 0.375, 0]
pixels = list(pixel_image.pixels[:])
materials.write_tga(pixel_image, str(out / "pixels.tga"))
expected_pixels = bytes(max(0, min(255, round(pixels[i+c]*255))) for i in range(0,12,4) for c in [2,1,0,3])
assert (out / "pixels.tga").read_bytes()[18:] == expected_pixels
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.context.scene.o3d.cli_path = cli

def empty(name, parent=None):
    ob = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(ob)
    ob.parent = parent
    return ob

model = empty("authored_weapon")
lod = empty("authored_weapon_LOD0", model)
lod["_lod_index"] = 0
data = bpy.data.armatures.new("authored_skeleton")
rig = bpy.data.objects.new("authored_rig", data)
bpy.context.collection.objects.link(rig)
rig.parent = lod
rig.select_set(True)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode="EDIT")
root = data.edit_bones.new("BN01 Pelvis")
root.head, root.tail = (0, 0, 0), (0, 0, 0.1)
slide = data.edit_bones.new("BN02 Slide")
slide.head, slide.tail, slide.parent = (0, 0, 0.1), (0, 0.1, 0.1), root
bpy.ops.object.mode_set(mode="OBJECT")
mesh = bpy.data.meshes.new("authored_triangle")
mesh.from_pydata([(0, 0, 0.1), (0.05, 0, 0.1), (0, -0.1, 0.1)], [], [(0, 1, 2)])
uv0 = [(0.125, 0.25), (0.5, 0.75), (0.875, 0.0625)]
uv1 = [(0.25, 0.125), (0.75, 0.5), (0.0625, 0.875)]
for label, coordinates in [("Diffuse", uv0), ("Lightmap", uv1)]:
    layer = mesh.uv_layers.new(name=label)
    for loop, uv in zip(layer.data, coordinates):
        loop.uv = uv
mesh.uv_layers[0].active_render = True
ob = bpy.data.objects.new("03 Mesh0", mesh)
bpy.context.collection.objects.link(ob)
ob.parent = rig
group = ob.vertex_groups.new(name="BN02 Slide")
group.add([0, 1, 2], 1, "REPLACE")
ob.modifiers.new("Skin", "ARMATURE").object = rig
mat = bpy.data.materials.new("Material_0_VS_SKBASIC")
ob.data.materials.append(mat)

def make_clip(name, role, start, end, amount=0):
    rig.animation_data_create()
    action = bpy.data.actions.new(name)
    rig.animation_data.action = action
    bone = rig.pose.bones["BN02 Slide"]
    bone.rotation_mode = "QUATERNION"
    for frame, value in [(start, 0), (end, amount)]:
        bone.location = (0, value, 0)
        bone.keyframe_insert("location", frame=frame)
    action.o3d.translation = True
    assert bpy.ops.opennova_3di.assign_weapon_action(role=role) == {"FINISHED"}
    assert not action.o3d.loop  # retail's weapon clips, idle holds too, are one-shots
    return action

reset = make_clip("authored_reset", "RESET", -5, -3)
idle = make_clip("authored_idle", "idle", 10, 25)
fire = make_clip("authored_fire", "fire", 10, 16, 0.015)
reload = make_clip("authored_reload", "reload", 20, 50, -0.02)
assert len(animation.clip_actions(rig)) == 4
assert len(model.o3d.rows) == 4
assert fire.pose_markers[weapon.SHOT].frame == 10
assert fire.pose_markers[weapon.READY].frame == 16
model.o3d.weapon_mode = "auto"
model.o3d.weapon_cadence = "RPM"
model.o3d.weapon_rpm = 750
model.o3d.output_path = str(out / "authored.3di")
model.o3d.adm_path = str(out / "authored.adm")
model.o3d.clip_prefix = "test_"
model.o3d.rows[2].weapon_end_sound = "GS_AUTHORED"

preview, snippet = weapon.compile_timing(bpy.context, model)
assert preview["cycle_ticks"] == 5 and preview["rpm"] == 750
assert "DELAYEND 3" in snippet and "GS_AUTHORED" in snippet
# Preview updates remain independent of exported or imported data.
assert bpy.ops.opennova_3di.preview_weapon() == {"FINISHED"}
assert json.loads(model.o3d.weapon_preview)["signature"] == weapon.signature(weapon.request_text(model))
bpy.ops.wm.save_as_mainfile(filepath=str(out / "authored.blend"))
print(export.export_model(bpy.context, model))
subprocess.run([cli, "scene", str(out / "authored.3di"), "-o", str(out / "authored.o3d")], check=True, capture_output=True)
rebuilt = importer.read_o3d(str(out / "authored.o3d"))
actual_uvs = {v["uv"] + v["uv1"] for part in rebuilt["lods"][0]["parts"] for strip in part["strips"] for v in strip["verts"]}
expected_uvs = {(a[0], 1-a[1], b[0], 1-b[1]) for a,b in zip(uv0, uv1)}
assert actual_uvs == expected_uvs, (actual_uvs, expected_uvs)
# A newly authored FP mesh can use the root and omit its ordinal and bullet
# faces. Geometry/weights still travel through the real native exporter.
ob.name = "01 Mesh"
model.o3d.export_bullet_faces = False
model.o3d.output_path = str(out / "arms.3di")
print(export.export_model(bpy.context, model))
subprocess.run([cli, "scene", str(out / "arms.3di"), "-o", str(out / "arms.o3d")], check=True)
arms = importer.read_o3d(str(out / "arms.o3d"))
assert len(arms["lods"][0]["parts"]) == 2
assert sum(len(s["tris"]) for p in arms["lods"][0]["parts"] for s in p["strips"]) == 1
assert not any(line.startswith("cf ") for line in (out / "arms.o3d").read_text().splitlines())
assert any(line.startswith("cf ") for line in (out / "authored.o3d").read_text().splitlines())
print(animation.export_animations(bpy.context, model))
assert (out / "authored_weapon_actions.txt").read_text() == snippet
assert all((out / ("test_" + a.name + ".bad")).is_file() for a in [reset, idle, fire, reload])
adm_text = (out / "authored.adm").read_text()
assert '"test_authored_fire"' in adm_text and 'anim_wpn_fire' in adm_text
assert not (out / "authored_fire.bad").exists()

# Reopening the authored .blend, with no source path, is sufficient to export.
bpy.ops.wm.open_mainfile(filepath=str(out / "authored.blend"))
model = bpy.data.objects["authored_weapon"]
assert model.o3d.clip_prefix == "test_"
rig = animation.rig_of(model)
fire = bpy.data.actions["authored_fire"]
model.o3d.weapon_preview = "deliberately invalid cached preview; never an export input"
model.o3d.weapon_rpm = 468.75
_, changed = weapon.compile_timing(bpy.context, model)
assert "DELAYEND 6" in changed and changed != snippet
print(animation.export_animations(bpy.context, model))
assert (out / "authored_weapon_actions.txt").read_text() == changed

# Marker timing uses the Action's own origin and exported fps, not scene fps
# or its NLA strip's scene placement. A moved NLA strip must not change it.
model.o3d.weapon_cadence = "MARKER"
fire.o3d.fps = 60
fire.pose_markers[weapon.READY].frame = 16 # 6 / 60 seconds => 6 ticks
bpy.context.scene.render.fps = 24
for a, strip in animation.clip_strips(rig):
    if a == fire:
        strip.frame_start_ui = 500
preview, _ = weapon.compile_timing(bpy.context, model)
assert preview["cycle_ticks"] == 6

def hashes():
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir()
            if p.suffix in (".bad", ".adm", ".txt")}

# Invalid markers cannot partially replace a previously exported clip set.
before = hashes()
fire.pose_markers[weapon.SHOT].frame = 17
try:
    animation.export_animations(bpy.context, model)
except ExportError as e:
    assert "inside the exported clip" in str(e)
else:
    raise AssertionError("out-of-range Shot exported")
assert hashes() == before
fire.pose_markers[weapon.SHOT].frame = 10
extra = fire.pose_markers.new("ON:Typo")
try:
    weapon.compile_timing(bpy.context, model)
except ExportError as e:
    assert "unknown timing marker" in str(e)
else:
    raise AssertionError("unknown marker exported")
fire.pose_markers.remove(extra)

# Variants cannot silently fight over the single pair of ACTION delays.
rig.hide_set(False)
rig.select_set(True)
bpy.context.view_layer.objects.active = rig
variant = make_clip("authored_fire_variant", "fire", 0, 10)
try:
    weapon.compile_timing(bpy.context, model)
except ExportError as e:
    assert "variants disagree" in str(e)
else:
    raise AssertionError("conflicting variants exported")

# The same UI used by an animator selects the clip's actual action slot.
assert bpy.ops.opennova_3di.edit_clip(row=2, variant=0) == {"FINISHED"}
assert rig.animation_data.action == fire
print("BLENDER_WEAPON_AUTHORING_OK: authored model, clips, markers, native timing, export, reopen, invalid-input preservation")
