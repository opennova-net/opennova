extends GutTest

# M9.5 gate: NovaMnuTable builds the column template (headers, widths, justify,
# bitmap columns, value->image SUBST) and the shell populates rows at runtime. Covers
# row binding, single/multi selection, header-click sort, the SUBST image cell, and
# edit_mode inertness.

const FIXTURE := "res://../fixtures/mnu/all_widgets.mnu"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _find_widget(doc: NovaMnuDocument, id: int, wanted: String) -> int:
	if doc.get_widget_name(id) == wanted:
		return id
	for child in doc.get_child_ids(id):
		var found := _find_widget(doc, child, wanted)
		if found >= 0:
			return found
	return -1


func _widget_named(doc: NovaMnuDocument, wanted: String) -> int:
	for screen_id in doc.get_screen_ids():
		var found := _find_widget(doc, doc.get_screen_root_id(screen_id), wanted)
		if found >= 0:
			return found
	return -1


func _build_menu(edit_mode: bool = false, doc: NovaMnuDocument = null) -> NovaMnuMenu:
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(edit_mode)
	menu.menu = doc if doc != null else _load_doc()
	return menu


func _table(menu: NovaMnuMenu) -> NovaMnuTable:
	return menu.find_child("MissionTable", true, false) as NovaMnuTable


func _write_png(path: String, w: int, h: int, c: Color) -> void:
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	img.fill(c)
	img.save_png(path)


func test_table_builds_columns_and_headers() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	assert_not_null(table, "MissionTable built")
	assert_true(table is Control, "NovaMnuTable is a Control")
	assert_eq(table.get_column_count(), 3, "three columns from COLUMN count")
	var h0 := table.find_child("Header0", true, false)
	var h1 := table.find_child("Header1", true, false)
	assert_true(h0 is BaseButton, "sortable header (sort=A) is a Button")
	assert_eq((h0 as Button).text, "Name", "header text")
	assert_true(h1 is Label and not (h1 is BaseButton), "non-sortable header is a Label")


func test_table_preview_honors_cleared_presence_with_latent_values() -> void:
	var doc := _load_doc()
	var id := _widget_named(doc, "MissionTable")
	assert_gte(id, 0)
	var state: Dictionary = doc.get_widget_authoring_state(id)
	var headers: Array = (state["table"] as Dictionary)["headers"]
	headers = headers.duplicate(true)
	headers[0]["has_width"] = false # retain latent width=120
	assert_true(doc.apply_widget_patch(id, {
		"items": {"present": false},
		"table": {
			"has_count": false, # retain latent count=3
			"has_spacing": false, # retain latent spacing=4
			"has_min_item_height": false, # retain latent height=18
			"headers": headers,
		},
	}))
	var table := _table(_build_menu(false, doc))
	assert_eq(table.get_column_count(), 3,
		"absent COUNT falls back to modeled header rows, not its latent scalar")
	var h1 := table.find_child("Header1", true, false) as Control
	assert_eq(h1.position.x, 80.0,
		"absent WIDTH uses the runtime default and absent SPACING contributes zero")
	assert_null(table.find_child("HeaderRule", true, false),
		"an absent ITEMS container does not leak its latent outline color")
	table.add_row_values(["A", "1", ""])
	table.add_row_values(["B", "2", ""])
	var row0 := table.find_child("Row0", true, false) as Control
	assert_eq(row0.size.y, 16.0,
		"absent MIN_ITEM_HEIGHT does not leak its latent authored value")
	table.select_row(0)
	table.select_row(1, true)
	assert_eq(table.get_selected_rows(), PackedInt32Array([1]),
		"an absent ITEMS container does not leak latent MULTISELECT")


func test_table_runtime_rows_and_cells() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	table.add_row_values(["Alpha", "8", "30"])
	table.add_row_values(["Bravo", "12", "45"])
	assert_eq(table.get_row_count(), 2, "shell populated two rows")
	var row0 := table.find_child("Row0", true, false)
	assert_not_null(row0, "Row0 built")
	var cell0 := row0.find_child("Cell0", true, false)
	var cell2 := row0.find_child("Cell2", true, false)
	assert_true(cell0 is Label, "text column renders a Label")
	assert_eq((cell0 as Label).text, "Alpha", "cell text")
	assert_true(cell2 is TextureRect, "BODY bitmap_draw column renders a TextureRect")


func test_table_honors_header_body_vjustify_and_bitmap_scale_policy() -> void:
	var doc := _load_doc()
	var id := _widget_named(doc, "MissionTable")
	var headers := doc.get_table_headers(id)
	var sortable_header: Dictionary = headers[0]
	sortable_header["vjustify"] = "TOP"
	doc.set_table_header(id, 0, sortable_header)
	assert_gte(doc.add_table_body(id, {
		"has_column": true,
		"column": 0,
		"justify": "LEFT",
		"vjustify": "BOTTOM",
	}), 0)
	var bitmap_body: Dictionary = doc.get_table_bodies(id)[0]
	bitmap_body["vjustify"] = "TOP"
	bitmap_body["scale_bitmap"] = false
	doc.set_table_body(id, 0, bitmap_body)

	var table := _table(_build_menu(false, doc))
	var header_text := table.find_child("HeaderText", true, false) as Label
	assert_not_null(header_text, "sortable header exposes a vertically aligned text layer")
	if header_text != null:
		assert_eq(header_text.vertical_alignment, VERTICAL_ALIGNMENT_TOP,
			"HEADER vjustify applies to sortable headers")
	table.add_row_values(["Alpha", "8", ""])
	var row := table.find_child("Row0", true, false)
	assert_eq((row.find_child("Cell0", false, false) as Label).vertical_alignment,
		VERTICAL_ALIGNMENT_BOTTOM, "text BODY vjustify reaches the rendered cell")
	var bitmap := row.find_child("Cell2", false, false) as TextureRect
	assert_eq(bitmap.stretch_mode, TextureRect.STRETCH_KEEP_CENTERED,
		"BITMAP_DRAW without SCALE_BITMAP preserves native image size")

	bitmap_body["scale_bitmap"] = true
	doc.set_table_body(id, 0, bitmap_body)
	var scaled_table := _table(_build_menu(false, doc))
	scaled_table.add_row_values(["Alpha", "8", ""])
	var scaled := scaled_table.find_child("Row0", true, false).find_child(
		"Cell2", false, false) as TextureRect
	assert_eq(scaled.stretch_mode, TextureRect.STRETCH_SCALE,
		"SCALE_BITMAP has a distinct fill-cell stretch policy")


func test_table_custom_draw_requests_a_fresh_shell_slot() -> void:
	var doc := _load_doc()
	var id := _widget_named(doc, "MissionTable")
	var body: Dictionary = doc.get_table_bodies(id)[0]
	body["custom_draw"] = true
	doc.set_table_body(id, 0, body)
	var table := _table(_build_menu(false, doc))
	var requests: Array = []
	table.custom_cell_requested.connect(func(row: int, column: int, value: String, slot: Control) -> void:
		requests.append([row, column, value, slot])
		var content := Label.new()
		content.name = "ShellContent"
		content.text = value
		slot.add_child(content)
	)

	table.add_row_values(["Alpha", "8", "lan"])
	assert_eq(requests.size(), 1, "one request emitted for the CUSTOM_DRAW column")
	assert_eq(requests[0][0], 0)
	assert_eq(requests[0][1], 2)
	assert_eq(requests[0][2], "lan", "effective cell value crosses the shell seam")
	var first_slot := requests[0][3] as Control
	assert_eq(first_slot.name, "Cell2", "table owns and sizes the custom cell slot")
	assert_not_null(first_slot.find_child("ShellContent", false, false),
		"shell can populate the synchronous slot")

	table.rebuild()
	assert_eq(requests.size(), 2, "rebuild requests fresh custom content")
	var second_slot := requests[1][3] as Control
	assert_ne(second_slot, first_slot, "rebuilt rows never expose stale slots")
	assert_not_null(second_slot.find_child("ShellContent", false, false))


func test_table_selection_single_and_multi() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	table.add_row_values(["A", "1", ""])
	table.add_row_values(["B", "2", ""])
	table.add_row_values(["C", "3", ""])
	watch_signals(table)
	table.select_row(1)
	assert_eq(table.get_selected_row(), 1, "single selection")
	assert_signal_emitted(table, "row_selected")
	# Fixture marks the table MULTISELECT, so additive selection accumulates.
	table.select_row(0, true)
	assert_eq(table.get_selected_rows(), PackedInt32Array([0, 1]), "additive multi-select")


func test_table_row_input_selects_and_activates() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	table.add_row_values(["A", "1", ""])
	var row0 := table.find_child("Row0", true, false)
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	row0.emit_signal("gui_input", ev)
	assert_eq(table.get_selected_row(), 0, "click selected the row")

	var fresh := table.find_child("Row0", true, false)  # rebuilt after selection
	var dbl := InputEventMouseButton.new()
	dbl.button_index = MOUSE_BUTTON_LEFT
	dbl.pressed = true
	dbl.double_click = true
	watch_signals(table)
	fresh.emit_signal("gui_input", dbl)
	assert_signal_emitted(table, "row_activated")


func test_table_sort_by_column() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	table.add_row_values(["Charlie", "1", ""])
	table.add_row_values(["Alpha", "2", ""])
	table.add_row_values(["Bravo", "3", ""])
	watch_signals(table)
	table.sort_by_column(0, true)
	assert_signal_emitted_with_parameters(table, "column_sorted", [0, true])
	var row0 := table.find_child("Row0", true, false)
	var cell0 := row0.find_child("Cell0", true, false) as Label
	assert_eq(cell0.text, "Alpha", "rows sorted ascending by column 0")


func test_table_subst_value_to_image() -> void:
	var dir := OS.get_temp_dir().path_join("mnu_m9_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_write_png(dir.path_join("ping_lan.png"), 8, 8, Color(0, 1, 0, 1))
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		pass_test("temp resource root unavailable: %s" % root.get_last_error())
		return

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.menu = _load_doc()

	var table := _table(menu)
	table.add_row_values(["Srv", "4", ""])
	# Column 2 has a SUBST: value "lan" -> ping_lan.tga.
	table.set_cell_value(0, 2, "lan")
	var cell2 := table.find_child("Row0", true, false).find_child("Cell2", true, false)
	assert_true(cell2 is TextureRect, "bitmap column is a TextureRect")
	assert_not_null((cell2 as TextureRect).texture, "SUBST resolved value 'lan' to its image")


func test_table_add_rows_bulk() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	var batch: Array[PackedStringArray] = [
		PackedStringArray(["A", "1", ""]),
		PackedStringArray(["B", "2", ""]),
		PackedStringArray(["C", "3", ""]),
	]
	table.add_rows(batch)
	assert_eq(table.get_row_count(), 3, "add_rows appended all rows in one call")
	var cell := table.find_child("Row2", true, false).find_child("Cell0", true, false) as Label
	assert_eq(cell.text, "C", "bulk-added cell text")


# The Options -> Controls (CONTROL_MAPPING) catalog model: byte-exact JO defaults,
# class grouping, and the player-facing visibility filter. [orig:
# UI_PopulateControlMappingList @ 0x55c0c0; aAbsoluteTurnLe @ 0x8159cb]
func test_controls_model_keyboard_defaults() -> void:
	var model := NovaControlsModel.new()
	var rows: Array = model.get_rows(NovaControlsModel.DEVICE_KEYBOARD)
	assert_gt(rows.size(), 40, "keyboard catalog populated")
	var found_forward := false
	for r in rows:
		var cells := r as PackedStringArray
		assert_eq(cells.size(), 3, "row is [class, action, control]")
		if cells[1] == "Forward":
			found_forward = true
			assert_eq(cells[0], "Movement", "Forward is in the Movement class")
			assert_eq(cells[2], "W or Up", "Forward default keyboard binding")
		# Admin/internal classes are hidden from the player-facing remap table.
		assert_ne(cells[0], "Server", "admin class hidden")
		assert_ne(cells[0], "Cheat", "cheat class hidden")
	assert_true(found_forward, "Forward row present in the keyboard rows")


func test_table_inert_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var table := _table(menu)
	# edit_mode seeds sample rows so the table reads in the canvas.
	assert_eq(table.get_row_count(), 3, "sample rows seeded while authoring")
	var h0 := table.find_child("Header0", true, false)
	assert_true((h0 as BaseButton).disabled, "sortable header disabled while authoring")
	var row0 := table.find_child("Row0", true, false)
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	row0.emit_signal("gui_input", ev)
	assert_eq(table.get_selected_row(), -1, "row click does not select in edit_mode")
