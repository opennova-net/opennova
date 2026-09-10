extends GutTest

const RUNTIME_ROOT_PATH := "res://game/game_runtime_root.tscn"
const RUNTIME_SCRIPT_PATH := "res://game/game_runtime_root.gd"


func test_startup_boots_through_the_runtime_root() -> void:
	assert_eq(ProjectSettings.get_setting("application/run/main_scene"), RUNTIME_ROOT_PATH)
	assert_true(ResourceLoader.exists(RUNTIME_ROOT_PATH))
	assert_true(ResourceLoader.exists(RUNTIME_SCRIPT_PATH))
	if not ResourceLoader.exists(RUNTIME_ROOT_PATH):
		return
	var packed := load(RUNTIME_ROOT_PATH) as PackedScene
	assert_not_null(packed)
	if packed == null:
		return
	var runtime_root := packed.instantiate()
	assert_true(runtime_root.has_method("get_main_game"),
			"launch automation has one stable way through the added nesting")
	assert_true(runtime_root.has_method("sync_game_viewport_to_window"),
			"blocking loading presents can synchronize the optional game viewport")
	assert_null(runtime_root.get_main_game(), "the game is created only after tree entry")
	runtime_root.free()


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
