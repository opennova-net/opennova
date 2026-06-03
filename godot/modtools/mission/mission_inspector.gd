extends MarginContainer

# Inspector for the Mission workspace. Two regions stacked in a scroll:
#
#   _edit_box  — the editable panel for the selected entity (position, rotation,
#                team, group). Built ONCE and only repopulated in place, because the
#                controller fires `changed` on every edit and a torn-down SpinBox
#                would lose focus / caret mid-keystroke. Hidden when nothing is
#                selected.
#   _box       — the read-only mission summary (metadata, world, object counts).
#                Cheap to rebuild, so it is torn down and rebuilt on every `changed`.
#
# All edits go through the controller (never NovaMissionData directly); the controller
# moves the in-world object and writes the record. Referenced via preload (no
# class_name), the same convention as the controller and placer.

const ObjectUiHelpers = preload("res://modtools/object/ui/object_ui_helpers.gd")
# Preloaded only for its Mode enum (the tab <-> mode map); the live controller is injected via
# setup() and used untyped, the same no-class_name convention as the rest of the workspace.
const MissionController = preload("res://modtools/mission/mission_controller.gd")

var _controller  # MissionController (preloaded, no class_name)
var _root: VBoxContainer
var _edit_box: VBoxContainer
var _box: VBoxContainer

# True while programmatically repopulating the edit widgets, so the value_changed
# handlers ignore the echo and do not re-commit (and re-emit) what they just read.
var _loading: bool = false

var _identity_label: Label  # heading: the selected model's name (or kind + index)
var _identity_sub: Label    # muted subline: kind + index, shown when a name resolved
var _animated_note: Label
var _pos_spins: Array = []  # [x, y, z]
var _rot_spins: Array = []  # [pitch, yaw, roll]
var _team_spin: SpinBox
var _group_spin: SpinBox
# Collapsible "Behavior" section: the per-entity AI + waypoint fields the format carries
# beyond team / group. Bound through a FieldBinder (its own reentrancy guard), so a
# programmatic repopulate never echoes back as an edit. The "Waypoint path" field here is
# what links a unit to a path authored in Waypoints mode.
var _behavior_toggle: CheckButton
var _behavior_box: VBoxContainer
var _behavior_binder: FieldBinder
var _delete_button: Button

# --- Place-object palette (persistent) ----------------------------------------
var _place_box: VBoxContainer
var _place_search: LineEdit
var _place_list: ItemList
var _place_status: Label
var _place_stop: Button
# items.def ids parallel to the currently-shown _place_list rows (the list is filtered
# by the search box, so row index != item index in the full set).
var _place_row_ids: Array = []
# The mission the palette rows were built for; the (expensive) list is only repopulated
# when this changes, not on every `changed` (which fires on each edit / placement).
var _place_built_for: NovaMissionData
# Placeable items for the current mission, cached so the palette does not re-enumerate
# the whole items.def (1000+ entries) on every refresh. Rebuilt when the mission changes.
var _placeable_cache: Array = []
var _placeable_names: Dictionary = {}  # id -> display label
# True while programmatically syncing the list selection, so item_selected echoes do
# not re-arm.
var _place_syncing: bool = false

# --- Edit-mode tabs + Waypoints panel (P7) ------------------------------------
# A segmented Objects / Waypoints switch at the top drives the controller's mode; the
# inspector shows the object panels in Objects mode and the waypoint panel in Waypoints
# mode. The waypoint panel lists the paths and reports the selected marker (authoring
# buttons land in later phases). _mode_syncing / _wp_syncing guard programmatic updates.
var _mode_tabs: TabBar
var _mode_syncing: bool = false
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

# --- Mission properties (header) editable form --------------------------------
# A collapsible form for the mission-level header fields. Built ONCE and synced in
# place via _props_binder (FieldBinder), since LineEdits would lose their caret if torn
# down on every `changed`. Setters route through the controller's set_header_* (one undo
# step each). Mission-global, so it shows in both Objects and Waypoints mode.
var _props_toggle: CheckButton
var _props_box: VBoxContainer
var _props_binder: FieldBinder

# --- Weapon loadout + groups (mission-global collapsibles) --------------------
# Like the header form, these are shown whenever a mission is loaded, in any mode, and built
# ONCE. The loadout is a list of (name, value1, value2) records; groups are 64 fixed records
# with three editable ints each. Selection is inspector-local (no in-world interaction). The
# *_syncing guards stop a programmatic repopulate from echoing back as an edit. Name/value
# edits commit on Enter / focus-out (not per keystroke) to keep the caret; group spins commit
# on change. Every commit replaces the whole loadout / writes one group = one undo step.
var _loadout_toggle: CheckButton
var _loadout_box: VBoxContainer
var _loadout_status: Label
var _loadout_list: ItemList
var _loadout_name: LineEdit
var _loadout_value1: LineEdit
var _loadout_value2: LineEdit
var _loadout_delete: Button
var _loadout_selected: int = -1
var _loadout_syncing: bool = false

var _groups_toggle: CheckButton
var _groups_box: VBoxContainer
var _groups_list: ItemList
var _group_spins: Array = []  # [field0, field8, field12]
var _groups_selected: int = -1
var _groups_syncing: bool = false


func setup(controller) -> void:
	_controller = controller
	add_theme_constant_override("margin_left", 12)
	add_theme_constant_override("margin_top", 12)
	add_theme_constant_override("margin_right", 12)
	add_theme_constant_override("margin_bottom", 12)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _root == null:
		var scroll := ScrollContainer.new()
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
		add_child(scroll)
		_root = VBoxContainer.new()
		_root.add_theme_constant_override("separation", 8)
		_root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.add_child(_root)
		_build_mode_tabs()
		_build_edit_panel()
		_build_place_panel()
		_build_waypoint_panel()
		_build_area_trigger_panel()
		_build_props_panel()
		_build_loadout_panel()
		_build_groups_panel()
		_box = VBoxContainer.new()
		_box.add_theme_constant_override("separation", 6)
		_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_root.add_child(_box)
	if _controller != null and not _controller.changed.is_connected(_refresh):
		_controller.changed.connect(_refresh)
	_refresh()


func _refresh() -> void:
	_refresh_mode_tabs()
	_refresh_edit_panel()
	_refresh_place_panel()
	_refresh_waypoint_panel()
	_refresh_area_trigger_panel()
	_refresh_props_panel()
	_refresh_loadout_panel()
	_refresh_groups_panel()
	# Mode gating: the object panels show only in Objects mode; the waypoint / trigger panels
	# hide themselves outside their mode. The read-only summary shows in every mode.
	if _controller != null and not _controller.is_objects_mode():
		_edit_box.visible = false
		_place_box.visible = false
	_rebuild_summary()


# --- Editable selection panel (persistent) ------------------------------------

func _build_edit_panel() -> void:
	_edit_box = VBoxContainer.new()
	_edit_box.add_theme_constant_override("separation", 4)
	_edit_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_edit_box)

	# The identity line is the section heading: it reads the selected model's name (resolved
	# from items.def) prominently, with a muted kind + index subline beneath, so the user
	# sees "Humvee" rather than just "Item #42".
	_identity_label = ObjectUiHelpers.add_section_heading(_edit_box, "Selected entity")
	_identity_sub = ObjectUiHelpers.add_muted_label(_edit_box, "")

	ObjectUiHelpers.add_section_heading(_edit_box, "Position")
	_pos_spins = [
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionPosX", "X", -1000000.0, 1000000.0, 0.001),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionPosY", "Y", -1000000.0, 1000000.0, 0.001),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionPosZ", "Z", -1000000.0, 1000000.0, 0.001),
	]

	ObjectUiHelpers.add_section_heading(_edit_box, "Rotation")
	_rot_spins = [
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionRotPitch", "Pitch", -360.0, 360.0, 1.0),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionRotYaw", "Yaw", -360.0, 360.0, 1.0),
		ObjectUiHelpers.add_spin_row(_edit_box, "MissionRotRoll", "Roll", -360.0, 360.0, 1.0),
	]

	# team / group are stored on every entity kind by the format, so they are shown for
	# all selectable objects; in practice they drive organics (units) at runtime.
	ObjectUiHelpers.add_section_heading(_edit_box, "Faction")
	_team_spin = ObjectUiHelpers.add_spin_row(_edit_box, "MissionTeam", "Team", 0.0, 255.0, 1.0)
	_group_spin = ObjectUiHelpers.add_spin_row(_edit_box, "MissionGroup", "Group", 0.0, 255.0, 1.0)

	_build_behavior_section()

	_animated_note = ObjectUiHelpers.add_muted_label(_edit_box, "Animated object.")

	# Delete sits at the bottom of the edit panel as the one destructive action; the whole
	# panel is hidden when nothing is selected, so the button only shows with a selection.
	# Removing an entity is not saved to the .bms until Save Mission, so an accidental
	# delete is recovered by reopening the mission rather than a modal confirm here.
	_edit_box.add_child(HSeparator.new())
	_delete_button = Button.new()
	_delete_button.name = "MissionDeleteEntity"
	_delete_button.text = "Delete object"
	_delete_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_edit_box.add_child(_delete_button)

	for axis in 3:
		_pos_spins[axis].value_changed.connect(_on_position_axis.bind(axis))
		_rot_spins[axis].value_changed.connect(_on_rotation_axis.bind(axis))
	_team_spin.value_changed.connect(_on_team_changed)
	_group_spin.value_changed.connect(_on_group_changed)
	# Behavior spins wire themselves to the controller through the FieldBinder in
	# _build_behavior_section, so they are not connected here.
	_delete_button.pressed.connect(_on_delete_pressed)


# Build the collapsed-by-default "Behavior" section: a toggle that shows / hides a box of
# the per-entity AI + waypoint fields. Each field is a FieldBinder-bound SpinBox whose
# setter routes through controller.set_selected_property; sync happens in
# _refresh_edit_panel via _behavior_binder.sync_from. Ranges follow the format's field
# widths so a real value is never clamped on display.
func _build_behavior_section() -> void:
	_behavior_binder = FieldBinder.new()
	_behavior_toggle = CheckButton.new()
	_behavior_toggle.name = "MissionBehaviorToggle"
	_behavior_toggle.text = "Behavior"
	_behavior_toggle.tooltip_text = "Per-unit AI and waypoint settings (these mainly drive organic units at runtime)."
	_behavior_toggle.button_pressed = false
	_edit_box.add_child(_behavior_toggle)

	_behavior_box = VBoxContainer.new()
	_behavior_box.add_theme_constant_override("separation", 4)
	_behavior_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_behavior_box.visible = false
	_edit_box.add_child(_behavior_box)
	_behavior_toggle.toggled.connect(func(on: bool) -> void: _behavior_box.visible = on)

	# Waypoint path (waypoint_id) is the bridge: it names which authored path a unit follows.
	var wp_spin := _add_behavior_spin("waypoint_id", "Waypoint path", 0.0, 127.0)
	wp_spin.tooltip_text = "Which waypoint path this unit follows. Author paths in the Waypoints tab."
	_add_behavior_spin("wp_number", "WP number", 0.0, 255.0)
	ObjectUiHelpers.add_section_heading(_behavior_box, "Combat")
	_add_behavior_spin("perception", "Perception", -1000000.0, 1000000.0)
	_add_behavior_spin("accuracy", "Accuracy", -32768.0, 32767.0)
	_add_behavior_spin("alert_state", "Alert state", 0.0, 255.0)
	_add_behavior_spin("min_engagement_distance", "Min combat range", -1000000.0, 1000000.0)
	_add_behavior_spin("max_engagement_distance", "Max combat range", -1000000.0, 1000000.0)
	_add_behavior_spin("max_attack_distance", "Max attack range", -1000000.0, 1000000.0)
	ObjectUiHelpers.add_section_heading(_behavior_box, "Spawning")
	_add_behavior_spin("spawn_count", "Spawn count", -32768.0, 32767.0)
	# no_more_than (byte 74) / no_less_than (byte 75) pair with the RemoveIfMoreThan /
	# RemoveIfLessThan AI flags to gate spawning by player count.
	var nmt := _add_behavior_spin("max_simultaneous", "No more than", 0.0, 255.0)
	nmt.tooltip_text = "Max copies kept (no_more_than, byte 74). Pairs with the RemoveIfMoreThan AI flag."
	var nlt := _add_behavior_spin("no_less_than", "No less than", 0.0, 255.0)
	nlt.tooltip_text = "Min copies kept (no_less_than, byte 75). Pairs with the RemoveIfLessThan AI flag."
	ObjectUiHelpers.add_section_heading(_behavior_box, "Identity")
	var sym := _add_behavior_spin("map_symbol", "Map symbol", 0.0, 255.0)
	sym.tooltip_text = "Tactical-map icon index (byte 81)."
	_add_behavior_line("name1", "AI class",
		func(info) -> String: return String(info.get("name1", "")),
		func(text: String) -> void: _behavior_set_string("name1", text),
		"AI class name (iai_name), max 7 chars. Press Enter to apply.")
	_add_behavior_line("name2", "AI script",
		func(info) -> String: return String(info.get("name2", "")),
		func(text: String) -> void: _behavior_set_string("name2", text),
		"AI script file (ai_textfile), max 7 chars. Press Enter to apply.")
	ObjectUiHelpers.add_section_heading(_behavior_box, "Flags")
	# A 32-bit bitfield: a decimal field is unreadable, so author it as hexadecimal.
	_add_behavior_line("ai_flags", "AI flags",
		func(info) -> String: return "0x%08X" % (int(info.get("ai_flags", 0)) & 0xFFFFFFFF),
		func(text: String) -> void: _ai_flags_set(text),
		"Bitfield of unit AI flags, in hexadecimal (e.g. 0x0000000A). Press Enter to apply.")


func _add_behavior_spin(property: String, label: String, min_value: float, max_value: float) -> SpinBox:
	var spin := ObjectUiHelpers.add_spin_row(_behavior_box, "MissionBeh_" + property, label, min_value, max_value, 1.0)
	_behavior_binder.bind_spin(spin,
		func(info): return float(int(info.get(property, 0))),
		func(value: float) -> void: _behavior_set(property, value))
	return spin


# A text field bound through the FieldBinder (Enter to apply), for a property that reads
# better as text than a number. Used for the ai_flags bitfield, shown as hexadecimal.
func _add_behavior_line(property: String, label: String, getter: Callable, setter: Callable, tooltip: String = "") -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_behavior_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(76, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = "MissionBeh_" + property
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if not tooltip.is_empty():
		line.tooltip_text = tooltip
	row.add_child(line)
	_behavior_binder.bind_line(line, getter, setter)
	return line


func _behavior_set(property: String, value: float) -> void:
	if _controller != null:
		_controller.set_selected_property(property, int(value))


func _behavior_set_string(property: String, value: String) -> void:
	if _controller != null:
		_controller.set_selected_string_property(property, value)


# Apply a hexadecimal ai_flags edit. Pass the value as a signed int32 so the engine's int
# parameter carries the exact 32-bit pattern (the high bit becomes negative and reads back
# as the same bits). An unparseable entry restores the field from the model rather than
# writing garbage.
func _ai_flags_set(text: String) -> void:
	if _controller == null:
		return
	var parsed := _parse_uint32(text)
	if parsed < 0:
		_behavior_binder.sync_from(_controller.get_selected_entity())
		return
	var signed: int = parsed if parsed < 0x80000000 else parsed - 0x100000000
	_controller.set_selected_property("ai_flags", signed)


# Parse a uint32 from "0x..." hex, bare hex, or decimal text. Returns -1 when the text is
# empty, malformed, or out of the 0..0xFFFFFFFF range (the caller treats -1 as "no change").
func _parse_uint32(text: String) -> int:
	var t := text.strip_edges()
	if t.is_empty():
		return -1
	var value := -1
	if t.to_lower().begins_with("0x"):
		if t.is_valid_hex_number(true):
			value = t.hex_to_int()
	elif t.is_valid_int():
		value = t.to_int()
	elif t.is_valid_hex_number(false):
		value = ("0x" + t).hex_to_int()
	if value < 0 or value > 0xFFFFFFFF:
		return -1
	return value


func _refresh_edit_panel() -> void:
	var entity: Dictionary = _controller.get_selected_entity() if _controller != null else {}
	if entity.is_empty():
		_edit_box.visible = false
		return
	_edit_box.visible = true

	# Bracket every .value write: assigning a SpinBox value fires value_changed
	# synchronously, and without the guard each repopulate would re-commit the value
	# back into the model and loop.
	_loading = true
	var kind_index := "%s #%d" % [_kind_label(int(entity.get("kind", -1))), int(entity.get("index", -1))]
	var model_name: String = _controller.get_selected_display_name() if _controller != null else ""
	if model_name.is_empty():
		_identity_label.text = kind_index
		_identity_sub.visible = false
	else:
		_identity_label.text = model_name
		_identity_sub.text = kind_index
		_identity_sub.visible = true
	var pos: Vector3 = entity.get("position", Vector3.ZERO)
	_pos_spins[0].value = pos.x
	_pos_spins[1].value = pos.y
	_pos_spins[2].value = pos.z
	var rot: Vector3 = entity.get("rotation_deg", Vector3.ZERO)
	_rot_spins[0].value = rot.x
	_rot_spins[1].value = rot.y
	_rot_spins[2].value = rot.z
	_team_spin.value = float(int(entity.get("team", 0)))
	_group_spin.value = float(int(entity.get("group", 0)))
	_loading = false

	# The Behavior spins carry their own reentrancy guard (FieldBinder), independent of
	# _loading, so syncing them here cannot echo back as an edit.
	_behavior_binder.sync_from(entity)

	var summary: Dictionary = _controller.get_selection_summary() if _controller != null else {}
	_animated_note.visible = bool(summary.get("animated", false))


func _on_position_axis(value: float, axis: int) -> void:
	if _loading or _controller == null:
		return
	# Read the unchanged axes from the controller (exact), not the sibling SpinBoxes
	# (which may show a step-snapped value), so editing one axis never nudges another.
	var p: Vector3 = _controller.get_selected_position()
	match axis:
		0:
			p.x = value
		1:
			p.y = value
		2:
			p.z = value
	_controller.set_selected_position(p)


func _on_rotation_axis(value: float, axis: int) -> void:
	if _loading or _controller == null:
		return
	var r: Vector3 = _controller.get_selected_rotation()
	match axis:
		0:
			r.x = value
		1:
			r.y = value
		2:
			r.z = value
	_controller.set_selected_rotation(r)


func _on_team_changed(value: float) -> void:
	if _loading or _controller == null:
		return
	_controller.set_selected_team(int(value))


func _on_group_changed(value: float) -> void:
	if _loading or _controller == null:
		return
	_controller.set_selected_group(int(value))


func _on_delete_pressed() -> void:
	if _controller == null:
		return
	# The controller removes the selected entity, re-bakes the world, and fires `changed`;
	# _refresh_edit_panel then hides this panel (nothing is selected after a delete).
	_controller.delete_selected()


# --- Place-object palette (persistent) ----------------------------------------
# A searchable list of placeable items. Selecting one arms placement on the controller;
# a terrain click then places it (handled in the viewport input path). Built once and
# only repopulated when the mission changes (the list can be 1000+ rows), like the edit
# panel above; on other refreshes only the armed-state affordance is synced.

func _build_place_panel() -> void:
	_place_box = VBoxContainer.new()
	_place_box.add_theme_constant_override("separation", 4)
	_place_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_place_box)

	ObjectUiHelpers.add_section_heading(_place_box, "Place object")
	_place_status = ObjectUiHelpers.add_muted_label(_place_box, "")
	_place_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_place_search = LineEdit.new()
	_place_search.placeholder_text = "Search items"
	_place_search.clear_button_enabled = true
	_place_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place_box.add_child(_place_search)

	_place_list = ItemList.new()
	_place_list.select_mode = ItemList.SELECT_SINGLE
	_place_list.custom_minimum_size = Vector2(0, 220)
	_place_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place_box.add_child(_place_list)

	_place_stop = Button.new()
	_place_stop.text = "Stop placing"
	_place_stop.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_place_box.add_child(_place_stop)

	_place_search.text_changed.connect(_on_place_search_changed)
	_place_list.item_selected.connect(_on_place_item_selected)
	_place_stop.pressed.connect(_on_place_stop)


func _refresh_place_panel() -> void:
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_place_box.visible = false
		_place_built_for = null
		_placeable_cache = []
		_placeable_names = {}
		return
	_place_box.visible = true
	# Rebuild the item cache + rows when the mission changes, OR keep retrying while the
	# cache is still empty: a mission can open before its items.def is resolvable (e.g.
	# the resource directory is repointed afterwards), and the palette must not stay
	# stuck empty for the life of that open mission. The common case (cache already
	# populated, same mission) skips this entirely, so edits / placements stay cheap.
	var mission_changed := mission != _place_built_for
	if mission_changed or _placeable_cache.is_empty():
		var fresh: Array = _controller.get_placeable_items()
		if mission_changed or not fresh.is_empty():
			_place_built_for = mission
			_placeable_cache = fresh
			_placeable_names = {}
			for entry in _placeable_cache:
				var id := int(entry.get("id", 0))
				var name := String(entry.get("display_name", ""))
				_placeable_names[id] = name if not name.is_empty() else "item %d" % id
			# Only clear the search when the mission itself changes; a late-arriving item
			# database must not wipe a filter the user is mid-typing.
			if mission_changed:
				_place_search.text = ""
			_populate_palette(_place_search.text)
	_sync_place_affordance()


# Fill the list from the cached placeable items, filtered by a case-insensitive
# substring of the search text. Row order matches _place_row_ids.
func _populate_palette(filter: String) -> void:
	_place_syncing = true
	_place_list.clear()
	_place_row_ids = []
	var needle := filter.strip_edges().to_lower()
	for entry in _placeable_cache:
		var id := int(entry.get("id", 0))
		var label: String = _placeable_names.get(id, "item %d" % id)
		if not needle.is_empty() and not label.to_lower().contains(needle):
			continue
		_place_list.add_item(label)
		_place_row_ids.append(id)
	_place_syncing = false


# Reflect the controller's armed state: select the armed row, show the Stop button, and
# write a status line. Selection changes here never echo (select()/deselect_all() do
# not emit item_selected).
func _sync_place_affordance() -> void:
	var armed_id := int(_controller.get_placement_item_id()) if _controller != null else 0
	var armed := armed_id != 0
	_place_stop.visible = armed

	_place_syncing = true
	var row := _place_row_ids.find(armed_id) if armed else -1
	if row >= 0:
		_place_list.select(row)
	else:
		_place_list.deselect_all()
	_place_syncing = false

	if _placeable_cache.is_empty():
		_place_status.text = "No item database (items.def) found. Set a resource directory in the Terrain workspace, then reopen the mission."
	elif armed:
		_place_status.text = "Placing %s. Click the terrain to place it; right-click or Esc to stop." % _placeable_names.get(armed_id, "item %d" % armed_id)
	elif _place_list.item_count == 0 and not _place_search.text.strip_edges().is_empty():
		# The search filtered everything out: say so, rather than leaving the generic prompt
		# over an empty list (which reads like the mission has no items).
		_place_status.text = "No items match \"%s\". Try a different search." % _place_search.text.strip_edges()
	else:
		_place_status.text = "Pick an item, then click the terrain to place it."


func _on_place_search_changed(text: String) -> void:
	_populate_palette(text)
	_sync_place_affordance()


func _on_place_item_selected(row: int) -> void:
	if _place_syncing or _controller == null:
		return
	if row < 0 or row >= _place_row_ids.size():
		return
	_controller.arm_placement(int(_place_row_ids[row]))


func _on_place_stop() -> void:
	if _controller != null:
		_controller.disarm_placement()


# --- Edit-mode tabs (Objects / Waypoints) -------------------------------------
# The tabs drive the controller's mode; the controller is the single source of truth, so
# _refresh_mode_tabs syncs the current tab back from it (guarded against echo).

# Tab index <-> controller Mode. Tab order: 0 Objects, 1 Waypoints, 2 Triggers (zones).
const _TAB_TO_MODE := [
	MissionController.Mode.OBJECTS,
	MissionController.Mode.WAYPOINTS,
	MissionController.Mode.AREA_TRIGGERS,
]

func _build_mode_tabs() -> void:
	_mode_tabs = TabBar.new()
	_mode_tabs.name = "MissionModeTabs"
	_mode_tabs.add_tab("Objects")
	_mode_tabs.add_tab("Waypoints")
	_mode_tabs.add_tab("Triggers")
	_mode_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_mode_tabs)
	_mode_tabs.tab_changed.connect(_on_mode_tab_changed)


func _on_mode_tab_changed(tab: int) -> void:
	if _mode_syncing or _controller == null:
		return
	if tab >= 0 and tab < _TAB_TO_MODE.size():
		_controller.set_mode(_TAB_TO_MODE[tab])


func _refresh_mode_tabs() -> void:
	if _mode_tabs == null:
		return
	var has_mission := _controller != null and _controller.get_mission() != null
	_mode_tabs.visible = has_mission
	var want := _TAB_TO_MODE.find(_controller.get_mode()) if _controller != null else 0
	if want < 0:
		want = 0
	if _mode_tabs.current_tab != want:
		_mode_syncing = true
		_mode_tabs.current_tab = want
		_mode_syncing = false


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
	_root.add_child(_wp_box)

	ObjectUiHelpers.add_section_heading(_wp_box, "Waypoint paths")
	_wp_status = ObjectUiHelpers.add_muted_label(_wp_box, "")
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

	# Active-path flags. "Loop" is shown (not "DoesNotLoop") so the toggle reads the way the
	# route behaves; the controller inverts it back to the stored bit.
	_wp_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_wp_box, "Path")
	var flags_row := HBoxContainer.new()
	flags_row.add_theme_constant_override("separation", 10)
	_wp_box.add_child(flags_row)
	_wp_loop_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionWpLoop", "Loop")
	_wp_blue_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionWpBlue", "Blue")
	_wp_red_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionWpRed", "Red")
	_wp_loop_check.toggled.connect(_on_wp_flag_toggled)
	_wp_blue_check.toggled.connect(_on_wp_flag_toggled)
	_wp_red_check.toggled.connect(_on_wp_flag_toggled)

	# The active path's markers in route order. Selecting a row selects that marker.
	ObjectUiHelpers.add_section_heading(_wp_box, "Markers")
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

	_wp_box.add_child(HSeparator.new())
	_wp_marker_label = ObjectUiHelpers.add_muted_label(_wp_box, "")
	_wp_marker_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART


func _make_wp_button(parent: Control, node_name: String, text: String) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(button)
	return button


func _on_wp_add_pressed() -> void:
	if _controller == null:
		return
	# The button toggles the add-marker tool: arm it, or disarm if already armed.
	if _controller.is_marker_placement_armed():
		_controller.disarm_marker_placement()
	else:
		_controller.arm_marker_placement()


func _on_wp_move_pressed(delta: int) -> void:
	if _controller != null:
		_controller.move_selected_marker(delta)


func _on_wp_delete_marker_pressed() -> void:
	if _controller != null:
		_controller.delete_selected_marker()


func _on_wp_clear_pressed() -> void:
	if _controller != null:
		_controller.clear_active_path()


func _on_wp_flag_toggled(_pressed: bool) -> void:
	if _wp_flags_syncing or _controller == null:
		return
	_controller.set_waypoint_flags(_wp_loop_check.button_pressed, _wp_blue_check.button_pressed, _wp_red_check.button_pressed)


func _on_wp_marker_row_selected(row: int) -> void:
	if _wp_marker_syncing or _controller == null:
		return
	if row < 0 or row >= _wp_marker_rows.size():
		return
	_controller.select_waypoint_marker(int(_wp_marker_rows[row]))


func _on_wp_path_selected(row: int) -> void:
	if _wp_syncing or _controller == null:
		return
	if row < 0 or row >= _wp_row_paths.size():
		return
	_controller.select_waypoint_path(int(_wp_row_paths[row]))


func _on_wp_new_path_pressed() -> void:
	if _controller != null:
		_controller.select_new_waypoint_path()


func _refresh_waypoint_panel() -> void:
	if _wp_box == null:
		return
	var wp: bool = _controller != null and _controller.is_waypoint_mode() and _controller.get_mission() != null
	_wp_box.visible = wp
	if not wp:
		return

	# List every populated path, plus the active path even when empty (so a freshly chosen
	# path is visible). Rebuilt each refresh: the list is small (<= 128) and changes shape
	# as paths are authored.
	var active := int(_controller.get_selected_waypoint_path_index())
	var summaries: Array = _controller.get_waypoint_summaries()
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
	var active_path: Dictionary = _controller.get_active_waypoint_path()
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

	var mission: NovaMissionData = _controller.get_mission()
	var marker_sel: Dictionary = _controller.get_selected_marker()
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
	var armed: bool = _controller.is_marker_placement_armed()
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
	_root.add_child(_at_box)

	ObjectUiHelpers.add_section_heading(_at_box, "Area triggers / zones")
	_at_status = ObjectUiHelpers.add_muted_label(_at_box, "")
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

	_at_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_at_box, "Bounds (mission units)")
	# The format stores bounds as signed 16.16 fixed-point, so the representable range is
	# ~±32768 mission units; the spins are bounded to that (the lib also clamps on write).
	const ZONE_MIN := -32767.0
	const ZONE_MAX := 32767.0
	_at_min_spins = [
		ObjectUiHelpers.add_spin_row(_at_box, "MissionAtMinX", "Min X", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_box, "MissionAtMinY", "Min Y", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_box, "MissionAtMinZ", "Min Z", ZONE_MIN, ZONE_MAX, 0.001),
	]
	_at_max_spins = [
		ObjectUiHelpers.add_spin_row(_at_box, "MissionAtMaxX", "Max X", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_box, "MissionAtMaxY", "Max Y", ZONE_MIN, ZONE_MAX, 0.001),
		ObjectUiHelpers.add_spin_row(_at_box, "MissionAtMaxZ", "Max Z", ZONE_MIN, ZONE_MAX, 0.001),
	]
	for axis in 3:
		_at_min_spins[axis].value_changed.connect(_on_at_bounds_changed)
		_at_max_spins[axis].value_changed.connect(_on_at_bounds_changed)

	ObjectUiHelpers.add_section_heading(_at_box, "Flags")
	var flags_row := HBoxContainer.new()
	flags_row.add_theme_constant_override("separation", 10)
	_at_box.add_child(flags_row)
	_at_active_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionAtActive", "Active")
	_at_constrain_check = ObjectUiHelpers.add_checkbox(flags_row, "MissionAtConstrainZ", "Constrain height")
	_at_active_check.tooltip_text = "Zone is enforced (flags bit 0x01). When clear, the engine ignores it."
	_at_constrain_check.tooltip_text = "Limit the zone to its Z (height) range (bit 0x02). When clear, the zone is unbounded vertically (+/-16384)."
	_at_active_check.toggled.connect(_on_at_flag_toggled)
	_at_constrain_check.toggled.connect(_on_at_flag_toggled)

	_at_box.add_child(HSeparator.new())
	_at_delete_button = Button.new()
	_at_delete_button.name = "MissionAtDeleteZone"
	_at_delete_button.text = "Delete zone"
	_at_delete_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_at_box.add_child(_at_delete_button)
	_at_delete_button.pressed.connect(_on_at_delete_pressed)


func _on_at_add_pressed() -> void:
	if _controller != null:
		_controller.add_area_trigger_default()


func _on_at_delete_pressed() -> void:
	if _controller != null:
		_controller.delete_selected_area_trigger()


func _on_at_row_selected(row: int) -> void:
	if _at_syncing or _controller == null:
		return
	if row < 0 or row >= _at_rows.size():
		return
	_controller.select_area_trigger(int(_at_rows[row]))


func _on_at_bounds_changed(_value: float) -> void:
	if _at_bounds_syncing or _controller == null:
		return
	var mn := Vector3(_at_min_spins[0].value, _at_min_spins[1].value, _at_min_spins[2].value)
	var mx := Vector3(_at_max_spins[0].value, _at_max_spins[1].value, _at_max_spins[2].value)
	_controller.set_selected_zone_bounds(mn, mx)


func _on_at_flag_toggled(_pressed: bool) -> void:
	if _at_flags_syncing or _controller == null:
		return
	_controller.set_selected_zone_flags(_at_active_check.button_pressed, _at_constrain_check.button_pressed)


func _refresh_area_trigger_panel() -> void:
	if _at_box == null:
		return
	var on: bool = _controller != null and _controller.is_area_trigger_mode() and _controller.get_mission() != null
	_at_box.visible = on
	if not on:
		return

	var zones: Array = _controller.get_area_triggers()
	var selected := int(_controller.get_selected_zone_index())
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
	var sel_zone: Dictionary = _controller.get_selected_zone()
	var has_sel := not sel_zone.is_empty()
	var sel_mn: Vector3 = sel_zone.get("min", Vector3.ZERO)
	var sel_mx: Vector3 = sel_zone.get("max", Vector3.ZERO)
	_at_bounds_syncing = true
	# _sync_spin skips a spin whose inner LineEdit is focused, so a refresh mid-edit (e.g. an undo while
	# the user is typing a bound) does not clobber the in-flight keystroke.
	_sync_spin(_at_min_spins[0], sel_mn.x)
	_sync_spin(_at_min_spins[1], sel_mn.y)
	_sync_spin(_at_min_spins[2], sel_mn.z)
	_sync_spin(_at_max_spins[0], sel_mx.x)
	_sync_spin(_at_max_spins[1], sel_mx.y)
	_sync_spin(_at_max_spins[2], sel_mx.z)
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


# --- Mission properties (header) editable form --------------------------------

func _build_props_panel() -> void:
	_props_binder = FieldBinder.new()
	_props_toggle = CheckButton.new()
	_props_toggle.name = "MissionPropsToggle"
	_props_toggle.text = "Mission properties"
	_props_toggle.tooltip_text = "Mission-level header: title, world, gameplay, game modes."
	_props_toggle.button_pressed = false
	_props_toggle.visible = false
	_root.add_child(_props_toggle)

	_props_box = VBoxContainer.new()
	_props_box.name = "MissionPropsBox"
	_props_box.add_theme_constant_override("separation", 4)
	_props_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.visible = false
	_root.add_child(_props_box)
	_props_toggle.toggled.connect(func(on: bool) -> void: _props_box.visible = on)

	_add_props_line("mission_name", "Name", "Mission title.")
	_add_props_line("designer", "Designer", "Mission author.")
	_add_props_line("briefing", "Briefing", "Mission briefing text.")
	ObjectUiHelpers.add_section_heading(_props_box, "World")
	_add_props_option("climate", "Climate", [[0, "Desert"], [1, "Jungle"], [2, "Snow"]])
	_add_props_option("weather", "Weather", [[0, "Nice day"], [1, "Rainy"], [2, "Snow"]])
	_add_props_option("mission_type", "Type", [[1, "Normal"], [2, "Combat vehicle"], [3, "Tenth Mountain"]])
	ObjectUiHelpers.add_section_heading(_props_box, "Gameplay")
	_add_props_spin("player_health", "Player health", 0.0, 1000000.0)
	_add_props_spin("minutes_per_day", "Minutes / day", 0.0, 65535.0)
	_add_props_spin("max_saves", "Max saves", 0.0, 255.0)
	_add_props_spin("start_time", "Start time", 0.0, 65535.0)
	ObjectUiHelpers.add_section_heading(_props_box, "Game modes")
	_add_props_flag(NovaMissionData.ATTRIB_COOP, "Co-op")
	_add_props_flag(NovaMissionData.ATTRIB_DEATHMATCH, "Deathmatch")
	_add_props_flag(NovaMissionData.ATTRIB_TEAM_DEATHMATCH, "Team deathmatch")
	_add_props_flag(NovaMissionData.ATTRIB_CAPTURE_THE_FLAG, "Capture the flag")
	_add_props_flag(NovaMissionData.ATTRIB_KING_OF_THE_HILL, "King of the hill")
	_add_props_flag(NovaMissionData.ATTRIB_ENABLE_NVG, "Night vision")
	_add_props_flag(NovaMissionData.ATTRIB_ROTATE_MAP_180, "Rotate map 180")
	ObjectUiHelpers.add_section_heading(_props_box, "Audio")
	_add_props_spin("music", "Music track", 0.0, 1000000.0)
	_add_props_spin("reverb", "Reverb", 0.0, 1000000.0)


func _add_props_line(field: String, label: String, tooltip: String = "") -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = "MissionProp_" + field
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(line)
	_props_binder.bind_line(line,
		func(info) -> String: return String(info.get(field, "")),
		func(text: String) -> void: _set_header_string(field, text))
	return line


func _add_props_option(field: String, label: String, choices: Array) -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var option := OptionButton.new()
	option.name = "MissionProp_" + field
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for choice in choices:
		var idx := option.item_count
		option.add_item(String(choice[1]))
		option.set_item_id(idx, int(choice[0]))
	row.add_child(option)
	_props_binder.bind_option(option,
		func(info) -> int: return int(info.get(field, 0)),
		func(value: int) -> void: _set_header_int(field, value))
	return option


func _add_props_spin(field: String, label: String, min_value: float, max_value: float) -> SpinBox:
	var spin := ObjectUiHelpers.add_spin_row(_props_box, "MissionProp_" + field, label, min_value, max_value, 1.0)
	_props_binder.bind_spin(spin,
		func(info) -> float: return float(int(info.get(field, 0))),
		func(value: float) -> void: _set_header_int(field, int(value)))
	return spin


func _add_props_flag(bit: int, label: String) -> CheckBox:
	var check := CheckBox.new()
	check.name = "MissionFlag_%d" % bit
	check.text = label
	_props_box.add_child(check)
	_props_binder.bind_checkbox(check,
		func(info) -> bool: return (int(info.get("attrib_flags", 0)) & bit) != 0,
		func(on: bool) -> void: _set_header_flag(bit, on))
	return check


func _set_header_string(field: String, value: String) -> void:
	if _controller != null:
		_controller.set_header_string(field, value)


func _set_header_int(field: String, value: int) -> void:
	if _controller != null:
		_controller.set_header_int(field, value)


func _set_header_flag(bit: int, on: bool) -> void:
	if _controller != null:
		_controller.set_header_flag(bit, on)


func _refresh_props_panel() -> void:
	if _props_toggle == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_props_toggle.visible = false
		_props_box.visible = false
		return
	_props_toggle.visible = true
	_props_binder.sync_from(mission.get_info())


# --- Weapon loadout (mission-global collapsible) ------------------------------

func _build_loadout_panel() -> void:
	_loadout_toggle = CheckButton.new()
	_loadout_toggle.name = "MissionLoadoutToggle"
	_loadout_toggle.text = "Weapon loadout"
	_loadout_toggle.tooltip_text = "Weapons allowed for this mission. An empty list means no restriction (the game uses its default)."
	_loadout_toggle.button_pressed = false
	_loadout_toggle.visible = false
	_root.add_child(_loadout_toggle)

	_loadout_box = VBoxContainer.new()
	_loadout_box.name = "MissionLoadoutBox"
	_loadout_box.add_theme_constant_override("separation", 4)
	_loadout_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.visible = false
	_root.add_child(_loadout_box)
	# Repopulate on expand (the per-`changed` refresh skips the list rebuild while collapsed).
	_loadout_toggle.toggled.connect(func(on: bool) -> void:
		_loadout_box.visible = on
		if on:
			_refresh_loadout_panel())

	_loadout_status = ObjectUiHelpers.add_muted_label(_loadout_box, "")
	_loadout_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	var add_button := Button.new()
	add_button.name = "MissionLoadoutAdd"
	add_button.text = "Add weapon"
	add_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(add_button)
	add_button.pressed.connect(_on_loadout_add_pressed)

	_loadout_list = ItemList.new()
	_loadout_list.name = "MissionLoadoutList"
	_loadout_list.select_mode = ItemList.SELECT_SINGLE
	_loadout_list.custom_minimum_size = Vector2(0, 120)
	_loadout_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(_loadout_list)
	_loadout_list.item_selected.connect(_on_loadout_row_selected)

	_loadout_box.add_child(HSeparator.new())
	_loadout_name = _add_loadout_line("MissionLoadoutName", "Name",
		"Weapon name, e.g. WPN_CAR15AUTO. Matched against the game's weapon table; unknown names are ignored at load.")
	_loadout_value1 = _add_loadout_line("MissionLoadoutValue1", "Value 1", "First loadout value (usually -1).")
	_loadout_value2 = _add_loadout_line("MissionLoadoutValue2", "Value 2", "Second loadout value (usually -1).")

	_loadout_box.add_child(HSeparator.new())
	_loadout_delete = Button.new()
	_loadout_delete.name = "MissionLoadoutDelete"
	_loadout_delete.text = "Delete weapon"
	_loadout_delete.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(_loadout_delete)
	_loadout_delete.pressed.connect(_on_loadout_delete_pressed)


func _add_loadout_line(node_name: String, label: String, tooltip: String) -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_loadout_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip
	lbl.custom_minimum_size = Vector2(72, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = node_name
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(line)
	# Commit on Enter / focus-out (not per keystroke) so the caret survives the post-commit refresh.
	line.text_submitted.connect(func(_t: String) -> void: _commit_loadout_editors())
	line.focus_exited.connect(_commit_loadout_editors)
	return line


func _on_loadout_add_pressed() -> void:
	if _controller == null:
		return
	var entries: Array = _controller.get_weapon_loadout()
	entries.append({ "name": "", "value1": "-1", "value2": "-1" })
	_loadout_selected = entries.size() - 1
	_controller.set_weapon_loadout(entries)
	_loadout_name.grab_focus()


func _on_loadout_delete_pressed() -> void:
	if _controller == null or _loadout_selected < 0:
		return
	var entries: Array = _controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		return
	entries.remove_at(_loadout_selected)
	_loadout_selected = mini(_loadout_selected, entries.size() - 1)
	_controller.set_weapon_loadout(entries)


func _on_loadout_row_selected(row: int) -> void:
	if _loadout_syncing:
		return
	# Flush a pending edit to the previously-selected row before switching (so a click-away keeps it).
	_commit_loadout_editors()
	_loadout_selected = row
	_refresh_loadout_panel()


func _commit_loadout_editors() -> void:
	if _loadout_syncing or _controller == null or _loadout_selected < 0:
		return
	var entries: Array = _controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		return
	var entry: Dictionary = entries[_loadout_selected]
	# No-op edits add no undo step.
	if String(entry.get("name", "")) == _loadout_name.text \
			and String(entry.get("value1", "")) == _loadout_value1.text \
			and String(entry.get("value2", "")) == _loadout_value2.text:
		return
	entry["name"] = _loadout_name.text
	entry["value1"] = _loadout_value1.text
	entry["value2"] = _loadout_value2.text
	entries[_loadout_selected] = entry
	_controller.set_weapon_loadout(entries)


func _refresh_loadout_panel() -> void:
	if _loadout_toggle == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_loadout_toggle.visible = false
		_loadout_box.visible = false
		return
	_loadout_toggle.visible = true
	# Skip the (relatively heavy) list/editor rebuild while collapsed; the toggle handler refreshes
	# on expand, so an open panel is always current.
	if not _loadout_box.visible:
		return

	var entries: Array = _controller.get_weapon_loadout()
	if _loadout_selected >= entries.size():
		_loadout_selected = entries.size() - 1
	_loadout_syncing = true
	_loadout_list.clear()
	for i in entries.size():
		var e := entries[i] as Dictionary
		_loadout_list.add_item("%s   (%s, %s)" % [String(e.get("name", "")), String(e.get("value1", "")), String(e.get("value2", ""))])
		if i == _loadout_selected:
			_loadout_list.select(i)
	_loadout_syncing = false

	var has_sel := _loadout_selected >= 0 and _loadout_selected < entries.size()
	var sel: Dictionary = entries[_loadout_selected] if has_sel else {}
	_loadout_syncing = true
	_sync_line(_loadout_name, String(sel.get("name", "")))
	_sync_line(_loadout_value1, String(sel.get("value1", "")))
	_sync_line(_loadout_value2, String(sel.get("value2", "")))
	_loadout_syncing = false
	_loadout_name.editable = has_sel
	_loadout_value1.editable = has_sel
	_loadout_value2.editable = has_sel
	_loadout_delete.disabled = not has_sel

	if entries.is_empty():
		_loadout_status.text = "No weapons restricted (mission uses the default loadout). Add a weapon to restrict it."
	elif not has_sel:
		_loadout_status.text = "Select a weapon to edit its name and values."
	else:
		_loadout_status.text = "%d weapon%s in the loadout." % [entries.size(), "" if entries.size() == 1 else "s"]


# Set a LineEdit only if the text differs AND it is not focused, so a programmatic sync (which fires on
# every `changed`, including an undo that lands while the user is mid-edit) never moves the caret or
# clobbers an uncommitted keystroke. The commit-on-Enter/focus-out path reconciles the value on exit.
func _sync_line(line: LineEdit, value: String) -> void:
	if line.text != value and not line.has_focus():
		line.text = value


# SpinBox counterpart of _sync_line: don't overwrite a value the user is mid-typing (its inner LineEdit
# holds the focus). The per-change commit has already pushed any real edit, so skipping the sync is safe.
func _sync_spin(spin: SpinBox, value: float) -> void:
	if spin.get_line_edit().has_focus():
		return
	if spin.value != value:
		spin.value = value


# --- Groups (mission-global collapsible) --------------------------------------

func _build_groups_panel() -> void:
	_groups_toggle = CheckButton.new()
	_groups_toggle.name = "MissionGroupsToggle"
	_groups_toggle.text = "Groups"
	_groups_toggle.tooltip_text = "Per-group data (64 groups). Three raw integer fields per group; their exact meaning is not yet reverse-engineered."
	_groups_toggle.button_pressed = false
	_groups_toggle.visible = false
	_root.add_child(_groups_toggle)

	_groups_box = VBoxContainer.new()
	_groups_box.name = "MissionGroupsBox"
	_groups_box.add_theme_constant_override("separation", 4)
	_groups_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_groups_box.visible = false
	_root.add_child(_groups_box)
	_groups_toggle.toggled.connect(func(on: bool) -> void:
		_groups_box.visible = on
		if on:
			_refresh_groups_panel())

	_groups_list = ItemList.new()
	_groups_list.name = "MissionGroupsList"
	_groups_list.select_mode = ItemList.SELECT_SINGLE
	_groups_list.custom_minimum_size = Vector2(0, 140)
	_groups_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_groups_box.add_child(_groups_list)
	_groups_list.item_selected.connect(_on_group_row_selected)

	_groups_box.add_child(HSeparator.new())
	ObjectUiHelpers.add_section_heading(_groups_box, "Fields (raw)")
	const INT32_MIN := -2147483648.0
	const INT32_MAX := 2147483647.0
	_group_spins = [
		ObjectUiHelpers.add_spin_row(_groups_box, "MissionGroupField0", "Field 0", INT32_MIN, INT32_MAX, 1.0),
		ObjectUiHelpers.add_spin_row(_groups_box, "MissionGroupField8", "Field 8", INT32_MIN, INT32_MAX, 1.0),
		ObjectUiHelpers.add_spin_row(_groups_box, "MissionGroupField12", "Field 12", INT32_MIN, INT32_MAX, 1.0),
	]
	for spin in _group_spins:
		spin.value_changed.connect(_on_group_spin_changed)


func _on_group_row_selected(row: int) -> void:
	if _groups_syncing:
		return
	_groups_selected = row
	_refresh_groups_panel()


func _on_group_spin_changed(_value: float) -> void:
	if _groups_syncing or _controller == null or _groups_selected < 0:
		return
	_controller.set_group(_groups_selected,
		int(_group_spins[0].value), int(_group_spins[1].value), int(_group_spins[2].value))


func _refresh_groups_panel() -> void:
	if _groups_toggle == null:
		return
	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_groups_toggle.visible = false
		_groups_box.visible = false
		return
	_groups_toggle.visible = true
	if not _groups_box.visible:
		return

	var groups: Array = _controller.get_groups()
	_groups_syncing = true
	_groups_list.clear()
	for i in groups.size():
		var g := groups[i] as Dictionary
		_groups_list.add_item("Group %d   (%d, %d, %d)" % [int(g.get("index", i)), int(g.get("field0", 0)), int(g.get("field8", 0)), int(g.get("field12", 0))])
		if i == _groups_selected:
			_groups_list.select(i)
	_groups_syncing = false

	var has_sel := _groups_selected >= 0 and _groups_selected < groups.size()
	var sel: Dictionary = groups[_groups_selected] if has_sel else {}
	_groups_syncing = true
	_sync_spin(_group_spins[0], float(int(sel.get("field0", 0))))
	_sync_spin(_group_spins[1], float(int(sel.get("field8", 0))))
	_sync_spin(_group_spins[2], float(int(sel.get("field12", 0))))
	for spin in _group_spins:
		spin.editable = has_sel
	_groups_syncing = false


# --- Read-only mission summary (rebuilt) --------------------------------------

func _rebuild_summary() -> void:
	if _box == null:
		return
	for child in _box.get_children():
		child.queue_free()

	var mission: NovaMissionData = _controller.get_mission() if _controller != null else null
	if mission == null:
		_add_heading("Mission")
		_add_body("Open a .bms mission to load its terrain, environment, and placed objects.")
		return

	var info := mission.get_info()

	_add_heading(_nonempty(mission.get_mission_name(), "Untitled mission"))
	var designer := mission.get_designer().strip_edges()
	if not designer.is_empty():
		_add_row("Designer", designer)

	# The selected entity has its own editable panel above; here, only prompt when
	# nothing is selected so the viewer always explains how to begin.
	var selection: Dictionary = _controller.get_selection_summary() if _controller != null else {}
	if selection.is_empty():
		_add_separator()
		_add_heading("Selection")
		_add_body("Click an object to select it, then drag it on the terrain or edit its fields above. Save Mission to write changes.")

	_add_separator()
	_add_heading("World")
	_add_row("Terrain", _nonempty(mission.get_terrain_ref(), "(none)"))
	_add_row("Environment", _nonempty(mission.get_environment_ref(), "(none)"))
	_add_row("Climate", str(int(info.get("climate", 0))))
	_add_row("Weather", str(int(info.get("weather", 0))))

	_add_separator()
	_add_heading("Objects")
	var stats: Dictionary = _controller.get_stats()
	_add_row("Placed", str(int(stats.get("placed", 0))))
	_add_row("Batched", "%d in %d draw groups" % [int(stats.get("batched", 0)), int(stats.get("batches", 0))])
	_add_row("Animated", str(int(stats.get("animated", 0))))
	var unresolved := int(stats.get("unresolved", 0))
	if unresolved > 0:
		_add_row("Unresolved", str(unresolved))
	_add_row("Markers (hidden)", str(int(stats.get("markers", 0))))

	_add_separator()
	_add_heading("Entities")
	_add_row("Items", str(mission.get_entity_count(NovaMissionData.KIND_ITEM)))
	_add_row("Buildings", str(mission.get_entity_count(NovaMissionData.KIND_BUILDING)))
	_add_row("Organics", str(mission.get_entity_count(NovaMissionData.KIND_ORGANIC)))
	_add_row("Markers", str(mission.get_entity_count(NovaMissionData.KIND_MARKER)))


# --- Row builders -------------------------------------------------------------

func _add_heading(text: String) -> void:
	var label := Label.new()
	label.text = text
	label.theme_type_variation = &"Heading"
	_box.add_child(label)


func _add_body(text: String) -> void:
	var label := Label.new()
	label.text = text
	label.theme_type_variation = &"Muted"
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_box.add_child(label)


func _add_row(key: String, value: String) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var key_label := Label.new()
	key_label.text = key
	key_label.theme_type_variation = &"Muted"
	key_label.custom_minimum_size = Vector2(128, 0)
	var value_label := Label.new()
	value_label.text = value
	value_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	value_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	row.add_child(key_label)
	row.add_child(value_label)
	_box.add_child(row)


func _add_separator() -> void:
	_box.add_child(HSeparator.new())


func _nonempty(text: String, fallback: String) -> String:
	var trimmed := text.strip_edges()
	return trimmed if not trimmed.is_empty() else fallback


func _kind_label(kind: int) -> String:
	match kind:
		NovaMissionData.KIND_ITEM:
			return "Item"
		NovaMissionData.KIND_BUILDING:
			return "Building"
		NovaMissionData.KIND_ORGANIC:
			return "Organic"
		NovaMissionData.KIND_MARKER:
			return "Marker"
		_:
			return "Entity"
