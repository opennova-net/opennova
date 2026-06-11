extends GutTest

const WORLD_TEST_ROOT := "nova_world_test"


func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


class TransportRuntimeStub:
	extends Node
	var ticks := 0
	var _playing := false
	func is_playing() -> bool:
		return _playing
	func play() -> void:
		_playing = true
	func pause() -> void:
		_playing = false
	func tick() -> bool:
		ticks += 1
		return true


func test_tick_gates_the_runtime_on_its_transport() -> void:
	# The game host's tick must respect MissionRuntime's play flag - the debug
	# overlay's Pause/Step work on a live mission BECAUSE this gate exists
	# (before it, play()/pause() were inert in the game).
	var world := _make_world()
	add_child_autofree(world)
	var runtime := TransportRuntimeStub.new()
	add_child_autofree(runtime)
	world._runtime = runtime
	world._loaded = true

	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 0, "a paused runtime never ticks")
	runtime.play()
	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 1, "a playing runtime ticks once per host frame")
	runtime.pause()
	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 1, "pausing stops it again")
	world._runtime = null
	world._loaded = false


func test_load_world_requires_hardcoded_environment_in_global_root() -> void:
	var root := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("missing_env_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	_write_fixture_file(root.path_join("Dvxi5.trn"), "terrain_name \"Dvxi5\"\n")

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame

	assert_eq(world.load_world(root), ERR_FILE_NOT_FOUND, "Runtime global root must contain full_00.env next to Dvxi5.trn.")


func test_packaged_scene_instantiates_with_intact_wiring() -> void:
	# nova_world.tscn is the embeddable world (the game instances it in
	# main_game.tscn; play-in-editor instances it in a workspace viewport). Pin
	# the extraction: every engine node is present and the intra-scene NodePaths
	# survived the move out of main_game.tscn.
	var packed := load("res://engine/world/nova_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	assert_true(world is NovaWorld, "the root carries the NovaWorld script")
	for child_name in ["NovaTerrain", "NovaEnvironment", "NovaSky", "NovaWeather", "NovaWater", "NovaCelestial"]:
		assert_not_null(world.get_node_or_null(child_name), "%s is in the packaged scene" % child_name)
	assert_not_null(world.get_node_or_null("NovaTerrain/FoliageDispatcher"))
	assert_not_null(world.get_node_or_null("NovaTerrain/TileOverlay"))
	var terrain: NovaTerrain = world.get_node("NovaTerrain")
	assert_eq(terrain.environment_path, NodePath("../NovaEnvironment"), "terrain env path survived extraction")
	assert_eq(terrain.weather_path, NodePath("../NovaWeather"), "terrain weather path survived extraction")


func test_load_mission_data_rejects_an_empty_document() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(reason))
	assert_eq(world.load_mission_data(null, "x.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_mission_data(NovaMissionData.new(), "x.bms"), ERR_INVALID_PARAMETER,
		"an unloaded document is rejected before any root resolution")
	assert_eq(failures.size(), 2, "both rejections explain themselves via load_failed")


func test_injected_root_bypasses_settings_mount() -> void:
	# The editor injects its own mounted root; the load must resolve through it
	# (and report ITS directory in errors) instead of mounting from settings.
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("injected_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	var injected := NovaResourceRoot.new()
	assert_eq(injected.set_root_dir(root_dir), OK)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_resource_root(injected)

	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(String(reason)))
	assert_eq(world.load_mission("missing.bms"), ERR_FILE_NOT_FOUND,
		"the missing file resolves against the injected root")
	assert_eq(failures.size(), 1)
	assert_string_contains(failures[0], root_dir.get_file(),
		"the error names the injected root's directory, proving no settings mount ran")


func _make_world() -> NovaWorld:
	var world := NovaWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	return world


func _write_fixture_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
