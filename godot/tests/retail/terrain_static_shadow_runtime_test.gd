extends GutTest

# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file (TestFs.staged_tmap), removed at the end.
const TMAP_STAGE := "static_shadow"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


func test_retail_scrate1_constant_alpha_does_not_reject_opaque_projshad() -> void:
	var resource_root := RetailData.mount_install_with("Scrate1.3di")
	if resource_root == null:
		pending("OPENNOVA_JO_DIR / retail JO PFFs serving Scrate1.3di are required for the shadow witness")
		return
	var object_data := ObjectData.new()
	assert_eq(object_data.open_from_resource_root(
			resource_root, "Scrate1.3di", false), OK,
		"Scrate1 must resolve from the mounted retail archives")
	# Scrate1 authors retail's constant AlphaGen style (24, start 128) on an
	# opaque FF_ST_OP material with no alpha test — the asset-gated
	# threedi_retail_material_facts ctest pins the MTRL row; the evaluated
	# constant alpha is the runtime's own read.
	assert_almost_eq(float(data_alpha_mod(object_data)), 128.0 / 255.0, 0.0001,
		"the constant generator supplies 128/255 material alpha")

	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var terrain_data := TerrainData.new()
	terrain_data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(terrain_data.load(), OK)
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(terrain_data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	# This suite asserts on the gated byte-level shadow diff counters.
	terrain.set_tile_cache_capture_diagnostics(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var placer := MissionObjectPlacer.create(resource_root, null)
	assert_true(placer.register_object_data("Scrate1", object_data))
	var origin := Vector3(64.0,
		terrain_data.get_height_world(Vector3(64.0, 0.0, 64.0)), 64.0)
	placer.register_static_instance(889, "Scrate1", 0,
		Transform3D(Basis().scaled(Vector3(4.0, 4.0, 4.0)), origin), true)
	terrain.set_static_shadow_placer(placer)
	var diagnostics := await TestFs.settle_tile_cache(self, terrain)
	assert_gt(int(diagnostics["shadow_provider_epoch_projection_draws"]), 0,
		"the mounted Scrate1 must enter at least one resident PROJSHAD page")
	assert_eq(int(diagnostics["shadow_provider_epoch_plan_failures"]), 0)
	assert_eq(int(diagnostics["shadow_provider_epoch_unsupported_draw_count"]), 0,
		"constant AlphaGen is irrelevant to an opaque, non-alpha-tested PROJSHAD draw")
	assert_gt(int(diagnostics["shadow_provider_epoch_triangles"]), 0)
	assert_gt(int(diagnostics["shadow_epoch_alpha_changed_bytes"]), 0,
		"Scrate1 must rasterize a real opaque silhouette into page alpha")
	assert_eq(int(diagnostics["shadow_epoch_rgb_changed_bytes"]), 0)

	terrain.set_static_shadow_placer(null)
	terrain.set_terrain_data(null)
	viewport.free()
	placer = null
	object_data = null
	resource_root = null
	terrain_data = null


# Retires every cached page (the destroyed-entity walk over the whole map) so
# the next frame recomposes them from the current casters.
static func data_alpha_mod(object_data: ObjectData) -> float:
	return float(object_data.eval_material_runtime(0, 0, {}).get("alpha_mod", -1.0))
