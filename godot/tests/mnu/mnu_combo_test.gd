extends GutTest

# M9.4 gate: NovaMnuCombo builds a closed TextureButton + selected-text label and an
# in-tree layered popup styled from LIST_BOX. Options seed from the file or a host
# populates them at runtime; selection relays through the menu. The popup never opens
# in edit_mode.

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


func _combo(menu: NovaMnuMenu) -> NovaMnuCombo:
	return menu.find_child("ServerList", true, false) as NovaMnuCombo


func test_combo_builds_and_seeds() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	assert_not_null(combo, "ServerList built")
	assert_true(combo is TextureButton, "NovaMnuCombo is a TextureButton")
	assert_eq(combo.get_item_count(), 3, "three LIST_BOX items seeded")
	assert_eq(combo.get_item_text(0), "Easy Server", "first option text")
	assert_eq(combo.get_item_value(0), "0", "first option value")
	assert_eq(combo.get_selected(), 0, "first option selected by default")
	var label := combo.find_child("SelectedText", true, false) as Label
	assert_eq(label.text, "Easy Server", "closed label shows the selection")


func test_combo_runtime_populate_and_select() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.set_items(["Alpha", "Bravo"])  # host repopulates (server browser refresh)
	assert_eq(combo.get_item_count(), 2, "set_items replaced the seed")
	watch_signals(menu)
	watch_signals(combo)
	combo.select(1)
	assert_eq(combo.get_selected(), 1, "selection updated")
	assert_signal_emitted_with_parameters(combo, "item_selected", [1, ""])
	assert_signal_emitted_with_parameters(
		menu, "widget_value_changed", ["ServerList", "combo", 1, "Bravo"])


func test_combo_popup_opens_and_closes() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	assert_false(combo.is_popup_open(), "popup starts closed")
	combo.open_popup()
	assert_true(combo.is_popup_open(), "popup open")
	assert_not_null(combo.find_child("Popup", true, false), "popup node built")
	assert_not_null(combo.find_child("Item0", true, false), "row built for each item")
	assert_not_null(combo.find_child("Item2", true, false), "last row built")
	combo.close_popup()
	assert_false(combo.is_popup_open(), "popup closed")


func test_combo_row_click_selects_and_closes() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.open_popup()
	var row := combo.find_child("Item1", true, false)
	row.emit_signal("pressed")  # click the second option
	assert_eq(combo.get_selected(), 1, "row click selected the option")
	assert_eq(combo.get_item_text(1), "Hard Server", "expected option")
	assert_false(combo.is_popup_open(), "popup closes after a pick")


func test_combo_popup_suppressed_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var combo := _combo(menu)
	assert_true((combo as BaseButton).disabled, "closed combo disabled while authoring")
	combo.open_popup()
	assert_false(combo.is_popup_open(), "popup never opens in edit_mode")
