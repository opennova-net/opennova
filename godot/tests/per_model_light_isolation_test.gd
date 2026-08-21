extends GutTest

## Facelift local-light device seam: the portable LightScene owns lifecycle and
## evaluated values; EffectLightDirector realizes those rows as pooled native
## OmniLight3D/SpotLight3D nodes. The old nearest-four per-model shader-uniform
## contract intentionally no longer exists.


func _world() -> GameWorld:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	return world


func _director(world: GameWorld) -> EffectLightDirector:
	var director := EffectLightDirector.new()
	director.setup(world, Callable())
	return director


func _camera(world: GameWorld) -> Camera3D:
	var camera := Camera3D.new()
	world.add_child(camera)
	return camera


func _fixture_object_data(model: String) -> ObjectData:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, model), OK)
	return data


func _placed_model(parent: Node) -> ObjectModel:
	var model := ObjectModel.new()
	parent.add_child(model)
	model.set_object_data(_fixture_object_data("House.3di"))
	return model


func _effect_lights(world: GameWorld, active_only: bool = true) -> Array[Light3D]:
	var lights: Array[Light3D] = []
	for child in world.get_children():
		var light := child as Light3D
		if light == null or not light.has_meta("effect_light_handle"):
			continue
		if active_only and not light.visible:
			continue
		lights.append(light)
	return lights


func test_active_snapshot_realizes_native_omni_and_spot_nodes() -> void:
	var world := _world()
	var director := _director(world)
	assert_gt(director.spawn_light_record({
		"position": Vector3(2.0, 1.0, -3.0),
		"atten_start": 1.0,
		"atten_end": 8.0,
		"colorgen_style": 24,
		"color_start": Color(1.0, 0.5, 0.25),
	}, Transform3D.IDENTITY), 0)
	assert_gt(director.spawn_light_record({
		"position": Vector3(-4.0, 2.0, 1.0),
		"direction": Vector3(1.0, -0.25, 0.5).normalized(),
		"light_type": 1,
		"falloff_deg": 32.0,
		"atten_end": 12.0,
		"colorgen_style": 24,
		"color_start": Color(0.2, 0.4, 1.0),
	}, Transform3D.IDENTITY), 0)

	director.render_frame(_camera(world))
	var lights := _effect_lights(world)
	assert_eq(lights.size(), 2, "every live row owns one active native node")
	var omni: OmniLight3D
	var spot: SpotLight3D
	for light in lights:
		if light is OmniLight3D:
			omni = light as OmniLight3D
		elif light is SpotLight3D:
			spot = light as SpotLight3D
	assert_not_null(omni)
	assert_not_null(spot)
	if omni == null or spot == null:
		return
	assert_true(omni.global_position.is_equal_approx(Vector3(2.0, 1.0, -3.0)))
	assert_almost_eq(omni.omni_range, 8.0, 0.001)
	assert_true(omni.light_color.is_equal_approx(Color(
			1.0, 128.0 / 255.0, 64.0 / 255.0)),
			"Godot Light3D stores the authored normalized sRGB color")
	assert_almost_eq(spot.spot_range, 12.0, 0.001)
	assert_almost_eq(spot.spot_angle, 32.0, 0.001)
	var expected_direction := Vector3(1.0, -0.25, 0.5).normalized()
	assert_true((-spot.global_basis.z).normalized().is_equal_approx(
			expected_direction), "target-light direction drives SpotLight3D -Z")
	for light in lights:
		assert_false(light.shadow_enabled, "local-light shadows stay disabled")
		assert_true((light.light_cull_mask & Water.VISUAL_LAYER_WORLD) != 0)
		assert_true((light.light_cull_mask & Water.VISUAL_LAYER_WORLD_NO_MIRROR) != 0)
		assert_true((light.light_cull_mask & Water.VISUAL_LAYER_VIEWMODEL) != 0,
				"authored object lights reach first-person arms")
	var report := director.get_report()
	assert_eq(report.selected, 2)
	assert_eq(report.selection_mode, "native_lights")
	assert_eq(report.owner_isolation, "native_cull_mask")


func test_native_omni_pool_reuses_nodes_and_tracks_round_lifecycle() -> void:
	var world := _world()
	var director := _director(world)
	var camera := _camera(world)
	director.sync_round_glows([{
		"id": 11,
		"pos": Vector3(1.0, 2.0, 3.0),
		"radius": 6.0,
		"color": Color(0.8, 0.6, 0.2),
	}])
	director.render_frame(camera)
	var active := _effect_lights(world)
	assert_eq(active.size(), 1)
	if active.is_empty():
		return
	var first := active[0] as OmniLight3D
	var pooled_id := first.get_instance_id()
	assert_false((first.light_cull_mask & Water.VISUAL_LAYER_VIEWMODEL) == 0)

	director.sync_round_glows([])
	director.render_frame(camera)
	assert_eq(_effect_lights(world).size(), 0,
			"despawn hides the retired native node on the next render frame")
	assert_eq(_effect_lights(world, false).size(), 1,
			"retired nodes remain in the small reusable pool")

	director.sync_round_glows([{
		"id": 12,
		"pos": Vector3(8.0, 4.0, -2.0),
		"radius": 3.0,
		"color": Color.WHITE,
	}])
	director.render_frame(camera)
	active = _effect_lights(world)
	assert_eq(active.size(), 1)
	if active.is_empty():
		return
	var reused := active[0] as OmniLight3D
	assert_eq(reused.get_instance_id(), pooled_id,
			"a later omni lease reuses the retired Godot node")
	assert_almost_eq(reused.omni_range, 3.0, 0.001)

	director.sync_round_glows([{
		"id": 12,
		"pos": Vector3(10.0, 5.0, -4.0),
		"radius": 3.0,
		"color": Color.WHITE,
	}])
	director.render_frame(camera)
	assert_true(reused.global_position.is_equal_approx(Vector3(10.0, 5.0, -4.0)),
			"moving pool leases update the reused native node")


func test_participation_flags_drive_native_cull_masks() -> void:
	var world := _world()
	var director := _director(world)
	assert_gt(director.spawn_light_record({
		"position": Vector3.ZERO,
		"atten_end": 4.0,
		"disable_lightterrain": true,
		"disable_lightobjects": true,
	}, Transform3D.IDENTITY), 0)
	director.render_frame(_camera(world))
	var all_nodes := _effect_lights(world, false)
	assert_eq(all_nodes.size(), 1)
	if all_nodes.is_empty():
		return
	assert_eq(all_nodes[0].light_cull_mask, 0)
	assert_false(all_nodes[0].visible,
			"a light disabled for every authored receiver stays pooled but inactive")


func test_null_camera_releases_nodes_without_destroying_pool_leases() -> void:
	var world := _world()
	var director := _director(world)
	assert_gt(director.spawn_light_record({
		"position": Vector3.ZERO,
		"atten_end": 4.0,
	}, Transform3D.IDENTITY), 0)
	director.render_frame(_camera(world))
	assert_eq(_effect_lights(world).size(), 1)
	director.render_frame(null)
	assert_eq(_effect_lights(world).size(), 0)
	assert_eq(director.get_report().live, 1,
			"temporary camera loss preserves portable light leases")
	assert_eq(director.get_report().selected, 0)


func test_owned_corona_still_gates_on_owner_section_visibility() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var owner_model := _placed_model(container)
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light({
		"position": Vector3(0.0, 1.0, 0.0),
		"atten_end": 4.0,
		"owner_entity": owner_model.get_instance_id(),
		"owner_section": 2,
	}), 0)
	var models: Array[Node3D] = [owner_model]
	var owners := PackedInt64Array([owner_model.get_instance_id()])
	var rows: Array = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, models,
			owners, {})
	assert_eq(rows.size(), 3,
			"an owner without an occlusion verdict still presents its corona")
	owner_model.set_section_visibility_mask(~(1 << 2))
	rows = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, models,
			owners, {})
	assert_eq(rows.size(), 0, "a hidden owner section suppresses its corona")
	owner_model.set_section_visibility_mask(1 << 2)
	rows = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, models,
			owners, {})
	assert_eq(rows.size(), 3, "a visible owner section restores its corona")
