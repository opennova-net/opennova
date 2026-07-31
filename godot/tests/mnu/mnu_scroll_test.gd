extends GutTest

# M9.3 gate: NovaMnuScroll builds a themed scrollbar/slider from the SHUTTLE /
# SCROLLUP / SCROLLDOWN art with Range-like value semantics, drives a linked
# scroll target, and stays inert in edit_mode.

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


func _scroll(menu: NovaMnuMenu) -> NovaMnuScroll:
	return menu.find_child("VolumeBar", true, false) as NovaMnuScroll


func test_scroll_builds_with_parts() -> void:
	var menu := _build_menu()
	var scroll := _scroll(menu)
	assert_not_null(scroll, "VolumeBar built")
	assert_true(scroll is Control, "NovaMnuScroll is a Control")
	assert_true(scroll.is_vertical(), "ORIENTATION VERTICAL parsed")
	assert_not_null(scroll.find_child("Track", true, false), "track built")
	assert_not_null(scroll.find_child("ArrowUp", true, false), "up arrow built")
	assert_not_null(scroll.find_child("ArrowDown", true, false), "down arrow built")
	assert_not_null(scroll.find_child("Shuttle", true, false), "shuttle built")


func test_range_and_value_clamp() -> void:
	var menu := _build_menu()
	var scroll := _scroll(menu)
	scroll.set_range(0, 100, 10)  # shell binds the real range
	scroll.set_value(50)
	assert_eq(scroll.get_value(), 50.0, "value set within range")
	scroll.set_value(200)
	assert_eq(scroll.get_value(), 90.0, "clamped to max - page")
	scroll.set_value(-5)
	assert_eq(scroll.get_value(), 0.0, "clamped to min")


func test_value_changed_signal() -> void:
	var menu := _build_menu()
	var scroll := _scroll(menu)
	scroll.set_range(0, 100, 0)
	watch_signals(scroll)
	scroll.set_value(30)
	assert_signal_emitted_with_parameters(scroll, "value_changed", [30.0])


func test_linked_target_scrolls() -> void:
	var menu := _build_menu()
	var scroll := _scroll(menu)
	var target := Control.new()
	target.name = "Target"
	scroll.add_child(target)
	scroll.link_scroll_target(NodePath("Target"))
	scroll.set_value(20)
	assert_eq(target.position.y, -20.0, "linked target shifts by -value along the axis")


func test_range_target_syncs_both_directions() -> void:
	var menu := _build_menu()
	var scroll := _scroll(menu)
	var native := VScrollBar.new()
	native.name = "NativeRange"
	native.min_value = 5
	native.max_value = 105
	native.page = 20
	native.step = 2
	native.value = 25
	scroll.add_child(native)
	scroll.link_range_target(NodePath("NativeRange"))
	assert_eq(scroll.get_min(), 5.0, "authored control imports native minimum")
	assert_eq(scroll.get_max(), 105.0, "authored control imports native maximum")
	assert_eq(scroll.get_page(), 20.0, "authored control imports native page")
	assert_eq(scroll.get_value(), 25.0, "authored control imports native value")
	native.value = 45
	assert_eq(scroll.get_value(), 45.0, "native scroll updates authored shuttle")
	scroll.set_value(65)
	assert_eq(native.value, 65.0, "authored control drives native range")


func test_shipped_list_and_multiline_use_authored_scrollbar_geometry() -> void:
	var vehicle_doc := NovaMnuDocument.new()
	assert_eq(vehicle_doc.load_from_bytes(FileAccess.get_file_as_bytes(
		"res://../fixtures/mnu/jo_vehicle.mnu")), OK)
	var vehicle_menu := NovaMnuMenu.new()
	vehicle_menu.build_on_ready = false
	add_child_autofree(vehicle_menu)
	vehicle_menu.menu = vehicle_doc
	var item_list := vehicle_menu.find_child("ITEM_LIST", true, false) as NovaMnuList
	var list_scroll := item_list.find_child("Scrollbar", false, false) as NovaMnuScroll
	assert_not_null(list_scroll, "shipped list owns the shared authored scrollbar")
	assert_eq(list_scroll.position, Vector2(300, 0), "list scrollbar authored origin")
	assert_eq(list_scroll.size, Vector2(16, 200), "list scrollbar authored size")

	var sp_doc := NovaMnuDocument.new()
	assert_eq(sp_doc.load_from_bytes(FileAccess.get_file_as_bytes(
		"res://../fixtures/mnu/jo_sp.mnu")), OK)
	var sp_menu := NovaMnuMenu.new()
	sp_menu.build_on_ready = false
	add_child_autofree(sp_menu)
	sp_menu.menu = sp_doc
	var briefing := sp_menu.find_child("BRIEFING", true, false) as NovaMnuMultilineEdit
	var brief_scroll := briefing.find_child("Scrollbar", false, false) as NovaMnuScroll
	assert_not_null(brief_scroll, "shipped multiline owns the shared authored scrollbar")
	assert_eq(brief_scroll.position, Vector2(321, 0), "multiline scrollbar authored origin")
	assert_eq(brief_scroll.size, Vector2(20, 318), "multiline scrollbar authored size")


func test_ratio_round_trip() -> void:
	var menu := _build_menu()
	var scroll := _scroll(menu)
	scroll.set_range(0, 200, 0)
	scroll.set_ratio(0.25)
	assert_eq(scroll.get_value(), 50.0, "ratio maps onto the value range")
	assert_almost_eq(scroll.get_ratio(), 0.25, 0.001, "ratio round-trips")


func test_scroll_inert_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var scroll := _scroll(menu)
	var up := scroll.find_child("ArrowUp", true, false)
	assert_true((up as BaseButton).disabled, "arrows disabled while authoring")
	scroll.set_value(5)
	up.emit_signal("pressed")
	assert_eq(scroll.get_value(), 5.0, "arrow not wired in edit_mode -> no change")
