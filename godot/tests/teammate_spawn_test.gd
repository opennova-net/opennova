extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


# JO ships the legacy helper DEFs but no medic ADM/3DI files. This fixture
# explicitly supplies those assets; it does not claim retail medic coverage.
func test_bms_helpers_receive_native_animation_and_presentation_on_retry() -> void:
	var clip_path := RetailData.fixture("bad/BINOC.bad")
	if clip_path.is_empty():
		pending(RetailData.fixture_pending_text("bad/BINOC.bad"))
		return
	var definitions := FileAccess.get_file_as_string("res://../fixtures/def/items.def")
	for row in [
		[104529, "person", "DeltaMED", "Medic02", "org1"],
		[104520, "person", "Medic01", "Medic01", "org1"],
		[101281, "vehicle", "Fblkhawm", "", "chel"],
		[106088, "marker", "", "", ""],
	]:
		definitions += "\nbegin \"Teammate fixture %s\"\n" % row[0]
		definitions += "id %s\ntype %s\nhp 100\nattrib: AIData\n" % [row[0], row[1]]
		if not String(row[2]).is_empty():
			definitions += "graphic %s\n" % row[2]
		if not String(row[3]).is_empty():
			definitions += "anim_def %s\n" % row[3]
		if not String(row[4]).is_empty():
			definitions += "ai_function %s\nmove_function %s\n" % [row[4], row[4]]
		definitions += "end\n"
	var animation := "anim_reset \"BINOC.bad\"\nanim_idle \"BINOC.bad\"\nanim_idle_2 \"BINOC.bad\"\n"
	var root_dir := WorldFixture.stage_minimal_root("teammate_spawn", false, {
		"items.def": definitions,
		"Medic01.adm": animation,
		"Medic02.adm": animation,
		"H_BHawkN.aip": "type HELO\nsubtype STD\n",
	})
	staged_dirs.append(root_dir)
	assert_eq(DirAccess.copy_absolute(clip_path, root_dir.path_join("BINOC.bad")), OK)
	for model in ["DeltaMED", "Medic01"]:
		assert_eq(DirAccess.copy_absolute(
				ProjectSettings.globalize_path("res://../fixtures/threedi/synth/person.3di"),
				root_dir.path_join(model + ".3di")), OK)
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/pump.3di"),
			root_dir.path_join("Fblkhawm.3di")), OK)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				var patient := mission.add_entity(MissionData.KIND_ORGANIC, 104520,
						Vector3(3, 1, 2), Vector3(0, 90, 0))
				assert_true(mission.set_entity_property_int(
						MissionData.KIND_ORGANIC, patient.index, "team", 1))
				var marker := mission.add_entity(MissionData.KIND_MARKER, 106088,
						Vector3(4, 1, 2), Vector3(0, 90, 0))
				assert_true(mission.set_entity_property_int(
						MissionData.KIND_MARKER, marker.index, "wp_number", 9))
				var spawn_event := mission.add_event(0, 0, 0)
				assert_not_null(mission.add_event_trigger(spawn_event.index,
						MissionEventTrigger.make(4, 1, 60, 1)))
				assert_not_null(mission.add_event_action(spawn_event.index,
						MissionEventAction.make(39, 2, patient.bms_id, 9)))
				var active_event := mission.add_event(0, 0, 0)
				assert_not_null(mission.add_event_trigger(active_event.index,
						MissionEventTrigger.make(6, 2, 0, 0)))
				assert_not_null(mission.add_event_action(active_event.index,
						MissionEventAction.make(5, 1, 61, 1)))), OK)
	var sim := world.get_sim()
	var runtime := world.get_runtime()
	var presenter := runtime.get_entity_presenter()
	for attempt in 2:
		assert_eq(sim.get_mission_variable(61), 0, "retry clears the teammate-active query")
		runtime.play()
		sim.set_mission_variable(60, 1)
		for _frame in 128:
			world.tick(Vector3.ZERO, Transform3D(), 0.02)
		assert_eq(sim.get_mission_variable(61), 1, "scheduled BMS action publishes its active operation")
		var models: Array[ObjectModel] = []
		var types: Array[int] = []
		var rows := sim.get_present_snapshot()
		for base in range(0, rows.size(), Simulation.PF_STRIDE):
			var item := int(rows[base + Simulation.PF_TYPE_ID])
			if int(rows[base + Simulation.PF_KIND]) != 255 or item not in [1281, 4529, 4520]:
				continue
			types.append(item)
			var handle := int(rows[base + Simulation.PF_WIRE_HANDLE])
			var model := presenter.resolve_wire_handle(handle) as ObjectModel
			assert_not_null(model, "the dynamic native helper has a real presented model")
			if model == null:
				continue
			models.append(model)
			assert_eq(model.entity_ref.runtime_type_id, item)
			assert_eq(model.entity_ref.item_id, item + 100000)
			if item in [4529, 4520]:
				var card := sim.entity_card(handle)
				assert_eq(card.get_adm_name().to_lower(), "medic02.adm" if item == 4529 else "medic01.adm")
				assert_true(model.has_skeleton(), "each helper receives its authored animated rig")
				if model.has_skeleton():
					assert_eq(model.get_skeleton().get_bone_count(), 19)
		types.sort()
		assert_eq(types, [1281, 4520, 4529], "one operation creates exactly the helicopter and both medics")
		runtime.stop()
		await get_tree().process_frame
		for model in models:
			assert_false(is_instance_valid(model), "retry releases dynamic presentation ownership")
		world.tick(Vector3.ZERO, Transform3D(), 0.02)
		rows = sim.get_present_snapshot()
		for base in range(0, rows.size(), Simulation.PF_STRIDE):
			assert_false(int(rows[base + Simulation.PF_KIND]) == 255
					and int(rows[base + Simulation.PF_TYPE_ID]) in [1281, 4529, 4520],
					"retry removes helpers from the authoritative snapshot")
	world.unload()
