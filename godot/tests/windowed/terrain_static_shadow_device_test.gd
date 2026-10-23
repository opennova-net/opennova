extends GutTest

# The terrain static-shadow device pass against its CPU oracle (godot/src/terrain/
# terrain_static_shadow_device_pass.h). Retail draws the tile models' casters on its device
# into a temp target twice the page and composites the temp's blue into the page's alpha
# (Terrain_CollectAndRenderTileModels @0x60D250: Render_SubmitEntity @0x5DAD80 and
# CRenderBatchQueue_SortAndFlush @0x5DAE40 per caster; PolyTrn_RenderTile's composite
# @0x60E0C6..0x60E19D). The same resident pages are composed twice into the device page
# array: once by the device pass, once by the CPU raster (the engine's evaluation of the same
# job), then read back and compared under the GPU rasterization rules: RGB identical, alpha
# within one step but for a bounded share of edge and alpha-test pixels. A tests/windowed/
# script, run by `scripts/test_godot.sh --windowed` (a headless run pends it).

const SYN_HOUSE_SINE := "res://../fixtures/threedi/synth/house_lod0_sine_rotx.3di"
const TMAP_STAGE := "static_shadow_device"
# The share of a page's pixels whose alpha may differ by more than one step: a pixel centre
# on a triangle edge, a fragment within a float step of the receiver plane, an alpha-tested
# texel at its reference (fixed-point edges, D32F depth and 8-bit filter weights against the
# oracle's exact float arithmetic).
const EDGE_SHARE_LIMIT := 0.005


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


# One terrain over the staged Tmap with the house caster, its shadowed pages composed by
# the device pass or the CPU raster, settled and read back in page-key order.
func _resident_pages(device_shadow: bool) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_curtime(900)
	var environment := MissionEnvironment.new()
	environment.name = "Env"
	environment.environment_data = env_data
	viewport.add_child(environment)
	environment.configure_mission_clock(0x0900, 60)
	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(terrain_data.load(), OK, "the Tmap terrain fixture must load")
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_environment_path(NodePath("../Env"))
	terrain.set_terrain_data(terrain_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	terrain.set_tile_cache_capture_diagnostics(true)
	terrain.set_tile_cache_device_shadow(device_shadow)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()
	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(SYN_HOUSE_SINE)), OK)
	var placer := MissionObjectPlacer.create(null, null)
	assert_true(placer.register_object_data("house", object_data))
	var ground_sample := Vector3(64.125, 0.0, 64.125)
	var origin := Vector3(ground_sample.x, terrain_data.get_height_world(ground_sample),
			ground_sample.z)
	placer.register_static_instance(100, "house", 0,
		Transform3D(Basis().scaled(Vector3(3.0, 3.0, 3.0)), origin), true)
	terrain.set_static_shadow_placer(placer)
	var diagnostics := await TestFs.settle_tile_cache(self, terrain)
	var pages: Array[PackedByteArray] = []
	var layers: PackedInt32Array = diagnostics.get("resident_layers", PackedInt32Array())
	for layer in layers:
		pages.append(terrain.get_tile_cache_layer_bytes(layer))
	return {"diagnostics": diagnostics, "pages": pages}


func test_the_device_pass_draws_the_cpu_rasters_pages() -> void:
	if not _rd_available():
		pending("needs a RenderingDevice (scripts/test_godot.sh --windowed)")
		return
	var device: Dictionary = await _resident_pages(true)
	var oracle: Dictionary = await _resident_pages(false)
	var device_diagnostics: Dictionary = device["diagnostics"]
	var oracle_diagnostics: Dictionary = oracle["diagnostics"]
	assert_true(bool(device_diagnostics["page_array_device"]), "the pages live on the device")
	assert_true(bool(device_diagnostics["shadow_device_pass"]),
		"the device builds the pass: %s" % str(device_diagnostics["shadow_device_failure"]))
	assert_gt(int(device_diagnostics["shadow_device_jobs"]), 0,
		"the house's pages are drawn by the device pass")
	assert_eq(int(device_diagnostics["shadow_raster_failures"]), 0)
	assert_eq(int(oracle_diagnostics["shadow_device_jobs"]), 0,
		"the oracle's pages are rasterized on the CPU")
	assert_gt(int(oracle_diagnostics["shadow_raster_jobs"]), 0)
	# The pass changes the alpha alone (the readback against the upload).
	assert_gt(int(device_diagnostics["shadow_epoch_alpha_changed_bytes"]), 0,
		"the device draws the silhouette into the page alpha")
	assert_eq(int(device_diagnostics["shadow_epoch_rgb_changed_bytes"]), 0,
		"the device pass preserves the page RGB byte for byte")
	var device_pages: Array[PackedByteArray] = device["pages"]
	var oracle_pages: Array[PackedByteArray] = oracle["pages"]
	assert_eq(device_pages.size(), oracle_pages.size(), "the same resident pages")
	assert_gt(device_pages.size(), 0)
	for index in mini(device_pages.size(), oracle_pages.size()):
		var a: PackedByteArray = device_pages[index]
		var b: PackedByteArray = oracle_pages[index]
		assert_eq(a.size(), b.size(), "page %d: the same layer size" % index)
		if a.size() != b.size() or a.is_empty():
			continue
		var rgb_differences := 0
		var wide_alpha := 0
		var largest := 0
		for byte in range(0, a.size(), 4):
			if a[byte] != b[byte] or a[byte + 1] != b[byte + 1] or a[byte + 2] != b[byte + 2]:
				rgb_differences += 1
			var difference := absi(int(a[byte + 3]) - int(b[byte + 3]))
			largest = maxi(largest, difference)
			if difference > 1:
				wide_alpha += 1
		var pixels := a.size() / 4
		assert_eq(rgb_differences, 0, "page %d: RGB identical to the oracle" % index)
		assert_lte(float(wide_alpha) / float(pixels), EDGE_SHARE_LIMIT,
			"page %d: %d of %d alpha pixels past one step (largest %d)" % [
				index, wide_alpha, pixels, largest])
