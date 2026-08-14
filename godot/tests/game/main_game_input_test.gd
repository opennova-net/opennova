extends GutTest


class MainGameInputHarness:
	extends MainGame

	func _ready() -> void:
		pass

	func configure_input_targets(hud: GameHudPresenter,
			player: LocalPlayerPresenter) -> void:
		_state = State.WORLD
		_hud_presenter = hud
		_player_presenter = player

	func dispatch_key(event: InputEventKey) -> void:
		_unhandled_key_input(event)


class WorldInputHarness:
	extends GameWorld

	func _ready() -> void:
		pass

	func is_loaded() -> bool:
		return true


class CaptureHudPresenter:
	extends GameHudPresenter
	var friendly_tag_cycles := 0

	func cycle_friendly_tags() -> void:
		friendly_tag_cycles += 1


class CapturePlayerPresenter:
	extends LocalPlayerPresenter
	var handled_keys := PackedInt32Array()

	func handle_key_input(event: InputEvent, active: bool) -> bool:
		if active and event is InputEventKey:
			handled_keys.append(int((event as InputEventKey).keycode))
			return true
		return false


func _pressed_key(keycode: Key, shift_pressed := false) -> InputEventKey:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.pressed = true
	event.shift_pressed = shift_pressed
	return event


func _make_input_harness() -> MainGameInputHarness:
	var game := MainGameInputHarness.new()
	var world := WorldInputHarness.new()
	world.name = "World"
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	game.add_child(world)
	var camera := FlyCamera.new()
	camera.name = "Camera3D"
	game.add_child(camera)
	var hud_layer := CanvasLayer.new()
	hud_layer.name = "HUD"
	game.add_child(hud_layer)
	var menu_layer := CanvasLayer.new()
	menu_layer.name = "MenuLayer"
	var menu_shell := MenuShell.new()
	menu_shell.name = "MenuShell"
	menu_layer.add_child(menu_shell)
	game.add_child(menu_layer)
	add_child_autofree(game)
	return game


func test_friendly_tags_use_f_while_n_reaches_the_nvg_router() -> void:
	var game := _make_input_harness()
	var hud := CaptureHudPresenter.new()
	var player := CapturePlayerPresenter.new()
	game.add_child(hud)
	game.add_child(player)
	game.configure_input_targets(hud, player)

	game.dispatch_key(_pressed_key(KEY_F))
	assert_eq(hud.friendly_tag_cycles, 1, "F cycles the retail friendly-tag mode")
	assert_true(player.handled_keys.is_empty(), "the shell consumes the friendly-tag key")

	game.dispatch_key(_pressed_key(KEY_N))
	assert_eq(hud.friendly_tag_cycles, 1, "N does not cycle friendly tags")
	assert_eq(player.handled_keys, PackedInt32Array([KEY_N]),
			"N reaches the local-player router for NVG")


func test_h_is_not_a_hud_key_and_reaches_the_player_router() -> void:
	# Retail H is only the secondary `pause` binding (SP-only); there is no
	# HUD-visibility toggle and no H color mapping.
	# [orig: catalog row 70 vk2 0x48; case 25 @0x49b520]
	var game := _make_input_harness()
	var hud := CaptureHudPresenter.new()
	var player := CapturePlayerPresenter.new()
	game.add_child(hud)
	game.add_child(player)
	game.configure_input_targets(hud, player)

	game.dispatch_key(_pressed_key(KEY_H))
	assert_eq(hud.friendly_tag_cycles, 0, "H drives no HUD presenter action")
	assert_eq(player.handled_keys, PackedInt32Array([KEY_H]),
			"H falls through the shell to the player router")


func test_plain_f6_reaches_the_binding_rows_while_shift_f6_is_debug_pick() -> void:
	var game := _make_input_harness()
	var hud := CaptureHudPresenter.new()
	var player := CapturePlayerPresenter.new()
	game.add_child(hud)
	game.add_child(player)
	game.configure_input_targets(hud, player)

	game.dispatch_key(_pressed_key(KEY_F6))
	assert_eq(player.handled_keys, PackedInt32Array([KEY_F6]),
			"Plain F6 remains available to the gameplay HUDDETAIL router.")
	game.dispatch_key(_pressed_key(KEY_F6, true))
	assert_eq(player.handled_keys, PackedInt32Array([KEY_F6]),
			"Shift+F6 is consumed by the shell's debug picker.")
