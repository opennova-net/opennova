extends GutTest

# The methods of tests/terrain_inset_frame_test.gd that need a RenderingDevice, split out
# of it so the windowed run selects them (G-100).

const TMAP_STAGE := "inset_frame"
const WORLD := 1 << 0
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


func _center(view: SubViewport) -> Color:
	for _frame in 2:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return view.get_texture().get_image().get_pixel(16, 16)


func _distance(a: Color, b: Color) -> float:
	return maxf(maxf(absf(a.r - b.r), absf(a.g - b.g)), absf(a.b - b.b))
