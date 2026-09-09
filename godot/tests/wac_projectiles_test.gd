extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func test_mounted_ammo_names_and_scheduled_script_fire_reach_presentation() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_projectiles", false, {
		"mnml.wac": (
			"if never then set(v1,AMMO_AM_556MM) endif\n" +
			"if eq(v3,1) then ammorain(v1) store(v2) set(v3,0) endif\n"),
	})
	staged_dirs.append(root_dir)
	var world := WorldFixture.make_world(self)
	world.set_playable(true)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	var sim := world.get_sim()
	assert_eq(sim.get_mission_variable(1), 1,
			"layered boot resolves the mounted ammo.def before script execution")
	sim.drain_fire_presentation_events()
	sim.set_mission_variable(3, 1)
	for _tick in range(61):
		sim.step()
	assert_eq(sim.drain_fire_presentation_events().size(), 0)
	var origin := sim.get_local_player_position()
	sim.step() # WAC runs before the motor on the 62nd gameplay tick.
	assert_eq(sim.get_mission_variable(2), 0)
	var events := sim.drain_fire_presentation_events()
	assert_eq(events.size(), 1, "script fire reaches the normal fire presentation drain")
	if events.size() == 1:
		var event := events[0] as FirePresentationEvent
		assert_eq(event.ammo_index, 1)
		assert_false(event.adm_arm)
		assert_false(event.is_local_player)
		assert_eq(event.shooter_handle, 65535)
		var expected := origin + Vector3(1310700.0, 1965056.0, -1307400.0) / 65536.0
		assert_almost_eq(event.origin.x, expected.x, 0.001)
		assert_almost_eq(event.origin.y, expected.y, 0.001)
		assert_almost_eq(event.origin.z, expected.z, 0.001)
		assert_lt(event.forward.y, -0.999)
	world.unload()
