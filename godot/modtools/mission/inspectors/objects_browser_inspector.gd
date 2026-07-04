extends "res://modtools/mission/inspectors/inspector_section.gd"


# --- Placed-objects browser (persistent) --------------------------------------
# A searchable list of every object already placed in the mission (items / buildings / people).
# Selecting a row selects that entity AND frames the camera on it, so a named unit is found on a
# large map without hunting the viewport. Built once; the (potentially 1000+ row) cache is only
# rebuilt when the object set changes (count / mission) or item names first become resolvable,
# not on every `changed` (which fires on each edit / drag frame).
var _objects_box: VBoxContainer
var _objects_search: LineEdit
var _objects_list: ItemList
var _objects_status: Label
# Cached rows for the current mission: { kind, index, label, category, search }. `label` carries a
# duplicate-name ordinal so two "Soldier" rows are distinguishable; `search` is the lowercase match key.
var _objects_cache: Array = []
# { kind, index } parallel to the currently-shown (filtered) _objects_list rows.
var _objects_rows: Array = []
var _objects_built_for: NovaMissionData
var _objects_built_count: int = -1
# The controller membership revision the rows were built for. Gating on the count alone would miss an
# identity change at a constant total (e.g. an undo/redo that swaps an entity for a different one of the
# same kind, or a future "change item" edit); the revision bumps on every such re-bake, so include it.
var _objects_built_rev: int = -1
var _objects_db_ready: bool = false
# True while programmatically syncing the list selection, so item_selected echoes do not re-select.
var _objects_syncing: bool = false


# --- Placed-objects browser (persistent) --------------------------------------
# A searchable list of every object already in the mission. Selecting a row drives the controller
# to select that entity and frame the camera on it (find-on-map). Built once; the row cache is
# rebuilt only when the object set changes, like the palette below, so edits / drags stay cheap.

func _build_object_browser() -> void:
	_objects_box = VBoxContainer.new()
	_objects_box.add_theme_constant_override("separation", 4)
	_objects_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._root.add_child(_objects_box)

	InspectorForms.add_section_heading(_objects_box, "Placed objects")
	_objects_status = InspectorForms.add_muted_label(_objects_box, "")
	_objects_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	# Show the translate/rotate gizmo on the selected object (drag the arrows to move, the rings to
	# rotate). On by default; turn it off for a plain terrain free-drag.
	var gizmo_check := CheckBox.new()
	gizmo_check.name = "MissionGizmoCheck"
	gizmo_check.text = "Show transform gizmo"
	gizmo_check.tooltip_text = "Show the move/rotate gizmo on the selected object: drag an axis arrow to move it, a ring to rotate it."
	gizmo_check.button_pressed = _inspector._controller == null or _inspector._controller.is_gizmo_enabled()
	_objects_box.add_child(gizmo_check)
	gizmo_check.toggled.connect(func(pressed: bool) -> void:
		if _inspector._controller != null:
			_inspector._controller.set_gizmo_enabled(pressed)
	)

	# Debug: visualize the convex collision hulls that picking tests against, in world.
	var pick_debug := CheckBox.new()
	pick_debug.name = "MissionPickDebugCheck"
	pick_debug.text = "Show pick collision (debug)"
	pick_debug.tooltip_text = "Draw the convex collision hulls used for click-picking, over the placed objects."
	pick_debug.button_pressed = _inspector._controller != null and _inspector._controller.is_pick_debug()
	_objects_box.add_child(pick_debug)
	pick_debug.toggled.connect(func(pressed: bool) -> void:
		if _inspector._controller != null:
			_inspector._controller.set_pick_debug(pressed)
	)

	_objects_search = SearchField.new("Search placed objects")
	_objects_box.add_child(_objects_search)

	_objects_list = ItemList.new()
	_objects_list.select_mode = ItemList.SELECT_SINGLE
	_objects_list.custom_minimum_size = Vector2(0, 260)
	_objects_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_objects_box.add_child(_objects_list)

	_objects_search.text_changed.connect(_on_object_search_changed)
	_objects_list.item_selected.connect(_on_object_row_selected)


func _refresh_object_browser() -> void:
	var mission: NovaMissionData = _inspector._controller.get_mission() if _inspector._controller != null else null
	if mission == null:
		_objects_box.visible = false
		_objects_built_for = null
		_objects_built_count = -1
		_objects_db_ready = false
		_objects_cache = []
		_objects_rows = []
		return
	_objects_box.visible = true
	var count: int = _inspector._controller.get_object_count()
	var rev: int = _inspector._controller.get_membership_revision()
	var db_ready: bool = _inspector._controller.has_item_database()
	var mission_changed := mission != _objects_built_for
	# Rebuild rows when the document, its object set (count), or its composition (membership revision,
	# which bumps on every entity-set re-bake) changes, or whenever the item database's readiness flips
	# in EITHER direction (names become resolvable -> replace "Item <id>" placeholders; or the resource
	# dir is cleared/repointed and they must fall back to placeholders again).
	if mission_changed or count != _objects_built_count or rev != _objects_built_rev or db_ready != _objects_db_ready:
		_objects_built_for = mission
		_objects_built_count = count
		_objects_built_rev = rev
		_objects_db_ready = db_ready
		_rebuild_object_cache()
		if mission_changed:
			_objects_search.text = ""
		_populate_object_list(_objects_search.text)
	_sync_object_selection()


# Pull the flat object list from the controller and bake per-row display labels + search keys
# once. Duplicate base names get a "(n)" ordinal so repeated units are distinguishable in the list.
func _rebuild_object_cache() -> void:
	_objects_cache = []
	var rows: Array = _inspector._controller.get_object_list() if _inspector._controller != null else []
	var name_total: Dictionary = {}
	for r in rows:
		var base := _object_base_label(r)
		name_total[base] = int(name_total.get(base, 0)) + 1
	var name_seen: Dictionary = {}
	for r in rows:
		var base := _object_base_label(r)
		var label := base
		if int(name_total.get(base, 0)) > 1:
			var n := int(name_seen.get(base, 0)) + 1
			name_seen[base] = n
			label = "%s (%d)" % [base, n]
		var category := String(r.get("category", "Object"))
		_objects_cache.append({
			"kind": int(r.get("kind", 0)),
			"index": int(r.get("index", 0)),
			"label": label,
			"category": category,
			"search": (label + " " + category).to_lower(),
		})


# A row's base label: the resolved model name, or an "Item <id>" placeholder when the name is
# not resolvable (no item database yet, or an id the database does not carry).
func _object_base_label(row: Dictionary) -> String:
	var nm := String(row.get("name", "")).strip_edges()
	if not nm.is_empty():
		return nm
	return "Item %d" % int(row.get("item_id", 0))


# Fill the list from the cache, filtered by a case-insensitive substring of the search text.
# Row order matches _objects_rows.
func _populate_object_list(filter: String) -> void:
	_objects_syncing = true
	_objects_list.clear()
	_objects_rows = []
	var needle := filter.strip_edges().to_lower()
	for entry in _objects_cache:
		if not needle.is_empty() and not String(entry["search"]).contains(needle):
			continue
		var row := _objects_list.add_item(String(entry["label"]))
		_objects_list.set_item_tooltip(row, String(entry["category"]))
		_objects_rows.append({ "kind": int(entry["kind"]), "index": int(entry["index"]) })
	_objects_syncing = false
	_update_object_status()


func _update_object_status() -> void:
	var total := _objects_cache.size()
	var shown := _objects_list.item_count
	if total == 0:
		_objects_status.text = "No objects placed yet. Use the palette below to add some."
	elif shown == 0:
		_objects_status.text = "No objects match \"%s\"." % _objects_search.text.strip_edges()
	elif shown == total:
		_objects_status.text = "%d placed object%s. Click one to jump to it." % [total, "" if total == 1 else "s"]
	else:
		_objects_status.text = "Showing %d of %d. Click one to jump to it." % [shown, total]


# Highlight the row for the controller's current selection (so a viewport pick lights up the
# matching list row) and scroll it into view. A selection of another kind (e.g. a marker) or
# nothing clears the list selection. select()/deselect_all() do not emit item_selected.
func _sync_object_selection() -> void:
	var summary: Dictionary = _inspector._controller.get_selection_summary() if _inspector._controller != null else {}
	_objects_syncing = true
	var found := -1
	if not summary.is_empty():
		var want_kind := int(summary.get("kind", -1))
		var want_index := int(summary.get("index", -1))
		for i in _objects_rows.size():
			var r: Dictionary = _objects_rows[i]
			if int(r["kind"]) == want_kind and int(r["index"]) == want_index:
				found = i
				break
	if found >= 0:
		_objects_list.select(found)
		_objects_list.ensure_current_is_visible()
	else:
		_objects_list.deselect_all()
	_objects_syncing = false


func _on_object_search_changed(text: String) -> void:
	_populate_object_list(text)
	_sync_object_selection()


func _on_object_row_selected(row: int) -> void:
	if _objects_syncing or _inspector._controller == null:
		return
	if row < 0 or row >= _objects_rows.size():
		return
	var r: Dictionary = _objects_rows[row]
	_inspector._controller.select_object(int(r["kind"]), int(r["index"]))
