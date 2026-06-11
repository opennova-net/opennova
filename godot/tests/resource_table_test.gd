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
