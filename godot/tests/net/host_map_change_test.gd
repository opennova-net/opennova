extends GutTest

# The game's listen host through the round-end map change (D-NET-331, engine
# inmatch/map_change.h): GameWorld keeps the live session across its own reload.
# The world's begin_map_change runs the engine's teardown arms and router and
# names the next map; its unload no longer closes the kept session (the socket
# stays bound, the simulation lives on); load_next_mission boots the next map
# inside that same session. At the rotation's end nothing is kept and the
# shell leaves for the menu. The engine legs themselves (the rotation, the
# kept joiner's reload over real UDP) are ctest's npruntime_mission_rotation
# and npruntime_map_change.


func _host_options(replay: int) -> HostSessionOptions:
	var config := HostSessionConfig.new()
	config.mission = "mnml.bms"
	config.missions.assign(["mnml.bms"])
	config.mission_launch_options.assign([0])
	config.bind_port = 0
	config.game_type = HostSessionConfig.GAME_TYPE_COOP
	var options := config.to_session_options()
	options.replay_enabled = replay
	return options


func _hosted_world(replay: int) -> GameWorld:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path(RuntimeFixture.directory())), OK)
	world.set_resource_root(root)
	assert_eq(world.load_mission_as_host(_host_options(replay)), OK)
	return world


func test_the_host_screen_hands_every_row_and_its_switch_cell_to_the_session() -> void:
	var config := HostSessionConfig.new()
	config.missions.assign(["a.bms", "b.bms"])
	config.mission_launch_options.assign([1, 0])
	var options := config.to_session_options()
	assert_eq(options.mission_file, "a.bms", "the first row is the starting map")
	assert_eq(options.rotation_missions, PackedStringArray(["a.bms", "b.bms"]))
	assert_eq(options.rotation_launch_options, PackedInt32Array([1, 0]))


func test_the_map_change_keeps_the_listen_hosts_session_across_the_reload() -> void:
	var world: GameWorld = await _hosted_world(1)
	var sim: Simulation = world.get_sim()
	assert_not_null(sim)
	if sim == null:
		return
	var port := sim.get_host_listen_port()
	assert_gt(port, 0, "the host bound a real port")
	assert_true(sim.is_mp_session())

	# REPLAY on a one-entry rotation: the advance wraps to the same map.
	var next_map: String = world.begin_map_change()
	assert_eq(next_map.to_lower(), "mnml.bms", "the rotation names the next map")
	world.unload()
	# The kept session outlived the world's unload: still in session, still bound.
	assert_true(sim.is_mp_session(), "the kept session was not closed")
	assert_eq(sim.get_host_listen_port(), port, "the socket stayed bound")

	assert_eq(world.load_next_mission(next_map), OK)
	assert_same(world.get_sim(), sim, "the next map runs on the same simulation")
	assert_eq(sim.get_host_listen_port(), port, "the same socket serves the next map")
	assert_true(sim.is_mp_session())
	var config := sim.get_host_session_config()
	assert_eq(String(config.mission_file).to_lower(), "mnml.bms")
	# The next map's freeze is the first map's: its listed models' materials resolve and none
	# builds, then the full arm's device warm runs (D-NET-NEW-L3-31).
	var timeline: LoadTimeline = world.last_load_timeline()
	assert_not_null(timeline)
	if timeline != null:
		var names := Array(timeline.span_names())
		var freeze := names.find("model_materials")
		assert_true(freeze >= 0 and names.find("effects_warm") > freeze,
				"the listen host's map change freezes, then warms: %s" % [names])
		assert_false(names.has("model_warm"), "no listed model builds: %s" % [names])
	world.unload()


# Retail opens its archives once per process, never at a map change (engine inmatch/map_change.h
# carries the witness, D-NET-NEW-L3-24): a host mounted on its own archives (no injected root)
# loads the next map on the same mount.
func test_the_map_change_loads_on_the_sessions_mount() -> void:
	var dir := OS.get_cache_dir().path_join("opennova_host_mount_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	WorldFixture.stage_shell_archives(self, dir)
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var options := _host_options(1)
	options.dir = dir
	assert_eq(world.load_mission_as_host(options), OK)
	var mount: ResourceRoot = world.get_resource_root()
	assert_not_null(mount)
	var next_map: String = world.begin_map_change()
	assert_eq(next_map.to_lower(), "mnml.bms")
	world.unload()
	assert_eq(world.load_next_mission(next_map), OK)
	assert_same(world.get_resource_root(), mount, "the next map loads on the session's mount")
	world.unload()
	if mount != null:
		mount.clear()
	await get_tree().process_frame
	TestFs.remove_dir_recursive(dir)


# A Serve Only host's map change runs the next mission start's reduced arm, as its first load
# does: no effect system at the head (D-NET-NEW-L3-23), its listed models' materials resolved and
# none of the warm's legs: no effect spawn, no model build, no pipeline warm, no forced frame
# (engine inmatch/loaded_model_list.h LoadedModelList::model_files carries the witness,
# D-NET-NEW-L3-25).
func test_a_serve_only_hosts_map_change_runs_the_reduced_arm() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path(RuntimeFixture.directory())), OK)
	world.set_resource_root(root)
	var options := _host_options(1)
	options.serve_and_play = false
	assert_eq(world.load_mission_as_host(options), OK)
	var sim: Simulation = world.get_sim()
	assert_not_null(sim)
	if sim == null:
		return
	assert_eq(sim.session_role(), Simulation.ROLE_DEDICATED_HOST, "a Serve Only host")
	var next_map: String = world.begin_map_change()
	assert_eq(next_map.to_lower(), "mnml.bms", "the rotation names the next map")
	world.unload()
	assert_eq(world.load_next_mission(next_map), OK)
	assert_same(world.get_sim(), sim, "the next map runs on the same simulation")
	assert_eq(sim.session_role(), Simulation.ROLE_DEDICATED_HOST, "still a Serve Only host")
	assert_null(world.get_effect_world(), "the next map loads no effect system either")
	var timeline: LoadTimeline = world.last_load_timeline()
	assert_not_null(timeline)
	if timeline != null:
		var names := Array(timeline.span_names())
		assert_false(names.has("effect_system"), "no effect system at the head: %s" % [names])
		assert_true(names.has("model_materials"),
				"the next map resolves its listed models' materials: %s" % [names])
		for leg in ["effects_warm", "effect_spawn", "model_warm", "present_pipelines",
				"forced_draw_1", "forced_draw_2", "force_sync", "warm_reset"]:
			assert_false(names.has(leg),
					"no %s at a Serve Only host's map change: %s" % [leg, names])
	world.unload()


func test_the_rotations_end_keeps_nothing() -> void:
	# REPLAY off on a one-entry rotation: the advance runs out.
	var world: GameWorld = await _hosted_world(0)
	var sim: Simulation = world.get_sim()
	assert_not_null(sim)
	assert_eq(world.begin_map_change(), "", "no next mission at the rotation's end")
	assert_same(world.get_sim(), sim, "nothing was taken out of the runtime")
	world.unload()
