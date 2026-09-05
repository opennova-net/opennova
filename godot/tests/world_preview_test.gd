extends GutTest

const EXAMPLE := "res://examples/world_preview.tscn"
const DATA := "res://../examples/world_preview"

var _worlds: Array[GameWorld] = []
var _dirs: PackedStringArray = []


func after_each() -> void:
	for world in _worlds:
		world.unload_preview()
	await get_tree().process_frame
	_worlds.clear()
	for directory in _dirs:
		TestFs.remove_dir_recursive(directory)
	_dirs.clear()


func _world(source: WorldSource = null) -> GameWorld:
	var world := load(EXAMPLE).instantiate() as GameWorld
	add_child_autofree(world)
	_worlds.append(world)
	if source != null:
		world.world_source = source
	return world


func _staged_source() -> WorldSource:
	var directory := OS.get_cache_dir().path_join("opennova_preview_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(directory), OK)
	_dirs.append(directory)
	var source_dir := ProjectSettings.globalize_path(DATA)
	for filename in DirAccess.get_files_at(source_dir):
		assert_eq(DirAccess.copy_absolute(source_dir.path_join(filename),
				directory.path_join(filename)), OK)
	var source := WorldSource.new()
	source.data_directory = directory
	source.mission_name = "preview.bms"
	return source


func test_example_loads_without_gameplay_and_camera_can_belong_to_another_viewport() -> void:
	var world := _world()
	assert_eq(world.load_preview(), OK)
	assert_eq(world.get_preview_status(), "ready", "\n".join(world.get_preview_diagnostics()))
	assert_null(world.get_sim())
	assert_null(world.get_mission_audio())
	assert_null(world.get_effect_world())
	assert_gt(world.get_static_live_population_count(), 0)
	var viewport := SubViewport.new()
	add_child_autofree(viewport)
	viewport.size = Vector2i(800, 600)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.position = Vector3(0, 60, 40)
	camera.look_at(Vector3(0, 20, 0))
	camera.make_current()
	var environment := world.get_node("MissionEnvironment") as MissionEnvironment
	var time_before := environment.get_time_of_day()
	for frame in range(3):
		camera.position.x += 2.0
		assert_eq(world.refresh_preview(camera), OK)
		world.tick(camera.position, camera.transform, 1.0)
		await get_tree().process_frame
	assert_eq(environment.get_time_of_day(), time_before)
	assert_null(world.get_sim(), "even an ordinary frame call cannot activate a preview")
	world.unload_preview()
	assert_false(world.is_preview_active())
	assert_null(world.get_loaded_mission())


func test_missing_dependency_is_partial_and_reload_discards_old_content() -> void:
	var source := _staged_source()
	var world := _world(source)
	assert_eq(world.load_preview(), OK)
	assert_eq(DirAccess.remove_absolute(source.data_directory.path_join("crate.tga")), OK)
	assert_eq(world.load_preview(), OK)
	assert_eq(world.get_preview_status(), "partial")
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "crate.tga")
	source.mission_name = "missing.bms"
	assert_ne(world.load_preview(), OK)
	assert_eq(world.get_preview_status(), "failed")
	assert_false(world.is_preview_active())
	assert_null(world.get_sim())
	assert_null(world.get_loaded_mission())
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "missing.bms")


func test_missing_cpt_is_explicit_instead_of_ready_over_empty_terrain() -> void:
	var source := _staged_source()
	assert_eq(DirAccess.remove_absolute(source.data_directory.path_join("Tmap.cpt")), OK)
	var world := _world(source)
	assert_eq(world.load_preview(), OK)
	assert_eq(world.get_preview_status(), "partial")
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "Tmap.cpt")


func test_required_environment_failure_and_local_install_configuration() -> void:
	var source := _staged_source()
	assert_eq(DirAccess.remove_absolute(source.data_directory.path_join("mnml.env")), OK)
	var world := _world(source)
	assert_eq(world.load_preview(), ERR_FILE_NOT_FOUND)
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "mnml.env")
	source.install_key = "my_local_data"
	assert_eq(world.load_preview(), ERR_UNCONFIGURED)
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "my_local_data")
	source.data_directory = "irrelevant"
	assert_eq(world.load_preview(ProjectSettings.globalize_path(DATA)), OK)
	assert_eq(world.get_preview_status(), "ready")


func _property_usage(object: Object, name: String) -> int:
	for property in object.get_property_list():
		if property.name == name:
			return property.usage
	return 0


func test_saved_scene_keeps_source_while_authored_nodes_hold_loaded_configuration() -> void:
	var authored := _world()
	var terrain := authored.get_node("Terrain") as Terrain
	var water := authored.get_node("Water") as Water
	var environment := authored.get_node("MissionEnvironment") as MissionEnvironment
	var foliage := terrain.get_node("FoliageDispatcher") as FoliageDispatcher
	environment.time_of_day = 2330
	water.water_height = 17
	var original_mission := authored.mission_file
	var original_terrain := authored.terrain_file
	var original_environment := authored.env_file
	var original_time := environment.time_of_day
	var original_water := water.water_height
	var clear := (authored.get_node("ClearColor") as WorldEnvironment).environment
	var original_clear := clear.background_color
	var original_compositor := (authored.get_node("ClearColor") as WorldEnvironment).compositor
	terrain.lod_quality = 1.7
	assert_eq(authored.load_preview(), OK)
	assert_eq(authored.mission_file, "preview.bms")
	assert_eq(authored.terrain_file, "Tmap.trn")
	assert_eq(authored.env_file, "mnml.env")
	assert_not_null(terrain.terrain_data)
	assert_same(terrain.terrain_data, water.terrain_data)
	assert_same(terrain.terrain_data, foliage.terrain_data)
	assert_same(terrain.terrain_data, authored.get_node("Celestial").terrain_data)
	assert_same(terrain.terrain_data, authored.get_node("EnvironmentCubeCapture").terrain_data)
	assert_same(foliage.get_terrain(), terrain)
	assert_same(foliage.get_weather(), authored.get_node("Weather"))
	assert_true(environment.environment_data.is_loaded())
	assert_ne(authored.get_preview_environment(), clear)
	assert_eq(clear.background_color, original_clear)
	# A non-editor test world already installed its runtime compositor on ready.
	# Preview preserves it; the EditorScript checks that editor Save has no effects.
	assert_same((authored.get_node("ClearColor") as WorldEnvironment).compositor, original_compositor)
	for pair in [[terrain, "terrain_data"], [water, "terrain_data"],
			[environment, "environment_data"], [foliage, "terrain_data"], [authored, "mission_file"]]:
		var usage := _property_usage(pair[0], pair[1])
		assert_eq(usage & PROPERTY_USAGE_STORAGE, 0)
		assert_ne(usage & PROPERTY_USAGE_EDITOR, 0)
		assert_ne(usage & PROPERTY_USAGE_READ_ONLY, 0)
	var packed := PackedScene.new()
	assert_eq(packed.pack(authored), OK)
	var directory := OS.get_cache_dir().path_join("opennova_preview_save_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(directory), OK)
	_dirs.append(directory)
	var scene_path := directory.path_join("world.tscn")
	assert_eq(ResourceSaver.save(packed, scene_path), OK)
	var saved := FileAccess.get_file_as_string(scene_path)
	for generated in ["TerrainData", "WorldPreview", "MissionObjects", "MissionRoot", "shadow_caster_mask"]:
		assert_false(saved.contains(generated), generated + " is not scene data")
	assert_true(authored.is_preview_active())
	assert_not_null(terrain.terrain_data, "Save keeps the node configured")
	var reopened := (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene).instantiate() as GameWorld
	autofree(reopened)
	assert_eq(reopened.world_source.mission_name, "preview.bms")
	assert_almost_eq((reopened.get_node("Terrain") as Terrain).lod_quality, 1.7, 0.001)
	assert_null((reopened.get_node("Terrain") as Terrain).terrain_data)
	assert_eq(reopened.mission_file, original_mission)
	authored.unload_preview()
	assert_eq(authored.mission_file, original_mission)
	assert_eq(authored.terrain_file, original_terrain)
	assert_eq(authored.env_file, original_environment)
	assert_eq(environment.time_of_day, original_time)
	assert_eq(water.water_height, original_water)
	assert_null(terrain.terrain_data)
	assert_null(water.terrain_data)
	assert_null(foliage.terrain_data)
	assert_null(environment.environment_data)
	assert_null((authored.get_node("SkyDome") as SkyDome).get_mesh_instance())
	assert_false(water.is_built())
	assert_eq(foliage.get_slot_meshes().size(), 0)
	assert_ne(_property_usage(terrain, "terrain_data") & PROPERTY_USAGE_STORAGE, 0)
	assert_eq(_property_usage(terrain, "terrain_data") & PROPERTY_USAGE_READ_ONLY, 0)
	assert_eq(clear.background_color, original_clear)


func test_retail_and_loose_sources_choose_their_own_bms_and_dependencies() -> void:
	var source := _staged_source()
	var entries: Array = []
	for filename in DirAccess.get_files_at(source.data_directory):
		entries.append({"name": filename, "bytes": FileAccess.get_file_as_bytes(
				source.data_directory.path_join(filename))})
	assert_eq(TestPff.write(source.data_directory.path_join("resource.pff"), entries), OK)
	var loose := MissionData.new()
	assert_eq(loose.open_file(source.data_directory.path_join("preview.bms")), OK)
	assert_true(loose.set_header_string("mission_name", "Loose selection"))
	assert_eq(loose.save_as(source.data_directory.path_join("preview.bms")), OK)
	assert_eq(DirAccess.remove_absolute(source.data_directory.path_join("crate.tga")), OK)
	var world := _world(source)
	assert_eq(world.load_preview(), OK)
	assert_eq(world.get_loaded_mission().get_mission_name(), "Loose selection")
	assert_eq(world.get_preview_status(), "partial")
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "crate.tga",
			"Loose mode cannot silently fill a missing dependency from an archive")
	source.source_kind = WorldSource.RETAIL_INSTALL
	assert_eq(world.load_preview(), OK)
	assert_eq(world.get_loaded_mission().get_mission_name(), "World preview",
			"Retail mode ignores the edited loose BMS")
	assert_eq(world.get_preview_status(), "ready",
			"Retail mode resolves the archived texture")


func test_missing_named_sky_dependency_is_partial() -> void:
	var source := _staged_source()
	var environment := EnvFile.new()
	environment.source_path = source.data_directory.path_join("mnml.env")
	assert_eq(environment.load(), OK)
	environment.sky_map1 = "absent.pcx"
	assert_eq(environment.save_to_path(environment.source_path), OK)
	var world := _world(source)
	assert_eq(world.load_preview(), OK)
	assert_eq(world.get_preview_status(), "partial")
	assert_string_contains("\n".join(world.get_preview_diagnostics()), "mnml.env requires absent.pcx")
