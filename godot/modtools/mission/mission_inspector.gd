extends MarginContainer

# Read-only inspector for the Mission workspace: the open mission's metadata, the
# world it references, and a summary of what got placed. Rebuilds whenever the
# controller signals a change (mission loaded / cleared). Authoring UI is
# deferred; this is a viewer. Referenced via preload (no class_name).

var _controller  # MissionController (preloaded, no class_name)
var _box: VBoxContainer


func setup(controller) -> void:
	_controller = controller
	add_theme_constant_override("margin_left", 12)
	add_theme_constant_override("margin_top", 12)
	add_theme_constant_override("margin_right", 12)
	add_theme_constant_override("margin_bottom", 12)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _box == null:
		_box = VBoxContainer.new()
		_box.add_theme_constant_override("separation", 6)
		_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		add_child(_box)
	if _controller != null and not _controller.changed.is_connected(_rebuild):
		_controller.changed.connect(_rebuild)
	_rebuild()


func _rebuild() -> void:
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

	_add_separator()
	_add_heading("Selection")
	var selection: Dictionary = _controller.get_selection_summary() if _controller != null else {}
	if selection.is_empty():
		_add_body("Click an object to select it, then drag to move it onto the terrain. Save Mission to write changes.")
	else:
		_add_row("Entity", "%s #%d" % [_kind_label(int(selection.get("kind", -1))), int(selection.get("index", -1))])
		var pos: Vector3 = selection.get("position", Vector3.ZERO)
		_add_row("Position", "%.1f, %.1f, %.1f" % [pos.x, pos.y, pos.z])
		if bool(selection.get("animated", false)):
			_add_row("Type", "animated")

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
