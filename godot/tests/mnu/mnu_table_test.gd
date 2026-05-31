extends GutTest

# M9.5 gate: NovaMnuTable builds the column template (headers, widths, justify,
# bitmap columns, value->image SUBST) and a host populates rows at runtime. Covers
# row binding, single/multi selection, header-click sort, the SUBST image cell, and
# edit_mode inertness.

const FIXTURE := "res://../fixtures/mnu/all_widgets.mnu"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _build_menu(edit_mode: bool = false) -> NovaMnuMenu:
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(edit_mode)
	menu.menu = _load_doc()
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


func test_table_runtime_rows_and_cells() -> void:
	var menu := _build_menu()
	var table := _table(menu)
	table.add_row_values(["Alpha", "8", "30"])
	table.add_row_values(["Bravo", "12", "45"])
	assert_eq(table.get_row_count(), 2, "host populated two rows")
	var row0 := table.find_child("Row0", true, false)
	assert_not_null(row0, "Row0 built")
	var cell0 := row0.find_child("Cell0", true, false)
	var cell2 := row0.find_child("Cell2", true, false)
	assert_true(cell0 is Label, "text column renders a Label")
	assert_eq((cell0 as Label).text, "Alpha", "cell text")
	assert_true(cell2 is TextureRect, "BODY bitmap_draw column renders a TextureRect")


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
