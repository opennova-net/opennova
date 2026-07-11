extends GutTest

# Runtime shell (main_game): the F9 "change game folder" hotkey may only summon the
# asset picker from the menu front-end and never while one is already open or while a
# mission is live. The native dialog can't be shown headless, so these gate the pure
# predicate that decides whether the hotkey acts (the hotkey just calls into it).

const MainGameScript := preload("res://game/main_game.gd")


class FakeGameHud:
	extends RefCounted
	var crosshair_style := -1

	func set_crosshair_style(style: int) -> void:
		crosshair_style = style


func before_each() -> void:
	NovaStrings.clear()


func after_each() -> void:
	NovaStrings.clear()


func _make() -> Node:
	# Not added to the tree on purpose: the predicate only reads _state/_picker, and
	# staying out of the tree keeps _ready/@onready (which need the full scene) from running.
	var game = MainGameScript.new()
	autofree(game)
	return game


func test_can_summon_in_menu_state() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	game._picker = null
	assert_true(game._can_summon_dir_picker(), "picker summonable from the menu front-end")


func test_cannot_summon_during_mission() -> void:
	var game := _make()
	game._picker = null
	game._state = MainGameScript.State.WORLD
	assert_false(game._can_summon_dir_picker(), "not summonable while a world is live")
	game._state = MainGameScript.State.PAUSED
	assert_false(game._can_summon_dir_picker(), "not summonable from the pause overlay")


func test_cannot_summon_while_picker_open() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	var picker := FileDialog.new()
	game._picker = picker
	assert_false(game._can_summon_dir_picker(), "no second picker while one is already open")
	picker.queue_free()


func test_crosshair_option_updates_an_existing_hud() -> void:
	var game := _make()
	var hud := FakeGameHud.new()
	game._game_hud = hud
	game._on_crosshair_style_changed(13)
	assert_eq(hud.crosshair_style, 13, "A paused game's HUD adopts the menu selection immediately.")


func test_hud_loads_text_for_the_mission_that_actually_started() -> void:
	var root := NovaResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	assert_eq(root.set_root_dir(fixture_dir), OK)

	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_resource_root(root)
	assert_eq(world.mission_file, "", "normal SP/host/join starts do not populate the exported boot option")
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_eq(world.mission_file, "", "the exported boot option remains separate after a normal load")

	var game := _make()
	game._world = world
	game._load_hud_text_tables(root)

	assert_not_null(NovaStrings.get_table("mission"),
		"mnml.bin exists and must be selected from the successfully loaded BMS; medmssn.bin is absent")
