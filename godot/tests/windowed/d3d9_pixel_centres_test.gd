extends GutTest

# Where the original's pixels sit (engine renderer/d3d9_raster.h): it rasterises
# through Direct3D 9, pixel i's centre on the integer window coordinate, so its
# image of the world and its HUD land half a pixel right and down of a raster whose
# centres sit at i + 0.5. The 3D pass builds a plain projection
# (Render_SetViewAndProjectionMatrices @0x58D9DE) over an integer viewport
# (Render_SetViewport @0x58A720); the HUD draws pre-transformed vertices at its
# coordinates as given (GDynamicVB_DrawPrimitive @0x6788E0). Both pins render
# through the real device: a tests/windowed/ script, run by
# `scripts/test_godot.sh --windowed` (a headless run pends it).

const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"
const TICK_COUNT := 2

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


# A BGRA TGA checkerboard: texel (x, y) is `on` where x + y is even, black otherwise.
func _checker_tga(size: Vector2i, on: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18 + size.x * size.y * 4)
	bytes[2] = 2
	bytes.encode_u16(12, size.x)
	bytes.encode_u16(14, size.y)
	bytes[16] = 32
	bytes[17] = 0x28
	for y in size.y:
		for x in size.x:
			var texel := on if (x + y) % 2 == 0 else Color(0, 0, 0, 1)
			var at := 18 + (y * size.x + x) * 4
			bytes[at] = texel.b8
			bytes[at + 1] = texel.g8
			bytes[at + 2] = texel.r8
			bytes[at + 3] = texel.a8
	return bytes


func _surface(size: Vector2i) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.own_world_3d = true
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	return viewport


func _render(viewport: SubViewport) -> Image:
	await get_tree().process_frame
	await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


# The HUD's draw lists are the original's window coordinates: a quad at integer
# coordinates covers the same pixels on either raster, and D3D9 samples pixel i at
# its centre, the window coordinate i itself, so pixel x0 + k reads the quad's UV
# at k. The static frame's bordered quad insets its UVs by 0.05 texel and widens
# them by one pixel's worth, u0 = 0.05 / tex_w, u1 = 1 - u0 + 1 / width [orig:
# HUD_DrawTexturedQuadWithBorder @0x590C40 - @0x590D2F..0x590D8C], so a
# texel-for-texel checker filters to a drifting blend of its texels, which this
# pins against the D3D9 model; drawn at this raster's own centres it would read the
# UVs half a pixel on, a different blend. The checker's odd height makes it the
# same read either way up, so a loader's row order cannot change the expectation.
const CHECKER := Vector2i(32, 17)
const CHECKER_ON := 64.0 / 255.0


# Texel (x, y) of the checker, clamped to its edge.
func _checker_at(x: int, y: int) -> float:
	x = clampi(x, 0, CHECKER.x - 1)
	y = clampi(y, 0, CHECKER.y - 1)
	return CHECKER_ON if (x + y) % 2 == 0 else 0.0


# The bilinear read at texel coordinates (tx, ty), texel centres on the halves.
func _bilinear(tx: float, ty: float) -> float:
	var fx := tx - 0.5
	var fy := ty - 0.5
	var x0 := floori(fx)
	var y0 := floori(fy)
	var ax := fx - x0
	var ay := fy - y0
	var top := _checker_at(x0, y0) * (1.0 - ax) + _checker_at(x0 + 1, y0) * ax
	var bottom := _checker_at(x0, y0 + 1) * (1.0 - ax) + _checker_at(x0 + 1, y0 + 1) * ax
	return top * (1.0 - ay) + bottom * ay


# Pixel (kx, ky) of the bordered quad as D3D9 reads it (`centre` 0), or as a raster
# with its centres half a pixel on would (`centre` 0.5).
func _bordered_read(kx: int, ky: int, centre: float) -> float:
	var u0 := 0.05 / CHECKER.x
	var v0 := 0.05 / CHECKER.y
	var u1 := 1.0 - u0 + 1.0 / CHECKER.x
	var v1 := 1.0 - v0 + 1.0 / CHECKER.y
	var tx := (u0 + (u1 - u0) * (kx + centre) / CHECKER.x) * CHECKER.x
	var ty := (v0 + (v1 - v0) * (ky + centre) / CHECKER.y) * CHECKER.y
	return _bilinear(tx, ty)


# The largest miss of the measured pixels against a model scaled by its best fit
# (the texture's colour stage scales the read; the sampling is what is pinned).
func _fit_miss(measured: PackedFloat32Array, model: PackedFloat32Array) -> float:
	var num := 0.0
	var den := 0.0
	for i in measured.size():
		num += measured[i] * model[i]
		den += model[i] * model[i]
	var scale := num / den if den > 0.0 else 0.0
	var miss := 0.0
	for i in measured.size():
		miss = maxf(miss, absf(measured[i] - scale * model[i]))
	return miss


func test_the_hud_samples_its_textures_where_d3d9_does() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var dir := TestFs.cache_dir(self, "d3d9_pixel_centres")
	_temp_dirs.append(dir)
	TestFs.write_bytes(self, dir.path_join("frame.tga"),
			_checker_tga(CHECKER, Color(CHECKER_ON, CHECKER_ON, CHECKER_ON, 1)))
	TestFs.write_text(self, dir.path_join("hudpos.def"),
			"\n".join(PackedStringArray(HudFixture.DECLUTTER_ROWS)
					+ PackedStringArray(["STATICFRAME frame.tga 100 100"])))
	var layout := HudPos.new()
	assert_eq(layout.load(dir.path_join("hudpos.def")), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var viewport := _surface(Vector2i(1024, 768))
	var hud := HudOverlay.new()
	hud.size = Vector2(1024, 768)
	viewport.add_child(hud)
	hud.configure(layout, root)
	hud.set_frame_clocks(100, 100, 100, 100)
	hud.set_player_state(1.0, 0, 80.0)
	var image: Image = await _render(viewport)
	# The frame's footprint (authored at 100, 100 of the 1024 x 768 design, drawn
	# 1:1 on this surface): every pixel the quad lit around it.
	var background := image.get_pixel(70, 70).r
	var low := Vector2i(1 << 20, 1 << 20)
	var high := Vector2i(-1, -1)
	for y in range(80, 140):
		for x in range(80, 160):
			if absf(image.get_pixel(x, y).r - background) > 0.02:
				low = low.min(Vector2i(x, y))
				high = high.max(Vector2i(x, y))
	var lit := Rect2i(low, high - low + Vector2i.ONE)
	assert_eq(lit.size, CHECKER,
			"the quad at integer coordinates covers its 32 x 17 pixels, as on either raster")
	if lit.size != CHECKER:
		return
	var measured := PackedFloat32Array()
	var d3d9 := PackedFloat32Array()
	var godot_centres := PackedFloat32Array()
	for ky in range(2, CHECKER.y - 2):
		for kx in range(2, CHECKER.x - 2):
			measured.append(image.get_pixel(lit.position.x + kx, lit.position.y + ky).r)
			d3d9.append(_bordered_read(kx, ky, 0.0))
			godot_centres.append(_bordered_read(kx, ky, 0.5))
	var miss := _fit_miss(measured, d3d9)
	assert_lt(miss, 0.02,
			"every interior pixel reads the bordered UVs at D3D9's pixel centre (miss %.3f)" % miss)
	assert_gt(_fit_miss(measured, godot_centres), 0.05,
			"and not where this raster's own centres would read them")


# The frame's camera draws through the surface's D3D9 half pixel: an edge the
# original's projection puts a quarter pixel past a pixel centre lights the
# next pixel on, on both axes (D3D9: centre >= edge), where a plain projection
# on this raster would light the pixel the edge falls in.
func test_the_frame_camera_rasterises_on_d3d9_pixel_centres() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var world := WorldFixture.boot_minimal(self)
	var surface := _surface(Vector2i(64, 48))
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	environment.environment.background_mode = Environment.BG_COLOR
	environment.environment.background_color = Color(0, 0, 0, 1)
	surface.add_child(environment)
	var camera := Camera3D.new()
	surface.add_child(camera)
	camera.make_current()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, ControlsModel.new())
	world.get_sim().set_local_player_aspect_mode(-1)
	var dt := Simulation.tick_dt()
	for i in TICK_COUNT:
		var frame_input := presenter.before_world_tick(dt, false, true)
		world.tick(camera.global_position, camera.global_transform, dt, frame_input)
	assert_eq(camera.projection, Camera3D.PROJECTION_FRUSTUM,
			"the frame camera draws through its offset frustum")
	assert_null(presenter.projection_viewport(), "the native mode draws the surface directly")
	# A white card in front of the eye whose top-left corner the original's
	# projection (screen_projection: the raster shift taken out) puts at window
	# (32.25, 24.25) on the 64 x 48 surface.
	var screen := presenter.screen_projection()
	var depth := 5.0
	var ndc := Vector2(32.25 / 64.0 * 2.0 - 1.0, 1.0 - 24.25 / 48.0 * 2.0)
	var corner := Vector3(ndc.x * depth / screen.x.x, ndc.y * depth / screen.y.y, -depth)
	var mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([corner, corner + Vector3(4, 0, 0),
			corner + Vector3(4, -4, 0), corner, corner + Vector3(4, -4, 0),
			corner + Vector3(0, -4, 0)])
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.disable_fog = true
	material.albedo_color = Color(1, 1, 1, 1)
	var card := MeshInstance3D.new()
	card.mesh = mesh
	card.material_override = material
	card.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	surface.add_child(card)
	card.global_transform = camera.get_camera_transform()
	camera.cull_mask = card.layers
	var image: Image = await _render(surface)
	assert_lt(image.get_pixel(32, 30).r, 0.5, "column 32's centre (32) lies left of the edge at 32.25")
	assert_gt(image.get_pixel(33, 30).r, 0.5, "column 33's centre (33) lies past it")
	assert_lt(image.get_pixel(40, 24).r, 0.5, "row 24's centre (24) lies above the edge at 24.25")
	assert_gt(image.get_pixel(40, 25).r, 0.5, "row 25's centre (25) lies below it")
	presenter.teardown()
	assert_eq(camera.projection, Camera3D.PROJECTION_PERSPECTIVE,
			"teardown hands the camera its own projection back")
