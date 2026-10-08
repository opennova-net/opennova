extends GutTest


func test_project_starts_at_the_fresh_profiles_texture_filter() -> void:
	# A fresh profile's texfilter_level is 1 (trilinear), whose device mode
	# programs no anisotropy (engine renderer/texture_filter.h); the world
	# raises the viewport's at a mission start under levels 2 and 3.
	assert_eq(int(ProjectSettings.get_setting(
			"rendering/textures/default_filters/anisotropic_filtering_level", -1)), 0)


func test_render_snapshot_reports_the_texture_filter_state() -> void:
	var world := preload("res://game/world/game_world.tscn").instantiate() as GameWorld
	add_child_autofree(world)
	var value: Dictionary = GameRenderDiagnostics.sample(world).to_json_value()
	var anisotropy := value["renderer"]["default_anisotropy"] as Dictionary
	assert_eq(int(anisotropy["level"]), 0)
	assert_false(bool(anisotropy["active"]))
	var texfilter := value["renderer"]["texfilter"] as Dictionary
	assert_eq(int(texfilter["level"]), GameWorld.texfilter_level_fresh_profile())
	assert_eq(int(texfilter["session_level"]), GameWorld.texfilter_level_fresh_profile())
	assert_eq(int(texfilter["device_mode"]), 2)


func test_render_snapshot_carries_the_live_terrain_surface_inputs() -> void:
	var world := preload("res://game/world/game_world.tscn").instantiate() as GameWorld
	add_child_autofree(world)
	var terrain: Terrain = world.get_terrain_node()
	var data := TerrainData.new()
	var image := Image.create(8, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color8(90, 110, 70, 255))
	data.set_colormap(ImageTexture.create_from_image(image))
	data.set_detail_density(91)
	data.set_detail_density2(11)
	assert_true(terrain.get_surface_inputs().rebuild(data))

	var value: Dictionary = GameRenderDiagnostics.sample(world).to_json_value()
	var terrain_state := value["terrain"] as Dictionary
	assert_true(bool(terrain_state["available"]))
	assert_eq(terrain_state["tile_cache"], terrain.get_tile_cache_diagnostics(),
		"Capture diagnostics consume the tile-cache module's public record unchanged.")
	assert_true((terrain_state["tile_cache"] as Dictionary).has("ready_pages"))
	var surface_inputs := terrain_state["surface_inputs"] as Dictionary
	assert_eq(surface_inputs, terrain.get_surface_inputs().get_diagnostics(),
		"Capture diagnostics consume the terrain module's public record unchanged.")
	assert_eq(int(surface_inputs["detail_density"]), 91)
	assert_eq(int(surface_inputs["detail2_density"]), 11)
	assert_eq((surface_inputs["textures"] as Dictionary)["colormap"]["size"],
		Vector2i(8, 4))
	# The raw colormap: the page composer builds its own quadrant levels.
	assert_eq(int((surface_inputs["textures"] as Dictionary)[
			"colormap"]["mipmap_count"]), 0)
	assert_true(surface_inputs.has("tile_info_available"))


func test_game_world_exposes_an_exact_json_safe_render_snapshot() -> void:
	var world := preload("res://game/world/game_world.tscn").instantiate() as GameWorld
	add_child_autofree(world)
	var camera := Camera3D.new()
	camera.fov = 50.534
	camera.near = 0.2
	camera.far = 2048.0
	camera.global_transform = Transform3D(
			Basis.from_euler(Vector3(deg_to_rad(-6.5), deg_to_rad(45.0), 0.0)),
			Vector3(12.0, 34.0, 56.0))
	world.add_child(camera)
	camera.make_current()
	var active_light := OmniLight3D.new()
	active_light.name = "ActiveProbeLight"
	active_light.set_shadow(true)
	world.add_child(active_light)
	var hidden_light := OmniLight3D.new()
	hidden_light.name = "HiddenProbeLight"
	hidden_light.visible = false
	world.add_child(hidden_light)
	await get_tree().process_frame

	var snapshot: GameRenderDiagnostics = GameRenderDiagnostics.sample(world, camera)
	var value: Dictionary = snapshot.to_json_value()
	assert_eq(value["schema"], "OpenNovaRenderDiagnosticsV1")
	assert_eq(value["frame"]["process"], Engine.get_process_frames())
	assert_eq(value["camera"]["global_transform"]["origin"], Vector3(12.0, 34.0, 56.0))
	assert_almost_eq(float(value["camera"]["fov_deg"]), 50.534, 0.0001)
	assert_almost_eq(float(value["camera"]["near"]), 0.2, 0.0001)
	assert_eq(value["camera"]["far"], 2048.0)
	assert_eq((value["camera"]["projection_columns"] as Array).size(), 4)
	assert_eq((value["camera"]["projection_columns"][0] as Array).size(), 4)
	assert_true(value["environment"].has("loaded"))
	assert_true(value["water"].has("render_active"))
	assert_true(value["shadows"].has("dynamic"))
	assert_true(value["shadows"].has("static_terrain"))
	var static_terrain_shadow := value["shadows"]["static_terrain"] as Dictionary
	assert_eq(static_terrain_shadow["implementation"], "terrain_page_alpha")
	assert_false(world.has_node("StaticSunShadow"),
		"Diagnostics must not preserve a stale static DirectionalLight witness.")
	assert_true(static_terrain_shadow.has("enabled"))
	assert_true(static_terrain_shadow.has("active"))
	assert_true(static_terrain_shadow.has("raster_jobs"))
	assert_true(static_terrain_shadow.has("raster_failures"))
	assert_true(static_terrain_shadow.has("alpha_changed_bytes"))
	assert_true(static_terrain_shadow.has("rgb_changed_bytes"))
	assert_true(static_terrain_shadow.has("frame_pages_with_draws"))
	assert_true(static_terrain_shadow.has("frame_triangles"))
	assert_true((value["terrain"]["tile_cache"] as Dictionary).has(
			"shadow_provider_frame_triangles"))
	assert_true(value["passes"].has("root"))
	assert_true(value["passes"].has("water_reflection"))
	assert_true(value["renderer"].has("method"))
	assert_gte(int(value["lights"]["total_nodes"]),
			int(value["lights"]["active"]))
	assert_gte(int(value["lights"]["inactive"]), 1)
	assert_gte(int(value["lights"]["omni"]), 1)
	assert_gte(int(value["lights"]["shadowed"]), 1)
	assert_true(value["lights"].has("effectworld"),
			"the EffectWorld pool census rides beside the node walk")
	for raw_row in value["lights"]["rows"]:
		var row: Dictionary = raw_row
		assert_true(bool(row["visible"]))
		assert_true(bool(row["visible_in_tree"]))
		assert_false(String(row["path"]).contains("HiddenProbeLight"))

	var json_safe: Variant = McpJson.sanitize(value)
	assert_true(json_safe is Dictionary)
	assert_eq(json_safe["camera"]["global_transform"]["origin"], [12.0, 34.0, 56.0])
