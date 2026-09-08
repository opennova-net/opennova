extends GutTest


func _model(parent: Node, filename: String) -> ObjectModel:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/" + filename)), OK)
	var model := ObjectModel.new()
	parent.add_child(model)
	model.set_object_data(data)
	return model


func _assert_light_count(parts: Array[ObjectModel], count: float, message: String) -> void:
	for part in parts:
		var surfaces := part.find_children("*", "GeometryInstance3D", true, false)
		assert_gt(surfaces.size(), 0, "the model has rendered surfaces")
		for surface: GeometryInstance3D in surfaces:
			assert_eq(float(surface.get_instance_shader_parameter("u_point_light_count")),
					count, message)


func test_muzzle_light_returns_on_the_viewmodel_after_a_pause_between_shots() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var mission := MissionRoot.new()
	mission.name = "MissionRoot"
	world.add_child(mission)
	var objects := Node3D.new()
	objects.name = "MissionObjects"
	mission.add_child(objects)
	var bystander := _model(objects, "gun.3di")
	var parts: Array[ObjectModel] = [_model(world, "person.3di"), _model(world, "gun.3di")]
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.position = Vector3(0, 1, 3)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_passes(null, null, null, null, null, director, null, null)
	var last_handle := 0
	for shot in range(3):
		presenter.present_fires([FirePresentationEvent.make(Vector3.ZERO, 0, 77, true, 1)])
		director.render_frame(camera, parts, 77)
		_assert_light_count(parts, 1.0,
				"shot %d lights the owner's arms and weapon after the previous flash expired" % (shot + 1))
		_assert_light_count([bystander], 0.0, "the muzzle self-light retains the shooter owner gate")
		var rows := director.get_report().rows
		assert_eq(rows.size(), 1, "one muzzle light is selected per shot")
		if rows.size() == 1:
			assert_ne(rows[0].handle, last_handle, "an expired lease is replaced")
			last_handle = rows[0].handle
		for tick in range(LightScene.muzzle_glow_fade_ticks()):
			director.advance_fixed_tick()
		director.render_frame(camera, parts, 77)
		_assert_light_count(parts, 0.0, "the flash expires after the original five ticks")
		for tick in range(7):
			director.advance_fixed_tick()
	presenter.present_fires([FirePresentationEvent.make(Vector3.ZERO, 0, 77, true, 0)])
	director.render_frame(camera, parts, 77)
	_assert_light_count(parts, 0.0, "ammo without MF_Light does not spawn a muzzle light")
	presenter.teardown()


func test_rearming_muzzle_does_not_modify_a_light_reusing_the_expired_slot() -> void:
	var director := EffectLightDirector.new()
	var scene := director.scene()
	director.on_muzzle_fire(77, Vector3.ZERO)
	scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null)
	var original := scene.get_report().rows[0].handle
	director.advance_fixed_tick()
	director.on_muzzle_fire(77, Vector3.ONE)
	scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null)
	assert_eq(scene.get_report().rows[0].handle, original,
			"shots within the active window reuse the same light")
	for tick in range(LightScene.muzzle_glow_fade_ticks()):
		director.advance_fixed_tick()
	var replacement_pos := Vector3(10, 0, 0)
	var replacement := scene.spawn_glow(GlowSpawn.make(replacement_pos, 4.0, Color.GREEN))
	assert_eq(replacement & 0xffff, original & 0xffff, "another light reuses the old pool slot")
	director.on_muzzle_fire(77, Vector3.ZERO)
	scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null)
	var report := scene.get_report()
	assert_eq(report.live, 2, "the next shot allocates its own light beside the replacement")
	assert_eq(report.rows.size(), 2)
	for row: EffectLightRow in report.rows:
		if row.handle == replacement:
			assert_true(row.position.is_equal_approx(replacement_pos), "the other light does not move")
			assert_almost_eq(row.range, 5.0, 0.001, "the other light retains its radius")
	for tick in range(LightScene.muzzle_glow_fade_ticks()):
		director.advance_fixed_tick()
	assert_true(scene.is_alive(replacement), "the other light does not inherit the muzzle timeout")
	assert_eq(director.get_report().live, 1)
