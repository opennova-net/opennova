extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func test_facial_commands_load_grm_and_reset_with_normal_retry() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_faces", false, {
		"items.def": FileAccess.get_file_as_string("res://../fixtures/def/items.def"),
		"US01.grm": FileAccess.get_file_as_string("res://../fixtures/grm/person.grm"),
		"mnml.wac": (
			"if eq(v10,1) then face(HAPPY) store(v1) " +
			"ssnface(player,FACE_ANGRY) store(v2) set(v10,0) endif\n"),
	})
	staged_dirs.append(root_dir)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	var sim := world.get_sim()
	for attempt in 2:
		var card: EntityCard = sim.entity_card(sim.get_local_player_wire_handle())
		assert_not_null(card)
		if card == null:
			break
		assert_true(card.has_facial_animation(), "the player's model name resolves US01.GRM")
		assert_eq(card.get_facial_override(), -1, "retry restores the unforced expression")
		var baseline_frame := card.get_facial_display_frame()
		world.get_runtime().play()
		sim.set_mission_variable(10, 1)
		for _frame in 62:
			world.tick(Vector3.ZERO, Transform3D(), 0.02)
		card = sim.entity_card(sim.get_local_player_wire_handle())
		assert_eq(sim.get_mission_variable(1), 0, "face uses the retail local-player return")
		assert_eq(sim.get_mission_variable(2), 1, "ssnface reaches the allocated GRM slot")
		assert_eq(card.get_facial_override(), 4, "both names resolve to numeric expression rows")
		assert_eq(card.get_facial_target(), 4)
		assert_gt(card.get_facial_override_timer(), 0)
		assert_lt(card.get_facial_override_timer(), 80, "simulation ticks advance the override")
		assert_gt(card.get_facial_display_frame(), baseline_frame, "normal presentation advances its own counter")
		assert_eq(card.get_facial_texture_priority(), -1, "the first-person local body is excluded")
		world.get_runtime().stop()
	world.unload()
