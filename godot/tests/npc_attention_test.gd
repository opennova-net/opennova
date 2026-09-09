extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func test_idle_spotting_drives_bms_and_the_presented_head_on_retry() -> void:
	var clip_path := RetailData.fixture("bad/BINOC.bad")
	if clip_path.is_empty():
		pending(RetailData.fixture_pending_text("bad/BINOC.bad"))
		return
	var root_dir := WorldFixture.stage_minimal_root("npc_attention", false, {
		"items.def": FileAccess.get_file_as_string("res://../fixtures/def/items.def"),
		"US02.adm": (
			"anim_reset \"BINOC.bad\"\n" +
			"anim_idle \"BINOC.bad\"\n" +
			"anim_idle_2 \"BINOC.bad\"\n" +
			"anim_idle_look \"BINOC.bad\"\n" +
			"anim_idle_2_look \"BINOC.bad\"\n"),
	})
	staged_dirs.append(root_dir)
	assert_eq(DirAccess.copy_absolute(clip_path, root_dir.path_join("BINOC.bad")), OK)
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/person.3di"),
			root_dir.path_join("US02.3di")), OK)
	var ids: Array[int] = []
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				for i in 2:
					var actor := mission.add_entity(MissionData.KIND_ORGANIC, 105311,
							Vector3(3 * i, i, 2), Vector3(0, 90, 0))
					assert_not_null(actor)
					ids.append(actor.bms_id)
					for field in {"team": 1, "group": 20 + i, "max_attack_distance": 20, "ai_flags": 1}:
						var value: int = {"team": 1, "group": 20 + i,
							"max_attack_distance": 20, "ai_flags": 1}[field]
						assert_true(mission.set_entity_property_int(
								MissionData.KIND_ORGANIC, actor.index, field, value))
				var event := mission.add_event(0, 0, 0)
				assert_not_null(event)
				assert_not_null(mission.add_event_trigger(event.index,
						MissionEventTrigger.make(1, 1, 20, 21)))
				assert_not_null(mission.add_event_action(event.index,
						MissionEventAction.make(5, 1, 40, 1)))), OK)
	var sim := world.get_sim()
	var actor := world.get_runtime().get_entity_index().resolve_single(ids[0]) as ObjectModel
	assert_not_null(actor)
	if actor == null:
		world.unload()
		return
	assert_true(actor.has_skeleton(), "the real NPC has its authored 19-bone rig")
	if not actor.has_skeleton():
		world.unload()
		return
	var skeleton: Skeleton3D = actor.get_skeleton()
	assert_eq(skeleton.get_bone_count(), 19)
	for attempt in 2:
		assert_eq(sim.get_mission_variable(40), 0, "retry resets the BMS sees event")
		world.get_runtime().play()
		var saw_head_offset := false
		var saw_pose_change := false
		skeleton.force_update_all_bone_transforms()
		var before := skeleton.get_bone_global_pose(14).basis
		for _frame in 320:
			world.tick(Vector3.ZERO, Transform3D(), 0.02)
			var rows := sim.get_present_snapshot()
			for base in range(0, rows.size(), Simulation.PF_STRIDE):
				if int(rows[base + Simulation.PF_BMS_ID]) != ids[0]:
					continue
				var head := base + Simulation.PF_AIM_ANGLES + 8 * Simulation.PF_AIM_CLASS_STRIDE
				var offset := absf(rows[head + 1] - rows[base + Simulation.PF_AIM_BODY_YAW_DEG])
				if int(rows[base + Simulation.PF_AIM_OVERLAY_VALID]) == 1 and offset > 1.0:
					saw_head_offset = true
			skeleton.force_update_all_bone_transforms()
			if not skeleton.get_bone_global_pose(14).basis.is_equal_approx(before):
				saw_pose_change = true
			if saw_head_offset and sim.get_mission_variable(40) == 1:
				break
		assert_eq(sim.get_mission_variable(40), 1, "idle spotting satisfies the authored BMS sees trigger")
		assert_true(saw_head_offset, "native independent gaze reaches the batched head overlay")
		assert_true(saw_pose_change, "the placed NPC's head bone consumes that pose")
		world.get_runtime().stop()
	world.unload()
