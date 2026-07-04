extends "res://modtools/mission/inspectors/inspector_section.gd"


# --- Area-trigger (zone) panel (Phase 2) --------------------------------------
# Shown in Triggers mode: the zone list, the selected zone's six min/max spins, the two known
# flag toggles (Active / Constrain height), and Add / Delete. Built once, repopulated under
# guards so a programmatic set never echoes back as a user edit. Edits route through the
# controller (set_selected_zone_bounds / _flags, add_area_trigger_default, delete...).
var _at_box: VBoxContainer
var _at_status: Label
var _at_list: ItemList
var _at_rows: Array = []  # zone indices parallel to the _at_list rows
var _at_syncing: bool = false
var _at_min_spins: Array = []  # [x, y, z] SpinBox
var _at_max_spins: Array = []  # [x, y, z] SpinBox
var _at_active_check: CheckBox
var _at_constrain_check: CheckBox
var _at_flags_syncing: bool = false
var _at_bounds_syncing: bool = false
var _at_add_button: Button
var _at_delete_button: Button


# --- Area-trigger (zone) panel (Phase 2) --------------------------------------
# Lists the mission's zones and edits the selected one: six min/max spins (mission units), the
# two known flag toggles (Active = bit 0x01, Constrain height = bit 0x02), and Add / Delete.
# Selecting a row focuses that zone; a zone is also picked / translated in the viewport. Built
# once, repopulated under guards so a programmatic set never echoes back as a user edit.

func _build_area_trigger_panel() -> void:
	_at_box = VBoxContainer.new()
	_at_box.add_theme_constant_override("separation", 4)
	_at_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.visible = false
	_inspector._root.add_child(_at_box)

	InspectorForms.add_section_heading(_at_box, "Area triggers / zones")
	_at_status = InspectorForms.add_muted_label(_at_box, "")
	_at_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_at_add_button = Button.new()
	_at_add_button.name = "MissionAtAddZone"
	_at_add_button.text = "Add zone"
	_at_add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.add_child(_at_add_button)
	_at_add_button.pressed.connect(_on_at_add_pressed)

	_at_list = ItemList.new()
	_at_list.name = "MissionAreaTriggers"
	_at_list.select_mode = ItemList.SELECT_SINGLE
	_at_list.custom_minimum_size = Vector2(0, 120)
	_at_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.add_child(_at_list)
	_at_list.item_selected.connect(_on_at_row_selected)

	# --- Detail (dock Selection): the selected zone's bounds + flags + delete ------
	# The six bounds spins gain the wider dock column over the cramped left lane.
	_inspector._at_detail_box = VBoxContainer.new()
	_inspector._at_detail_box.add_theme_constant_override("separation", 4)
	_inspector._at_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._at_detail_box.visible = false
	_inspector._sel_content.add_child(_inspector._at_detail_box)

	InspectorForms.add_section_heading(_inspector._at_detail_box, "Bounds (mission units)")
	# The format stores bounds as signed 16.16 fixed-point, so the representable range is
	# ~±32768 mission units; the spins are bounded to that (the lib also clamps on write).
	const ZONE_MIN := -32767.0
	const ZONE_MAX := 32767.0
	_at_min_spins = [
		InspectorForms.add_spin_row(_inspector._at_detail_box, "MissionAtMinX", "Min X", ZONE_MIN, ZONE_MAX, 0.001),
		InspectorForms.add_spin_row(_inspector._at_detail_box, "MissionAtMinY", "Min Y", ZONE_MIN, ZONE_MAX, 0.001),
		InspectorForms.add_spin_row(_inspector._at_detail_box, "MissionAtMinZ", "Min Z", ZONE_MIN, ZONE_MAX, 0.001),
	]
	_at_max_spins = [
		InspectorForms.add_spin_row(_inspector._at_detail_box, "MissionAtMaxX", "Max X", ZONE_MIN, ZONE_MAX, 0.001),
		InspectorForms.add_spin_row(_inspector._at_detail_box, "MissionAtMaxY", "Max Y", ZONE_MIN, ZONE_MAX, 0.001),
		InspectorForms.add_spin_row(_inspector._at_detail_box, "MissionAtMaxZ", "Max Z", ZONE_MIN, ZONE_MAX, 0.001),
	]
	for axis in 3:
		_at_min_spins[axis].value_changed.connect(_on_at_bounds_changed)
		_at_max_spins[axis].value_changed.connect(_on_at_bounds_changed)

	InspectorForms.add_section_heading(_inspector._at_detail_box, "Flags")
	var flags_row := HBoxContainer.new()
	flags_row.add_theme_constant_override("separation", 10)
	_inspector._at_detail_box.add_child(flags_row)
	_at_active_check = InspectorForms.add_checkbox(flags_row, "MissionAtActive", "Active")
	_at_constrain_check = InspectorForms.add_checkbox(flags_row, "MissionAtConstrainZ", "Constrain height")
	_at_active_check.tooltip_text = "Flags bit 0x01. NOTE: the in-zone trigger condition (*IsWithinArea) does NOT read this bit; only the mission-boundary out-of-bounds check does. Shipped missions leave it clear."
	_at_constrain_check.tooltip_text = "Limit the zone to its Z (height) range (bit 0x02). When clear, the zone is unbounded vertically (+/-16384)."
	_at_active_check.toggled.connect(_on_at_flag_toggled)
	_at_constrain_check.toggled.connect(_on_at_flag_toggled)

	_inspector._at_detail_box.add_child(HSeparator.new())
	_at_delete_button = Button.new()
	_at_delete_button.name = "MissionAtDeleteZone"
	_at_delete_button.text = "Delete zone"
	_at_delete_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._at_detail_box.add_child(_at_delete_button)
	_at_delete_button.pressed.connect(_on_at_delete_pressed)


func _on_at_add_pressed() -> void:
	if _inspector._controller != null:
		_inspector._controller.add_area_trigger_default()


func _on_at_delete_pressed() -> void:
	if _inspector._controller != null:
		_inspector._controller.delete_selected_area_trigger()


func _on_at_row_selected(row: int) -> void:
	if _at_syncing or _inspector._controller == null:
		return
	if row < 0 or row >= _at_rows.size():
		return
	_inspector._controller.select_area_trigger(int(_at_rows[row]))


func _on_at_bounds_changed(_value: float) -> void:
	if _at_bounds_syncing or _inspector._controller == null:
		return
	var mn := Vector3(_at_min_spins[0].value, _at_min_spins[1].value, _at_min_spins[2].value)
	var mx := Vector3(_at_max_spins[0].value, _at_max_spins[1].value, _at_max_spins[2].value)
	_inspector._controller.set_selected_zone_bounds(mn, mx)


func _on_at_flag_toggled(_pressed: bool) -> void:
	if _at_flags_syncing or _inspector._controller == null:
		return
	_inspector._controller.set_selected_zone_flags(_at_active_check.button_pressed, _at_constrain_check.button_pressed)


func _refresh_area_trigger_panel() -> void:
	if _at_box == null:
		return
	var on: bool = _inspector._controller != null and _inspector._controller.is_area_trigger_mode() and _inspector._controller.get_mission() != null
	_at_box.visible = on
	_inspector._at_detail_box.visible = on
	if not on:
		return

	var zones: Array = _inspector._controller.get_area_triggers()
	var selected := int(_inspector._controller.get_selected_zone_index())
	_at_syncing = true
	_at_list.clear()
	_at_rows = []
	for z in zones:
		var zd := z as Dictionary
		var zidx := int(zd.get("index", -1))
		var zmn: Vector3 = zd.get("min", Vector3.ZERO)
		var zmx: Vector3 = zd.get("max", Vector3.ZERO)
		var zactive := bool(zd.get("active", false))
		var label := "Zone %d  (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)%s" % [zidx, zmn.x, zmn.y, zmn.z, zmx.x, zmx.y, zmx.z, "" if zactive else "  [off]"]
		_at_list.add_item(label)
		_at_rows.append(zidx)
		if zidx == selected:
			_at_list.select(_at_list.item_count - 1)
	_at_syncing = false

	if zones.is_empty():
		_at_status.text = "No zones yet. Click Add zone, then drag it in the viewport or set its bounds below."
	elif selected < 0:
		_at_status.text = "Select a zone, or click one in the viewport."
	else:
		_at_status.text = "Drag the zone in the viewport to move it; set exact bounds below."

	# Selected zone's bounds + flags; the editors are disabled when nothing is selected.
	var sel_zone: Dictionary = _inspector._controller.get_selected_zone()
	var has_sel := not sel_zone.is_empty()
	var sel_mn: Vector3 = sel_zone.get("min", Vector3.ZERO)
	var sel_mx: Vector3 = sel_zone.get("max", Vector3.ZERO)
	_at_bounds_syncing = true
	# _sync_spin skips a spin whose inner LineEdit is focused, so a refresh mid-edit (e.g. an undo while
	# the user is typing a bound) does not clobber the in-flight keystroke.
	_inspector._sync_spin(_at_min_spins[0], sel_mn.x)
	_inspector._sync_spin(_at_min_spins[1], sel_mn.y)
	_inspector._sync_spin(_at_min_spins[2], sel_mn.z)
	_inspector._sync_spin(_at_max_spins[0], sel_mx.x)
	_inspector._sync_spin(_at_max_spins[1], sel_mx.y)
	_inspector._sync_spin(_at_max_spins[2], sel_mx.z)
	for axis in 3:
		_at_min_spins[axis].editable = has_sel
		_at_max_spins[axis].editable = has_sel
	_at_bounds_syncing = false

	_at_flags_syncing = true
	_at_active_check.button_pressed = bool(sel_zone.get("active", false))
	_at_constrain_check.button_pressed = bool(sel_zone.get("constrain_z", false))
	_at_active_check.disabled = not has_sel
	_at_constrain_check.disabled = not has_sel
	_at_flags_syncing = false

	_at_delete_button.disabled = not has_sel
