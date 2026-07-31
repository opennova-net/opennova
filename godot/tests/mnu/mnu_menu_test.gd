extends GutTest

# M3 gate: NovaMnuMenu builds a live Control tree (one NovaMnuScreen per screen)
# from a NovaMnuDocument, positions widgets, and toggles screen visibility.
# M4 gate: assets resolve through NovaResourceRoot / MnsStyleSheet / RtxtStringFile
# (textures, fonts, %VAR% colors, string-id text), degrading gracefully and
# reporting unresolved asset names.

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"
const STYLE_FIXTURE := "res://../fixtures/mns/menu_style.mns"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _build_menu(edit_mode: bool = false, document: NovaMnuDocument = null) -> NovaMnuMenu:
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)  # in-tree so built widgets are not orphans
	menu.set_edit_mode(edit_mode)
	menu.menu = document if document != null else _load_doc()
	# set_menu rebuilds because the node is in the tree
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


func test_absent_screen_music_var_ignores_its_latent_value() -> void:
	var doc := _load_doc()
	var screen := int(doc.get_screen_ids()[0])
	assert_eq(doc.get_screen_music_var(screen), 3)
	doc.set_screen_property(screen, "has_music_var", false)
	assert_false(doc.get_screen_has_music_var(screen))

	var menu := _build_menu(false, doc)
	var s0 := menu.get_child(0) as NovaMnuScreen
	assert_eq(s0.get_music_var(), 0,
			"presence bit is authoritative even while the parsed value remains latent")


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


func test_real_style_var_color_resolves_onto_label() -> void:
	# ROOT declares FONT DEFAULT_FG=%DEF_TEXT_FG%, inherited by Title. With a
	# real menu_style.mns resolving that variable to white, the label adopts the
	# color. This is the fixture-level guard for an all-black preview regression.
	var ss := MnsStyleSheet.new()
	assert_eq(ss.load_from_bytes(FileAccess.get_file_as_bytes(STYLE_FIXTURE)), OK)
	assert_eq(String(ss.get_variable("DEF_TEXT_FG")), "FFFFFFFF",
		"the shipped-style fixture authors opaque white in MNU AARRGGBB order")

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


func test_var_in_literal_text_resolves_through_stylesheet() -> void:
	# The engine expands %VAR% over the whole buffer pre-parse, so a var can
	# appear in ANY field including literal text [orig: NapiXML_ExpandVariablesInText
	# @ 0x63a000]. We keep raw tokens in the document and expand per consumed field
	# at build time (ADR 0005); this pins the text field into that coverage.
	var xml := """<MENU>
<SCREEN name=\"MAIN\">
<WINDOW type=\"WINDOW\" name=\"Root\"><POSITION left=\"0\" top=\"0\" right=\"640\" bottom=\"480\"></POSITION>
<WINDOW type=\"STATIC\" name=\"Build\"><POSITION left=\"10\" top=\"10\" right=\"300\" bottom=\"34\"></POSITION><STRING>%BUILD_LABEL%</STRING></WINDOW>
</WINDOW>
</SCREEN>
</MENU>"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(xml.to_utf8_buffer()), OK, "var-text menu parses")
	# The document keeps the raw token (round-trip), proving we did not pre-expand.
	assert_true(doc.to_byte_array().get_string_from_utf8().contains("%BUILD_LABEL%"),
		"the document round-trips the raw %VAR% token")

	var ss := MnsStyleSheet.new()
	ss.set_variable("BUILD_LABEL", "Build 1337")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.stylesheet = ss
	menu.menu = doc
	var node := menu.find_child("Build", true, false)
	var lbl := node.find_child("Label", true, false) as Label
	assert_not_null(lbl, "static label built")
	assert_eq(lbl.text, "Build 1337", "literal-text %VAR% expanded through the stylesheet at build")


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


func test_each_screen_resolves_its_own_text_resource() -> void:
	var dir := OS.get_temp_dir().path_join("mnu_screen_rtxt_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	for spec in [["one.BIN", "First screen"], ["two.BIN", "Second screen"]]:
		var table := RtxtStringFile.new()
		var section := table.add_section("MENU")
		table.add_entry("SHARED", spec[1], section, Vector2i())
		assert_eq(table.save_to_path(dir.path_join(spec[0])), OK, "temporary RTXT saves")
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK, "temporary resource root opens")
	var bytes := ('<SCREEN><NAME>ONE</NAME><TEXT_RSRC>one.BIN</TEXT_RSRC>' +
		'<WINDOW type="window" name="ROOT"><WINDOW type="static" name="TEXT_ONE">' +
		'<STRING type="id">SHARED</STRING></WINDOW></WINDOW></SCREEN>' +
		'<SCREEN><NAME>TWO</NAME><TEXT_RSRC>two.BIN</TEXT_RSRC>' +
		'<WINDOW type="window" name="ROOT"><WINDOW type="static" name="TEXT_TWO">' +
		'<STRING type="id">SHARED</STRING></WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(bytes), OK)
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.menu = doc
	assert_eq(_widget_text(menu.find_child("TEXT_ONE", true, false)), "First screen",
		"first screen uses one.BIN")
	assert_eq(_widget_text(menu.find_child("TEXT_TWO", true, false)), "Second screen",
		"second screen uses two.BIN")
	DirAccess.remove_absolute(dir.path_join("one.BIN"))
	DirAccess.remove_absolute(dir.path_join("two.BIN"))
	DirAccess.remove_absolute(dir)


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

	# The ROOT window's FRAME renders as the faithful 8-piece stencil border
	# (corners + stretched edges, drawn from the stencil's tile grid) plus the
	# tiling brush fill [orig: CUIElement_DrawFrame @ 0x64a210;
	# init_border_materials @ 0x646f70]. 64px stencil / 4 = 16px tiles.
	var tl := menu.find_child("FrameTL", true, false) as TextureRect
	assert_not_null(tl, "frame stencil rendered as 8 border pieces")
	assert_eq((tl.texture as AtlasTexture).region.size, Vector2(8, 8),
		"tile size comes from the authored STENCIL size attr (widgets.mnu authors 8)")
	assert_eq(tl.offset_left, -8.0, "the border hangs outside the rect by SIZE (inset 0)")
	for piece in ["FrameTop", "FrameTR", "FrameLeft", "FrameRight", "FrameBL", "FrameBottom", "FrameBR"]:
		assert_not_null(menu.find_child(piece, true, false), "%s drawn" % piece)
	var fill := menu.find_child("FrameFill", true, false) as TextureRect
	assert_not_null(fill, "the brush tiles across the window rect")
	assert_eq(fill.stretch_mode, TextureRect.STRETCH_TILE, "brush is tiled, not stretched")

	# The MAIN screen's cursor (newarow1.tga) resolved onto the screen node.
	var main_screen := menu.get_child(0) as NovaMnuScreen
	assert_not_null(main_screen.get_cursor_texture(), "cursor texture resolved onto the screen")

	# Every concrete texture resolved, so nothing is reported unresolved.
	assert_eq(menu.get_unresolved_asset_count(), 0, "all textures resolved")

	# Best-effort cleanup of the temp assets.
	for f in ["border2.png", "boxtile.png", "btn_up.png", "btn_over.png", "newarow1.png"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)


func test_frame_honors_authored_stencil_insets() -> void:
	# STENCIL INSETX/INSETY pull the overhanging border back toward the rect
	# [orig: INSETX/INSETY floats at elem+0x288/+0x28C, applied in
	#  CUIElement_DrawFrame @ 0x64a210: left - SIZE + INSETX etc.]. Previously
	# hardcoded 16/24; now data-driven with a 0 default.
	var dir := OS.get_temp_dir().path_join("mnu_insets_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	img.fill(Color(0.5, 0.5, 0.5, 1.0))
	img.save_png(dir.path_join("border2.png"))
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		pass_test("temp resource root unavailable in this environment")
		return
	var xml := """<MENU>
<SCREEN name=\"MAIN\">
<WINDOW type=\"WINDOW\" name=\"Root\" DRAW_FRAME><POSITION left=\"0\" top=\"0\" right=\"640\" bottom=\"480\"></POSITION>
<FRAME><STENCIL size=\"16\" insetx=\"6\" insety=\"9\">border2.tga</STENCIL></FRAME>
</WINDOW>
</SCREEN>
</MENU>"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(xml.to_utf8_buffer()), OK, "inset menu parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.menu = doc
	var tl := menu.find_child("FrameTL", true, false) as TextureRect
	assert_not_null(tl, "stencil border built")
	assert_eq(tl.offset_left, -16.0 + 6.0, "INSETX pulls the left overhang in")
	assert_eq(tl.offset_top, -16.0 + 9.0, "INSETY pulls the top overhang in")
	var tr := menu.find_child("FrameTR", true, false) as TextureRect
	assert_eq(tr.offset_left, -6.0 - 1.0, "right pieces sit at right - INSETX - 1")
	DirAccess.remove_absolute(dir.path_join("border2.png"))
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


func test_sound_slots_route_by_state_not_trigger() -> void:
	# The original keys widget sounds by the STATE attribute, not the trigger
	# string [orig: CUIElement_ParseXMLDefinition @ 0x648120 - MOUSEIN=1,
	# MOUSEOUT=2, SELECTED=3]. A trigger named "MOUSE_OVER" authored under
	# state="selected" must fire on press, not on hover.
	var doc := NovaMnuDocument.new()
	var xml := """<MENU>
<SCREEN name=\"MAIN\">
<WINDOW type=\"WINDOW\" name=\"Root\"><POSITION left=\"0\" top=\"0\" right=\"640\" bottom=\"480\"></POSITION>
<WINDOW type=\"BUTTON\" name=\"OddBtn\"><POSITION left=\"10\" top=\"10\" right=\"110\" bottom=\"34\"></POSITION>
<SOUND state=\"selected\" trigger=\"MOUSE_OVER\">click_bank.lwf</SOUND>
<SOUND state=\"mouseout\" trigger=\"LEAVE_SET\">out_bank.lwf</SOUND>
</WINDOW>
</WINDOW>
</SCREEN>
</MENU>"""
	assert_eq(doc.load_from_bytes(xml.to_utf8_buffer()), OK, "test menu parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	watch_signals(menu)
	var btn := menu.find_child("OddBtn", true, false) as NovaMnuButton
	assert_not_null(btn, "button built")

	# Hover: no mousein slot authored -> no sound at all.
	btn.emit_signal("mouse_entered")
	assert_signal_emit_count(menu, "sound_requested", 0, "no MOUSEIN slot -> hover silent")

	# Leaving fires the MOUSEOUT slot [orig: widget_process_mouse_event @ 0x647a00].
	btn.emit_signal("mouse_exited")
	assert_signal_emitted_with_parameters(menu, "sound_requested", ["out_bank.lwf", "LEAVE_SET"])

	# Press fires the SELECTED slot even though its trigger says "MOUSE_OVER".
	btn.emit_signal("pressed")
	assert_signal_emitted_with_parameters(menu, "sound_requested", ["click_bank.lwf", "MOUSE_OVER"])
	assert_signal_emit_count(menu, "sound_requested", 2)


func test_master_volume_property_defaults_and_clamps() -> void:
	# [orig: CGameMenu+92 master volume, ctor default 0xFF @ 0x63e060]
	var menu := NovaMnuMenu.new()
	add_child_autofree(menu)
	assert_eq(menu.master_volume, 255, "default master volume is 255")
	menu.master_volume = 300
	assert_eq(menu.master_volume, 255, "clamped high")
	menu.master_volume = -5
	assert_eq(menu.master_volume, 0, "clamped low")


func test_music_changed_fires_on_screen_show() -> void:
	# MAIN declares <MUSICVAR>3</MUSICVAR>; the initial build emits music_changed(3).
	# OPTIONS declares no MUSICVAR, whose parsed default 0 is still written. Every
	# screen event repeats the write [orig: UI_DispatchScreenEvent @ 0x54e6a0,
	# AudioVM_SetVariable(2, value) @ 0x54eff4].
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	watch_signals(menu)
	menu.menu = _load_doc()
	assert_signal_emitted_with_parameters(menu, "music_changed", [3])
	assert_signal_emit_count(menu, "music_changed", 1, "only MAIN's music applied")

	menu.navigate_to_screen("OPTIONS")
	assert_signal_emitted_with_parameters(menu, "music_changed", [0])
	assert_signal_emit_count(menu, "music_changed", 2, "no-MUSICVAR screen resets the selector")
	menu.show_screen("OPTIONS")
	assert_signal_emit_count(menu, "music_changed", 3, "repeated screen events repeat the write")


# --- URL action + MONOGRAM render -------------------------------------------


func test_url_action_emits_url_requested() -> void:
	# <ACTION type="URL"> (shipped splash "buy"/website buttons) routes to
	# url_requested for the shell to open externally; it is not intra-menu nav.
	var menu := _build_menu()
	watch_signals(menu)
	var handled := menu.dispatch_action("url", "www.novalogic.com/buy", "", "")
	assert_true(handled, "url action handled")
	assert_signal_emitted_with_parameters(menu, "url_requested", ["www.novalogic.com/buy"])


func test_url_action_preserves_external_browser_in_shell_payload() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="button" name="BUY"><ACTION type="URL" EXTERNAL_BROWSER>' +
		'https://example.invalid/buy</ACTION></WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	watch_signals(menu)
	(menu.find_child("BUY", true, false) as BaseButton).emit_signal("pressed")
	assert_signal_emitted_with_parameters(
		menu, "url_requested", ["https://example.invalid/buy"])
	assert_signal_emitted(menu, "shell_action_requested")
	var args: Array = get_signal_parameters(menu, "shell_action_requested", 0)
	assert_eq(args[0], "url", "URL remains the shell action type")
	var payload := args[1] as Dictionary
	assert_eq(payload.get("target"), "https://example.invalid/buy", "full target preserved")
	assert_true(bool(payload.get("external_browser", false)),
		"EXTERNAL_BROWSER survives into the shell payload")


func test_shell_owned_action_preserves_complete_payload_without_invented_effects() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="button" name="SEARCH"><ACTION type="LAN_SEARCH" source="lan" ' +
		'field="mode" target_form="7" test="EQ">SERVER_ROWS</ACTION></WINDOW>' +
		'</WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	watch_signals(menu)
	(menu.find_child("SEARCH", true, false) as BaseButton).emit_signal("pressed")
	assert_signal_emitted(menu, "shell_action_requested")
	var args: Array = get_signal_parameters(menu, "shell_action_requested", 0)
	assert_eq(args[0], "lan_search")
	var payload := args[1] as Dictionary
	assert_eq(payload.get("target"), "SERVER_ROWS")
	assert_eq(payload.get("source"), "lan")
	assert_eq(payload.get("field"), "mode")
	assert_eq(payload.get("target_form"), 7)
	assert_eq(payload.get("test"), "EQ")
	assert_eq(menu.current_screen, "S", "generic menu invents no LAN navigation effect")


func test_monogram_parsed_but_not_drawn() -> void:
	# The shipped engine parses a FRAME's MONOGRAM but never draws it: the window
	# render path is frame + appearance + text only [orig: CStaticWnd_Render @ 0x657b10],
	# and CUIElement_DrawFrame @ 0x64a210 has no monogram pass (the only "monogram.tga"
	# use is the loading screen). So the frame stencil renders but no Monogram overlay
	# is created (the earlier centered heuristic produced a stray glyph). A synthetic menu
	# + temp textures drive the resolved path.
	var dir := OS.get_temp_dir().path_join("mnu_mono_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_write_png(dir.path_join("border2.png"), 64, 64, Color(0.7, 0.7, 0.7, 1.0))
	_write_png(dir.path_join("mono.png"), 16, 16, Color(1, 1, 1, 1))

	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		pass_test("temp resource root unavailable: %s" % root.get_last_error())
		return

	# DRAW_FRAME so the frame actually draws (this test's point is that the frame
	# renders but the MONOGRAM does not) [orig: CStaticWnd_Render @ 0x657b10 -> +0x134].
	var mnu_text := "<SCREEN><NAME>S</NAME>" + \
		"<WINDOW type=\"window\" name=\"PANEL\" DRAW_FRAME>" + \
		"<FRAME><STENCIL size=\"64\">border2.tga</STENCIL><MONOGRAM>mono.tga</MONOGRAM></FRAME>" + \
		"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>200</RIGHT><BOTTOM>200</BOTTOM></POSITION>" + \
		"</WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(mnu_text.to_utf8_buffer()), OK, "synthetic menu parses")

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.menu = doc

	# The frame stencil still renders...
	assert_not_null(menu.find_child("FrameTL", true, false), "frame stencil pieces drawn")
	# ...but the MONOGRAM is not (parity: the engine does not draw the menu monogram).
	assert_null(menu.find_child("Monogram", true, false), "MONOGRAM is parsed but not drawn")

	DirAccess.remove_absolute(dir.path_join("border2.png"))
	DirAccess.remove_absolute(dir.path_join("mono.png"))
	DirAccess.remove_absolute(dir)


# --- Keyboard hotkey router (Esc-to-back / Enter-to-accept) ------------------


func test_hotkey_routes_escape_to_pop_button() -> void:
	# A button carrying <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY> + pop_screen fires when
	# Escape is routed: navigate MAIN->SUB, then VK_ESCAPE pops back to MAIN.
	var mnu_text := "<SCREEN><NAME>MAIN</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"GO\"><ACTION type=\"screen\" target=\"SUB\"></ACTION></WINDOW>" + \
		"</WINDOW></SCREEN>" + \
		"<SCREEN><NAME>SUB</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"BACK\"><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>" + \
		"<ACTION type=\"pop_screen\"></ACTION></WINDOW></WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(mnu_text.to_utf8_buffer()), OK, "two-screen menu parses")

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc

	assert_true(menu.navigate_to_screen("SUB"), "navigated MAIN -> SUB")
	assert_eq(menu.current_screen, "SUB", "on SUB")
	assert_true(menu.handle_hotkey("VK_ESCAPE"), "VK_ESCAPE routed to the BACK button")
	assert_eq(menu.current_screen, "MAIN", "Esc popped back to MAIN")
	assert_false(menu.handle_hotkey("VK_F12"), "unbound key not handled")


func test_hotkey_router_inert_in_edit_mode() -> void:
	var mnu_text := "<SCREEN><NAME>MAIN</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"BACK\"><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>" + \
		"<ACTION type=\"pop_screen\"></ACTION></WINDOW></WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(mnu_text.to_utf8_buffer())
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(true)
	menu.menu = doc
	assert_false(menu.handle_hotkey("VK_ESCAPE"), "router inert in edit_mode")


func test_real_shipped_button_dispatches_its_action() -> void:
	# Press the real jo_options BACK button (<ACTION type="pop_screen">) and assert
	# the navigation verb dispatches - proving the verbs work on shipped content,
	# not just the synthetic widgets.mnu.
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_options.mnu"))
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	watch_signals(menu)
	var back := menu.find_child("BACK", true, false)
	assert_not_null(back, "real BACK button built")
	assert_true(back is NovaMnuButton, "BACK is a NovaMnuButton")
	(back as NovaMnuButton).emit_signal("pressed")
	assert_signal_emitted_with_parameters(menu, "action_dispatched", ["pop_screen", ""])


# --- Router hardening (visibility, no-op, real key adapter, aliases) ---------


func _key(keycode: int, pressed := true, echo := false) -> InputEventKey:
	var k := InputEventKey.new()
	k.keycode = keycode
	k.pressed = pressed
	k.echo = echo
	return k


func _menu_from(bytes: PackedByteArray, edit_mode := false) -> NovaMnuMenu:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(bytes)
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(edit_mode)
	menu.menu = doc
	return menu


func _esc_back_menu_bytes() -> PackedByteArray:
	return ("<SCREEN><NAME>MAIN</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"GO\"><ACTION type=\"screen\" target=\"SUB\"></ACTION></WINDOW>" + \
		"</WINDOW></SCREEN>" + \
		"<SCREEN><NAME>SUB</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"BACK\"><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>" + \
		"<ACTION type=\"pop_screen\"></ACTION></WINDOW></WINDOW></SCREEN>").to_utf8_buffer()


func test_unhandled_key_input_drives_router() -> void:
	# Exercise the real keycode->VK adapter (not handle_hotkey directly).
	var menu := _menu_from(_esc_back_menu_bytes())
	menu.navigate_to_screen("SUB")
	menu.handle_key_input(_key(KEY_ESCAPE))
	assert_eq(menu.current_screen, "MAIN", "real KEY_ESCAPE routed to pop")
	# Unmapped key, echo, and release are all inert.
	menu.navigate_to_screen("SUB")
	menu.handle_key_input(_key(KEY_A))
	menu.handle_key_input(_key(KEY_ESCAPE, true, true))
	menu.handle_key_input(_key(KEY_ESCAPE, false))
	assert_eq(menu.current_screen, "SUB", "unmapped / echo / release ignored")


func test_hotkey_skips_hidden_widget() -> void:
	# A hidden widget sharing the hotkey must NOT fire; the visible target wins.
	var bytes := ("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"HIDDEN_BACK\" HIDDEN><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>" + \
		"<ACTION type=\"screen\" target=\"WRONG\"></ACTION></WINDOW>" + \
		"<WINDOW type=\"button\" name=\"VISIBLE_OK\"><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>" + \
		"<ACTION type=\"screen\" target=\"RIGHT\"></ACTION></WINDOW></WINDOW></SCREEN>" + \
		"<SCREEN><NAME>WRONG</NAME><WINDOW type=\"window\" name=\"ROOT\"></WINDOW></SCREEN>" + \
		"<SCREEN><NAME>RIGHT</NAME><WINDOW type=\"window\" name=\"ROOT\"></WINDOW></SCREEN>").to_utf8_buffer()
	var menu := _menu_from(bytes)
	assert_true(menu.handle_hotkey("VK_ESCAPE"), "a visible hotkey target handled it")
	assert_eq(menu.current_screen, "RIGHT", "hidden HIDDEN_BACK skipped; VISIBLE_OK fired")


func test_hotkey_actionless_named_button_activates_and_consumes() -> void:
	# Shipped actionless named controls are shell Command seams: normal activation
	# still emits pressed and owns the authored accelerator.
	var bytes := ("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"NOOP\"><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY></WINDOW>" + \
		"</WINDOW></SCREEN>").to_utf8_buffer()
	var menu := _menu_from(bytes)
	var button := menu.find_child("NOOP", true, false) as BaseButton
	watch_signals(button)
	assert_true(menu.handle_hotkey("VK_ESCAPE"), "named actionless control owns its key")
	assert_signal_emitted(button, "pressed")


func test_hotkey_keeps_all_bindings_and_matches_literal_unicode_case_insensitively() -> void:
	var bytes := ("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"DUAL\"><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>" + \
		"<HOTKEY>é</HOTKEY></WINDOW></WINDOW></SCREEN>").to_utf8_buffer()
	var menu := _menu_from(bytes)
	var button := menu.find_child("DUAL", true, false) as BaseButton
	watch_signals(button)
	assert_true(menu.handle_character_hotkey("É"), "Unicode accelerator is case-insensitive")
	assert_true(menu.handle_hotkey("VK_ESCAPE"), "the preceding virtual binding also remains")
	assert_signal_emit_count(button, "pressed", 2, "both authored bindings activate normally")


func test_literal_ascii_hotkey_routes_through_real_key_adapter() -> void:
	var bytes := ("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"LETTER\"><HOTKEY>V</HOTKEY></WINDOW>" + \
		"</WINDOW></SCREEN>").to_utf8_buffer()
	var menu := _menu_from(bytes)
	var button := menu.find_child("LETTER", true, false) as BaseButton
	watch_signals(button)
	assert_true(menu.handle_key_input(_key(KEY_V)), "printable keycode reaches literal router")
	assert_signal_emitted(button, "pressed")


func test_hotkey_enter_aliases() -> void:
	# VK_RETURN / VK_ENTER are interchangeable, and numpad Enter maps to VK_RETURN.
	var bytes := ("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"button\" name=\"OK\"><HOTKEY VIRTUAL>VK_ENTER</HOTKEY>" + \
		"<ACTION type=\"screen\" target=\"NEXT\"></ACTION></WINDOW></WINDOW></SCREEN>" + \
		"<SCREEN><NAME>NEXT</NAME><WINDOW type=\"window\" name=\"ROOT\"></WINDOW></SCREEN>").to_utf8_buffer()
	var menu := _menu_from(bytes)
	assert_true(menu.handle_hotkey("VK_RETURN"), "VK_RETURN matches a stored VK_ENTER")
	assert_eq(menu.current_screen, "NEXT", "Enter accept navigated")
	menu.show_screen("S")
	menu.handle_key_input(_key(KEY_KP_ENTER))
	assert_eq(menu.current_screen, "NEXT", "numpad Enter routed via VK_RETURN")


func test_hotkey_router_inert_when_menu_hidden() -> void:
	# A hidden menu kept in the tree must not route/consume keys.
	var menu := _menu_from(_esc_back_menu_bytes())
	menu.navigate_to_screen("SUB")
	menu.visible = false
	menu.handle_key_input(_key(KEY_ESCAPE))
	assert_eq(menu.current_screen, "SUB", "hidden menu's key adapter is inert")


func test_hotkey_toggles_checkbox() -> void:
	# A hotkey on a checkbox flips it (a bare emit "pressed" would not toggle).
	var bytes := ("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<WINDOW type=\"checkbox\" name=\"CHK\" CHECKED><HOTKEY VIRTUAL>VK_RETURN</HOTKEY></WINDOW>" + \
		"</WINDOW></SCREEN>").to_utf8_buffer()
	var menu := _menu_from(bytes)
	var chk := menu.find_child("CHK", true, false) as BaseButton
	assert_not_null(chk, "checkbox built")
	assert_true(chk.button_pressed, "starts checked")
	assert_true(menu.handle_hotkey("VK_RETURN"), "checkbox hotkey handled")
	assert_false(chk.button_pressed, "hotkey toggled the checkbox off")


func test_checkbox_toggle_drives_underline() -> void:
	var menu := _build_menu()
	var chk := menu.find_child("SoundChk", true, false) as NovaMnuCheckBox
	var underline := chk.find_child("Underline", true, false) as ColorRect
	assert_not_null(underline, "checkbox builds an Underline child at runtime")
	assert_true(chk.button_pressed, "SoundChk starts checked (CHECKED flag)")
	assert_true(underline.visible, "underline shown while checked")

	chk.button_pressed = false  # fires toggled -> update_appearance
	assert_false(underline.visible, "underline hidden once unchecked")


func test_checkbox_as_button_uses_full_rect_label_layout() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="checkbox" name="FULL" AS_BUTTON CHECKED>' +
		'<POSITION left="0" top="0" right="120" bottom="24"></POSITION>' +
		'<STRING justify="RIGHT">Full</STRING></WINDOW>' +
		'<WINDOW type="checkbox" name="NORMAL">' +
		'<POSITION left="0" top="30" right="120" bottom="54"></POSITION>' +
		'<STRING>Normal</STRING></WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	var full := menu.find_child("FULL", true, false) as BaseButton
	var full_label := full.find_child("Label", false, false) as Label
	var normal_label := menu.find_child("NORMAL", true, false).find_child(
		"Label", false, false) as Label
	assert_true(full.button_pressed, "AS_BUTTON keeps normal checkbox toggle state")
	assert_eq(full_label.anchor_right, 1.0, "AS_BUTTON label spans the full widget")
	assert_eq(full_label.position, Vector2.ZERO, "AS_BUTTON has no checkbox-art indent")
	assert_eq(full_label.horizontal_alignment, HORIZONTAL_ALIGNMENT_RIGHT,
		"AS_BUTTON honors authored alignment")
	assert_eq(normal_label.position.x, 26.0, "normal checkbox label follows 24px art + 2px")


func test_shell_owned_widget_preview_is_explicit_and_never_fake_data() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="GLB_TABLE" name="GLOBAL"></WINDOW>' +
		'<WINDOW type="LAN_LIST" name="LAN"></WINDOW>' +
		'<WINDOW type="GOPHER" name="NEWS"></WINDOW>' +
		'</WINDOW></SCREEN>').to_utf8_buffer()
	var author_menu := _menu_from(bytes, true)
	for name in ["GLOBAL", "LAN", "NEWS"]:
		var widget := author_menu.find_child(name, true, false) as Control
		assert_true(bool(widget.get_meta("mnu_shell_owned")), "%s marks shell ownership" % name)
		var notice := widget.find_child("ShellOwnedPreview", false, false) as Label
		assert_not_null(notice, "%s has an honest authoring notice" % name)
		assert_string_contains(notice.text, "Game-supplied runtime data")
	var runtime_menu := _menu_from(bytes)
	assert_null(runtime_menu.find_child("ShellOwnedPreview", true, false),
		"runtime tree contains no fabricated preview rows or labels")


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


func test_parent_reenable_preserves_child_local_disabled_state() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="window" name="PARENT">' +
		'<WINDOW type="button" name="LOCAL_OFF" DISABLE></WINDOW>' +
		'<WINDOW type="button" name="LOCAL_ON"></WINDOW>' +
		'</WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	var parent := menu.find_child("PARENT", true, false) as Control
	var local_off := menu.find_child("LOCAL_OFF", true, false) as BaseButton
	var local_on := menu.find_child("LOCAL_ON", true, false) as BaseButton
	assert_true(local_off.disabled, "authored child starts locally disabled")
	assert_false(local_on.disabled, "sibling starts locally enabled")
	assert_true(menu.handle_window_action("PARENT", "DISABLE"), "parent disable handled")
	assert_true(local_off.disabled and local_on.disabled, "parent gates both descendants")
	assert_true(menu.handle_window_action("PARENT", "ENABLE"), "parent re-enable handled")
	assert_true(local_off.disabled, "local authored DISABLE survives parent re-enable")
	assert_false(local_on.disabled, "locally enabled sibling wakes back up")
	assert_eq(parent.process_mode, Node.PROCESS_MODE_INHERIT, "parent local state restored")
	assert_true(menu.handle_window_action("LOCAL_OFF", "ENABLE"), "child can be enabled explicitly")
	assert_false(local_off.disabled, "explicit child ENABLE overrides its initial local state")


func test_tab_action_focuses_named_control_without_shell_dispatch() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="button" name="DO_TAB"><ACTION type="TAB">TARGET</ACTION></WINDOW>' +
		'<WINDOW type="edit" name="TARGET"><POSITION left="0" top="0" right="120" bottom="24">' +
		'</POSITION></WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	watch_signals(menu)
	var target := menu.find_child("TARGET", true, false) as LineEdit
	(menu.find_child("DO_TAB", true, false) as BaseButton).emit_signal("pressed")
	assert_true(target.has_focus(), "TAB selects the named focus target")
	assert_signal_not_emitted(menu, "shell_action_requested",
		"TAB is menu-owned, not delegated with GLB/LAN operations")


func test_edit_constraints_filter_clamp_password_and_notify_committed_value() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="edit" name="PIN" NUMBER PASSWORD MINVAL="1" MAXVAL="99" MAXCHAR="3">' +
		'<POSITION left="0" top="0" right="120" bottom="24"></POSITION>' +
		'</WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	var edit := menu.find_child("PIN", true, false) as NovaMnuEdit
	assert_not_null(edit, "constrained edit built")
	assert_true(edit.is_numeric_only(), "NUMBER enables numeric filtering")
	assert_true(edit.secret, "PASSWORD uses LineEdit secret display")
	assert_eq(edit.max_length, 3, "MAXCHAR maps to max_length")
	edit.text = "a2b"
	edit.text_changed.emit("a2b")
	assert_eq(edit.text, "2", "non-numeric input is filtered")
	watch_signals(menu)
	edit.text = "999"
	edit.focus_exited.emit()
	assert_eq(edit.text, "99", "focus commit clamps to MAXVAL")
	assert_signal_emitted_with_parameters(
		menu, "widget_value_changed", ["PIN", "edit", -1, "99"])


func test_radioedit_select_then_edit_then_copy_back_on_focus_loss() -> void:
	var bytes := ('<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' +
		'<WINDOW type="radioedit" name="CALLSIGN" group="3">' +
		'<POSITION left="0" top="0" right="160" bottom="24"></POSITION>' +
		'<STRING>Alpha</STRING></WINDOW></WINDOW></SCREEN>').to_utf8_buffer()
	var menu := _menu_from(bytes)
	var edit := menu.find_child("CALLSIGN", true, false) as NovaMnuEdit
	var radio := edit.find_child("Radio", false, false) as BaseButton
	var label := radio.find_child("Label", false, false) as Label
	assert_not_null(radio, "RADIOEDIT owns its radio presentation")
	assert_false(radio.button_pressed, "starts unselected when CHECKED is absent")
	assert_false(edit.editable, "edit child starts hidden behind the radio presentation")
	# First activation selects only.
	radio.button_down.emit()
	radio.button_pressed = true
	radio.pressed.emit()
	assert_false(edit.is_radio_editing(), "first activation selects without editing")
	assert_true(radio.visible, "radio presentation remains after first selection")
	# Activating the selected radio opens/focuses the edit child.
	radio.button_down.emit()
	radio.pressed.emit()
	assert_true(edit.is_radio_editing(), "second activation enters edit mode")
	assert_false(radio.visible, "radio child hides while editing")
	assert_true(edit.editable, "edit child becomes editable")
	edit.text = "Bravo"
	edit.focus_exited.emit()
	assert_false(edit.is_radio_editing(), "focus loss returns to radio mode")
	assert_true(radio.visible, "radio presentation restored")
	assert_eq(label.text, "Bravo", "edited text copied back to radio presentation")


func test_interactive_preview_button_runs_window_action() -> void:
	# In the ONED interactive preview, an edit_mode tree wires its navigators so a tab
	# click runs its window show/hide actions (clicking a tab shows only its panel).
	var xml := '<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">' + '<WINDOW type="button" name="TAB"><ACTION type="window" state="HIDE">PANEL</ACTION></WINDOW>' + '<WINDOW type="window" name="PANEL"></WINDOW></WINDOW></SCREEN>'
	var menu := _menu_from(xml.to_utf8_buffer(), true)  # edit_mode preview
	assert_true((menu.find_child("TAB", true, false) as BaseButton).disabled, "author-mode button is inert")
	menu.set_interactive(true)
	var tab := menu.find_child("TAB", true, false) as BaseButton  # tree rebuilt
	var panel := menu.find_child("PANEL", true, false) as Control
	assert_not_null(tab, "button rebuilt")
	assert_not_null(panel, "panel rebuilt")
	assert_false(tab.disabled, "interactive button is live")
	assert_true(panel.visible, "panel visible before the click")
	tab.emit_signal("pressed")
	assert_false(panel.visible, "interactive click ran the HIDE window action")


func test_interactive_preview_clamps_external_side_effects() -> void:
	# The interactive sandbox must not reach the editor shell: quit/url/cross-file are
	# no-ops, while window show/hide still runs.
	var xml := '<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT"><WINDOW type="window" name="PANEL"></WINDOW></WINDOW></SCREEN>'
	var menu := _menu_from(xml.to_utf8_buffer(), true)
	menu.set_interactive(true)
	watch_signals(menu)
	assert_true(menu.dispatch_action("quit", "", "", ""), "quit consumed")
	assert_true(menu.dispatch_action("url", "http://x", "", ""), "url consumed")
	assert_true(menu.dispatch_action("screen", "X", "other.mnu", ""), "cross-file consumed")
	assert_signal_not_emitted(menu, "quit_requested")
	assert_signal_not_emitted(menu, "url_requested")
	assert_signal_not_emitted(menu, "menu_requested")
	var panel := menu.find_child("PANEL", true, false) as Control
	assert_true(panel.visible, "panel visible before")
	assert_true(menu.dispatch_action("window", "PANEL", "", "hide"), "window action still runs")
	assert_false(panel.visible, "window hidden in the sandbox")


func test_interactive_collapses_to_single_screen() -> void:
	# Author preview shows every screen at once; interactive shows only the current one.
	var xml := '<SCREEN><NAME>A</NAME><WINDOW type="window" name="ROOT"></WINDOW></SCREEN><SCREEN><NAME>B</NAME><WINDOW type="window" name="ROOT"></WINDOW></SCREEN>'
	var menu := _menu_from(xml.to_utf8_buffer(), true)
	assert_true(_screen_visible(menu, "A") and _screen_visible(menu, "B"), "author shows all screens")
	menu.set_interactive(true)
	assert_true(_screen_visible(menu, "A"), "current screen A visible")
	assert_false(_screen_visible(menu, "B"), "other screen hidden in interactive")


func _screen_visible(menu: NovaMnuMenu, name: String) -> bool:
	for c in menu.get_children():
		if c is NovaMnuScreen and c.get_screen_name() == name:
			return c.visible
	return false


func test_navigate_to_menu_emits_menu_requested() -> void:
	# Cross-.mnu jumps are shell policy: navigate_to_menu (and a screen action with a
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
	# OPTIONS has no MUSICVAR: its parsed default zero is still pushed.
	menu.navigate_to_screen("OPTIONS")
	assert_eq(dir.get_var(0), 0, "no-MUSICVAR screen resets the director var")


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


const MENU_SOUND_DIR := "res://../fixtures/menu_sound"  # menu.LWF + its loose .wav members


# Build a menu wired to the real menu.lwf profile over the menu_sound fixtures
# dir. Returns null when the fixtures/resource root are unavailable (caller skips).
func _menu_with_profile() -> NovaMnuMenu:
	var root := NovaResourceRoot.new()
	if root.set_root_dir(ProjectSettings.globalize_path(MENU_SOUND_DIR)) != OK:
		return null
	var profile := NovaLwfData.new()
	if profile.open_from_resource_root(root, "menu.LWF") != OK or profile.get_set_count() == 0:
		return null
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	menu.set_edit_mode(false)
	menu.set_resource_root(root)
	menu.set_sound_profile(profile)
	add_child_autofree(menu)
	return menu


func _has_playing_stream(menu: NovaMnuMenu) -> bool:
	for child in menu.get_children():
		if child is AudioStreamPlayer and (child as AudioStreamPlayer).stream != null:
			return true
	return false


func test_lwf_profile_resolves_trigger_to_wav_and_plays() -> void:
	# The faithful path: the trigger names a set in menu.lwf, whose member resolves
	# to a loose .wav (MOUSE_OVER -> MSOVR_*.wav) that gets decoded onto the pool.
	var menu := _menu_with_profile()
	if menu == null:
		pass_test("menu_sound fixtures unavailable")
		return
	watch_signals(menu)
	menu.play_widget_sound("MOUSE_OVER", "menu.lwf")
	assert_signal_emitted_with_parameters(menu, "sound_requested", ["menu.lwf", "MOUSE_OVER"])
	assert_true(_has_playing_stream(menu), "MOUSE_OVER resolves menu.lwf -> a .wav and plays it")


func test_lwf_profile_unknown_trigger_plays_nothing() -> void:
	# An unknown trigger matches no set: emit the signal, but resolve to silence
	# (no SBF fallback noise, no pooled player materialized).
	var menu := _menu_with_profile()
	if menu == null:
		pass_test("menu_sound fixtures unavailable")
		return
	menu.play_widget_sound("NOT_A_TRIGGER", "menu.lwf")
	assert_false(_has_playing_stream(menu), "an unknown trigger plays nothing")


func test_lwf_profile_inert_in_edit_mode() -> void:
	# Edit mode (the ONED preview) suppresses audio even with a valid profile.
	var menu := _menu_with_profile()
	if menu == null:
		pass_test("menu_sound fixtures unavailable")
		return
	menu.set_edit_mode(true)
	menu.play_widget_sound("MOUSE_OVER", "menu.lwf")
	assert_false(_has_playing_stream(menu), "edit mode keeps the menu silent")


# Bare menu over the menu_sound fixtures with NO profile/SBF set, or null when
# the fixtures are unavailable (caller skips).
func _menu_with_root_only() -> NovaMnuMenu:
	var root := NovaResourceRoot.new()
	if root.set_root_dir(ProjectSettings.globalize_path(MENU_SOUND_DIR)) != OK:
		return null
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	menu.set_edit_mode(false)
	menu.set_resource_root(root)
	add_child_autofree(menu)
	return menu


func test_widget_sound_resolves_per_element_bank_without_profile() -> void:
	# The faithful path needs no shell profile: the <SOUND> file names the bank,
	# which loads into the menu's cache on first use, and the trigger names a
	# set inside it [orig: sound_bank_collection_add_or_ref @ 0x652b40 from
	# CUIElement_ParseXMLDefinition @ 0x648ada].
	var menu := _menu_with_root_only()
	if menu == null:
		pass_test("menu_sound fixtures unavailable")
		return
	menu.play_widget_sound("MOUSE_OVER", "menu.lwf")
	assert_true(_has_playing_stream(menu), "per-element bank resolves and plays with no profile")


func test_widget_sound_missing_bank_is_silent() -> void:
	# An unresolvable bank file leaves the element silent, like the original's
	# failed add-ref leaving the element's bank id 0 [orig: @ 0x652c95].
	var menu := _menu_with_root_only()
	if menu == null:
		pass_test("menu_sound fixtures unavailable")
		return
	menu.play_widget_sound("MOUSE_OVER", "no_such_bank.lwf")
	assert_false(_has_playing_stream(menu), "unresolvable bank file plays nothing")


func test_navigation_stack_clear_and_no_redundant_push() -> void:
	var menu := _build_menu()
	# Navigating to the already-current screen must not self-push.
	menu.navigate_to_screen("MAIN")
	assert_false(menu.pop_screen(), "same-screen navigation pushed nothing")
	# A real forward move pushes; clear empties the stack.
	menu.navigate_to_screen("OPTIONS")
	menu.clear_navigation_stack()
	assert_false(menu.pop_screen(), "cleared stack pops to the empty (quit) path")
