class_name GameRuntimeRoot
extends Node

## Startup boundary for the optional ImGui-embedded game view. Debug windowed
## runs with imgui-godot keep MainGame in one always-updating SubViewport;
## release, headless, and missing-addon runs immediately restore MainGame as
## the SceneTree's direct current scene.

const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")

var _main_game: MainGame = null
var _game_container: SubViewportContainer = null
var _game_viewport: SubViewport = null
var _embedded_game_view := false
var _tools_open := false


static func should_embed_game_view(
		debug_build: bool, display_name: String, imgui_available: bool) -> bool:
	return debug_build and display_name != "headless" and imgui_available


## The headless display server (--headless): no window, no capture, no
## splash; the one place the display-server name is compared.
static func is_headless() -> bool:
	return DisplayServer.get_name() == "headless"


func _ready() -> void:
	if _can_embed_game_view():
		_start_embedded_game()
	else:
		_start_direct_game.call_deferred()


func _exit_tree() -> void:
	if _embedded_game_view and is_instance_valid(_main_game):
		_main_game.get_dev_tools().set_game_viewport(null)


func _input(event: InputEvent) -> void:
	if not _embedded_game_view or not is_instance_valid(_main_game) \
			or not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return
	var tools := _main_game.get_dev_tools()
	var handled := false
	if key.keycode == KEY_F3:
		handled = tools.handle_tools_toggle()
	elif key.keycode == KEY_ESCAPE:
		handled = tools.handle_game_escape()
	if handled:
		get_viewport().set_input_as_handled()


func _process(_delta: float) -> void:
	if not _embedded_game_view or not is_instance_valid(_game_container) or _tools_open:
		return
	# The window, never the container: an unstretched SubViewportContainer's
	# minimum size is its viewport, so reading the container back would pin
	# the viewport at its largest size after a shrink (F11 back to windowed).
	_resize_game_viewport(get_window().size)


func get_main_game() -> MainGame:
	return _main_game


func is_game_view_embedded() -> bool:
	return _embedded_game_view


func _can_embed_game_view() -> bool:
	return should_embed_game_view(
			OS.is_debug_build(), DisplayServer.get_name(), Engine.has_singleton("ImGuiGD"))


func _start_embedded_game() -> void:
	_embedded_game_view = true
	_game_container = SubViewportContainer.new()
	_game_container.name = "GameViewportContainer"
	_game_container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_game_container.stretch = false
	_game_container.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_game_container)

	_game_viewport = SubViewport.new()
	_game_viewport.name = "GameViewport"
	_game_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_game_viewport.handle_input_locally = true
	_game_viewport.audio_listener_enable_2d = true
	_game_viewport.audio_listener_enable_3d = true
	_game_viewport.gui_disable_input = false
	_resize_game_viewport(get_window().size)
	_game_container.add_child(_game_viewport)

	_main_game = MAIN_GAME_SCENE.instantiate() as MainGame
	var tools := _main_game.get_dev_tools()
	tools.set_game_viewport(_game_viewport)
	tools.open_changed.connect(_on_tools_open_changed)
	_game_viewport.add_child(_main_game)
	_on_tools_open_changed(tools.is_open())


func _start_direct_game() -> void:
	# Use SceneTree's normal replacement path so the temporary decision scene
	# cannot outlive or overlap the direct MainGame ownership at shutdown.
	var result := get_tree().change_scene_to_packed(MAIN_GAME_SCENE)
	if result != OK:
		push_error("GameRuntimeRoot: direct MainGame handoff failed (%d)" % result)


func _on_tools_open_changed(open: bool) -> void:
	if not is_instance_valid(_game_container):
		return
	_tools_open = open
	# visible=false also suppresses SubViewport redraws through this ancestor.
	# A zero canvas layer removes only the direct composite, while UPDATE_ALWAYS
	# keeps the same texture alive for imgui-godot.
	_game_container.visibility_layer = 0 if open else 1
	_game_container.mouse_filter = (
			Control.MOUSE_FILTER_IGNORE if open else Control.MOUSE_FILTER_STOP)
	if not open:
		_resize_game_viewport(get_window().size)


func _resize_game_viewport(requested_size: Vector2i) -> void:
	if not is_instance_valid(_game_viewport):
		return
	var size := Vector2i(maxi(1, requested_size.x), maxi(1, requested_size.y))
	if _game_viewport.size != size:
		_game_viewport.size = size
