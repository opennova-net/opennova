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
	world.unload()


func test_the_rotations_end_keeps_nothing() -> void:
	# REPLAY off on a one-entry rotation: the advance runs out.
	var world: GameWorld = await _hosted_world(0)
	var sim: Simulation = world.get_sim()
	assert_not_null(sim)
	assert_eq(world.begin_map_change(), "", "no next mission at the rotation's end")
	assert_same(world.get_sim(), sim, "nothing was taken out of the runtime")
	world.unload()
