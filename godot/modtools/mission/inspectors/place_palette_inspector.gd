extends "res://modtools/mission/inspectors/inspector_section.gd"


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


# --- Place-object palette (persistent) ----------------------------------------
# A searchable list of placeable items. Selecting one arms placement on the controller;
# a terrain click then places it (handled in the viewport input path). Built once and
# only repopulated when the mission changes (the list can be 1000+ rows), like the edit
# panel above; on other refreshes only the armed-state affordance is synced.

func _build_place_panel() -> void:
	_place_box = VBoxContainer.new()
	_place_box.add_theme_constant_override("separation", 4)
	_place_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector._root.add_child(_place_box)

	InspectorForms.add_section_heading(_place_box, "Place object")
	_place_status = InspectorForms.add_muted_label(_place_box, "")
	_place_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_place_search = SearchField.new("Search items")
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
	var mission: NovaMissionData = _inspector._controller.get_mission() if _inspector._controller != null else null
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
		var fresh: Array = _inspector._controller.get_placeable_items()
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
	var armed_id := int(_inspector._controller.get_placement_item_id()) if _inspector._controller != null else 0
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
	if _place_syncing or _inspector._controller == null:
		return
	if row < 0 or row >= _place_row_ids.size():
		return
	_inspector._controller.arm_placement(int(_place_row_ids[row]))


func _on_place_stop() -> void:
	if _inspector._controller != null:
		_inspector._controller.disarm_placement()
