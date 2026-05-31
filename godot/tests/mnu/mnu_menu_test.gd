extends GutTest

# M3 gate: NovaMnuMenu builds a live Control tree (one NovaMnuScreen per screen)
# from a NovaMnuDocument, positions widgets, and toggles screen visibility.

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _build_menu(edit_mode: bool = false) -> NovaMnuMenu:
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)  # in-tree so built widgets are not orphans
	menu.set_edit_mode(edit_mode)
	menu.menu = _load_doc()  # set_menu rebuilds because the node is in the tree
	return menu


func test_builds_one_node_per_screen() -> void:
	var menu := _build_menu()
	assert_eq(menu.get_child_count(), 2, "two screen nodes built")
	var s0 := menu.get_child(0)
	assert_true(s0 is NovaMnuScreen, "screen node is NovaMnuScreen")
	assert_eq(s0.get_screen_name(), "MAIN", "first screen name")
	assert_eq(s0.get_music_var(), 3, "screen music_var carried onto node")


func test_widget_tree_and_types() -> void:
	var menu := _build_menu()
	# Title (static with text) -> Label; StartBtn -> Button.
	var title := menu.find_child("Title", true, false)
	assert_not_null(title, "Title node built")
	assert_true(title is Label, "static-with-text becomes a Label")

	var start := menu.find_child("StartBtn", true, false)
	assert_not_null(start, "StartBtn node built")
	assert_true(start is Button, "button becomes a Button")
	assert_eq((start as Button).text, "MM_Start", "button text is the raw string (RTXT resolved in M4)")

	var chk := menu.find_child("SoundChk", true, false)
	assert_true(chk is CheckBox, "checkbox becomes a CheckBox")
	assert_true((chk as CheckBox).button_pressed, "checked flag carried")


func test_widget_position() -> void:
	var menu := _build_menu()
	var start := menu.find_child("StartBtn", true, false) as Control
	assert_not_null(start, "StartBtn present")
	assert_eq(start.position, Vector2(270, 120), "absolute MNU position applied")
	assert_eq(start.size.x, 100.0, "explicit width applied")
	# The placeholder Godot Button enforces a theme min-height (~31px), so it may
	# exceed the requested 30; M5's TextureButton (ignore_texture_size) honors it
	# exactly. Assert the request is at least respected.
	assert_almost_eq(start.size.y, 30.0, 2.0, "height near requested 30")


func test_screen_visibility_default_and_navigation() -> void:
	var menu := _build_menu()
	# current_screen defaults to the first screen.
	assert_eq(menu.current_screen, "MAIN", "defaults to first screen")
	var main_node := menu.get_child(0) as Control
	var options_node := menu.get_child(1) as Control
	assert_true(main_node.visible, "MAIN visible by default")
	assert_false(options_node.visible, "OPTIONS hidden by default")

	assert_true(menu.show_screen("OPTIONS"), "navigate to OPTIONS")
	assert_false(main_node.visible, "MAIN now hidden")
	assert_true(options_node.visible, "OPTIONS now visible")
	assert_false(menu.show_screen("NOPE"), "unknown screen is rejected")


func test_edit_mode_shows_all_and_is_click_through() -> void:
	var menu := _build_menu(true)
	for i in menu.get_child_count():
		assert_true((menu.get_child(i) as Control).visible, "all screens visible in edit_mode")
	# Buttons are inert (disabled) while authoring.
	var start := menu.find_child("StartBtn", true, false)
	assert_true((start as Button).disabled, "buttons disabled in edit_mode")


func test_get_screen_names() -> void:
	var menu := _build_menu()
	assert_eq(menu.get_screen_names(), PackedStringArray(["MAIN", "OPTIONS"]), "screen names listed")
