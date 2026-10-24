extends GutTest

# The mission load's one forced frame (GameWorld.warm_effect_world_catalog, D-NET-NEW-L3-47) draws
# what the first revealed frame would otherwise build and compile: the local body and its view model
# and the wire walk's cold identities, made, drawn and freed inside the load (D-NET-NEW-LF-15), and
# the shell's HUD, which the load's HUD legs built and loaded, mounted for that frame alone with its
# draw submitted at once (D-NET-NEW-LF-16), and the water's reflection target, armed for that frame's
# draw (D-NET-NEW-LF-23; water_reflection_warm_test.gd pins the arming). The frame runs ahead of the local player's birth on every
# role (a host's and a single player's Player_InitPlayer runs in the mission start past the device
# stages, a joiner's at the push past its load), so the body and the view model warm from the
# identity and the kit the birth will bind: the local profile's character and the staged spawn kit.
# No retail files: the minimal pack with the committed synthetic person model as the body, head, gun
# and arms (player_viewmodel_rotation_test.gd's fixture) and the synthetic particle documents.

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
const MODELS_PREFIX := "person_models_"

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


# The count the warm's zero-length span names (-1: no such span).
func _models_warmed(names: Array) -> int:
	for name in names:
		if String(name).begins_with(MODELS_PREFIX):
			return int(String(name).trim_prefix(MODELS_PREFIX))
	return -1


func _join_target(port: int) -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = port
	target.mission = "mnml"
	target.player_name = "Joiner"
	return target


# A playable single-player load: L is born at the mission start past the device stages, so past the
# frame, as retail's Player_InitPlayer follows its mission textures; the warm builds the body the
# profile's character names and the staged kit's weapon's view model (gun and arms), draws them in
# the one frame and frees them in it, and the shell's HUD draws in that frame off its mount and
# leaves the tree again.
func test_the_forced_frame_draws_the_local_models_and_the_hud_ahead_of_the_birth() -> void:
	var root_dir := _stage_root("forced_frame_warm")
	var world := WorldFixture.make_world(self)
	var layer := CanvasLayer.new()
	add_child_autofree(layer)
	var hud: GameHudPresenter = add_child_autofree(GameHudPresenter.new())
	hud.setup(world, null, layer)
	# The shell's mission start: the world hidden and its draw held by the loading presentation.
	world.visible = false
	world.set_presentation_held(true)
	var water: Water = world.get_water_node()
	var frame_state := {}
	var warm_frames: Array = []
	# Connected after the presenter's handler, so it reads what that handler left.
	world.hud_pipeline_warm.connect(func(drawing: bool) -> void:
		var overlay := hud.get_game_hud()
		warm_frames.append([drawing, overlay != null and overlay.is_inside_tree()]))
	world.hud_pipeline_warm.connect(func(drawing: bool) -> void:
		if drawing:
			frame_state["visible"] = world.visible
			frame_state["disable_3d"] = world.get_viewport().disable_3d
			frame_state["mirror"] = water.is_mirror_enabled() if water != null else true
	, CONNECT_ONE_SHOT)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
	loadout.player_class = 8
	world.set_local_player_spawn_loadout(loadout)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)

	assert_true(world.get_sim().has_local_player(), "the playable load's mission start spawned L")
	assert_eq(world.local_player_weapon_name(), "WPN_M4AUTO",
			"the load's spawn kit presented the weapon")
	var timeline: LoadTimeline = world.last_load_timeline()
	assert_not_null(timeline)
	if timeline != null:
		var names := Array(timeline.span_names())
		var person := names.find("person_pipelines")
		var body := names.find("local_body")
		var view_model := names.find("view_model")
		var wire := names.find("wire_identities")
		var hud_leg := names.find("hud_pipelines")
		var menu := names.find("menu_pipelines")
		var mirror := names.find("water_mirror")
		var frame := names.find("forced_draw_1")
		# The slot captures' casters pack their geometry ahead of the person warm, whose models
		# leave with it (D-NET-NEW-LF-27).
		var casters := names.find("slot_casters")
		assert_true(casters >= 0 and casters < person,
				"the casters' geometry packs ahead of the person warm: %s" % [names])
		assert_true(person >= 0 and person < body and body < view_model and view_model < wire
				and wire < hud_leg and hud_leg < menu and menu < mirror and mirror < frame,
				"the body, the view model and the wire identities, the HUD, the menus' glyph program (D-NET-NEW-LF-28), the water's reflection target, then the one frame: %s"
				% [names])
		assert_gt(names.find("mission_start"), frame,
				"L's birth, the mission start's, runs past the frame: %s" % [names])
		assert_false(names.has("forced_draw_2"), "still one forced frame: %s" % [names])
		assert_gte(_models_warmed(names), 2,
				"the body and the staged kit's view model drew in the frame: %s" % [names])
	assert_null(world.get_node_or_null("PersonPipelineWarm"),
			"the warm's models are freed inside the load")
	assert_eq(warm_frames, [[true, true], [false, false]],
			"the HUD is on the tree for the forced frame alone")
	var overlay := hud.get_game_hud()
	assert_not_null(overlay, "the load's HUD legs built the overlay")
	if overlay != null:
		assert_true(overlay.is_configured(), "and loaded it ahead of the frame")
		assert_false(overlay.is_inside_tree(), "the first live frame mounts it, as before")
	# The frame draws the world with the hold lifted for it alone, and leaves the world shown: every
	# later frame under the screen keeps the hold, and the reveal drops it with no node shown
	# (D-NET-NEW-LF-29).
	assert_eq(frame_state, {"visible": true, "disable_3d": false, "mirror": true},
			"the forced frame draws the shown world and its mirror")
	assert_true(world.visible, "the load leaves the world shown")
	assert_true(world.is_presentation_held())
	assert_true(world.get_viewport().disable_3d, "and its draw held under the loading screen")
	if water != null:
		assert_false(water.is_mirror_enabled(), "the water draws no mirror under the hold")
	# A frame under the hold is no render frame: the environment cube neither counts it nor captures
	# in it, as retail's wait draws no frame; the frame the hold drops in is its next.
	var cube := world.get_node_or_null("EnvironmentCubeCapture") as EnvironmentCubeCapture
	assert_not_null(cube, "the world's environment cube")
	var camera := world.get_viewport().get_camera_3d()
	var eye := camera.global_transform if camera != null else Transform3D()
	var cube_frames: int = cube.get_render_frame_index() if cube != null else 0
	world.tick(eye.origin, eye)
	if cube != null:
		assert_eq(cube.get_render_frame_index(), cube_frames, "a held frame is no render frame")
	world.set_presentation_held(false)
	assert_false(world.get_viewport().disable_3d, "the reveal drops the hold")
	if water != null:
		assert_true(water.is_mirror_enabled())
	world.tick(eye.origin, eye)
	if cube != null:
		assert_eq(cube.get_render_frame_index(), cube_frames + 1, "the first drawn frame counts")
		# That frame takes this mission's six faces and samples them: the cube is ready at the request
		# and its copy runs in that frame's draw, ahead of the beauty view's opaque pass.
		if RenderingServer.get_rendering_device() != null:
			assert_true(cube.is_cube_ready(), "the reveal frame's cube is this mission's, ready at its request")
	hud.teardown()
	world.unload()


# A joiner's L is born at the mission start's push, past its load: the load's warm and its one forced
# frame run with no local player yet, and this rig's host has not streamed the joiner's own record
# by the load's end, so the body warms from the join's character (the local profile's).
func test_a_joiners_forced_frame_warms_its_body_ahead_of_its_birth() -> void:
	var root_dir := _stage_root("joiner_person_warm")
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Person Warm Host"
	host_options.mission_name = mission.get_mission_name()
	host_options.mission_file = "mnml.bms"
	host_options.expansion = ""
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	var observed := {"local_load": false, "local_player": true}
	world.world_loaded.connect(func() -> void:
		observed["local_load"] = true
		var sim: Simulation = world.get_sim()
		observed["local_player"] = sim != null and sim.has_local_player()
	, CONNECT_ONE_SHOT)
	assert_eq(world.load_mission_as_joiner(_join_target(host.get_host_listen_port())), OK)
	for _frame in range(600):
		host.step()
		await get_tree().process_frame
		if bool(observed["local_load"]):
			break
	assert_true(bool(observed["local_load"]), "the S2C 0x1A's mission start loaded the world")
	assert_false(bool(observed["local_player"]), "L is not born until the push past the load")
	var timeline: LoadTimeline = world.last_load_timeline()
	assert_not_null(timeline)
	if timeline != null:
		var names := Array(timeline.span_names())
		var person := names.find("person_pipelines")
		var body := names.find("local_body")
		var wire := names.find("wire_identities")
		var hud_leg := names.find("hud_pipelines")
		var frame := names.find("forced_draw_1")
		assert_true(person >= 0 and person < body and body < wire and wire < hud_leg
				and hud_leg < frame,
				"the joiner's warm: the body, the wire identities, then the HUD, then the one frame: %s"
				% [names])
		assert_false(names.has("forced_draw_2"), "still one forced frame: %s" % [names])
		assert_gte(_models_warmed(names), 1, "the join's character's body drew: %s" % [names])
	world.unload()


# The forced frame makes every overlay combine's pipeline too (D-NET-NEW-L3-73): each view's
# overlay pass takes the warm request with the particle passes', and the frame's own callback makes
# every combine's pipeline for its view's framebuffer format into the one device every view's
# overlay pass shares, drawing nothing, so the first overlay draw past the reveal (the first tracer
# seen in a reflection among them) makes none. Retail's tail draws through render states made with
# the device and the mission textures (engine renderer/scene_overlay.h kSceneOverlayPipelineStates
# carries the witness). The made pipelines need the real rendering device: pending under the
# headless dummy renderer.
func test_the_forced_frame_makes_every_overlay_pipeline() -> void:
	var root_dir := _stage_root("overlay_pipeline_warm")
	var world := WorldFixture.make_world(self)
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
	if not bool(world_overlay.get("callback_seen", false)):
		world.unload()
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gte(int(world_overlay.get("pipeline_warm_requests_serviced", 0)), 1,
			String(world_overlay.get("failure", "the forced frame serviced no overlay warm")))
	assert_gte(int(world_overlay.get("device_pipelines", 0)),
			int(world_overlay.get("pipeline_states", -1)),
			"every combine's pipeline exists after the load")
	world.unload()
