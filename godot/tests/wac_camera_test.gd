extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func test_scripted_fov_reaches_the_camera_current_and_retry() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_camera", false, {
		"mnml.wac": (
			"if never then fov(40) endif\n" +
			"if eq(v1,1) then fov(120) set(v1,0) endif\n"),
	})
	staged_dirs.append(root_dir)
	var world := WorldFixture.make_world(self)
	world.set_playable(true)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	var sim := world.get_sim()
	assert_eq(sim.get_local_player_view().fov_h_deg, 40.0,
			"startup weather initialization snaps to the authored target")
	sim.set_mission_variable(1, 1)
	for _tick in range(61):
		sim.step()
	var before := sim.get_local_player_view().fov_h_deg
	sim.step()
	var expected := before + (120.0 - before) / 8.0
	assert_almost_eq(sim.get_local_player_view().fov_h_deg, expected, 0.00002,
			"the scheduled WAC command reaches the next weather step")
	sim.reset_session()
	assert_eq(sim.get_local_player_view().fov_h_deg, 40.0,
			"retry restores the mission's original current and target")
	world.unload()
