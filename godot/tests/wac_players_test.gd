extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func test_player_script_state_and_experience_reach_the_local_client_on_retry() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_players", false, {
		"items.def": FileAccess.get_file_as_string("res://../fixtures/def/items.def"),
		"mnml.wac": (
			"if eq(v10,1) then pisvar(16) store(v1) psetvar(16) store(v2) " +
			"piskills(0) store(v3) pisgold() store(v4) AddExp(player,17) store(v5) " +
			"pisvar(16) store(v6) ppunt() store(v7) pkillpunt() store(v8) set(v10,0) endif\n"),
	})
	staged_dirs.append(root_dir)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	var sim := world.get_sim()
	for attempt in 2:
		assert_eq(sim.get_mission_variable(2), 0, "normal retry restores the script baseline")
		world.get_runtime().play()
		sim.set_mission_variable(10, 1)
		for _frame in range(62):
			world.tick(Vector3.ZERO, Transform3D(), 0.02)
		assert_eq(sim.get_mission_variable(1), 0, "the player's byte bank resets on retry")
		assert_eq(sim.get_mission_variable(2), 1, "the registered local slot accepts psetvar")
		assert_eq(sim.get_mission_variable(3), 1, "the local slot owns its kill statistics")
		assert_eq(sim.get_mission_variable(4), 0, "the retail Gold bit remains clear")
		assert_eq(sim.get_mission_variable(5), 1, "AddExp resolves the Player operand and ItemDef")
		assert_eq(sim.get_mission_variable(6), 1, "pisvar reads the same player byte bank")
		assert_eq(sim.get_mission_variable(7), 1, "ppunt finds the local player slot")
		assert_eq(sim.get_mission_variable(8), 1, "pkillpunt finds the same local slot")
		var feedback := sim.take_score_feedback()
		assert_not_null(feedback, "attempt %d: the native score change reaches the local client's 0x81 feed" % attempt)
		if feedback != null:
			assert_eq(feedback.get_score(), 17, "retry repeats the award from its baseline")
			assert_eq(feedback.get_delta(), 17)
		world.get_runtime().stop()
	world.unload()
