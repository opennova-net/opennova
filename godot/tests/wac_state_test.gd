extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func test_named_script_state_and_random_stream_restore_on_normal_retry() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_state", false, {
		"mnml.wac": (
			"if never then random(2147483647) store(v1) v2=RND " +
			"v3=SquadWho music(0) store(v4) endif\n" +
			"if eq(v10,1) then random(65536) store(v5) v6=RND " +
			"set(SquadWho,42) v7=SquadWho set(v10,0) endif\n"),
	})
	staged_dirs.append(root_dir)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	var sim := world.get_sim()
	assert_eq(sim.get_mission_variable(1), 0)
	assert_eq(sim.get_mission_variable(2), 856817665, "initial WAC stores the full random product")
	assert_eq(sim.get_mission_variable(3), 0, "new missions have no selected squad payload")
	assert_eq(sim.get_mission_variable(4), 1, "the dormant WAC music stream returns success")
	world.get_runtime().play()
	sim.set_mission_variable(10, 1)
	for _frame in range(62):
		world.tick(Vector3.ZERO, Transform3D(), 0.02)
	assert_eq(sim.get_mission_variable(6), 982, "scheduled WAC consumes the second draw")
	assert_eq(sim.get_mission_variable(7), 42, "the named squad value is writable")
	world.get_runtime().stop()
	assert_eq(sim.get_mission_variable(6), 0)
	assert_eq(sim.get_mission_variable(7), 0)
	assert_eq(sim.get_mission_variable(2), 856817665)
	world.get_runtime().play()
	sim.set_mission_variable(10, 1)
	for _frame in range(62):
		world.tick(Vector3.ZERO, Transform3D(), 0.02)
	assert_eq(sim.get_mission_variable(6), 982, "retry restores the post-startup RNG state")
	assert_eq(sim.get_mission_variable(7), 42)
	world.unload()
