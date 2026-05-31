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

var _controller  # MissionController (preloaded, no class_name)
var _root: VBoxContainer
var _edit_box: VBoxContainer
var _box: VBoxContainer

# True while programmatically repopulating the edit widgets, so the value_changed
# handlers ignore the echo and do not re-commit (and re-emit) what they just read.
var _loading: bool = false

var _identity_label: Label
var _animated_note: Label
var _pos_spins: Array = []  # [x, y, z]
var _rot_spins: Array = []  # [pitch, yaw, roll]
var _team_spin: SpinBox
var _group_spin: SpinBox
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
		_build_edit_panel()
		_build_place_panel()
		_box = VBoxContainer.new()
		_box.add_theme_constant_override("separation", 6)
		_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_root.add_child(_box)
	if _controller != null and not _controller.changed.is_connected(_refresh):
		_controller.changed.connect(_refresh)
	_refresh()


func _refresh() -> void:
	_refresh_edit_panel()
	_refresh_place_panel()
	_rebuild_summary()


# --- Editable selection panel (persistent) ------------------------------------

func _build_edit_panel() -> void:
	_edit_box = VBoxContainer.new()
	_edit_box.add_theme_constant_override("separation", 4)
	_edit_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root.add_child(_edit_box)

	ObjectUiHelpers.add_section_heading(_edit_box, "Selected entity")
	_identity_label = ObjectUiHelpers.add_muted_label(_edit_box, "")

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
	_delete_button.pressed.connect(_on_delete_pressed)


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
	_identity_label.text = "%s #%d" % [_kind_label(int(entity.get("kind", -1))), int(entity.get("index", -1))]
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
		_place_status.text = "No item database (items.def) was found in the resource directory."
	elif armed:
		_place_status.text = "Placing %s. Click the terrain to place it; right-click or Esc to stop." % _placeable_names.get(armed_id, "item %d" % armed_id)
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
