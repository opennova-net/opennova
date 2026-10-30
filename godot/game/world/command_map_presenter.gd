class_name CommandMapPresenter
extends Node

## The commander map's device half: cmap.mnu's CMAP screen over live play. The
## commander_menu key (controls catalog row 53, default V) opens it through the
## HUD toggles poll, which owns the key's gates and its respawn init (engine
## hud_toggles.h kCommandMapOpened). The screen is the engine's
## CommandMapController (menu/command_map_controller.h: every control the code
## registers by name, the show's order, the tab gates, the delete button's
## binding and the waypoint dialog), over the tables' runner (CommandMapScreen,
## menu/command_map_screen.h) and the map window. This node hosts the windowed
## map view on the MAP / ORDERS_MAP controls (ONE MapViewWindow in its command
## kind, placed over whichever of the two is on screen, since both share one view
## state: the process-lifetime MapViewState, like the retail globals) and the
## CHAT_MSGS draw slot, pumps the frame and closes it when the controller asks.
## Witness record hud-re D-HUD-19.

const MENU_FILE := "cmap.mnu"
const MENU_SCREEN := "CMAP"
# The two custom-draw map controls (the MAP tab and the ORDERS tab's map).
const MAP_WIDGETS: Array[String] = ["MAP", "ORDERS_MAP"]
const DIALOG_WIDGET := "WAYPOINTNAME_DLG"
# The ORDERS tab's console-message slot (a custom-appearance window).
const CHAT_MSGS_WIDGET := "CHAT_MSGS"

signal opened
signal closed

var _view: WorldView = null
var _ui_parent: Node = null
var _layout_control: Control = null
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _map_window: MapViewWindow = null
var _screen: CommandMapScreen = null
var _controller: CommandMapController = null
var _hud_source: Callable = Callable()
# The views outlive the per-mission menu rebuild, like the retail globals.
var _map_view_state := MapViewState.new()


func setup(view: WorldView, ui_parent: Node) -> void:
	_view = view
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


## Build + wire the presenter under `parent` (the shell's seam): `on_opened` /
## `on_closed` report the screen's cursor ownership.
static func install(parent: Node, view: WorldView, ui_parent: Node,
		on_opened: Callable, on_closed: Callable) -> CommandMapPresenter:
	var presenter := CommandMapPresenter.new()
	presenter.name = "CommandMapPresenter"
	parent.add_child(presenter)
	presenter.setup(view, ui_parent)
	presenter.opened.connect(func() -> void: on_opened.call())
	presenter.closed.connect(func() -> void: on_closed.call())
	return presenter


## The HUD overlay getter the map draws through (the HUD presenter's get_game_hud).
func set_hud_source(source: Callable) -> void:
	_hud_source = source


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## ADR 0018 read seams: the live driver and the map host (null before the menu
## builds, or when the screen authors neither map control).
func get_menu_driver() -> MenuDriver:
	return _driver


func get_map_window() -> MapViewWindow:
	return _map_window if _map_window != null and is_instance_valid(_map_window) else null


func get_map_view_state() -> MapViewState:
	return _map_view_state


## The screen's controller (null before the menu builds).
func get_controller() -> CommandMapController:
	return _controller


## The opened leg of the commander_menu action (the poll already ran its gates
## and the respawn init): build the CMAP screen over live play.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null or not _ensure_menu():
		return false
	var map_window := get_map_window()
	if map_window != null:
		var hud: HudOverlay = _hud_source.call() if _hud_source.is_valid() else null
		map_window.set_hud_overlay(hud)
		map_window.set_simulation(sim)
		map_window.screen_load()
	_screen.set_simulation(sim)
	# The show: the controller's (engine CommandMapController::open).
	_controller.setup(_driver, _view, _screen, map_window,
			Strings.get_table(Strings.TABLE_GAMETEXT), _hud_source)
	_driver.claim_screen(_controller)
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	_place_map_window()
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	set_process(false)
	if _driver != null:
		_driver.release_screen()
	if MenuFrameSurface.hide_frame(_frame):
		closed.emit()


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	# The session's end empties the order store the next session starts from.
	if _screen != null:
		_screen.clear_orders()
	_frame = null
	_audio = null
	_driver = null
	_map_window = null
	_screen = null
	_controller = null


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The menu's pump, once a frame after the input events (MenuFrameSurface).
	_driver.pump_mouse()
	_driver.tick(_view.frame_clock_ms)
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null:
		close()
		return
	_place_map_window()
	# The delete button's placement and the tables' refresh (the recruit holds run
	# on the frame's GetTickCount sample).
	_controller.frame()
	_draw_chat_msgs()


func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "CommandMapMenu", "CommandMapPresenter",
			_on_frame_gui_input, Callable())
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_screen = CommandMapScreen.new()
	_screen.setup(_driver, _map_view_state, Strings.get_table(Strings.TABLE_GAMETEXT),
			Strings.get_table(Strings.TABLE_GAMEUI))
	_screen.install_painters()
	_screen.seed_current_orders()
	var chat_msgs := _driver.frame_index(_driver.widget_id(CHAT_MSGS_WIDGET))
	_frame.set_custom_slot_widget(chat_msgs)
	for widget_name in MAP_WIDGETS:
		if _driver.widget_id(widget_name) >= 0:
			_map_window = MapViewWindow.new()
			_map_window.name = "CommandMapWindow"
			_map_window.set_view_kind(MapViewWindow.VIEW_COMMAND)
			_map_window.set_view_state(_map_view_state)
			_frame.add_child(_map_window)
			break
	_controller = CommandMapController.new()
	_controller.close_requested.connect(close)
	if _map_window != null:
		_map_window.waypoint_dialog_requested.connect(_controller.waypoint_dialog_requested)
	return true


# The one map host follows whichever map control is effectively shown (the
# MAP tab or the ORDERS tab): its frame rect, its authored design rect. The
# widgets after it paint above it (the name dialog, the delete button), and
# while the modal name dialog is up the map takes no mouse.
func _place_map_window() -> void:
	var map_window := get_map_window()
	if map_window == null or _driver == null:
		return
	map_window.visible = false
	var dialog_id := _driver.widget_id(DIALOG_WIDGET)
	var modal := dialog_id >= 0 and _driver.is_widget_shown(dialog_id)
	map_window.mouse_filter = Control.MOUSE_FILTER_IGNORE if modal else Control.MOUSE_FILTER_PASS
	for widget_name in MAP_WIDGETS:
		var id := _driver.widget_id(widget_name)
		var index := _driver.frame_index(id) if id >= 0 else -1
		if index < 0 or not _frame.is_widget_shown(index):
			continue
		var rect := _driver.widget_frame_rect(id)
		map_window.position = rect.position
		map_window.size = rect.size
		map_window.set_widget_design_rect(Rect2i(_frame.widget_rect(index)))
		map_window.visible = rect.size.x > 0.0 and rect.size.y > 0.0
		_frame.set_mount_widget(index if map_window.visible else -1)
		return
	_frame.set_mount_widget(-1)


# CHAT_MSGS: the HUD's console messages drawn into the frame's custom-draw
# slot item (between the ORDERS tab's earlier and later widgets); the slot
# item shows only while the tab is up.
func _draw_chat_msgs() -> void:
	if _frame.get_custom_slot_widget() < 0:
		return
	var hud: HudOverlay = _hud_source.call() if _hud_source.is_valid() else null
	if hud != null:
		hud.render_console_messages(_frame.get_custom_slot_canvas_item())


# The screen's authored hotkeys (the OK button's Escape, ZOOMIN's `=`) run
# through the driver's hotkey table while the screen is up.
func _unhandled_key_input(event: InputEvent) -> void:
	if not is_open() or _driver == null:
		return
	var key := event as InputEventKey
	if key == null or not key.pressed or key.echo:
		return
	if _driver.handle_key_input(key):
		get_viewport().set_input_as_handled()


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)


func _recompute_fit() -> void:
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
	_place_map_window()
