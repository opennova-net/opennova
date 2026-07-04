extends "res://modtools/mission/inspectors/inspector_section.gd"


var _wp_box: VBoxContainer
var _wp_status: Label
var _wp_new_path_button: Button
var _wp_list: ItemList
var _wp_marker_label: Label
var _wp_row_paths: Array = []  # path indices parallel to the _wp_list rows
var _wp_syncing: bool = false
# Active-path flag toggles (loop is the inverse of the stored DoesNotLoop bit) + the
# ordered marker sub-list. _wp_flags_syncing / _wp_marker_syncing guard programmatic sets.
var _wp_loop_check: CheckBox
var _wp_blue_check: CheckBox
var _wp_red_check: CheckBox
var _wp_flags_syncing: bool = false
var _wp_marker_list: ItemList
var _wp_marker_rows: Array = []  # marker indices parallel to the _wp_marker_list rows
var _wp_marker_syncing: bool = false
# Authoring buttons (P7d): add (a placement tool), reorder, delete, clear.
var _wp_add_button: Button
var _wp_up_button: Button
var _wp_down_button: Button
var _wp_delete_button: Button
var _wp_clear_button: Button


# --- Waypoints panel ----------------------------------------------------------
# Lists the mission's waypoint paths and reports the selected marker. Selecting a path row
# focuses it (controller.select_waypoint_path); markers are picked in the viewport. Built
# once and repopulated under the _wp_syncing guard (select() must not echo as a pick).
# Path flag editing + marker authoring buttons land in later phases.

func _build_waypoint_panel() -> void:
	_wp_box = VBoxContainer.new()
	_wp_box.add_theme_constant_override("separation", 4)
	_wp_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.visible = false
	_inspector._root.add_child(_wp_box)

	InspectorForms.add_section_heading(_wp_box, "Waypoint paths")
	_wp_status = InspectorForms.add_muted_label(_wp_box, "")
	_wp_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	# Always-available entry point: focus the first empty path so authoring works even when
	# the list is empty (a brand-new or all-cleared mission), where no row could be clicked.
	_wp_new_path_button = Button.new()
	_wp_new_path_button.name = "MissionWpNewPath"
	_wp_new_path_button.text = "New path"
	_wp_new_path_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_new_path_button)
	_wp_new_path_button.pressed.connect(_on_wp_new_path_pressed)

	_wp_list = ItemList.new()
	_wp_list.name = "MissionWaypointPaths"
	_wp_list.select_mode = ItemList.SELECT_SINGLE
	_wp_list.custom_minimum_size = Vector2(0, 140)
	_wp_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_list)
	_wp_list.item_selected.connect(_on_wp_path_selected)

	# The active path's markers in route order. Selecting a row selects that marker.
	InspectorForms.add_section_heading(_wp_box, "Markers")
	_wp_marker_list = ItemList.new()
	_wp_marker_list.name = "MissionWaypointMarkers"
	_wp_marker_list.select_mode = ItemList.SELECT_SINGLE
	_wp_marker_list.custom_minimum_size = Vector2(0, 140)
	_wp_marker_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_marker_list)
	_wp_marker_list.item_selected.connect(_on_wp_marker_row_selected)

	# Authoring buttons: Add marker (a placement tool) on its own row; reorder / delete on
	# the next; Clear path last.
	_wp_add_button = Button.new()
	_wp_add_button.name = "MissionWpAddMarker"
	_wp_add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_add_button)
	_wp_add_button.pressed.connect(_on_wp_add_pressed)

	var reorder_row := HBoxContainer.new()
	reorder_row.add_theme_constant_override("separation", 6)
	reorder_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(reorder_row)
	_wp_up_button = _make_wp_button(reorder_row, "MissionWpMoveUp", "Move up")
	_wp_down_button = _make_wp_button(reorder_row, "MissionWpMoveDown", "Move down")
	_wp_delete_button = _make_wp_button(reorder_row, "MissionWpDeleteMarker", "Delete marker")
	_wp_up_button.pressed.connect(_on_wp_move_pressed.bind(-1))
	_wp_down_button.pressed.connect(_on_wp_move_pressed.bind(1))
	_wp_delete_button.pressed.connect(_on_wp_delete_marker_pressed)

	_wp_clear_button = Button.new()
	_wp_clear_button.name = "MissionWpClearPath"
	_wp_clear_button.text = "Clear path"
	_wp_clear_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wp_box.add_child(_wp_clear_button)
	_wp_clear_button.pressed.connect(_on_wp_clear_pressed)

	# --- Detail (dock Selection): active-path flags + selected-marker readout -----
	# These edit the selected path / marker, so they live in the Selection tab beside the other
	# per-selection editors. "Loop" is shown (not "DoesNotLoop") so the toggle reads the way the
	# route behaves; the controller inverts it back to the stored bit.
	_inspector._wp_detail_box = VBoxContainer.new()
	_inspector._wp_detail_box.add_theme_constant_override("separation", 4)
	_inspector._wp_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._wp_detail_box.visible = false
	_inspector._sel_content.add_child(_inspector._wp_detail_box)

	InspectorForms.add_section_heading(_inspector._wp_detail_box, "Path")
	var flags_row := HBoxContainer.new()
	flags_row.add_theme_constant_override("separation", 10)
	_inspector._wp_detail_box.add_child(flags_row)
	_wp_loop_check = InspectorForms.add_checkbox(flags_row, "MissionWpLoop", "Loop")
	_wp_blue_check = InspectorForms.add_checkbox(flags_row, "MissionWpBlue", "Blue")
	_wp_red_check = InspectorForms.add_checkbox(flags_row, "MissionWpRed", "Red")
	_wp_loop_check.toggled.connect(_on_wp_flag_toggled)
	_wp_blue_check.toggled.connect(_on_wp_flag_toggled)
	_wp_red_check.toggled.connect(_on_wp_flag_toggled)

	_inspector._wp_detail_box.add_child(HSeparator.new())
	InspectorForms.add_section_heading(_inspector._wp_detail_box, "Selected marker")
	_wp_marker_label = InspectorForms.add_muted_label(_inspector._wp_detail_box, "")
	_wp_marker_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART


func _make_wp_button(parent: Control, node_name: String, text: String) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(button)
	return button


func _on_wp_add_pressed() -> void:
	if _inspector._controller == null:
		return
	# The button toggles the add-marker tool: arm it, or disarm if already armed.
	if _inspector._controller.is_marker_placement_armed():
		_inspector._controller.disarm_marker_placement()
	else:
		_inspector._controller.arm_marker_placement()


func _on_wp_move_pressed(delta: int) -> void:
	if _inspector._controller != null:
		_inspector._controller.move_selected_marker(delta)


func _on_wp_delete_marker_pressed() -> void:
	if _inspector._controller != null:
		_inspector._controller.delete_selected_marker()


func _on_wp_clear_pressed() -> void:
	if _inspector._controller != null:
		_inspector._controller.clear_active_path()


func _on_wp_flag_toggled(_pressed: bool) -> void:
	if _wp_flags_syncing or _inspector._controller == null:
		return
	_inspector._controller.set_waypoint_flags(_wp_loop_check.button_pressed, _wp_blue_check.button_pressed, _wp_red_check.button_pressed)


func _on_wp_marker_row_selected(row: int) -> void:
	if _wp_marker_syncing or _inspector._controller == null:
		return
	if row < 0 or row >= _wp_marker_rows.size():
		return
	_inspector._controller.select_waypoint_marker(int(_wp_marker_rows[row]))


func _on_wp_path_selected(row: int) -> void:
	if _wp_syncing or _inspector._controller == null:
		return
	if row < 0 or row >= _wp_row_paths.size():
		return
	_inspector._controller.select_waypoint_path(int(_wp_row_paths[row]))


func _on_wp_new_path_pressed() -> void:
	if _inspector._controller != null:
		_inspector._controller.select_new_waypoint_path()


func _refresh_waypoint_panel() -> void:
	if _wp_box == null:
		return
	var wp: bool = _inspector._controller != null and _inspector._controller.is_waypoint_mode() and _inspector._controller.get_mission() != null
	_wp_box.visible = wp
	_inspector._wp_detail_box.visible = wp
	if not wp:
		return

	# List every populated path, plus the active path even when empty (so a freshly chosen
	# path is visible). Rebuilt each refresh: the list is small (<= 128) and changes shape
	# as paths are authored.
	var active := int(_inspector._controller.get_selected_waypoint_path_index())
	var summaries: Array = _inspector._controller.get_waypoint_summaries()
	_wp_syncing = true
	_wp_list.clear()
	_wp_row_paths = []
	for s in summaries:
		var idx := int((s as Dictionary)["index"])
		var count := int((s as Dictionary)["marker_count"])
		if count == 0 and idx != active:
			continue
		_wp_list.add_item("Path %d  -  %d markers%s" % [idx, count, _flag_suffix(int((s as Dictionary)["flags"]))])
		_wp_row_paths.append(idx)
		if idx == active:
			_wp_list.select(_wp_list.item_count - 1)
	_wp_syncing = false

	if _wp_row_paths.is_empty():
		_wp_status.text = "No waypoint paths yet. Click New path to start a route."
	elif active < 0:
		_wp_status.text = "Select a path to see its route, then click a marker in the viewport."
	else:
		_wp_status.text = "Click a marker in the viewport to select it."

	# Active-path flags + ordered marker sub-list.
	var active_path: Dictionary = _inspector._controller.get_active_waypoint_path()
	var has_active := not active_path.is_empty()
	var flags := int(active_path.get("flags", 0))
	_wp_flags_syncing = true
	_wp_loop_check.button_pressed = (flags & NovaMissionData.WP_FLAG_DOES_NOT_LOOP) == 0
	_wp_blue_check.button_pressed = (flags & NovaMissionData.WP_FLAG_BLUE_TEAM) != 0
	_wp_red_check.button_pressed = (flags & NovaMissionData.WP_FLAG_RED_TEAM) != 0
	_wp_loop_check.disabled = not has_active
	_wp_blue_check.disabled = not has_active
	_wp_red_check.disabled = not has_active
	_wp_flags_syncing = false

	var mission: NovaMissionData = _inspector._controller.get_mission()
	var marker_sel: Dictionary = _inspector._controller.get_selected_marker()
	var selected_marker_index := int(marker_sel.get("marker_index", -1)) if not marker_sel.is_empty() else -1
	var indices: PackedInt32Array = active_path.get("marker_indices", PackedInt32Array()) if has_active else PackedInt32Array()
	_wp_marker_syncing = true
	_wp_marker_list.clear()
	_wp_marker_rows = []
	for order in indices.size():
		var mi: int = indices[order]
		var pos: Vector3 = mission.get_entity(NovaMissionData.KIND_MARKER, mi).get("position", Vector3.ZERO)
		_wp_marker_list.add_item("%d.  marker #%d  (%.0f, %.0f, %.0f)" % [order + 1, mi, pos.x, pos.y, pos.z])
		_wp_marker_rows.append(mi)
		if mi == selected_marker_index:
			_wp_marker_list.select(_wp_marker_list.item_count - 1)
	_wp_marker_syncing = false

	# Authoring affordances: Add is a toggle (arm / stop); reorder + delete need a selected
	# marker; clear needs a non-empty path. Armed state also drives the status line.
	var armed: bool = _inspector._controller.is_marker_placement_armed()
	var has_marker_sel := not marker_sel.is_empty()
	_wp_add_button.text = "Stop adding markers" if armed else "Add marker"
	_wp_add_button.disabled = not has_active
	_wp_up_button.disabled = not has_marker_sel
	_wp_down_button.disabled = not has_marker_sel
	_wp_delete_button.disabled = not has_marker_sel
	_wp_clear_button.disabled = not (has_active and indices.size() > 0)
	if armed:
		_wp_status.text = "Click the terrain to add a marker to this path; right-click or Esc to stop."

	var marker: Dictionary = marker_sel
	if marker.is_empty():
		_wp_marker_label.text = "No marker selected."
	else:
		var p: Vector3 = marker.get("position", Vector3.ZERO)
		# Whole units, matching the marker list rows above (the format stores integers).
		_wp_marker_label.text = "Marker #%d  (%.0f, %.0f, %.0f)" % [int(marker["marker_index"]), p.x, p.y, p.z]


# A short "[loop, blue]"-style suffix describing a path's flags, or "" when none apply.
func _flag_suffix(flags: int) -> String:
	var parts: Array = []
	if (flags & NovaMissionData.WP_FLAG_DOES_NOT_LOOP) == 0:
		parts.append("loop")
	if (flags & NovaMissionData.WP_FLAG_BLUE_TEAM) != 0:
		parts.append("blue")
	if (flags & NovaMissionData.WP_FLAG_RED_TEAM) != 0:
		parts.append("red")
	return "  [%s]" % ", ".join(parts) if not parts.is_empty() else ""
