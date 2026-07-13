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


func test_oned_handoff_never_falls_back_to_the_game_folder_picker() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	game._launch_flags = NovaLaunchFlags.parse(PackedStringArray([
		"/d", "--oned-resource-root", "C:/ONED loose root",
	]))

	assert_false(game._can_summon_dir_picker(),
		"an explicit ONED root owns the launch, including its failure path")


func test_installing_session_injects_the_same_root_into_game_world() -> void:
	var game := _make()
	var world := GameWorld.new()
	autofree(world)
	game._world = world
	var session := NovaRuntimeResourceSession.new()
	var root := NovaResourceRoot.new()
	session._root = root

	game._install_resource_session(session)

	assert_same(game._root, root)
	assert_same(world._injected_root, root,
		"menu host and world must never resolve separate launch roots")


func test_mission_text_effect_reaches_hud_objective() -> void:
	# Drained effects carry {kind, a..d, str} (NovaSimulation::drain_effects); the
	# WAC text/ptext family lands as kind=="text" with the string in "str". The
	# old handler read nonexistent "text"/"message" keys, so mission text never
	# reached the HUD.
	# The surface lives on the shared NovaGameHudHost (main_game passes through);
	# out-of-tree _make() never runs _ready, so drive the host directly.
	var host := NovaGameHudHost.new()
	autofree(host)
	host.apply_mission_effects([
		{"kind": "dialog", "a": 3},
		{"kind": "text", "str": "Proceed to the beach"},
		{"kind": "text", "str": ""},
	])
	assert_eq(host.hud_objective_line(), "Proceed to the beach",
		"kind=='text' effect drives the HUD objective line; empty/other kinds ignored")


func test_console_debug_text_does_not_reach_hud_objective() -> void:
	# consol/pconsol ride the distinct debug_text channel. The game does not yet
	# present an on-screen debug console, so these effects remain intentionally
	# unrouted instead of replacing player-facing mission text.
	var host := NovaGameHudHost.new()
	autofree(host)
	host.apply_mission_effects([
		{"kind": "text", "str": "Hold this position"},
		{"kind": "debug_text", "str": "trigger 17 entered"},
	])
	assert_eq(host.hud_objective_line(), "Hold this position",
		"debug_text stays off the player-facing HUD mission-text channel")


func test_crosshair_option_updates_an_existing_hud() -> void:
	# The Options signal reaches the built HUD through the shared host's public
	# set_crosshair_style (main_game delegates its _on_crosshair_style_changed there).
	var host := NovaGameHudHost.new()
	autofree(host)
	var hud := FakeGameHud.new()
	host._game_hud = hud
	host.set_crosshair_style(13)
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

	# The mission string table selection lives on the shared HUD host now (the
	# exists-only mission-bin fallback rides its world wiring).
	var host := NovaGameHudHost.new()
	autofree(host)
	host.setup(world, null, null)
	host._load_hud_text_tables(root)

	assert_not_null(NovaStrings.get_table("mission"),
		"mnml.bin exists and must be selected from the successfully loaded BMS; medmssn.bin is absent")
