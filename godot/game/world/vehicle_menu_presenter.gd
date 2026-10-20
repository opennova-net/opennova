class_name VehicleMenuPresenter
extends Node

## The in-world vehicle screen (vehicle.mnu VEHICLE, D-NET-NEW-L5-5): a
## placeholder overlay over the engine's inmatch::VehicleMenu, which carries
## every rule (the first open's C2S 0x42, the poll that asks again and refills
## the list, the rows over the bay mask and the S2C 0x70 counts, ACCEPT's C2S
## 0x40), reached through Simulation. This node only shows the ITEM_LIST rows
## the engine holds, routes a row pick and the two buttons, and closes.
## The shell opens it on the USE-ITEM vehicle-loadout arm
## (LocalPlayerPresenter.take_use_item_vehicle_menu_request); the world keeps
## ticking underneath. The witnesses are the engine's (runtime/inmatch/vehicle_menu.h).

signal opened
signal closed

var _sim_source := Callable()  # () -> Simulation (null without a live world)
var _ui_parent: Node = null
var _panel: PanelContainer = null
var _list: ItemList = null
var _shown_rows := PackedStringArray()
var _shown_selection := -2


## The live Simulation source and the node the overlay sits on (the HUD layer
## in the game shell).
func setup(sim_source: Callable, ui_parent: Node) -> void:
	_sim_source = sim_source
	_ui_parent = ui_parent


func is_open() -> bool:
	return _panel != null and is_instance_valid(_panel) and _panel.visible


## Open the screen over live play: the engine runs the screen's open events
## (Simulation.open_vehicle_menu) and this node shows what they listed.
func open() -> bool:
	if is_open() or _ui_parent == null:
		return false
	var sim := _sim()
	if sim == null or not sim.open_vehicle_menu():
		return false
	_build()
	_shown_rows = PackedStringArray()
	_shown_selection = -2
	_refresh(sim)
	_panel.visible = true
	opened.emit()
	return true


## The Escape close: CANCEL's click (the engine sends nothing for it).
func close() -> void:
	_click(true)


func teardown() -> void:
	if _panel != null and is_instance_valid(_panel):
		_panel.queue_free()
	_panel = null
	_list = null


func _process(_delta: float) -> void:
	# The engine's poll refills the list every 64 frames while a reply stands.
	if is_open():
		var sim := _sim()
		if sim != null:
			_refresh(sim)


func _sim() -> Simulation:
	return _sim_source.call() as Simulation if _sim_source.is_valid() else null


func _build() -> void:
	if _panel != null and is_instance_valid(_panel):
		return
	_panel = PanelContainer.new()
	_panel.name = "VehicleMenu"
	_panel.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	_panel.custom_minimum_size = Vector2(360, 240)
	var column := VBoxContainer.new()
	_panel.add_child(column)
	_list = ItemList.new()
	_list.name = "ITEM_LIST"
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.item_selected.connect(_on_item_selected)
	column.add_child(_list)
	var buttons := HBoxContainer.new()
	column.add_child(buttons)
	var accept := Button.new()
	accept.name = "ACCEPT"
	accept.text = "ACCEPT"
	accept.pressed.connect(_click.bind(false))
	buttons.add_child(accept)
	var cancel := Button.new()
	cancel.name = "CANCEL"
	cancel.text = "CANCEL"
	cancel.pressed.connect(_click.bind(true))
	buttons.add_child(cancel)
	_panel.visible = false
	_ui_parent.add_child(_panel)


func _refresh(sim: Simulation) -> void:
	var rows: PackedStringArray = sim.get_vehicle_menu_rows()
	var selected: int = sim.get_vehicle_menu_selected_row()
	if rows == _shown_rows and selected == _shown_selection:
		return
	_shown_rows = rows
	_shown_selection = selected
	_list.clear()
	for row in rows:
		_list.add_item(row)
	if selected >= 0 and selected < _list.item_count:
		_list.select(selected)


func _on_item_selected(index: int) -> void:
	var sim := _sim()
	if sim != null:
		sim.select_vehicle_menu_row(index)


# ACCEPT (cancel false) or CANCEL: the engine's click, then the screen closes.
func _click(cancel: bool) -> void:
	if not is_open():
		return
	var sim := _sim()
	if sim != null:
		sim.click_vehicle_menu(cancel)
	_panel.visible = false
	closed.emit()
