class_name PreGameMenuPresenter
extends Node

## The join screen: a join runs on its own screen (pre.mnu PRE_GAME_MENU) until
## the host starts the game, not under the loading screen. The status line the
## join stands at fills the MESSAGES box; a failure parks the screen with the
## connection's reason text and only Cancel; the game start hands over to the
## loading screen. A discovered session whose flag word offers a choice opens
## the choice panels (GAME_PASSWORD, SPECTATE, the shared TEAM_PASSWORD) that
## ACCEPT walks and BACK abandons. The panel table, the walk and the status
## lines are the engine's (inmatch/pre_game_menu.h, read through
## JoinScreenStatus); this presenter only renders them and reports Cancel and
## the committed choice.

const MENU_FILE := "pre.mnu"
const MENU_SCREEN := "PRE_GAME_MENU"
const MESSAGES := "MESSAGES"
const ABORT_CANCEL := "ABORT_CANCEL"
const BACK := "BACK"
const ACCEPT := "ACCEPT"
const GAME_PASSWORD := "GAME_PASSWORD"
const SPECTATE := "SPECTATE"
const TEAM_PASSWORD := "TEAM_PASSWORD"

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
var _world: GameWorld = null
var _panel := JoinScreenStatus.PANEL_PROGRESS
# The discovered flag word the choice walks.
var _choice_flags := 0
# The game start's status line shows for one frame before the handoff.
var _starting_shown := false


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
			func(driver: MenuDriver) -> void:
				_register_text_tables(root)
				driver.widget_activated.connect(_on_widget_activated))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_starting_shown = false
	show_progress("")
	# The menu's pump runs every frame the screen is up (_process).
	set_process(true)
	return true


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## Follow `world`'s join preload: each frame writes its status line, and the
## game start hands over (game_starting) after showing for one frame.
func follow(world: GameWorld) -> void:
	_world = world


## The join's progress: the status line with Cancel.
func show_progress(text: String) -> void:
	_set_panel(JoinScreenStatus.PANEL_PROGRESS)
	_set_message(text)


## A failed join: the reason with Cancel only, until the player leaves.
func show_failure(text: String) -> void:
	_world = null
	_set_panel(JoinScreenStatus.PANEL_ERROR)
	_set_message(text)


## The join machine's choice: the first panel the session's flag word asks for
## (engine pre_game_entry_panel carries the witness). False when the word offers
## no choice.
func show_choice(server_flags: int) -> bool:
	var panel_state := JoinScreenStatus.entry_panel(server_flags, false)
	if panel_state == JoinScreenStatus.PANEL_PROGRESS:
		return false
	_world = null
	_choice_flags = server_flags
	_set_panel(panel_state)
	return true


## ACCEPT on the showing panel: the next panel, else the commit, which returns the
## screen to its progress panel and reports the three controls (engine
## pre_game_accept carries the witness).
func accept() -> void:
	var step := JoinScreenStatus.accept_step(_panel, _choice_flags, _is_checked(SPECTATE), false)
	if step == JoinScreenStatus.ACCEPT_NONE:
		return
	if step == JoinScreenStatus.ACCEPT_ABORT:
		cancel()
		return
	if step != JoinScreenStatus.ACCEPT_COMMIT:
		_set_panel(step)
		return
	var game_password := _widget_text(GAME_PASSWORD)
	var spectate := _is_checked(SPECTATE)
	var team_password := _widget_text(TEAM_PASSWORD)
	_set_panel(JoinScreenStatus.PANEL_PROGRESS)
	choice_committed.emit(game_password, spectate, team_password)


## The Cancel button's action (ESC included).
func cancel() -> void:
	if is_open():
		cancelled.emit()


func close() -> void:
	set_process(false)
	_world = null
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null


func panel() -> int:
	return _panel


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
	# head of every update, engine/runtime/menu/menu_runtime.h).
	if _driver != null:
		_driver.pump_mouse()
	if _world == null or _panel != JoinScreenStatus.PANEL_PROGRESS:
		return
	if _starting_shown:
		_world = null
		game_starting.emit()
		return
	var status: JoinScreenStatus = _world.get_join_screen_status()
	if status == null:
		return
	_set_message(status.message_text(Strings.get_override_table(),
			Strings.get_table(Strings.TABLE_GAMEERR)))
	_starting_shown = status.get_stage() == JoinScreenStatus.STAGE_STARTING


func _set_panel(panel_state: int) -> void:
	_panel = panel_state
	if _driver == null:
		return
	for i in JoinScreenStatus.window_count():
		var id := _driver.widget_id(JoinScreenStatus.window_name(i))
		if id >= 0:
			_driver.set_widget_shown(id, JoinScreenStatus.panel_shows_window(panel_state, i))


# The status line replaces the box's text; it never appends.
func _set_message(text: String) -> void:
	if _driver == null:
		return
	var id := _driver.widget_id(MESSAGES)
	if id >= 0:
		_driver.set_widget_text(id, text)


# ACCEPT walks the choice; BACK and ABORT_CANCEL both abandon the join (engine
# inmatch/pre_game_menu.h pre_game_accept carries the witness).
func _on_widget_activated(_id: int, widget_name: String) -> void:
	if widget_name.nocasecmp_to(ABORT_CANCEL) == 0 or widget_name.nocasecmp_to(BACK) == 0:
		cancel()
	elif widget_name.nocasecmp_to(ACCEPT) == 0:
		accept()


func _widget_text(widget_name: String) -> String:
	if _driver == null:
		return ""
	var id := _driver.widget_id(widget_name)
	return String(_driver.get_widget_text(id)) if id >= 0 else ""


func _is_checked(widget_name: String) -> bool:
	if _driver == null:
		return false
	var id := _driver.widget_id(widget_name)
	return id >= 0 and bool(_driver.is_widget_checked(id))


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)


func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMEERR, "gameerr.bin"]]:
		if Strings.get_table(spec[0]) != null:
			continue
		var loaded := Strings.load_rtxt(root, spec[1])
		if loaded != null:
			Strings.register_table(spec[0], loaded)
