class_name CommandMapPresenter
extends Node

## The commander map: cmap.mnu's CMAP screen over live play. The commander_menu
## key (controls catalog row 53, default V) opens it through the HUD toggles poll,
## which owns the key's gates and its respawn init (engine hud_toggles.h
## kCommandMapOpened). This presenter hosts the windowed map view on the MAP /
## ORDERS_MAP controls — ONE MapViewWindow in its command kind, placed over
## whichever of the two is on screen, since both share one view state — plus the
## zoom buttons, the GRID / TEXT / WAYPOINTS / CREATE_WAYPOINTS toggles (each with
## its ORDERS_ twin) and the OK close. The witnessed model and the mode-4 compile
## are the engine's (hud/hud_map_view.h CommandMapView); witness record hud-re
## D-HUD-19. The rest of the screen (the team / orders / players / rules tabs'
## content, the chat panel, the waypoint-name dialog) is not ported yet: those
## controls show their authored chrome only.

const MENU_FILE := "cmap.mnu"
const MENU_SCREEN := "CMAP"
# The two custom-draw map controls (the MAP tab and the ORDERS tab's map).
const MAP_WIDGETS: Array[String] = ["MAP", "ORDERS_MAP"]
const ZOOM_IN_WIDGETS: Array[String] = ["ZOOMIN", "ORDERS_ZOOMIN"]
const ZOOM_OUT_WIDGETS: Array[String] = ["ZOOMOUT", "ORDERS_ZOOMOUT"]
# The draw toggles and their twins, each paired with the window's toggle
# index at the same position.
const TOGGLE_WIDGETS: Array[String] = ["GRID", "ORDERS_GRID", "TEXT", "ORDERS_TEXT",
		"WAYPOINTS", "ORDERS_WAYPOINTS", "CREATE_WAYPOINTS"]
const TOGGLE_INDICES: Array[int] = [MapViewWindow.COMMAND_TOGGLE_GRID,
		MapViewWindow.COMMAND_TOGGLE_GRID, MapViewWindow.COMMAND_TOGGLE_TEXT,
		MapViewWindow.COMMAND_TOGGLE_TEXT, MapViewWindow.COMMAND_TOGGLE_WAYPOINTS,
		MapViewWindow.COMMAND_TOGGLE_WAYPOINTS, MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS]
const CLOSE_WIDGET := "OK"
const MAP_TAB_WIDGET := "RADIO_TAB_MAP"

signal opened
signal closed

var _view: WorldView = null
var _ui_parent: Node = null
var _layout_control: Control = null
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _map_window: MapViewWindow = null
var _hud_source: Callable = Callable()


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
	# Every show selects the map tab (its authored actions show TAB_MAP and hide
	# the others) and re-checks the toggle controls from the view's bytes.
	var tab_id := _driver.widget_id(MAP_TAB_WIDGET)
	if tab_id >= 0:
		_driver.set_widget_checked(tab_id, true)
		_driver.activate(tab_id)
	_sync_toggle_checks()
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	_place_map_window()
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	set_process(false)
	if MenuFrameSurface.hide_frame(_frame):
		closed.emit()


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_map_window = null


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	_driver.tick(_view.frame_clock_ms)
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null:
		close()
		return
	_place_map_window()


func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "CommandMapMenu", "CommandMapPresenter",
			_on_frame_gui_input,
			func(driver: MenuDriver) -> void:
				driver.widget_activated.connect(_on_widget_activated))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	for widget_name in MAP_WIDGETS:
		if _driver.widget_id(widget_name) >= 0:
			_map_window = MapViewWindow.new()
			_map_window.name = "CommandMapWindow"
			_map_window.set_view_kind(MapViewWindow.VIEW_COMMAND)
			_frame.add_child(_map_window)
			break
	return true


# The one map host follows whichever map control is effectively shown (the
# MAP tab or the ORDERS tab): its frame rect, its authored design rect.
func _place_map_window() -> void:
	var map_window := get_map_window()
	if map_window == null or _driver == null:
		return
	map_window.visible = false
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
		return


# The checkboxes mirror the view's toggle bytes on every show (both twins).
func _sync_toggle_checks() -> void:
	var map_window := get_map_window()
	if map_window == null:
		return
	for i in TOGGLE_WIDGETS.size():
		var id := _driver.widget_id(TOGGLE_WIDGETS[i])
		if id >= 0:
			_driver.set_widget_checked(id, map_window.get_command_toggle(TOGGLE_INDICES[i]))


func _on_widget_activated(id: int, widget_name: String) -> void:
	var map_window := get_map_window()
	var upper := widget_name.to_upper()
	if upper == CLOSE_WIDGET:
		close()
		return
	if map_window == null:
		return
	if upper in ZOOM_IN_WIDGETS:
		map_window.zoom_button(1)
	elif upper in ZOOM_OUT_WIDGETS:
		map_window.zoom_button(-1)
	elif upper in TOGGLE_WIDGETS:
		var toggle := TOGGLE_INDICES[TOGGLE_WIDGETS.find(upper)]
		map_window.command_toggle_changed(toggle, _driver.is_widget_checked(id))
		_sync_toggle_checks()


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
