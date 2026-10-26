extends GutTest

const RUNTIME_ROOT_PATH := "res://game/game_runtime_root.tscn"
const RUNTIME_SCRIPT_PATH := "res://game/game_runtime_root.gd"


func test_startup_uses_the_runtime_root() -> void:
	assert_eq(ProjectSettings.get_setting("application/run/main_scene"), RUNTIME_ROOT_PATH)
	assert_true(ResourceLoader.exists(RUNTIME_ROOT_PATH))
	assert_true(ResourceLoader.exists(RUNTIME_SCRIPT_PATH))
	if not ResourceLoader.exists(RUNTIME_ROOT_PATH):
		return
	var packed := load(RUNTIME_ROOT_PATH) as PackedScene
	assert_not_null(packed)
	if packed == null:
		return
	var node := packed.instantiate()
	var runtime_root := node as GameRuntimeRoot
	assert_not_null(runtime_root, "the main scene's root is the GameRuntimeRoot")
	if runtime_root != null:
		assert_null(runtime_root.get_main_game(), "the game is created only after tree entry")
	node.free()


func test_embedding_decision_preserves_direct_runtime_fallbacks() -> void:
	var script = load(RUNTIME_SCRIPT_PATH)
	assert_not_null(script)
	if script == null:
		return
	assert_true(script.should_embed_game_view(true, "windows", true))
	assert_false(script.should_embed_game_view(false, "windows", true),
			"release runs pay no permanent SubViewport composite cost")
	assert_false(script.should_embed_game_view(true, "headless", true),
			"headless probes keep their direct MainGame arrangement")
	assert_false(script.should_embed_game_view(true, "windows", false),
			"a missing ImGui addon never blocks normal startup")


func test_the_game_start_reads_game_cfg_after_num_lock_and_ends_on_mpreset() -> void:
	# GameRuntimeRoot.begin_game is the start's one home (D-MNU-76): Num Lock's
	# start, then the start's read of game.cfg in the working directory, a set
	# mpreset answering false (the caller quits with 0) and a clear one true.
	var dir := TestFs.cache_dir(self, "begin_game_cfg")
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", dir]))
	var rows := [
		["version = 29\r\nmpreset = 1\r\n", false],
		["version = 29\r\nmpreset = 0\r\n", true],
	]
	for row in rows:
		TestFs.write_text(self, dir.path_join("game.cfg"), row[0])
		GameCfgStore.reset()
		assert_eq(GameRuntimeRoot.begin_game(), row[1],
				"mpreset %s" % ("clear" if row[1] else "set"))
		assert_eq(FileAccess.get_file_as_string(dir.path_join("game.cfg")), row[0],
				"the start's read writes nothing")
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(dir)


func test_bootstrap_requires_a_complete_packed_boot_set() -> void:
	var dir := TestFs.cache_dir(self, "bootstrap_content")
	assert_false(GameRuntimeRoot.has_content(dir), "a minimal download contains no game data yet")
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [])
	assert_false(GameRuntimeRoot.has_content(dir), "one partial archive does not complete installation")
	WorldFixture.stage_shell_archives(self, dir, false)
	assert_true(GameRuntimeRoot.has_content(dir), "installed boot resources permit offline play")
	TestFs.remove_dir_recursive(dir)


func test_bootstrap_accepts_an_explicitly_permitted_loose_boot_set() -> void:
	var dir := TestFs.cache_dir(self, "bootstrap_loose_content")
	var names := WorldFixture.SHELL_LANGUAGE_FILES + WorldFixture.SHELL_LOCALRES_FILES \
			+ WorldFixture.SHELL_RESOURCE_FILES
	for entry in WorldFixture.shell_archive_entries(self, names):
		var file := FileAccess.open(dir.path_join(entry.name), FileAccess.WRITE)
		assert_not_null(file)
		file.store_buffer(entry.bytes)
		file.close()
	assert_false(GameRuntimeRoot.has_content(dir), "loose content is not a packed installation")
	assert_false(GameRuntimeRoot.has_content(dir, true), "/d alone still requires an archive")
	assert_true(GameRuntimeRoot.has_content(dir, true, true), "--loose-root /d permits authored content")
	TestFs.remove_dir_recursive(dir)
