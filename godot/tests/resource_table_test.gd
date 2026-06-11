extends GutTest

# ResourceTable: the shared sortable resource list (modal picker + browser
# pane). Net-new unit coverage - the old browser's internals were only pinned
# through the shell dialog tests.

const ResourceTableScript = preload("res://modtools/framework/resource_table.gd")


func _entries() -> Array:
	return [
		{"display_name": "bravo.trn", "relative_path": "bravo.trn", "path": "C:/root/bravo.trn", "size_bytes": 300, "modified_time": 30},
		{"display_name": "alpha.env", "relative_path": "alpha.env", "path": "C:/root/alpha.env", "size_bytes": 100, "modified_time": 10},
		{"display_name": "charlie.bms", "relative_path": "missions/charlie.bms", "path": "C:/root/missions/charlie.bms", "size_bytes": 200, "modified_time": 20},
	]


func _make_table() -> ResourceTable:
	var table: ResourceTable = ResourceTableScript.new()
	add_child_autofree(table)
	return table


func _visible_names(table: ResourceTable) -> Array:
	var names := []
	var root := table.tree.get_root()
	var item := root.get_first_child() if root != null else null
	while item != null:
		names.append(item.get_text(ResourceTableScript.COLUMN_NAME))
		item = item.get_next()
	return names


func test_sorts_by_name_ascending_by_default() -> void:
	var table := _make_table()
	table.set_entries(_entries())
	assert_eq(_visible_names(table), ["alpha.env", "bravo.trn", "charlie.bms"])


func test_column_click_sorts_and_toggles_direction() -> void:
	var table := _make_table()
	table.set_entries(_entries())
	table._on_column_title_clicked(ResourceTableScript.COLUMN_SIZE, MOUSE_BUTTON_LEFT)
	assert_eq(_visible_names(table), ["alpha.env", "charlie.bms", "bravo.trn"], "size ascending")
	table._on_column_title_clicked(ResourceTableScript.COLUMN_SIZE, MOUSE_BUTTON_LEFT)
	assert_eq(_visible_names(table), ["bravo.trn", "charlie.bms", "alpha.env"], "second click flips to descending")


func test_search_filters_name_and_relative_path() -> void:
	var table := _make_table()
	table.set_entries(_entries())
	var counts: Array = []
	table.list_changed.connect(func(n: int) -> void: counts.append(n))
	table.search.text = "missions"
	table.refresh()
	assert_eq(_visible_names(table), ["charlie.bms"], "the filter searches the relative path too")
	assert_eq(counts.back(), 1, "list_changed reports the post-filter count")


func test_activation_emits_the_selected_entry() -> void:
	var table := _make_table()
	table.set_entries(_entries())
	var activated: Array = []
	table.entry_activated.connect(func(entry: Dictionary) -> void: activated.append(entry))
	# set_entries auto-selects the first (sorted) row for the keyboard flow.
	table.tree.item_activated.emit()
	assert_eq(activated.size(), 1, "activation emits exactly once")
	assert_eq(String((activated[0] as Dictionary).get("display_name", "")), "alpha.env",
		"the payload is the selected entry's dictionary")


func test_selection_changed_reports_emptiness() -> void:
	var table := _make_table()
	var states: Array = []
	table.selection_changed.connect(func(has_selection: bool) -> void: states.append(has_selection))
	table.set_entries([])
	assert_eq(states.back(), false, "an empty list reports no selection")
	table.set_entries(_entries())
	assert_eq(states.back(), true, "a populated list auto-selects the first row")


func test_drag_source_is_opt_in_and_returns_nothing_by_default() -> void:
	var table := _make_table()
	table.set_entries(_entries())
	assert_null(table._get_tree_drag_data(Vector2.ZERO),
		"without enable_drag_source the table never produces drag data (the modal stays a non-source)")


func test_enabled_drag_packs_the_selected_rows_payload() -> void:
	var table := _make_table()
	var provided: Array = []
	table.enable_drag_source(func(entry: Dictionary) -> Variant:
		provided.append(entry)
		if String(entry.get("display_name", "")) == "bravo.trn":
			return null  # the provider can veto rows
		return {"from": String(entry.get("relative_path", ""))})
	table.set_entries(_entries())
	# set_entries auto-selects the first sorted row (alpha.env); Vector2.ZERO
	# misses every row, exercising the headless selected-row fallback.
	var data: Variant = table._get_tree_drag_data(Vector2.ZERO)
	assert_eq(data, {"from": "alpha.env"}, "the provider's data for the selected row is the drag data")
	assert_eq(provided.size(), 1, "the provider runs once per drag")

	var root := table.tree.get_root()
	var second := root.get_first_child().get_next()
	second.select(ResourceTableScript.COLUMN_NAME)
	assert_null(table._get_tree_drag_data(Vector2.ZERO), "a provider null vetoes the drag")
