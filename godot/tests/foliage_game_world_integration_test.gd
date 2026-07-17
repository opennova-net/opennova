extends GutTest

# Collected production-seam gate for retail foliage. It stages the committed
# Dvxi5 terrain plus real 3DI geometry under both authored vegetation names,
# then drives GameWorld exactly through its public load/tick surface.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")
const DVXI5_FIXTURE := "res://../fixtures/godot/dvxi5"
const ENV_FIXTURE := "res://../fixtures/env/full_00.env"
const MODEL_FIXTURE := "res://../fixtures/3dp/CmpFireN/CmpFireN.3di"
const ROUTED_WITNESS_WORLD := Vector2(-120.0, -24.0)
const ROUTED_WITNESS_MAP := Vector2i(98, 122)
const ROUTED_WITNESS_FLAT_MAP := Vector2i(226, 250)
const ROUTED_WITNESS_MIRRORED_MAP := Vector2i(98, 134)
const DETAIL_WITNESS_WORLD := Vector2(120.0, 428.0)
const DETAIL_WITNESS_FLAT_MAP := Vector2i(30, 107)
const DETAIL_WITNESS_ROUTED_MAP := Vector2i(158, 235)
const DETAIL_WITNESS_MIRRORED_MAP := Vector2i(30, 149)
const FOLIAGE_MATCH := 254


func before_each() -> void:
	_cleanup_dir(_fixture_root())
	VegAssets.clear_cache()


func after_each() -> void:
	VegAssets.clear_cache()
	_cleanup_dir(_fixture_root())


func test_game_world_resolves_both_dvxi5_models_and_emits_foliage() -> void:
	_stage_runtime_fixture()
	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.set_root_dir(_fixture_root()), OK)

	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	assert_not_null(packed)
	if packed == null:
		return
	var world := packed.instantiate() as GameWorld
	assert_not_null(world)
	if world == null:
		return
	add_child_autofree(world)

	var camera := Camera3D.new()
	camera.current = true
	add_child_autofree(camera)
	world.set_resource_root(resource_root)
	world.visible = true
	assert_eq(world.load_world(), OK)

	var data := world.get_terrain_data()
	var dispatcher := world.get_node_or_null("NovaTerrain/FoliageDispatcher") as NovaFoliageDispatcher
	assert_not_null(data)
	assert_not_null(dispatcher)
	if data == null or dispatcher == null:
		world.unload()
		return

	# Two disjoint witnesses prove the authored foliagemap is interpreted with
	# the retail policy for each consumer: flat wrapping for detail grass and
	# sector routing for MODEL masks.
	var foliage_map: NovaTerrainFoliageMap = data.get_foliage_map()
	assert_not_null(foliage_map)
	if foliage_map == null:
		world.unload()
		return
	assert_eq(int(foliage_map.get_index(ROUTED_WITNESS_MAP.x, ROUTED_WITNESS_MAP.y)),
		FOLIAGE_MATCH,
		"The routed raw map coordinate should retain the authored foliage match.")
	assert_eq(int(foliage_map.get_index(
		ROUTED_WITNESS_FLAT_MAP.x, ROUTED_WITNESS_FLAT_MAP.y)), 255,
		"The routed witness's flat coordinate should remain a negative witness.")
	assert_eq(int(foliage_map.get_index(
		ROUTED_WITNESS_MIRRORED_MAP.x, ROUTED_WITNESS_MIRRORED_MAP.y)), 255,
		"The routed witness's mirrored coordinate should remain negative.")
	assert_eq(int(foliage_map.get_index(
		DETAIL_WITNESS_FLAT_MAP.x, DETAIL_WITNESS_FLAT_MAP.y)), FOLIAGE_MATCH,
		"The flat detail coordinate should retain the authored foliage match.")
	assert_eq(int(foliage_map.get_index(
		DETAIL_WITNESS_ROUTED_MAP.x, DETAIL_WITNESS_ROUTED_MAP.y)), 255,
		"The detail witness's routed coordinate should remain negative.")
	assert_eq(int(foliage_map.get_index(
		DETAIL_WITNESS_MIRRORED_MAP.x, DETAIL_WITNESS_MIRRORED_MAP.y)), 255,
		"The detail witness's mirrored coordinate should remain negative.")
	assert_eq(foliage_map.get_detail_map_position_world(
		DETAIL_WITNESS_WORLD.x, DETAIL_WITNESS_WORLD.y),
		DETAIL_WITNESS_FLAT_MAP,
		"Runtime preview and authoring tools must resolve the same flat detail pixel.")
	assert_eq(foliage_map.get_detail_sample_resolution(), 256,
		"Dvxi5's 256-wide map should expose the retail detail sample resolution.")

	assert_eq(int(data.get_foliage_index_world(
		ROUTED_WITNESS_WORLD.x, ROUTED_WITNESS_WORLD.y)), FOLIAGE_MATCH,
		"MODEL sampling must use the routed authored-map coordinate.")
	assert_eq(int(data.get_detail_foliage_index_world(
		ROUTED_WITNESS_WORLD.x, ROUTED_WITNESS_WORLD.y)), 255,
		"Detail sampling must not inherit MODEL's sector-routed match.")
	assert_eq(int(data.get_detail_foliage_index_world(
		DETAIL_WITNESS_WORLD.x, DETAIL_WITNESS_WORLD.y)), FOLIAGE_MATCH,
		"Detail sampling must use the flat wrapped authored-map coordinate.")
	assert_eq(int(data.get_foliage_index_world(
		DETAIL_WITNESS_WORLD.x, DETAIL_WITNESS_WORLD.y)), 255,
		"MODEL sampling must not inherit detail's flat wrapped match.")

	var defs: Array = data.get_foliage_defs()
	assert_eq(defs.size(), 2, "Dvxi5 should retain both authored foliage definitions.")
	var meshes := VegAssets.resolve_slot_meshes(resource_root, defs)
	assert_eq(meshes.size(), defs.size())
	for slot in range(defs.size()):
		var mesh := meshes[slot] as Mesh
		assert_not_null(mesh, "Authored foliage slot %d should resolve staged 3DI geometry." % slot)
		if mesh != null:
			assert_gt(mesh.get_surface_count(), 0)

	# Render from the flat-positive detail witness so the production dispatcher
	# must choose the correct data policy to produce visible grass.
	dispatcher.reset()
	var position := Vector3(DETAIL_WITNESS_WORLD.x, 0.0, DETAIL_WITNESS_WORLD.y)
	position.y = data.get_height_world_bilinear(position)
	camera.global_position = position + Vector3(0.0, 2.2, 0.0)
	camera.look_at(position + Vector3(0.0, 0.5, -12.0), Vector3.UP)

	var stats := {}
	for _frame in range(60):
		await get_tree().process_frame
		world.tick(camera.global_position, camera.global_transform, 1.0 / 60.0)
		stats = dispatcher.get_frame_stats()
		if _has_complete_detail_output(dispatcher, stats):
			break

	assert_true(bool(stats.get("native_detail_source", false)),
		"GameWorld foliage must consume NovaTerrain native detail cells.")
	assert_gt(int(stats.get("detail_cells", 0)), 0,
		"The detail camera witness should collect native 16-unit detail cells: %s" % stats)
	assert_gt(int(stats.get("runtime_detail_intents", 0)), 0,
		"Foliage match %d should emit detail intents: %s" % [FOLIAGE_MATCH, stats])
	assert_gt(int(stats.get("detail_vertices", 0)), 0,
		"Foliage match %d should expand real 3DI vertices: %s" % [FOLIAGE_MATCH, stats])
	assert_gt(int(stats.get("render_batches", 0)), 0,
		"Foliage match %d should submit render batches: %s" % [FOLIAGE_MATCH, stats])
	assert_gt(dispatcher.get_total_instances(), 0,
		"Foliage match %d should retain visible instances: %s" % [FOLIAGE_MATCH, stats])

	dispatcher.reset()
	world.unload()
	await get_tree().process_frame


func _has_complete_detail_output(dispatcher: NovaFoliageDispatcher, stats: Dictionary) -> bool:
	return (
		int(stats.get("runtime_detail_intents", 0)) > 0
		and int(stats.get("detail_vertices", 0)) > 0
		and int(stats.get("render_batches", 0)) > 0
		and dispatcher.get_total_instances() > 0
	)


func _stage_runtime_fixture() -> void:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	var terrain_source := ProjectSettings.globalize_path(DVXI5_FIXTURE)
	for filename in DirAccess.get_files_at(terrain_source):
		_copy_file(terrain_source.path_join(filename), root.path_join(filename))
	_copy_file(ProjectSettings.globalize_path(ENV_FIXTURE), root.path_join("full_00.env"))
	var model_source := ProjectSettings.globalize_path(MODEL_FIXTURE)
	_copy_file(model_source, root.path_join("mveg5.3di"))
	_copy_file(model_source, root.path_join("mveg5b.3di"))


func _copy_file(source: String, destination: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(source)
	assert_gt(bytes.size(), 0, "Fixture source should contain bytes: %s" % source)
	var file := FileAccess.open(destination, FileAccess.WRITE)
	assert_not_null(file, "Fixture destination should open: %s" % destination)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()


func _fixture_root() -> String:
	return OS.get_cache_dir().path_join("opennova_foliage_game_world_test")


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for filename in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(filename))
	for directory in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(directory))
	DirAccess.remove_absolute(path)
