extends GutTest

# D-TERRAIN-8: the camera-below-water terrain modulation plumbing. The engine
# compiler stamps TerrainDrawList.below_water from the RENDER eye vs the live
# water height (the bare retail strict < with no zero guard), and the Terrain
# node pushes the flag plus the water module's live noise texture onto the
# shared surface material [orig: cameraY < Env_WaterHeightFixed @ 0x60FEE0 ->
# dword_319FB3C @ 0x60915F; live t3 slot swap @ 0x6043f2]. The shader-side
# math contract lives in terrain_shader_contract_test.gd.

# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file (TestFs.staged_tmap), removed at the end.
const TMAP_STAGE := "underwater"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


const TICK := 1.0 / 62.0


func _make_fixture(water_height: float) -> Dictionary:
	var vp := SubViewport.new()
	vp.size = Vector2i(320, 180)
	add_child_autofree(vp)

	var data := TerrainData.new()
	data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(data.load(), OK, "the Tmap fixture terrain must load")

	var terrain: Terrain = Terrain.new()
	vp.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()

	var water: Node = Water.new()
	vp.add_child(water)
	water.water_height = water_height
	water.advance_frame(TICK)
	terrain.water_path = terrain.get_path_to(water)

	var cam := Camera3D.new()
	vp.add_child(cam)
	cam.make_current()
	return {"terrain": terrain, "water": water, "camera": cam}


func test_render_eye_height_flips_the_below_water_uniform() -> void:
	var fixture := _make_fixture(7.0)
	var terrain: Terrain = fixture["terrain"]
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	assert_true(terrain.get_terrain_material() != null,
			"the Tmap build must produce the shared surface material")

	cam.global_position = Vector3(64.0, 27.0, 64.0)
	terrain.render_frame()
	var material: ShaderMaterial = terrain.get_terrain_material()
	assert_false(bool(material.get_shader_parameter("u_below_water")),
			"a camera above the surface reads dry")

	cam.global_position = Vector3(64.0, 3.0, 64.0)
	terrain.render_frame()
	assert_true(bool(material.get_shader_parameter("u_below_water")),
			"a camera below the surface sets the flag")
	assert_eq(material.get_shader_parameter("u_water_noise"),
			water.get_noise_color_texture(),
			"the terrain material binds the water module's LIVE noise texture")

	# The classification samples the RENDER eye (get_camera_transform), so a
	# v_offset alone must flip it — the same contract the frame clear pins
	# (game_world_test.gd waterline cases).
	cam.global_position = Vector3(64.0, 8.0, 64.0)
	terrain.render_frame()
	assert_false(bool(material.get_shader_parameter("u_below_water")),
			"one unit above the surface reads dry before the offset")
	cam.v_offset = -2.0
	terrain.render_frame()
	assert_true(bool(material.get_shader_parameter("u_below_water")),
			"a v_offset dropping only the render eye below must flip the flag")
	cam.v_offset = 0.0


func test_zero_height_keeps_the_bare_retail_compare() -> void:
	var fixture := _make_fixture(0.0)
	var terrain: Terrain = fixture["terrain"]
	var cam: Camera3D = fixture["camera"]
	var material: ShaderMaterial = terrain.get_terrain_material()

	# The retail compare is UNGUARDED (cameraY < Env_WaterHeightFixed, no zero
	# test on either side [orig: @0x60fea5]): a sub-zero eye reads below even
	# at height 0. Terrain heights are non-negative, so a dry map never fires
	# this in practice.
	cam.global_position = Vector3(64.0, -5.0, 64.0)
	terrain.render_frame()
	assert_true(bool(material.get_shader_parameter("u_below_water")),
			"the compare is unguarded: a sub-zero eye reads below at height 0")

	cam.global_position = Vector3(64.0, 5.0, 64.0)
	terrain.render_frame()
	assert_false(bool(material.get_shader_parameter("u_below_water")),
			"an above-zero eye reads dry at height 0")

	# Unwire the water node entirely: the height feed reads 0, an above-zero
	# eye stays dry, and the noise unbinds.
	terrain.water_path = NodePath()
	terrain.render_frame()
	assert_false(bool(material.get_shader_parameter("u_below_water")))
	var unbound = material.get_shader_parameter("u_water_noise")
	assert_true(unbound == null or not (unbound is Texture2D),
			"no water node leaves no live noise texture bound")


func test_runtime_publishes_one_shared_retail_tile_page_array() -> void:
	var fixture := _make_fixture(7.0)
	var terrain: Terrain = fixture["terrain"]
	var cam: Camera3D = fixture["camera"]
	cam.global_position = Vector3(64.0, 27.0, 64.0)

	terrain.render_frame()
	var page_array: TextureLayered = terrain.get_tile_cache_texture()
	assert_not_null(page_array, "A rendered terrain frame publishes the composed page array.")
	assert_eq(page_array.get_layered_type(), TextureLayered.LAYERED_TYPE_2D_ARRAY)
	assert_eq(page_array.get_width(), 256)
	assert_eq(page_array.get_height(), 256)
	assert_eq(page_array.get_layers(), 128)
	assert_false(page_array.has_mipmaps(), "Retail page RTs have one level.")

	var material: ShaderMaterial = terrain.get_terrain_material()
	assert_same(material.get_shader_parameter("u_tile_cache"), page_array)
	assert_true(bool(material.get_shader_parameter("u_has_tile_cache")))
	var requested: Dictionary = terrain.get_tile_cache_diagnostics()
	assert_eq(int(requested["pending_jobs"]), 0)
	var first: Dictionary = await TestFs.settle_tile_cache(self, terrain)
	assert_gt(int(first["ready_pages"]), 0)
	assert_gt(int(first["compose_jobs"]), 0)
	assert_eq(int(first["frame_compose_jobs"]), 0)
	assert_eq(int(first["pending_jobs"]), 0)
	assert_eq(int(first["frame_capacity_fallbacks"]), 0)

	terrain.render_frame()
	var second: Dictionary = terrain.get_tile_cache_diagnostics()
	assert_eq(int(second["compose_jobs"]), int(first["compose_jobs"]),
			"An unchanged frame reuses resident pages instead of recomposing them.")
	assert_eq(int(second["frame_compose_jobs"]), 0)
	assert_eq(int(second["frame_compose_us"]), 0)
	assert_eq(int(second["frame_capacity_fallbacks"]), 0)
	assert_gt(int(second["cache_hits"]), int(first["cache_hits"]))


func test_visible_pages_compose_before_their_frame_draws() -> void:
	var fixture := _make_fixture(7.0)
	var terrain: Terrain = fixture["terrain"]
	var cam: Camera3D = fixture["camera"]
	cam.global_position = Vector3(64.0, 27.0, 64.0)

	# Retail's first terrain frame claims no page record: every record's last
	# use is only one frame old. (retail PolyTrn_RenderTile @ 0x60DAE4..0x60DB45)
	terrain.render_frame()
	var first_frame: Dictionary = terrain.get_tile_cache_diagnostics()
	assert_eq(int(first_frame["ready_pages"]), 0)
	assert_eq(int(first_frame["frame_capacity_fallbacks"]),
			int(first_frame["frame_requests"]),
			"Every patch of the first frame draws without a page.")
	# The next frame composes every missing visible page inside its sweep and
	# uploads it before the patches draw; nothing stays queued.
	# (retail PolyTrn_RenderFrame @ 0x60F080..0x60F0E3)
	terrain.render_frame()
	var composed: Dictionary = terrain.get_tile_cache_diagnostics()
	assert_gt(int(composed["frame_compose_jobs"]), 0)
	assert_eq(int(composed["frame_uploads"]), int(composed["frame_compose_jobs"]))
	assert_eq(int(composed.get("pending_jobs", -1)), 0)
	assert_eq(int(composed["frame_capacity_fallbacks"]), 0)
	assert_between(int(composed.get("worker_count", 0)), 2, 8,
			"half the hardware threads compose the pages, 2..8")
	assert_eq(int(composed.get("dimension", 0)), 256,
			"The sole highest-quality retail page path.")


func test_live_tile_info_mutation_invalidates_resident_page_sources() -> void:
	var fixture := _make_fixture(7.0)
	var terrain: Terrain = fixture["terrain"]
	var cam: Camera3D = fixture["camera"]
	cam.global_position = Vector3(64.0, 27.0, 64.0)
	terrain.render_frame()
	terrain.render_frame()
	var before_reset := terrain.get_tile_cache_diagnostics()
	assert_gt(int(before_reset["ready_pages"]), 0)

	var tile_info := TerrainTileInfo.new()
	terrain.set_tile_info_override(tile_info)
	var assigned: Dictionary = terrain.get_tile_cache_diagnostics()
	assert_eq(int(assigned["pending_jobs"]), 0,
			"source reset must retire queued and completed prior-source jobs")
	assert_eq(int(assigned["ready_pages"]), 0)
	tile_info.clear_entries()
	var mutated: Dictionary = terrain.get_tile_cache_diagnostics()
	assert_eq(int(mutated["pending_jobs"]), 0)

	assert_gt(int(mutated["source_revision"]), int(assigned["source_revision"]),
		"An in-place .til edit must invalidate the device page source immediately.")


func test_reflected_pass_clips_terrain_below_the_plane_less_0_05() -> void:
	# Retail's reflected pass arms a terrain clip plane of wh - 0.1 (retail
	# render_main_scene @ 0x5c1561..0x5c1578) and the sector batch's texgen
	# u = y + 0.45 - plane under AlphaRef 0x80 (render_terrain_sector_batch
	# @ 0x6092c6..0x60935b) keeps y >= wh - 0.05. The terrain shader's LOD
	# debug colour marks every kept fragment green over a 2x2 card spanning
	# y -1..1 across 64 rows: rows 32/33 sit at y = -0.016 / -0.047, rows
	# 34/35 at -0.078 / -0.109.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var saved := {}
	for name in ["opennova_water_active", "opennova_water_height"]:
		var setting = ProjectSettings.get_setting("shader_globals/" + name, {})
		saved[name] = (setting as Dictionary).get("value") if setting is Dictionary else null
	RenderingServer.global_shader_parameter_set("opennova_water_active", true)
	RenderingServer.global_shader_parameter_set("opennova_water_height", 0.0)
	var reflected: Image = await _render_terrain_card(Water.REFLECTION_CULL_MASK)
	var beauty: Image = await _render_terrain_card(0xFFFFF)
	for name in saved:
		if saved[name] != null:
			RenderingServer.global_shader_parameter_set(name, saved[name])
	assert_gt(reflected.get_pixel(32, 32).g, 0.5, "0.016 below the plane is kept")
	assert_gt(reflected.get_pixel(32, 33).g, 0.5, "0.047 below the plane is kept")
	assert_lt(reflected.get_pixel(32, 34).g, 0.05, "0.078 below the plane is clipped")
	assert_lt(reflected.get_pixel(32, 35).g, 0.05, "0.109 below the plane is clipped")
	assert_gt(beauty.get_pixel(32, 40).g, 0.5, "a camera drawing the water layer never clips")


func _render_terrain_card(cull_mask: int) -> Image:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 2.0
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.cull_mask = cull_mask
	camera.current = true
	viewport.add_child(camera)
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/terrain.gdshader") as Shader
	material.set_shader_parameter("u_debug_mode", 1)
	var card := MeshInstance3D.new()
	var card_mesh := QuadMesh.new()
	card_mesh.size = Vector2(2.0, 2.0)
	card.mesh = card_mesh
	card.material_override = material
	viewport.add_child(card)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()
