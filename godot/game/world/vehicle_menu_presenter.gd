class_name VehicleMenuPresenter
extends Node

## The in-world vehicle screen: vehicle.mnu's VEHICLE screen, opened over live
## play as the game opens it (UI_OpenMenuScreen("vehicle.mnu", "VEHICLE") on the
## USE-ITEM vehicle-loadout arm), on the compiled-menu surface the armory's
## weapon.mnu rides (MenuFrameSurface). The engine's inmatch::VehicleMenu carries
## every rule (the first open's C2S 0x42, the poll that asks again and refills
## the list, the rows over the bay mask and the S2C 0x70 counts, ACCEPT's C2S
## 0x40), reached through Simulation; this node shows the ITEM_LIST rows the
## engine holds, routes a row pick and the ACCEPT / CANCEL clicks, and closes.
## The shell opens it (LocalPlayerPresenter.take_use_item_vehicle_menu_request);
## the world keeps ticking underneath. The witnesses are the engine's
## (runtime/inmatch/vehicle_menu.h).

signal opened
signal closed

const MENU_FILE := "vehicle.mnu"
const MENU_SCREEN := "VEHICLE"
const LIST_CONTROL := "ITEM_LIST"

var _view: WorldView = null
var _ui_parent: Node = null
# The layout source (MenuFrameSurface.fit_frame): a Control parent drives the fit
# from its own size, a CanvasLayer parent (the game HUD) follows the viewport.
var _layout_control: Control = null
# The compiled menu surface (MenuFrameSurface.Surface's members).
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _shown_rows := PackedStringArray()
var _shown_selection := -2


## The world view (its Simulation and resource root) and the node the screen
## overlays (the HUD layer in the game shell; tests pass their own parent).
func setup(view: WorldView, ui_parent: Node) -> void:
	_view = view
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## The live menu driver and frame over the screen (read seams for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


func get_menu_frame() -> MenuFrame:
	return _frame


## Open the screen over live play: the screen shows, and the engine runs its open
## events (Simulation.open_vehicle_menu: the first parse's init, every open's
## activate) and this node shows what they listed.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null or not _ensure_menu():
		return false
	sim.open_vehicle_menu()
	_shown_rows = PackedStringArray()
	_shown_selection = -2
	_refresh(sim)
	# The menu draws over every HUD element, as the armory's does.
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	set_process(true)
	opened.emit()
	return true


## The Escape close: CANCEL's click (the engine sends nothing for it).
func close() -> void:
	_click(true)


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The menu's pump, once a frame after the input events (MenuFrameSurface).
	_driver.pump_mouse()
	_driver.tick(_view.frame_clock_ms)
	# The engine's poll refills the list every 64 frames while a reply stands.
	var sim: Simulation = _view.sim()
	if sim != null:
		_refresh(sim)


# Build the compiled surface over the gameplay view from the world's root, the
# screen's text tables registered as the armory's are.
func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "VehicleMenu", "VehicleMenuPresenter", _on_frame_gui_input,
			func(built: MenuDriver) -> void:
				_register_text_tables(root)
				built.widget_value_changed.connect(_on_widget_value_changed)
				built.widget_activated.connect(_on_widget_activated))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_frame.visible = false
	return true


# The engine's rows and selection into the ITEM_LIST when they changed.
func _refresh(sim: Simulation) -> void:
	var list := _driver.widget_id(LIST_CONTROL)
	if list < 0:
		return
	var rows: PackedStringArray = sim.get_vehicle_menu_rows()
	var selected: int = sim.get_vehicle_menu_selected_row()
	if rows == _shown_rows and selected == _shown_selection:
		return
	_shown_rows = rows
	_shown_selection = selected
	_driver.set_widget_items(list, rows)
	# A selection outside the rows (none, or past a refill's end) shows no row.
	_driver.select_row(list, selected if selected >= 0 and selected < rows.size() else -1, false)


# An ITEM_LIST pick: the engine's row select.
func _on_widget_value_changed(widget_name: String, _kind: String, index: int,
		_value: String) -> void:
	if not is_open() or widget_name.to_upper() != LIST_CONTROL or index < 0:
		return
	var sim: Simulation = _view.sim()
	if sim != null:
		sim.select_vehicle_menu_row(index)


# ACCEPT and CANCEL, the screen's two buttons.
func _on_widget_activated(_id: int, widget_name: String) -> void:
	match widget_name.to_upper():
		"ACCEPT":
			_click(false)
		"CANCEL":
			_click(true)


# ACCEPT (cancel false) or CANCEL: the engine's click, then the screen closes.
func _click(cancel: bool) -> void:
	if not is_open():
		return
	var sim: Simulation = _view.sim()
	if sim != null:
		sim.click_vehicle_menu(cancel)
	MenuFrameSurface.hide_frame(_frame)
	set_process(false)
	closed.emit()


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver != null and is_open():
		MenuFrameSurface.forward_gui_input(event, _driver, _frame)


# The screen's text tables from the world's root (the armory's rule): a table the
# root lacks is unregistered.
func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMETEXT, "gametext.bin"],
			[Strings.TABLE_GAMEUI, "Game.bin"]]:
		Strings.register_table(spec[0], Strings.load_rtxt(root, spec[1]))


func _recompute_fit() -> void:
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
