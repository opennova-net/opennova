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
