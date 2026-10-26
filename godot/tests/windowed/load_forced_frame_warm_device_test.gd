extends GutTest

# The methods of tests/load_forced_frame_warm_test.gd that need a RenderingDevice, split
# out of it so the windowed run selects them (G-100).

const WEAPON_DEF := """weapon "WPN_M4AUTO"
category 1
rank 0
flags sighted
animadm PoseGun
gfx1 PoseGun
pos 64,32,-160,22.5,22.5,337.5
tpos 32,16,-128,11.25,45,22.5
ACTION "IDLE"
ANIM anim_wpn_idle
FUNCTION WPN_STD_IDLE
DELAYEND auto
END
ACTION "SWITCHTO"
ANIM anim_wpn_idle
FUNCTION WPN_STD_SWITCHTO
DELAYSTART 1
DELAYEND 1
END
end
"""
const AVATARS_DEF := """define head HEAD
{
graphic US01.3di
camo 0 0 0
voice 1
sex m
}
define body BODY
{
graphic US01.3di
camo 0 0 0
}
define arms ARMS
{
graphic PoseArms.3di
camo 0 0 0
}
nationality 0 NAT
{
alignment good
division 0 DIV
{
combo 1 HEAD BODY ARMS
}
}
"""

var _root := ""


func after_each() -> void:
	if not _root.is_empty():
		TestFs.remove_dir_recursive(_root)
		_root = ""


# The minimal pack, the person model under the body, head, gun and arms names, the gun's and the
# body's animation maps, and the particle documents (a load with no catalog to warm runs no
# forced frame on a host or single player).
func _stage_root(name: String) -> String:
	_root = WorldFixture.stage_minimal_root(name, false, {
		"weapon.def": WEAPON_DEF,
		"PoseGun.adm": "anim_reset \"idle.bad\"\r\nanim_wpn_idle \"idle.bad\"\r\n",
		"Avatars.def": AVATARS_DEF,
	})
	for model in ["US01.3di", "Indo01.3di", "PoseGun.3di", "PoseArms.3di"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/threedi/synth/person.3di"), _root.path_join(model)), OK)
	for adm in ["US01.adm", "E_STAND.adm"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/anim/soldier.adm"), _root.path_join(adm)), OK)
	for bad in ["idle.bad", "walk.bad"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/anim/" + bad), _root.path_join(bad)), OK)
	assert_gt(WorldFixture.stage_effects(_root).size(), 0, "a catalog to warm")
	return _root


# The forced frame makes every overlay combine's pipeline too (D-NET-NEW-L3-73): each view's
# overlay pass takes the warm request with the particle passes', and the frame's own callback makes
# every combine's pipeline for its view's framebuffer format into the one device every view's
# overlay pass shares, drawing nothing, so the first overlay draw past the reveal (the first tracer
# seen in a reflection among them) makes none. Retail's tail draws through render states made with
# the device and the mission textures (engine renderer/scene_overlay.h kSceneOverlayPipelineStates
# carries the witness). The frame draws the world view through the viewport's current camera, the
# one the view's passes compose around (ParticleRenderer::render_now): the shell's GameplayCamera,
# current through every load (main_game.tscn), which this rig stands in for, as the fixture world
# carries no camera of its own and a frame with none draws no world view. The made pipelines need the
# real rendering device: pending under the headless dummy renderer.
func test_the_forced_frame_makes_every_overlay_pipeline() -> void:
	var root_dir := _stage_root("overlay_pipeline_warm")
	var world := WorldFixture.make_world(self)
	var camera := Camera3D.new()
	camera.position = Vector3(16, 300, -16)
	add_child_autofree(camera)
	camera.make_current()
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
	loadout.player_class = 8
	world.set_local_player_spawn_loadout(loadout)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	var timeline: LoadTimeline = world.last_load_timeline()
	assert_not_null(timeline)
	if timeline != null:
		assert_true(Array(timeline.span_names()).has("forced_draw_1"), "the load drew its forced frame")
	var report: Dictionary = world.get_effect_world().get_debug_draw_list_report()
	var world_overlay: Dictionary = report.get("world_overlay_backend", {})
	for key in ["world_overlay_backend", "reflection_overlay_backend",
			"second_scene_overlay_backend", "inset_mirror_overlay_backend"]:
		var overlay: Dictionary = report.get(key, {})
		assert_gte(int(overlay.get("pipeline_warm_requests", 0)), 1,
				"%s took the forced frame's warm request" % key)
		assert_eq(int(overlay.get("device_id", -1)), int(world_overlay.get("device_id", -2)),
				"%s draws through the one shared device" % key)
	var world_camera: Dictionary = report.get("world_camera_backend", {})
	if not bool(world_camera.get("rd_available", false)):
		world.unload()
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(world_overlay.get("callback_seen", false)),
			"the forced frame ran the world view's overlay pass: %s" % world_overlay)
	assert_gte(int(world_overlay.get("pipeline_warm_requests_serviced", 0)), 1,
			String(world_overlay.get("failure", "the forced frame serviced no overlay warm")))
	assert_gte(int(world_overlay.get("device_pipelines", 0)),
			int(world_overlay.get("pipeline_states", -1)),
			"every combine's pipeline exists after the load")
	world.unload()
