extends GutTest

# M3 gate: NovaMnuMenu builds a live Control tree (one NovaMnuScreen per screen)
# from a NovaMnuDocument, positions widgets, and toggles screen visibility.
# M4 gate: assets resolve through NovaResourceRoot / MnsStyleSheet / RtxtStringFile
# (textures, fonts, %VAR% colors, string-id text), degrading gracefully and
# reporting unresolved asset names.

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


# The faithful builder wraps a widget's text in a "Label" child (buttons are
# TextureButtons with no text of their own); this finds that text.
func _widget_text(widget: Node) -> String:
	var lbl := widget.find_child("Label", true, false) as Label
	return lbl.text if lbl != null else ""


func test_builds_one_node_per_screen() -> void:
	var menu := _build_menu()
	assert_eq(menu.get_child_count(), 2, "two screen nodes built")
	var s0 := menu.get_child(0)
	assert_true(s0 is NovaMnuScreen, "screen node is NovaMnuScreen")
	assert_eq(s0.get_screen_name(), "MAIN", "first screen name")
	assert_eq(s0.get_music_var(), 3, "screen music_var carried onto node")


func test_widget_tree_and_types() -> void:
	var menu := _build_menu()
	# Title (static with text) -> Control container with a Label child.
	var title := menu.find_child("Title", true, false)
	assert_not_null(title, "Title node built")
	assert_eq(_widget_text(title), "MM_Title", "static text (raw id; RTXT resolved separately)")

	# Button -> NovaMnuButton (a TextureButton subclass); text in child Label.
	var start := menu.find_child("StartBtn", true, false)
	assert_not_null(start, "StartBtn node built")
	assert_true(start is NovaMnuButton, "button becomes a NovaMnuButton")
	assert_true(start is TextureButton, "NovaMnuButton is a TextureButton")
	assert_eq(_widget_text(start), "MM_Start", "button text is the raw string (no text resource set)")

	# Checkbox -> NovaMnuCheckBox (a toggled TextureButton subclass).
	var chk := menu.find_child("SoundChk", true, false)
	assert_true(chk is NovaMnuCheckBox, "checkbox becomes a NovaMnuCheckBox")
	assert_true((chk as TextureButton).button_pressed, "checked flag carried")


func test_widget_position() -> void:
	var menu := _build_menu()
	var start := menu.find_child("StartBtn", true, false) as Control
	assert_not_null(start, "StartBtn present")
	assert_eq(start.position, Vector2(270, 120), "absolute MNU position applied")
	# TextureButton honors ignore_texture_size, so the explicit bounds are exact
	# (no theme min-size inflation like the old placeholder Button).
	assert_eq(start.size, Vector2(100, 30), "explicit width/height applied exactly")


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
	assert_true((start as TextureButton).disabled, "buttons disabled in edit_mode")


func test_get_screen_names() -> void:
	var menu := _build_menu()
	assert_eq(menu.get_screen_names(), PackedStringArray(["MAIN", "OPTIONS"]), "screen names listed")


# --- M4: asset resolution ---------------------------------------------------


func test_var_color_resolves_onto_label() -> void:
	# ROOT declares FONT DEFAULT_FG=%DEF_TEXT_FG%, inherited by Title. With a
	# stylesheet resolving that variable to white, the label adopts the color.
	var ss := MnsStyleSheet.new()
	ss.set_variable("DEF_TEXT_FG", "FFFFFF")
	ss.set_variable("DEF_FONTNAME", "Gunpl22b.fnt")

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.stylesheet = ss
	menu.menu = _load_doc()

	var title := menu.find_child("Title", true, false)
	var lbl := title.find_child("Label", true, false) as Label
	assert_not_null(lbl, "Title label built")
	assert_not_null(lbl.label_settings, "label settings created for a styled font")
	var c: Color = lbl.label_settings.font_color
	assert_almost_eq(c.r, 1.0, 0.01, "%DEF_TEXT_FG% resolved to white (r)")
	assert_almost_eq(c.g, 1.0, 0.01, "%DEF_TEXT_FG% resolved to white (g)")
	assert_almost_eq(c.b, 1.0, 0.01, "%DEF_TEXT_FG% resolved to white (b)")


func test_string_id_resolves_from_text_resource() -> void:
	# A type="id" string looks up the RTXT table when a text resource is provided.
	var rt := RtxtStringFile.new()
	var sec := rt.add_section("MENU")
	rt.add_entry("MM_START", "Start Game", sec, Vector2i(0, 0))

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.text_resource = rt
	menu.menu = _load_doc()

	var start := menu.find_child("StartBtn", true, false)
	assert_eq(_widget_text(start), "Start Game", "MM_Start resolved via RTXT (case-insensitive)")

	# A literal (non-id) string is unaffected by the text resource.
	var version := menu.find_child("Version", true, false)
	assert_eq(_widget_text(version), "v1.0", "literal string passes through")


func test_unresolved_assets_are_counted() -> void:
	# With no resource root, the fixture's concrete texture names (border2.tga,
	# btn_up.tga, btn_over.tga) cannot load and are reported; %VAR% font names are
	# not counted (they stay unresolved variables, not asset misses).
	var menu := _build_menu()
	assert_true(menu.get_unresolved_asset_count() >= 3,
			"unresolved concrete textures reported (got %d)" % menu.get_unresolved_asset_count())


# Writes a solid PNG so a fixture .tga reference resolves through the resolver's
# extension fallback (.tga -> ... -> .png) and case-insensitive directory match.
func _write_png(path: String, w: int, h: int, c: Color) -> void:
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	img.fill(c)
	img.save_png(path)


func test_real_assets_resolve_and_frame_bakes() -> void:
	# Drive the RESOLVED path (not the placeholder fallback): synthetic textures on
	# disk make the button textures load and the 9-patch frame bake for real.
	var dir := OS.get_temp_dir().path_join("mnu_m4_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_write_png(dir.path_join("border2.png"), 64, 64, Color(0.7, 0.7, 0.7, 1.0))  # stencil
	_write_png(dir.path_join("boxtile.png"), 8, 8, Color(0.2, 0.3, 0.4, 1.0))    # brush
	_write_png(dir.path_join("btn_up.png"), 100, 30, Color(0.2, 0.5, 0.8, 1.0))
	_write_png(dir.path_join("btn_over.png"), 100, 30, Color(0.3, 0.6, 0.9, 1.0))
	_write_png(dir.path_join("newarow1.png"), 16, 16, Color(1, 1, 1, 1))         # cursor

	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		pass_test("temp resource root unavailable in this environment: %s" % root.get_last_error())
		return

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.menu = _load_doc()

	# Button textures applied.
	var start := menu.find_child("StartBtn", true, false) as TextureButton
	assert_not_null(start.texture_normal, "btn_up.tga resolved onto texture_normal")
	assert_not_null(start.texture_hover, "btn_over.tga resolved onto texture_hover")

	# The ROOT window's FRAME baked into a NinePatchRect with cell-sized margins
	# (64px stencil / 4 = 16px patch margin), proving the sub_52D000 bake ran.
	var border := menu.find_child("FrameBorder", true, false) as NinePatchRect
	assert_not_null(border, "frame baked into a NinePatchRect")
	assert_eq(border.get_patch_margin(SIDE_LEFT), 16, "patch margin = stencil cell size")
	assert_false(border.draw_center, "frame center is transparent (not drawn)")

	# The MAIN screen's cursor (newarow1.tga) resolved onto the screen node.
	var main_screen := menu.get_child(0) as NovaMnuScreen
	assert_not_null(main_screen.get_cursor_texture(), "cursor texture resolved onto the screen")

	# Every concrete texture resolved, so nothing is reported unresolved.
	assert_eq(menu.get_unresolved_asset_count(), 0, "all textures resolved")

	# Best-effort cleanup of the temp assets.
	for f in ["border2.png", "boxtile.png", "btn_up.png", "btn_over.png", "newarow1.png"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)


# --- M5: interactivity + audio ----------------------------------------------


func test_button_screen_action_navigates() -> void:
	# StartBtn carries <ACTION type="screen" target="OPTIONS">. Pressing it routes
	# through the NovaMnuButton -> NovaMnuMenu navigation controller.
	var menu := _build_menu()
	var start := menu.find_child("StartBtn", true, false) as NovaMnuButton
	assert_not_null(start, "StartBtn is a NovaMnuButton")
	assert_eq(start.get_action_count(), 1, "one screen action parsed onto the button")

	start.emit_signal("pressed")
	assert_eq(menu.current_screen, "OPTIONS", "screen action navigated to OPTIONS")
	assert_false((menu.get_child(0) as Control).visible, "MAIN hidden after navigation")
	assert_true((menu.get_child(1) as Control).visible, "OPTIONS visible after navigation")


func test_pop_screen_action_returns_to_previous() -> void:
	var menu := _build_menu()
	# Forward: MAIN -> OPTIONS (pushes MAIN onto the stack).
	(menu.find_child("StartBtn", true, false) as NovaMnuButton).emit_signal("pressed")
	assert_eq(menu.current_screen, "OPTIONS", "navigated to OPTIONS")
	# BackBtn carries <ACTION type="POP_SCREEN">: pop back to MAIN.
	(menu.find_child("BackBtn", true, false) as NovaMnuButton).emit_signal("pressed")
	assert_eq(menu.current_screen, "MAIN", "pop_screen returned to MAIN")


func test_action_dispatched_signal_fires() -> void:
	var menu := _build_menu()
	watch_signals(menu)
	(menu.find_child("StartBtn", true, false) as NovaMnuButton).emit_signal("pressed")
	assert_signal_emitted_with_parameters(menu, "action_dispatched", ["screen", "OPTIONS"])


func test_hover_emits_sound_requested() -> void:
	# StartBtn has <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>.
	# Hover always emits sound_requested(file, trigger) even with no sound bank.
	var menu := _build_menu()
	watch_signals(menu)
	(menu.find_child("StartBtn", true, false) as NovaMnuButton).emit_signal("mouse_entered")
	assert_signal_emitted_with_parameters(menu, "sound_requested", ["menu.lwf", "MOUSE_OVER"])


func test_music_changed_fires_on_screen_show() -> void:
	# MAIN declares <MUSICVAR>3</MUSICVAR>; the initial build emits music_changed(3).
	# OPTIONS declares no MUSICVAR (0), so the track is left alone (no new emit) -
	# matching the reference's music_var > 0 guard.
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	watch_signals(menu)
	menu.menu = _load_doc()
	assert_signal_emitted_with_parameters(menu, "music_changed", [3])
	assert_signal_emit_count(menu, "music_changed", 1, "only MAIN's music applied")

	menu.navigate_to_screen("OPTIONS")
	assert_signal_emit_count(menu, "music_changed", 1, "no-music screen leaves the track alone")


func test_checkbox_toggle_drives_underline() -> void:
	var menu := _build_menu()
	var chk := menu.find_child("SoundChk", true, false) as NovaMnuCheckBox
	var underline := chk.find_child("Underline", true, false) as ColorRect
	assert_not_null(underline, "checkbox builds an Underline child at runtime")
	assert_true(chk.button_pressed, "SoundChk starts checked (CHECKED flag)")
	assert_true(underline.visible, "underline shown while checked")

	chk.button_pressed = false  # fires toggled -> update_appearance
	assert_false(underline.visible, "underline hidden once unchecked")


func test_edit_mode_widgets_are_inert() -> void:
	# In edit_mode the widgets connect nothing and stay disabled, so a synthetic
	# press never navigates (the ONED preview must not drive the menu).
	var menu := _build_menu(true)
	var start := menu.find_child("StartBtn", true, false) as NovaMnuButton
	start.emit_signal("pressed")
	assert_eq(menu.current_screen, "MAIN", "edit-mode press does not navigate")


func test_window_action_shows_hides_toggles_descendant() -> void:
	# The "window" action verb finds a named descendant in the current screen and
	# shows/hides/toggles it (handle_window_action / dispatch_action window branch).
	var menu := _build_menu()
	watch_signals(menu)
	var chk := menu.find_child("SoundChk", true, false) as Control
	assert_not_null(chk, "target descendant present")
	assert_true(chk.visible, "visible initially")

	assert_true(menu.dispatch_action("window", "SoundChk", "", "hide"), "hide handled")
	assert_false(chk.visible, "hidden")
	menu.dispatch_action("window", "SoundChk", "", "toggle")
	assert_true(chk.visible, "toggled back to visible")
	assert_false(menu.dispatch_action("window", "NoSuchWindow", "", "hide"), "missing target rejected")
	menu.dispatch_action("window", "SoundChk", "", "show")
	assert_true(chk.visible, "show keeps it visible")
	assert_signal_emitted_with_parameters(menu, "action_dispatched", ["window", "SoundChk"])


func test_navigate_to_menu_emits_menu_requested() -> void:
	# Cross-.mnu jumps are host policy: navigate_to_menu (and a screen action with a
	# file) emit menu_requested instead of navigating in-menu.
	var menu := _build_menu()
	watch_signals(menu)
	menu.navigate_to_menu("sp.mnu", "BRIEFING")
	assert_signal_emitted_with_parameters(menu, "menu_requested", ["sp.mnu", "BRIEFING"])

	menu.dispatch_action("screen", "BRIEFING", "sp.mnu", "")
	assert_signal_emitted_with_parameters(menu, "menu_requested", ["sp.mnu", "BRIEFING"])
	assert_eq(menu.current_screen, "MAIN", "cross-file action does not navigate in-menu")


func test_quit_paths_emit_quit_requested() -> void:
	var menu := _build_menu()
	watch_signals(menu)
	# Pop with an empty stack emits quit_requested and returns false.
	assert_false(menu.pop_screen(), "pop on empty stack returns false")
	assert_signal_emitted(menu, "quit_requested")
	# quit_game emits it too.
	menu.quit_game()
	assert_signal_emit_count(menu, "quit_requested", 2)


func test_music_director_receives_set_var() -> void:
	# When a director is wired, a screen's MUSICVAR is pushed into its VM var.
	var dir := NovaMusicDirector.new()
	add_child_autofree(dir)
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_music_director(dir)
	menu.menu = _load_doc()  # MAIN MUSICVAR 3 -> set_var(0, 3)
	assert_eq(dir.get_var(0), 3, "MAIN MUSICVAR pushed to the director")
	# OPTIONS has no MUSICVAR: the director var is left untouched (> 0 guard).
	menu.navigate_to_screen("OPTIONS")
	assert_eq(dir.get_var(0), 3, "no-music screen leaves the director var untouched")


func test_edit_mode_does_not_drive_director() -> void:
	var dir := NovaMusicDirector.new()
	add_child_autofree(dir)
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(true)
	menu.set_music_director(dir)
	menu.menu = _load_doc()
	assert_eq(dir.get_var(0), 0, "edit_mode suppresses driving the director")


func test_play_widget_sound_without_bank_is_signal_only() -> void:
	# With no sound bank, play_widget_sound still emits sound_requested but creates
	# no pooled players (the lazy pool only materializes when a bank can resolve).
	var menu := _build_menu()
	watch_signals(menu)
	menu.play_widget_sound("CLICK_SELECT", "menu.lwf")
	assert_signal_emitted_with_parameters(menu, "sound_requested", ["menu.lwf", "CLICK_SELECT"])
	assert_null(menu.find_child("_MnuSound0", true, false), "no audio players without a bank")


func test_navigation_stack_clear_and_no_redundant_push() -> void:
	var menu := _build_menu()
	# Navigating to the already-current screen must not self-push.
	menu.navigate_to_screen("MAIN")
	assert_false(menu.pop_screen(), "same-screen navigation pushed nothing")
	# A real forward move pushes; clear empties the stack.
	menu.navigate_to_screen("OPTIONS")
	menu.clear_navigation_stack()
	assert_false(menu.pop_screen(), "cleared stack pops to the empty (quit) path")
