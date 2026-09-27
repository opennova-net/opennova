extends GutTest

# The weapon Inset pass's own terrain frame (runtime/terrain/terrain_frame.h
# carries the witness): its own traversal draws its own patch pool on
# INSET_VIEW, the one bit only the Inset camera admits, while the main pool
# rides MAIN_VIEW so the Inset camera never draws the main view's patches;
# releasing the Inset frees its pool and returns the main pool to WORLD.

const TMAP_STAGE := "inset_frame"
const WORLD := 1 << 0
const TERRAIN_FLAT_FALLBACK := 1 << 18
const MAIN_VIEW := 1 << 20
const INSET_VIEW := 1 << 21


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


# The Tmap terrain in a world of its own, a main camera over it, and a second
# viewport on the same world holding the Inset camera; both look straight down
# at the same ground, drawing the LOD debug colours so no light is needed.
func _make_fixture() -> Dictionary:
	var main_view := SubViewport.new()
	main_view.size = Vector2i(32, 32)
	main_view.own_world_3d = true
	main_view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(main_view)
	var data := TerrainData.new()
	data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(data.load(), OK, "the Tmap fixture terrain must load")
	var terrain: Terrain = Terrain.new()
	main_view.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	terrain.debug_mode = Terrain.DEBUG_MODE_LOD_COLORS
	var camera := Camera3D.new()
	main_view.add_child(camera)
	camera.make_current()
	# High above the fixture's relief (ground near 48 here, 240 at its peak).
	camera.global_position = Vector3(64.0, 300.0, 64.0)
	camera.rotation_degrees = Vector3(-90.0, 0.0, 0.0)

	var inset_view := SubViewport.new()
	inset_view.size = Vector2i(32, 32)
	inset_view.world_3d = main_view.find_world_3d()
	inset_view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(inset_view)
	var inset_camera := Camera3D.new()
	inset_view.add_child(inset_camera)
	inset_camera.make_current()
	inset_camera.global_transform = camera.global_transform
	return {"terrain": terrain, "camera": camera, "main_view": main_view,
			"inset_camera": inset_camera, "inset_view": inset_view}


func _only(layers: PackedInt32Array, allowed: Array) -> bool:
	for layer in layers:
		if not allowed.has(layer):
			return false
	return true


func test_inset_frame_draws_its_own_pool_and_moves_the_main_pool() -> void:
	var fixture := _make_fixture()
	var terrain: Terrain = fixture["terrain"]
	var inset_camera: Camera3D = fixture["inset_camera"]
	terrain.render_frame()
	var main_layers := terrain.get_visible_patch_layers(false)
	assert_gt(main_layers.size(), 0, "the main view draws patches")
	assert_true(_only(main_layers, [WORLD, TERRAIN_FLAT_FALLBACK]), "on the world layers")
	assert_false(terrain.is_inset_frame_live())
	assert_eq(terrain.get_visible_patch_layers(true).size(), 0)

	terrain.render_inset_frame(inset_camera)
	assert_true(terrain.is_inset_frame_live())
	var inset_layers := terrain.get_visible_patch_layers(true)
	assert_gt(inset_layers.size(), 0, "the Inset draws its own patches")
	assert_true(_only(inset_layers, [INSET_VIEW]), "all on the Inset's own bit")
	assert_true(_only(terrain.get_visible_patch_layers(false), [MAIN_VIEW, TERRAIN_FLAT_FALLBACK]),
			"the main pool leaves WORLD while the Inset renders")
	# A main frame while the Inset is live keeps the main pool off WORLD.
	terrain.render_frame()
	assert_true(_only(terrain.get_visible_patch_layers(false), [MAIN_VIEW, TERRAIN_FLAT_FALLBACK]))

	terrain.release_inset_frame()
	assert_false(terrain.is_inset_frame_live())
	assert_eq(terrain.get_visible_patch_layers(true).size(), 0, "the Inset pool is freed")
	assert_true(_only(terrain.get_visible_patch_layers(false), [WORLD, TERRAIN_FLAT_FALLBACK]),
			"the main pool returns to WORLD")


# The pools render for their own cameras: an Inset camera admitting only
# INSET_VIEW sees the terrain exactly while the Inset frame lives, and a camera
# admitting only WORLD loses the main patches for that time.
func test_each_pool_renders_for_its_own_camera() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var fixture := _make_fixture()
	var terrain: Terrain = fixture["terrain"]
	var camera: Camera3D = fixture["camera"]
	var inset_camera: Camera3D = fixture["inset_camera"]
	var main_view: SubViewport = fixture["main_view"]
	var inset_view: SubViewport = fixture["inset_view"]
	camera.cull_mask = WORLD
	inset_camera.cull_mask = INSET_VIEW
	terrain.render_frame()
	var main_drawn := await _center(main_view)
	var inset_empty := await _center(inset_view)
	assert_gt(_distance(main_drawn, inset_empty), 0.1,
			"the main view draws the terrain (%s over %s)" % [main_drawn, inset_empty])

	terrain.render_inset_frame(inset_camera)
	var inset_drawn := await _center(inset_view)
	var main_hidden := await _center(main_view)
	assert_gt(_distance(inset_drawn, inset_empty), 0.1, "the Inset camera draws its own pool")
	assert_lt(_distance(main_hidden, inset_empty), 0.02,
			"a WORLD-only camera loses the main patches while the Inset renders")

	terrain.release_inset_frame()
	assert_lt(_distance(await _center(inset_view), inset_empty), 0.02,
			"the released Inset pool draws nothing")
	assert_lt(_distance(await _center(main_view), main_drawn), 0.02,
			"the main patches return to WORLD")


# The Inset's detail foliage draws the cells of the Inset's own terrain frame:
# with that frame live the dispatcher's Inset pass draws detail patches on
# INSET_VIEW, and once it is released the terrain hands the pass no cells.
func test_inset_foliage_takes_the_inset_frames_detail_cells() -> void:
	var fixture := _make_fixture()
	var terrain: Terrain = fixture["terrain"]
	var camera: Camera3D = fixture["camera"]
	var inset_camera: Camera3D = fixture["inset_camera"]
	# Near the ground, inside the detail-cell reach.
	camera.global_transform = Transform3D(Basis(), Vector3(64.0, 27.0, 64.0))
	inset_camera.global_transform = camera.global_transform
	var dispatcher := FoliageDispatcher.new()
	(fixture["main_view"] as SubViewport).add_child(dispatcher)
	var definition := TerrainFoliageDef.new()
	definition.graphic = "inset_frame_contract"
	definition.match = PackedInt32Array([1])
	dispatcher.configure_slots([definition], [BoxMesh.new()], [])
	dispatcher.height_sampler = func(_x: float, _z: float) -> float: return 10.0
	dispatcher.foliage_sampler = func(_x: float, _z: float) -> int: return 1
	dispatcher.set_terrain(terrain)

	# A detail patch draws once its terrain page is resident, a few frames in.
	var drawn: Array = []
	for _attempt in range(512):
		var time_ms := GameWorld.current_frame_clock_ms()
		terrain.render_frame()
		dispatcher.render_frame(camera.global_transform, time_ms)
		terrain.render_inset_frame(inset_camera)
		dispatcher.render_inset_frame(inset_camera, PackedVector3Array(), time_ms)
		drawn = _inset_detail_draws(dispatcher)
		if not drawn.is_empty():
			break
		await get_tree().process_frame
	assert_gt(drawn.size(), 0, "the Inset pass draws its own frame's detail cells")
	for row in drawn:
		assert_eq(int((row as Dictionary).get("layer_mask", 0)), INSET_VIEW)

	terrain.release_inset_frame()
	dispatcher.render_inset_frame(inset_camera, PackedVector3Array(),
			GameWorld.current_frame_clock_ms())
	assert_eq(_inset_detail_draws(dispatcher).size(), 0,
			"without the terrain's Inset frame the pass has no cells")

	# Release native texture owners before RenderingServer teardown.
	dispatcher.set_terrain(null)
	dispatcher.reset()
	terrain.set_terrain_data(null)


func _inset_detail_draws(dispatcher: FoliageDispatcher) -> Array:
	var rows: Array = []
	var inset: Dictionary = dispatcher.get_backend_report().get("inset", {})
	for row_value in inset.get("draws", []):
		var row := row_value as Dictionary
		if bool(row.get("visible", false)) and String(row.get("tier", "")) == "detail":
			rows.append(row)
	return rows


func _center(view: SubViewport) -> Color:
	for _frame in 2:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return view.get_texture().get_image().get_pixel(16, 16)


func _distance(a: Color, b: Color) -> float:
	return maxf(maxf(absf(a.r - b.r), absf(a.g - b.g)), absf(a.b - b.b))
