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


func _pressed_key(keycode: Key) -> InputEventKey:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.pressed = true
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
