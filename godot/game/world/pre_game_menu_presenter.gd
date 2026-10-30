class_name PreGameMenuPresenter
extends Node

## The join screen's device half: a join runs on its own screen (pre.mnu
## PRE_GAME_MENU) until the host starts the game, not under the loading screen.
## The screen is the engine's PreGameController (inmatch/pre_game_controller.h
## over inmatch/pre_game_menu.h): the status line in MESSAGES, the failure's
## reason with only Cancel, the choice panels (GAME_PASSWORD, SPECTATE, the
## shared TEAM_PASSWORD) that ACCEPT walks and BACK abandons, and the game
## start's handoff. This node mounts the frame on its own layer, pumps it, hands
## the controller the followed join's status each frame, and reports Cancel, the
## committed choice and the start.

const MENU_FILE := "pre.mnu"
const MENU_SCREEN := "PRE_GAME_MENU"
const MESSAGES := "MESSAGES"

## The player pressed Cancel or BACK (or ESC): the join is abandoned.
signal cancelled
## The choice's last ACCEPT: the three controls as the commit reads them
## (JoinTarget.apply_pre_game_choice routes them).
signal choice_committed(game_password: String, spectate: bool, team_password: String)
## The host started the game: the loading screen takes over.
signal game_starting

var _layer: CanvasLayer = null
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
# The join's status source while the controller follows it.
var _world: GameWorld = null
var _controller: PreGameController = null


## Open the screen over `owner` (its own CanvasLayer), reading pre.mnu from
## `root`. False when the menu cannot be built.
func open(root: ResourceRoot, owner: Node) -> bool:
	if is_open():
		return true
	if _layer == null:
		_layer = CanvasLayer.new()
		_layer.name = "PreGameMenuLayer"
		_layer.layer = 3
		owner.add_child(_layer)
	var surface := MenuFrameSurface.open_surface(root, _layer, null, MENU_FILE, MENU_SCREEN,
			"PreGameMenu", "PreGameMenuPresenter", _on_frame_gui_input,
			func(_built: MenuDriver) -> void: _register_text_tables(root))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_controller = PreGameController.new()
	_controller.setup(_driver)
	_controller.cancel_requested.connect(cancel)
	_controller.choice_committed.connect(choice_committed.emit)
	# The open: the progress panel, an empty status line.
	_driver.claim_screen(_controller)
	# The menu's pump runs every frame the screen is up (_process).
	set_process(true)
	return true


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## Follow `world`'s join preload: each frame writes its status line, and the
## game start hands over (game_starting) after showing for one frame.
func follow(world: GameWorld) -> void:
	_world = world
	if _controller != null:
		_controller.follow(world != null)


## The join's progress: the status line with Cancel.
func show_progress(text: String) -> void:
	if _controller != null:
		_controller.show_progress(text)


## A failed join: the reason with Cancel only, until the player leaves.
func show_failure(text: String) -> void:
	_world = null
	if _controller != null:
		_controller.show_failure(text)


## The join machine's choice: the first panel the session's flag word asks for
## (engine pre_game_entry_panel carries the witness). False when the word offers
## no choice.
func show_choice(server_flags: int) -> bool:
	if _controller == null or not _controller.show_choice(server_flags):
		return false
	_world = null
	return true


## ACCEPT on the showing panel: the next panel, else the commit, which returns the
## screen to its progress panel and reports the three controls (engine
## pre_game_accept carries the witness).
func accept() -> void:
	if _controller != null:
		_controller.accept()


## The Cancel button's action (ESC included).
func cancel() -> void:
	if is_open():
		cancelled.emit()


func close() -> void:
	set_process(false)
	_world = null
	if _driver != null:
		_driver.release_screen()
	if _frame != null and is_instance_valid(_frame):
		# The menu's pointer is the menu's alone (MenuFrameSurface.hide_frame).
		Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_controller = null


## The panel showing (JoinScreenStatus.PANEL_*).
func panel() -> int:
	return _controller.panel() if _controller != null else JoinScreenStatus.PANEL_PROGRESS


## The screen's menu driver (the tests drive its controls; null while closed).
func get_menu_driver() -> MenuDriver:
	return _driver


## The MESSAGES box's text (the test and probe read).
func message_text() -> String:
	if _driver == null:
		return ""
	var id := _driver.widget_id(MESSAGES)
	return String(_driver.get_widget_text(id)) if id >= 0 else ""


## Whether a pre.mnu window is showing (the test and probe read).
func is_window_shown(window_name: String) -> bool:
	if _driver == null:
		return false
	var id := _driver.widget_id(window_name)
	return id >= 0 and bool(_driver.is_widget_shown(id))


func _process(_delta: float) -> void:
	if not is_open():
		return
	# The menu's pump, once a frame after the input events, whatever the join
	# stands at (MenuFrameSurface; the join screen's mode pumps its menu at the
	# head of every update, engine/runtime/menu/menu_runtime.h). A click's
	# ACCEPT or BACK can close the screen inside the pump, which drops _driver:
	# the local keeps the driver alive until the pump returns.
	var driver := _driver
	if driver != null:
		driver.pump_mouse()
	var controller := _controller
	if _world == null or controller == null or not controller.is_following():
		return
	var status: JoinScreenStatus = _world.get_join_screen_status()
	var text := ""
	var stage: int = JoinScreenStatus.STAGE_JOINING
	if status != null:
		text = status.message_text(Strings.get_table(Strings.TABLE_GAMEERR))
		stage = status.get_stage()
	if controller.follow_frame(status != null, stage, text) == PreGameController.FOLLOW_HAND_OFF:
		_world = null
		game_starting.emit()


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)


# The password edits and the screen's authored hotkeys use the same keyboard
# path as MenuShell; the passive frame only forwards mouse events. The menu the
# screen opened over takes no keys while it is up (MenuShell.keyboard_covered).
# ACCEPT's Return and BACK's Escape can close the screen inside the call, as a
# click can in the pump: the local keeps the driver alive until it returns.
func _unhandled_key_input(event: InputEvent) -> void:
	var driver := _driver
	if driver == null or not is_open():
		return
	if event is InputEventKey and driver.handle_key_input(event as InputEventKey):
		get_viewport().set_input_as_handled()


func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMEERR, "gameerr.bin"]]:
		if Strings.get_table(spec[0]) != null:
			continue
		var loaded := Strings.load_rtxt(root, spec[1])
		if loaded != null:
			Strings.register_table(spec[0], loaded)
