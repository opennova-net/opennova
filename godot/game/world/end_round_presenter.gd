class_name EndRoundPresenter
extends Node

## The multiplayer end-of-round presentation (net-re §5.68), the device half:
## from the S2C 0x1D announcement the HUD overlay shows the text ladder while
## the deploy / armory screens are torn down; six seconds later, once the S2C
## 0x56 board has reassembled, stat.mnu's STAT screen opens ONCE. The ladder,
## the 6 s delay and the transition latch are the engine's
## (hud/end_round_overlay.h EndRoundTransition), and so is the STAT screen's
## controller (inmatch/end_round_screen.h, the EndRoundScreen binding: the
## table's fill, the tabs, the exit), claimed on this presenter's driver when
## the screen opens; this node owns the HUD element, the compiled stat.mnu
## frame and the cursor.

const MENU_FILE := "stat.mnu"
const MENU_SCREEN := "STAT"

signal opened
signal closed
# The witnessed CONFIRM_YES command exits the mission (the same close-screens
# + action-3 pair as the pause menu's CONFIRM_YES; docs/mnu/menu-re.md "The
# in-game exit confirmation"); the shell routes it to the return-to-menu
# teardown.
signal exit_to_menu_requested

var _view: WorldView = null
var _ui_parent: Node = null
var _layout_control: Control = null
var _hud_presenter: GameHudPresenter = null
var _deploy_presenter: DeployScreenPresenter = null
var _armory_presenter: ArmoryPresenter = null
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
# The engine's transition latch (the once-only bytes + the 6 s gate) and the
# STAT screen's controller.
var _transition := EndRoundTransition.new()
var _screen := EndRoundScreen.new()
var _overlay_shown := false


func _init() -> void:
	_screen.close_requested.connect(close)
	_screen.exit_to_menu_requested.connect(exit_to_menu_requested.emit)


func setup(view: WorldView, ui_parent: Node, hud_presenter: GameHudPresenter) -> void:
	_view = view
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	_hud_presenter = hud_presenter


## The shell seam: the announcement edge closes the deploy/armory screens (the
## UI scene teardown every transition pass runs; inmatch/end_round_screen.h);
## `opened`/`closed` report the STAT screen's cursor ownership through the two
## callables (the overlay phase is not a UI state: the world keeps the cursor).
func connect_shell(deploy_presenter: DeployScreenPresenter, armory_presenter,
		on_opened: Callable, on_closed: Callable) -> void:
	_deploy_presenter = deploy_presenter
	_armory_presenter = armory_presenter
	opened.connect(func() -> void: on_opened.call())
	closed.connect(func() -> void: on_closed.call())


## Build + wire the presenter under `parent` in one call (the shell's seam).
static func install(parent: Node, view: WorldView, ui_parent: Node,
		hud_presenter: GameHudPresenter, deploy_presenter: DeployScreenPresenter,
		armory_presenter, on_opened: Callable, on_closed: Callable) -> EndRoundPresenter:
	var presenter := EndRoundPresenter.new()
	presenter.name = "EndRoundPresenter"
	parent.add_child(presenter)
	presenter.setup(view, ui_parent, hud_presenter)
	presenter.connect_shell(deploy_presenter, armory_presenter, on_opened, on_closed)
	return presenter


# The UI scene teardown at the announcement (inmatch/end_round_screen.h).
func _tear_down_screens() -> void:
	if _deploy_presenter != null and _deploy_presenter.is_open():
		_deploy_presenter.close()
	if _armory_presenter != null and _armory_presenter.is_open():
		_armory_presenter.close()


func _hud() -> HudOverlay:
	return _hud_presenter.get_game_hud() if _hud_presenter != null else null


func _gametext() -> RtxtStringFile:
	return Strings.get_table(Strings.TABLE_GAMETEXT)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


func is_overlay_shown() -> bool:
	return _overlay_shown


## STAT's menu input pass-through word: its show raises it and only its
## deactivate clears it; no stat.mnu callback touches it (the exit
## confirmation's HIDDEN_BACK and CONFIRM_NO carry their authored actions
## alone), so the update runs the input frame under the whole screen (the
## shell's key ring and frame sample; engine world::pass_through_close_axis
## carries the witness).
func is_input_pass_through() -> bool:
	return is_open()


func _unhandled_key_input(event: InputEvent) -> void:
	if handle_key(event as InputEventKey):
		get_viewport().set_input_as_handled()


## One key-down over the open screen: the menu's keyboard hook runs stat.mnu's
## hotkeys at once (HIDDEN_BACK's confirmation, its NO and YES); the key itself
## waits in the shell's key ring for the input frame (KeyboardPass; engine
## world::pass_through_close_axis carries the key witness). True only when the
## hotkey closed the screen (the confirmation's YES), whose close resets the
## ring.
func handle_key(key: InputEventKey) -> bool:
	if not is_open() or key == null or not key.pressed or key.echo:
		return false
	if _driver == null or not _driver.handle_key_input(key):
		return false
	return not is_open()


func get_menu_driver() -> MenuDriver:
	return _driver


## Mission (re)start clears the once-only latches (the engine's
## EndRoundTransition.reset) and the device state behind them.
func reset() -> void:
	_transition.reset()
	_hide_overlay()
	if is_open():
		_driver.release_screen()
		_frame.visible = false


## One HUD frame: the engine's latch steps over the sim's header / board
## knowledge and the wall clock; this leg performs the device work its
## verdict names — the announcement edge shows the overlay (and tears the
## deploy/armory screens down every PRE-STAT pass), the 6 s + board gate opens
## STAT once, and once STAT has latched nothing from this path redraws.
func tick() -> void:
	if _view == null:
		return
	var sim: Simulation = _view.sim()
	if sim == null:
		return
	var state: EndRoundState = sim.get_end_round_state()
	var step := _transition.step(state.is_header_known(), state.is_board_known(),
			_view.frame_clock_ms)
	if step & EndRoundTransition.STEP_RESET:
		reset()
		return
	if step & EndRoundTransition.STEP_PRE_STAT:
		_tear_down_screens()
		_apply_overlay(sim)
		if step & EndRoundTransition.STEP_OPEN_STAT:
			_tear_down_screens()
			_open_stat_screen(sim)
	elif is_open():
		_driver.tick(_view.frame_clock_ms)


func _process(_delta: float) -> void:
	if is_open() and _driver != null:
		# The menu's pump, once a frame after the input events (MenuFrameSurface).
		_driver.pump_mouse()
		_driver.tick(_view.frame_clock_ms)


# The resolved overlay ladder (the engine's end_round_overlay_resolve over the
# gametext Overlays table) handed to the HUD element as text + y pairs.
func _apply_overlay(sim: Simulation) -> void:
	var hud := _hud()
	if hud == null:
		return
	var overlay := sim.get_end_round_overlay(_gametext())
	hud.set_end_round_overlay(true, overlay.top, overlay.bottom, overlay.texts, overlay.ys)
	_overlay_shown = true


func _hide_overlay() -> void:
	var hud := _hud()
	if hud != null and _overlay_shown:
		var sim: Simulation = _view.sim() if _view != null else null
		var overlay: EndRoundOverlay = (
				sim.get_end_round_overlay(null) if sim != null else EndRoundOverlay.new())
		hud.set_end_round_overlay(false, overlay.top, overlay.bottom,
				PackedStringArray(), PackedInt32Array())
	_overlay_shown = false


func _open_stat_screen(sim: Simulation) -> void:
	_hide_overlay()
	if not _ensure_menu():
		return
	# The STAT show (the engine controller's open): the RESULTLIST filled, the
	# tabs by the game type.
	_screen.set_simulation(sim)
	_screen.set_gametext(_gametext())
	_driver.claim_screen(_screen)
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	set_process(true)
	opened.emit()


func close() -> void:
	set_process(false)
	if _driver != null:
		_driver.release_screen()
	if MenuFrameSurface.hide_frame(_frame):
		closed.emit()


func teardown() -> void:
	set_process(false)
	_hide_overlay()
	if _driver != null:
		_driver.release_screen()
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_transition.reset()


func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var surface := MenuFrameSurface.open_surface(_view.resource_root(), _ui_parent,
			_layout_control, MENU_FILE, MENU_SCREEN, "EndRoundMenu", "EndRoundPresenter",
			_on_frame_gui_input, Callable())
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	return true


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)
