class_name CommandMapPresenter
extends Node

## The commander map: cmap.mnu's CMAP screen over live play. The commander_menu
## key (controls catalog row 53, default V) opens it through the HUD toggles poll,
## which owns the key's gates and its respawn init (engine hud_toggles.h
## kCommandMapOpened). This presenter hosts the windowed map view on the MAP /
## ORDERS_MAP controls — ONE MapViewWindow in its command kind, placed over
## whichever of the two is on screen, since both share one view state (the
## process-lifetime MapViewState, like the retail globals) — plus the zoom
## buttons, the GRID / TEXT / WAYPOINTS / CREATE_WAYPOINTS toggles (each with
## its ORDERS_ twin), the placed-waypoint legs (the WAYPOINTNAME_DLG dialog, the
## USERWP_CLOSE delete button with its clip to the bound map, CLEAR_WAYPOINTS),
## the six go-code buttons, the chat panel's two send buttons, the TEAM tab's
## TEAMLIST and ADDTO_* buttons, the PLAYERS tab's PLAYERLIST, the ORDERS tab's
## combos, NEW_ORDER and CURRENT_ORDERS (CommandMapScreen over the engine's
## menu/command_map_screen.h), the tab radios' gates and the OK close. The
## witnessed models and the mode-4 compile are the engine's (hud/hud_map_view.h,
## inmatch/client_squad.cpp); witness record hud-re D-HUD-19.

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
const ORDERS_TAB_WIDGET := "RADIO_TAB_ORDERS"
# The tabs whose radio unbinds the delete button.
const UNBINDING_TABS: Array[String] = ["RADIO_TAB_TEAM", "RADIO_TAB_PLAYERS", "RADIO_TAB_RULES"]
# The gated tab radios, each paired with its CommandMapScreen gate bit.
const GATED_TABS: Array[String] = ["RADIO_TAB_ORDERS", "RADIO_TAB_PLAYERS", "RADIO_TAB_TEAM",
		"RADIO_TAB_RULES"]
const GATED_TAB_BITS: Array[int] = [CommandMapScreen.GATE_ORDERS, CommandMapScreen.GATE_PLAYERS,
		CommandMapScreen.GATE_TEAM, CommandMapScreen.GATE_RULES]
const DIALOG_WIDGET := "WAYPOINTNAME_DLG"
const NAME_WIDGET := "WPNAME"
const NAME_OK_WIDGET := "WPNAME_OK"
const NAME_CANCEL_WIDGET := "WPNAME_CANCEL"
const DELETE_WIDGET := "USERWP_CLOSE"
const CLEAR_WIDGET := "CLEAR_WAYPOINTS"
const CREATE_WIDGET := "CREATE_WAYPOINTS"
const CHAT_TEXT_WIDGET := "CHAT_TEXT"
const CHAT_TEAM_WIDGET := "CHAT_TEAM"
const CHAT_ALL_WIDGET := "CHAT_ALL"
# The go-code buttons, codes 0..5.
const GO_CODE_WIDGETS: Array[String] = ["GOCODE_UNIFORM", "GOCODE_VICTOR", "GOCODE_WHISKEY",
		"GOCODE_XRAY", "GOCODE_YANKEE", "GOCODE_ZULU"]
# The fireteam buttons, fireteams 0 (none) .. 3 (C).
const FIRETEAM_WIDGETS: Array[String] = ["ADDTO_NO_FIRETEAM", "ADDTO_FIRETEAM_A",
		"ADDTO_FIRETEAM_B", "ADDTO_FIRETEAM_C"]
const NEW_ORDER_WIDGET := "NEW_ORDER"
const TEAM_TAB_WIDGET := "RADIO_TAB_TEAM"
const PLAYERS_TAB_WIDGET := "RADIO_TAB_PLAYERS"

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
var _hud_source: Callable = Callable()
# The views outlive the per-mission menu rebuild, like the retail globals.
var _map_view_state := MapViewState.new()
# The map control the waypoint legs follow and whether the delete button is
# bound to it.
var _bound_map := ""
var _delete_bound := false
var _delete_shown := false
var _delete_at := Vector2i(-1, -1)


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
	# The show (hud-re "The windowed map views"): the map tab selected (its
	# authored actions show TAB_MAP and hide the others; its callback binds the
	# delete button to MAP), the toggle controls re-checked from the view's
	# bytes, the delete button bound to MAP, then the tab radios' gates.
	var tab_id := _driver.widget_id(MAP_TAB_WIDGET)
	if tab_id >= 0:
		_driver.set_widget_checked(tab_id, true)
		_driver.activate(tab_id)
	_sync_toggle_checks()
	_bind_delete_button("MAP")
	_apply_tab_gates(_screen.populate_team_list(true))
	_screen.populate_player_list()
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
	# The session's end empties the order store the next session starts from.
	if _screen != null:
		_screen.clear_orders()
	_frame = null
	_audio = null
	_driver = null
	_map_window = null
	_screen = null
	_bound_map = ""
	_delete_bound = false
	_delete_shown = false


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
	_place_delete_button()
	var gates := _screen.refresh(Time.get_ticks_msec())
	if gates >= 0:
		_apply_tab_gates(gates)


func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "CommandMapMenu", "CommandMapPresenter",
			_on_frame_gui_input,
			func(driver: MenuDriver) -> void:
				driver.widget_activated.connect(_on_widget_activated)
				driver.edit_committed.connect(_on_edit_committed)
				driver.table_cell_clicked.connect(_on_table_cell_clicked))
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
	for widget_name in MAP_WIDGETS:
		if _driver.widget_id(widget_name) >= 0:
			_map_window = MapViewWindow.new()
			_map_window.name = "CommandMapWindow"
			_map_window.set_view_kind(MapViewWindow.VIEW_COMMAND)
			_map_window.set_view_state(_map_view_state)
			_map_window.waypoint_dialog_requested.connect(_on_waypoint_dialog_requested)
			_frame.add_child(_map_window)
			break
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


# The render leg's delete button: while bound, it takes the corner the last
# render reported beside the first hovered placed waypoint (at its own square
# size) and shows, or hides when none is hovered.
func _place_delete_button() -> void:
	var map_window := get_map_window()
	var id := _driver.widget_id(DELETE_WIDGET)
	if map_window == null or id < 0:
		return
	var width := int(_driver.widget_local_rect(id).size.x)
	map_window.set_close_button(_delete_bound, width)
	if not _delete_bound:
		return
	var shown := map_window.is_close_button_shown()
	if shown:
		var at := map_window.get_close_button_position()
		if at != _delete_at or not _delete_shown:
			_driver.set_widget_rect(id, Rect2i(at, Vector2i(width, width)))
			_delete_at = at
	_set_delete_shown(shown)


func _set_delete_shown(shown: bool) -> void:
	var id := _driver.widget_id(DELETE_WIDGET) if _driver != null else -1
	if id < 0 or shown == _delete_shown:
		return
	_delete_shown = shown
	_driver.set_widget_shown(id, shown)


# A tab radio's (or the show's) binding: the delete button hidden and bound
# to `map_name`, or unbound for "".
func _bind_delete_button(map_name: String) -> void:
	_delete_shown = true
	_set_delete_shown(false)
	_delete_bound = map_name != "" and _driver.widget_id(DELETE_WIDGET) >= 0
	if map_name != "":
		_bound_map = map_name
	var map_window := get_map_window()
	if map_window != null:
		map_window.set_close_button(_delete_bound,
				int(_driver.widget_local_rect(_driver.widget_id(DELETE_WIDGET)).size.x))


# A team-list populate's tab gates (RULES only from the show's full one).
func _apply_tab_gates(gates: int) -> void:
	for i in GATED_TABS.size():
		if GATED_TAB_BITS[i] == CommandMapScreen.GATE_RULES \
				and (gates & CommandMapScreen.GATE_SETS_RULES) == 0:
			continue
		var id := _driver.widget_id(GATED_TABS[i])
		if id >= 0:
			_driver.set_widget_disabled(id, (gates & GATED_TAB_BITS[i]) == 0)


# USERWP_CLOSE clipped to a map control's screen rect (the MAP / ORDERS
# radios; the engine's MenuRuntime clip).
func _clip_delete_button(map_name: String) -> void:
	var delete_id := _driver.widget_id(DELETE_WIDGET)
	var map_index := _driver.frame_index(_driver.widget_id(map_name))
	if delete_id < 0 or map_index < 0:
		return
	_driver.set_widget_clip_rect(delete_id, Rect2i(_frame.widget_rect(map_index)))


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
	if upper == CHAT_TEAM_WIDGET or upper == CHAT_ALL_WIDGET:
		_submit_chat(upper == CHAT_ALL_WIDGET)
		return
	if upper in GO_CODE_WIDGETS:
		var sim: Simulation = _view.sim() if _view != null else null
		if sim != null:
			sim.send_go_code(GO_CODE_WIDGETS.find(upper))
		return
	if upper == MAP_TAB_WIDGET:
		_bind_delete_button("MAP")
		_clip_delete_button("MAP")
		return
	if upper == ORDERS_TAB_WIDGET:
		_bind_delete_button("ORDERS_MAP")
		_clip_delete_button("ORDERS_MAP")
		_screen.populate_orders()
		return
	if upper in UNBINDING_TABS:
		_bind_delete_button("")
		if upper == TEAM_TAB_WIDGET:
			_apply_tab_gates(_screen.populate_team_list(false))
		elif upper == PLAYERS_TAB_WIDGET:
			_screen.populate_player_list()
		return
	if upper in FIRETEAM_WIDGETS:
		_screen.assign_fireteam(FIRETEAM_WIDGETS.find(upper))
		return
	if upper == NEW_ORDER_WIDGET:
		_screen.new_order()
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
	elif upper == NAME_OK_WIDGET:
		_confirm_waypoint(map_window)
	elif upper == NAME_CANCEL_WIDGET:
		_uncheck_create_waypoints(map_window)
	elif upper == DELETE_WIDGET:
		if _delete_bound:
			map_window.delete_hovered_waypoint()
			_set_delete_shown(false)
	elif upper == CLEAR_WIDGET:
		map_window.clear_waypoints()


# The CREATE_WAYPOINTS press: the name dialog centred on the click and pushed
# onto the bound map control's own rect, shown, the name field focused, and
# the click stored as the map's local point.
func _on_waypoint_dialog_requested(point: Vector2i) -> void:
	var map_window := get_map_window()
	var dialog_id := _driver.widget_id(DIALOG_WIDGET) if _driver != null else -1
	var map_id := _driver.widget_id(_bound_map) if _driver != null and _bound_map != "" else -1
	if map_window == null or dialog_id < 0 or map_id < 0:
		return
	var dialog_rect := Rect2i(_driver.widget_local_rect(dialog_id))
	var map_rect := Rect2i(_driver.widget_local_rect(map_id))
	_driver.set_widget_rect(dialog_id, MapViewWindow.waypoint_dialog_rect(point, dialog_rect,
			map_rect))
	_driver.set_widget_shown(dialog_id, true)
	var name_id := _driver.widget_id(NAME_WIDGET)
	if name_id >= 0:
		_driver.focus_widget(name_id)
	map_window.store_waypoint_click(point)


# A TEAMLIST / PLAYERLIST / CURRENT_ORDERS press after the table's own
# selection write (the engine's click handlers read the row's new state).
func _on_table_cell_clicked(_id: int, widget_name: String, row: int, column: int, state: int,
		cell_value: int, _double_click: bool) -> void:
	if _screen != null:
		_screen.table_cell_clicked(widget_name, row, column, state, cell_value,
				Time.get_ticks_msec())


# WPNAME's Enter presses WPNAME_OK.
func _on_edit_committed(_id: int, widget_name: String) -> void:
	if widget_name.to_upper() != NAME_WIDGET or _driver == null:
		return
	var ok_id := _driver.widget_id(NAME_OK_WIDGET)
	if ok_id >= 0:
		_driver.activate(ok_id)


# WPNAME_OK: the waypoint placed from the typed name (the dialog hides by its
# authored action), the delete button hidden when one was placed, the field
# cleared and CREATE_WAYPOINTS unchecked.
func _confirm_waypoint(map_window: MapViewWindow) -> void:
	var name_id := _driver.widget_id(NAME_WIDGET)
	if name_id < 0:
		return
	if map_window.confirm_waypoint(_driver.get_widget_text(name_id)):
		_set_delete_shown(false)
	_driver.set_widget_text(name_id, "")
	_uncheck_create_waypoints(map_window)


# WPNAME_CANCEL, and the confirm's tail: CREATE_WAYPOINTS unchecked and its
# byte cleared.
func _uncheck_create_waypoints(map_window: MapViewWindow) -> void:
	var id := _driver.widget_id(CREATE_WIDGET)
	if id < 0:
		return
	_driver.set_widget_checked(id, false)
	map_window.clear_create_waypoints()


# CHAT_TEAM / CHAT_ALL: the CHAT_TEXT line read and cleared, then sent through
# the team or the global sender.
func _submit_chat(all: bool) -> void:
	var id := _driver.widget_id(CHAT_TEXT_WIDGET)
	if id < 0:
		return
	var text := _driver.get_widget_text(id)
	_driver.set_widget_text(id, "")
	var hud: HudOverlay = _hud_source.call() if _hud_source.is_valid() else null
	var sim: Simulation = _view.sim() if _view != null else null
	if hud != null and sim != null:
		hud.send_command_map_chat(sim, all, text, Simulation.ticks_from_ms(_view.frame_clock_ms))


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
