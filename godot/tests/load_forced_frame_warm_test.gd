extends GutTest

# The mission load's one forced frame (GameWorld.warm_effect_world_catalog, D-NET-NEW-L3-47) draws
# what the first revealed frame would otherwise build and compile: the wire walk's cold identities
# (and the local body and its view model once the session names them), made, drawn and freed inside
# the load (D-NET-NEW-LF-15), and the shell's HUD, which the load's HUD legs built and loaded, mounted
# for that frame alone with its draw submitted at once (D-NET-NEW-LF-16). The frame runs ahead of the
# local player's birth on every role: a host's and a single player's Player_InitPlayer runs in the
# mission start past the device stages, a joiner's at the push past its load.
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


# A playable single-player load: the warm's legs (the wire identities, then the HUD) run ahead of the
# one frame, which draws them, and the warm's models are freed in the load; the shell's HUD draws in
# that frame off its mount and leaves the tree again. L is born at the mission start past the device
# stages, so past the frame, as retail's Player_InitPlayer follows its mission textures.
func test_the_forced_frame_draws_the_hud_ahead_of_the_local_players_birth() -> void:
	var root_dir := _stage_root("forced_frame_warm")
	var world := WorldFixture.make_world(self)
	var layer := CanvasLayer.new()
	add_child_autofree(layer)
	var hud: GameHudPresenter = add_child_autofree(GameHudPresenter.new())
	hud.setup(world, null, layer)
	var warm_frames: Array = []
	# Connected after the presenter's handler, so it reads what that handler left.
	world.hud_pipeline_warm.connect(func(drawing: bool) -> void:
		var overlay := hud.get_game_hud()
		warm_frames.append([drawing, overlay != null and overlay.is_inside_tree()]))
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
		var wire := names.find("wire_identities")
		var hud_leg := names.find("hud_pipelines")
		var frame := names.find("forced_draw_1")
		assert_true(person >= 0 and person < wire and wire < hud_leg and hud_leg < frame,
				"the wire identities, then the HUD, then the one frame: %s" % [names])
		assert_gt(names.find("mission_start"), frame,
				"L's birth, the mission start's, runs past the frame: %s" % [names])
		assert_false(names.has("forced_draw_2"), "still one forced frame: %s" % [names])
		assert_gte(_models_warmed(names), 0, "the warm names the models it drew: %s" % [names])
	assert_null(world.get_node_or_null("PersonPipelineWarm"),
			"the warm's models are freed inside the load")
	assert_eq(warm_frames, [[true, true], [false, false]],
			"the HUD is on the tree for the forced frame alone")
	var overlay := hud.get_game_hud()
	assert_not_null(overlay, "the load's HUD legs built the overlay")
	if overlay != null:
		assert_true(overlay.is_configured(), "and loaded it ahead of the frame")
		assert_false(overlay.is_inside_tree(), "the first live frame mounts it, as before")
	hud.teardown()
	world.unload()


# A joiner's L is born at the mission start's push, past its load: the load's warm and its one forced
# frame (the wire identities, then the HUD) run with no local player yet. The body arm needs the
# session to name the joiner's own record by then (Simulation.has_learned_self_record); this rig's
# host has not streamed it by the load's end, so that arm is not pinned here.
func test_a_joiners_forced_frame_runs_ahead_of_its_local_players_birth() -> void:
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
		var wire := names.find("wire_identities")
		var hud_leg := names.find("hud_pipelines")
		var frame := names.find("forced_draw_1")
		assert_true(person >= 0 and person < wire and wire < hud_leg and hud_leg < frame,
				"the joiner's warm: the wire identities, then the HUD, then the one frame: %s" % [names])
		assert_false(names.has("forced_draw_2"), "still one forced frame: %s" % [names])
	world.unload()
