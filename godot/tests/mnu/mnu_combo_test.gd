extends GutTest

# M9.4 gate: NovaMnuCombo builds a closed TextureButton + selected-text label and an
# in-tree popup styled from LIST_BOX. Options seed from the file or the shell populates
# them at runtime; selection relays through the menu. The popup never opens in
# edit_mode.
#
# Dropdown input routing (2026-07-16 grill, docs/mnu/menu-re.md D-MNU-11): while a
# dropdown is open the original routes mouse input EXCLUSIVELY to the open list
# [orig: dispatch_mouse_event @ 0x63ab00; scene_end_frame @ 0x63e600;
# CWnd_IsVisibleInHierarchy @ 0x646290], only one dropdown is open per scene
# [orig: combobox_handle_event @ 0x65c190 (@ 0x65c210)], and a press outside both
# the closed cell and the list closes it, consumed [@ 0x65c290]. The reimpl parks
# that as a full-menu catcher overlay added as the menu's LAST child, so it wins
# Godot picking/draw by tree order; the tests below pin the structure and the
# close semantics against the shipped jo_player.mnu combo stack the bug reproduced
# on (NATIONALITY / DIVISION / COMBO_LIST all open at the same authored rect).

const FIXTURE := "res://../fixtures/mnu/all_widgets.mnu"
const PLAYER_FIXTURE := "res://../fixtures/mnu/jo_player.mnu"


func _load_doc(path: String = FIXTURE) -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(path))
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


func _build_doc_menu(doc: NovaMnuDocument, edit_mode: bool = false) -> NovaMnuMenu:
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.size = Vector2(800, 600)
	menu.set_edit_mode(edit_mode)
	menu.menu = doc
	return menu


func _build_menu(edit_mode: bool = false, path: String = FIXTURE) -> NovaMnuMenu:
	return _build_doc_menu(_load_doc(path), edit_mode)


func _combo(menu: NovaMnuMenu) -> NovaMnuCombo:
	return menu.find_child("ServerList", true, false) as NovaMnuCombo


func _overlay(menu: NovaMnuMenu) -> Control:
	# The popup mount: a transparent full-menu catcher parked on the menu itself.
	var overlays: Array[Node] = []
	for child in menu.get_children():
		if child.name.begins_with("ComboPopupOverlay") and not child.is_queued_for_deletion():
			overlays.append(child)
	assert_lt(overlays.size(), 2, "at most one live overlay per menu")
	return overlays[0] as Control if overlays.size() == 1 else null


func _press_at(overlay: Control, at: Vector2) -> void:
	# Drive the catcher's connected gui_input handler with an overlay-local press,
	# the coordinates Godot's picking would deliver to the top-of-tree catcher.
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	ev.position = at
	overlay.gui_input.emit(ev)


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


func test_combo_containers_presence_and_layout_are_authoritative() -> void:
	var fallback_doc := _load_doc()
	var fallback_id := _widget_named(fallback_doc, "ServerList")
	assert_gte(fallback_id, 0)
	assert_true(fallback_doc.apply_widget_patch(fallback_id, {
		"string": {
			"present": true,
			"justify": "RIGHT",
			"vjustify": "BOTTOM",
		},
		"items": {
			"present": true,
			"justify": "CENTER",
			"vjustify": "BOTTOM",
			"rows": [{"type": "", "value": "9", "text": "Top fallback"}],
		},
		"list_box": {
			"has_min_item_height": false,
			"string": {
				"present": true,
				"has_edge": true,
				"edge": 6,
				"vjustify": "TOP",
			},
			"items": {"present": false},
		},
	}))
	var fallback_menu := _build_doc_menu(fallback_doc)
	var fallback_combo := _combo(fallback_menu)
	assert_eq(fallback_combo.get_item_count(), 1,
		"an absent nested ITEMS container falls back to authored top-level ITEMS")
	assert_eq(fallback_combo.get_item_text(0), "Top fallback")
	var closed := fallback_combo.find_child("SelectedText", true, false) as Label
	assert_eq(closed.horizontal_alignment, HORIZONTAL_ALIGNMENT_RIGHT,
		"the closed cell uses the Combo's outer STRING alignment")
	assert_eq(closed.vertical_alignment, VERTICAL_ALIGNMENT_BOTTOM,
		"the closed cell uses the Combo's outer STRING vertical alignment")
	fallback_combo.open_popup()
	var fallback_row := fallback_combo.get_popup().find_child("Item0", true, false) as Button
	assert_eq(fallback_row.custom_minimum_size.y, 16.0,
		"cleared MIN_ITEM_HEIGHT presence prevents its latent 14 from affecting preview")
	assert_eq(fallback_row.alignment, HORIZONTAL_ALIGNMENT_CENTER,
		"popup rows use the active ITEMS alignment")
	var popup_text := fallback_row.find_child("Text", false, false) as Label
	assert_not_null(popup_text, "popup row exposes its authored text layout")
	if popup_text != null:
		assert_eq(popup_text.vertical_alignment, VERTICAL_ALIGNMENT_BOTTOM,
			"active ITEMS vjustify wins over the LIST_BOX STRING fallback")
		assert_eq(popup_text.offset_left, 6.0,
			"LIST_BOX STRING edge insets popup row text")
		assert_eq(popup_text.offset_right, -6.0,
			"LIST_BOX STRING edge symmetrically insets popup row text")
	fallback_combo.close_popup()

	var empty_doc := _load_doc()
	var empty_id := _widget_named(empty_doc, "ServerList")
	assert_true(empty_doc.apply_widget_patch(empty_id, {
		"items": {
			"present": true,
			"rows": [{"type": "", "value": "9", "text": "Must stay hidden"}],
		},
		"list_box": {
			"items": {
				"present": true,
				"rows": [],
			},
		},
	}))
	var empty_combo := _combo(_build_doc_menu(empty_doc))
	assert_eq(empty_combo.get_item_count(), 0,
		"an explicitly authored empty LIST_BOX/ITEMS wins over top-level fallback")


func test_combo_runtime_populate_and_select() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.set_items(["Alpha", "Bravo"])  # shell repopulates (server browser refresh)
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
	assert_null(combo.get_popup(), "no popup box while closed")
	combo.open_popup()
	assert_true(combo.is_popup_open(), "popup open")
	var popup := combo.get_popup()
	assert_not_null(popup, "popup box built")
	assert_not_null(popup.find_child("Item0", true, false), "row built for each item")
	assert_not_null(popup.find_child("Item2", true, false), "last row built")
	combo.close_popup()
	assert_false(combo.is_popup_open(), "popup closed")
	assert_null(combo.get_popup(), "popup box gone after close")


func test_combo_row_click_selects_and_closes() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.open_popup()
	var row := combo.get_popup().find_child("Item1", true, false)
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


func test_combo_popup_uses_authored_listbox_rect() -> void:
	# The dropdown opens at the authored <LIST_BOX> POSITION (combo-relative, design
	# space), like the original's embedded CListWnd rect (D-MNU-7). The box now lives
	# inside the menu-top overlay, so the pin is on global position.
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.open_popup()
	var popup := combo.get_popup()
	assert_not_null(popup, "popup built")
	# ServerList authors LIST_BOX POSITION 230,290 -> 410,360 (combo-relative).
	assert_eq(popup.global_position, combo.global_position + Vector2(230, 290),
		"popup at the authored LIST_BOX offset from the combo")
	assert_eq(popup.size, Vector2(180, 70), "popup uses the authored LIST_BOX size")
	# MIN_ITEM_HEIGHT=14 drives the row height (D-MNU-8).
	var row0 := popup.find_child("Item0", true, false) as Control
	assert_eq(row0.custom_minimum_size.y, 14.0, "row height follows MIN_ITEM_HEIGHT")
	combo.close_popup()


func test_combo_popup_overlay_owns_exclusive_input() -> void:
	# While open, everything under the overlay is mouse-dead: the catcher is the
	# menu's LAST child (wins Godot picking and draw by tree order), covers the
	# whole menu, and stops mouse events; the popup box sits inside it so its rows
	# stay pickable. This is the reimpl home of the original's exclusive routing
	# [orig: 0x63abb5 / 0x63e691 / 0x646299] (D-MNU-11).
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.open_popup()
	var overlay := _overlay(menu)
	assert_not_null(overlay, "overlay parked on the menu")
	assert_eq(menu.get_child(menu.get_child_count() - 1), overlay,
		"overlay is the menu's last child (top of tree for picking)")
	assert_eq(overlay.mouse_filter, Control.MOUSE_FILTER_STOP, "catcher stops mouse events")
	assert_eq(overlay.global_position, menu.global_position, "catcher covers the menu origin")
	assert_eq(overlay.size, menu.size, "catcher covers the full menu rect")
	assert_eq(combo.get_popup().get_parent(), overlay, "popup box rides on the overlay")
	combo.close_popup()
	assert_null(_overlay(menu), "overlay freed on close")


func test_combo_single_open_per_menu() -> void:
	# Opening a combo closes the currently open one first
	# [orig: combobox_handle_event @ 0x65c210 sends the active combo its toggle].
	var menu := _build_menu(false, PLAYER_FIXTURE)
	var nat := menu.find_child("NATIONALITY", true, false) as NovaMnuCombo
	var div := menu.find_child("DIVISION", true, false) as NovaMnuCombo
	assert_not_null(nat, "NATIONALITY built")
	assert_not_null(div, "DIVISION built")
	nat.set_items(["USA", "Indonesia"])
	div.set_items(["1st", "2nd"])
	nat.open_popup()
	assert_true(nat.is_popup_open(), "first dropdown open")
	div.open_popup()
	assert_false(nat.is_popup_open(), "opening the second dropdown closed the first")
	assert_true(div.is_popup_open(), "second dropdown open")
	assert_not_null(_overlay(menu), "one live overlay after the swap")
	div.close_popup()


func test_combo_outside_press_closes_and_nothing_else_opens() -> void:
	# A press outside both the closed cell and the list closes the dropdown; the
	# press is consumed by the catcher, so a sibling combo under the press point
	# never sees it [orig: outside check @ 0x65c290; exclusivity @ 0x63abb5]. This
	# is the user-visible bug: pre-fix, the press opened the sibling's dropdown
	# while the first stayed open, stacking translucent lists.
	var menu := _build_menu(false, PLAYER_FIXTURE)
	var nat := menu.find_child("NATIONALITY", true, false) as NovaMnuCombo
	var div := menu.find_child("DIVISION", true, false) as NovaMnuCombo
	nat.set_items(["USA", "Indonesia"])
	nat.open_popup()
	var overlay := _overlay(menu)
	# DIVISION's closed cell lies inside the catcher, so a real press there lands
	# on the catcher by tree order; drive the handler with those coordinates.
	var div_center: Vector2 = (div.global_position - overlay.global_position) + div.size / 2.0
	assert_true(Rect2(Vector2(), overlay.size).has_point(div_center),
		"the sibling cell is covered by the catcher")
	_press_at(overlay, div_center)
	assert_false(nat.is_popup_open(), "outside press closed the dropdown")
	assert_false(div.is_popup_open(), "the consumed press did not open the sibling")


func test_combo_press_on_own_cell_keeps_popup_open() -> void:
	# While the list is open the closed cell is input-dead: a press there is
	# swallowed without closing (the original only closes when the point is outside
	# BOTH the cell and the list) [orig: combobox_handle_event @ 0x65c290].
	var menu := _build_menu(false, PLAYER_FIXTURE)
	var nat := menu.find_child("NATIONALITY", true, false) as NovaMnuCombo
	nat.set_items(["USA", "Indonesia"])
	nat.open_popup()
	var overlay := _overlay(menu)
	var cell_center: Vector2 = (nat.global_position - overlay.global_position) + nat.size / 2.0
	_press_at(overlay, cell_center)
	assert_true(nat.is_popup_open(), "press on the open combo's own cell does not close")
	nat.close_popup()


func test_screen_change_closes_popup() -> void:
	# Screen navigation clears the open dropdown, like the original clearing the
	# popup/capture globals on every screen switch
	# [orig: CUIScene_SelectNodeByName @ 0x63b6b0].
	var menu := _build_menu()
	var combo := _combo(menu)
	combo.open_popup()
	assert_true(combo.is_popup_open(), "popup open before the switch")
	menu.set_current_screen(menu.get_current_screen())
	assert_false(combo.is_popup_open(), "screen change closed the popup")
	combo.open_popup()
	assert_true(menu.close_active_combo_popup(), "menu-level close reports a close")
	assert_false(combo.is_popup_open(), "menu-level close closed the popup")
	assert_false(menu.close_active_combo_popup(), "second close is a no-op")


func test_combo_popup_fallback_when_no_listbox_rect() -> void:
	# A bare combo with no owning menu (shell/test builds; the original has no such
	# case) keeps the legacy child-of-combo popup: dropped below, clamped to the
	# window, scrolling. No overlay exists on this path.
	var combo := NovaMnuCombo.new()
	add_child_autofree(combo)
	combo.set_size(Vector2(120, 20))
	var many: Array = []
	for i in range(200):
		many.append("Opt %d" % i)
	combo.set_items(many)
	combo.open_popup()
	var popup := combo.get_popup()
	assert_not_null(popup, "fallback popup built")
	assert_eq(popup.get_parent(), combo, "fallback popup stays under the combo")
	assert_eq(popup.position, Vector2(0, 20), "fallback popup drops below the combo")
	assert_eq(popup.size.x, 120.0, "fallback popup matches the combo width")
	var row0 := popup.find_child("Item0", true, false) as Control
	var natural := 200.0 * row0.custom_minimum_size.y
	assert_lt(popup.size.y, natural, "fallback clamps below the natural list height")
	assert_gt(popup.size.y, 0.0, "fallback keeps a positive height")
	combo.close_popup()


func test_combo_popup_clamps_and_scrolls_long_list() -> void:
	var menu := _build_menu()
	var combo := _combo(menu)
	var many: Array = []
	for i in range(200):
		many.append("Option %d" % i)
	combo.set_items(many)
	combo.open_popup()
	assert_true(combo.is_popup_open(), "popup opens for a long list")
	var popup := combo.get_popup()
	assert_not_null(popup, "popup built")
	var row0 := popup.find_child("Item0", true, false) as Control
	assert_not_null(row0, "first row built")
	assert_not_null(popup.find_child("Item199", true, false), "last row built")
	# A 200-item list would tower past the authored rect; the popup keeps the
	# authored height and the rows scroll inside a ScrollContainer instead. Each
	# row's custom_minimum_size.y is min_item_height, so natural = count * that.
	var natural := 200.0 * row0.custom_minimum_size.y
	assert_lt(popup.size.y, natural, "popup height clamped below the natural list height")
	assert_gt(popup.size.y, 0.0, "popup keeps a positive height")
	var scroll := popup.find_child("Scrollbar", true, false) as NovaMnuScroll
	assert_not_null(scroll, "authored LIST_BOX scrollbar replaces the native fallback")
	var viewport := popup.find_child("ScrollViewport", true, false) as Control
	var rows := popup.find_child("Rows", true, false) as Control
	assert_not_null(viewport, "authored path clips rows in its own viewport")
	assert_eq(viewport.size.x, 164.0, "default 16px authored bar is reserved from row width")
	scroll.set_value(42)
	assert_eq(rows.position.y, -42.0, "authored shuttle moves the popup rows")
	combo.close_popup()
	assert_false(combo.is_popup_open(), "popup closes")


func test_shipped_combo_honors_scrollbar_rect_edge_pad_and_sound_bank() -> void:
	var menu := _build_menu(false, "res://../fixtures/mnu/jo_weapon.mnu")
	var combo := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	assert_not_null(combo, "shipped PRIMARY combo built")
	var items := PackedStringArray()
	for i in range(12):
		items.append("Weapon %d" % i)
	combo.set_items(items)
	watch_signals(menu)
	combo.open_popup()
	var popup := combo.get_popup()
	var scroll := popup.find_child("Scrollbar", true, false) as NovaMnuScroll
	var viewport := popup.find_child("ScrollViewport", true, false) as Control
	assert_not_null(scroll, "shipped authored scrollbar built")
	assert_eq(scroll.position, Vector2(180, 1), "SCROLLBAR POSITION origin honored")
	assert_eq(scroll.size, Vector2(19, 98), "SCROLLBAR POSITION size honored")
	assert_eq(viewport.size.x, 179.0, "SB_EDGE_PAD=21 reserves authored row space")
	(scroll.find_child("ArrowDown", true, false) as BaseButton).emit_signal("pressed")
	assert_signal_emitted_with_parameters(
		menu, "sound_requested", ["menu.lwf", "CLICK_VALUE"])
	combo.close_popup()
