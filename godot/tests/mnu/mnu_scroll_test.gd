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
	scroll.set_range(0, 100, 10)  # host binds the real range
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
