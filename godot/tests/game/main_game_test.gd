extends GutTest

# Runtime shell (main_game): the F9 "change game folder" hotkey may only summon the
# asset picker from the menu front-end and never while one is already open or while a
# mission is live. The native dialog can't be shown headless, so these gate the pure
# predicate that decides whether the hotkey acts (the hotkey just calls into it).

const MainGameScript := preload("res://game/main_game.gd")


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


func test_mission_text_effect_reaches_hud_objective() -> void:
	# Drained effects carry {kind, a..d, str} (NovaSimulation::drain_effects); the
	# WAC text/consol family lands as kind=="text" with the string in "str". The
	# old handler read nonexistent "text"/"message" keys, so mission text never
	# reached the HUD.
	var game := _make()
	game.apply_mission_effects([
		{"kind": "dialog", "a": 3},
		{"kind": "text", "str": "Proceed to the beach"},
		{"kind": "text", "str": ""},
	])
	assert_eq(game.hud_objective_line(), "Proceed to the beach",
		"kind=='text' effect drives the HUD objective line; empty/other kinds ignored")
