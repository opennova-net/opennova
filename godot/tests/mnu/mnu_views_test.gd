extends GutTest

# M9.6 gate: the special view widgets (Map, Globe, Marquee). They are inherently
# runtime/data-driven, so the builder shows a styled background or labeled
# placeholder and the shell drives them. Map pans/zooms a supplied image + markers,
# Globe auto-rotates a supplied image, Marquee scrolls DATASOURCE/STRING text.

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


func _tex(c: Color = Color.WHITE) -> Texture2D:
	var img := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	img.fill(c)
	return ImageTexture.create_from_image(img)


# --- Map --------------------------------------------------------------------

func test_map_builds_and_data_binding() -> void:
	var menu := _build_menu()
	var map := menu.find_child("TacMap", true, false)
	assert_true(map is NovaMnuMap, "map becomes a NovaMnuMap")
	assert_not_null(map.find_child("MapImage", true, false), "runtime map image layer")
	var tex := _tex()
	map.set_map_texture(tex)
	assert_eq(map.get_map_texture(), tex, "shell map texture round-trips")
	map.set_zoom(10.0)
	assert_eq(map.get_zoom(), 4.0, "zoom clamps to max")
	var id: int = map.add_marker(tex, Vector2(5, 5))
	assert_eq(map.get_marker_count(), 1, "marker added")
	assert_not_null(map.find_child("Marker%d" % id, true, false), "marker node built")
	map.remove_marker(id)
	assert_eq(map.get_marker_count(), 0, "marker removed")


func test_map_placeholder_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var map := menu.find_child("TacMap", true, false)
	# No resource root, so the unresolved appearance degrades to a labeled placeholder.
	assert_not_null(map.find_child("PlaceholderLabel", true, false), "labeled placeholder shown")


# --- Globe ------------------------------------------------------------------

func test_globe_data_and_rotation() -> void:
	var menu := _build_menu()
	var globe := menu.find_child("CampaignGlobe", true, false)
	assert_true(globe is NovaMnuGlobe, "globe becomes a NovaMnuGlobe")
	assert_not_null(globe.find_child("GlobeImage", true, false), "globe image layer")
	globe.set_globe_texture(_tex())
	globe.set_angle(45.0)
	assert_almost_eq(globe.get_angle(), 45.0, 0.01, "angle round-trips")
	globe.set_rotation_speed(30.0)
	globe.set_auto_rotate(true)
	assert_true(globe.is_processing(), "auto-rotates at runtime")


func test_globe_static_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var globe := menu.find_child("CampaignGlobe", true, false)
	globe.set_rotation_speed(30.0)
	globe.set_auto_rotate(true)
	assert_false(globe.is_processing(), "no rotation while authoring")


# --- Marquee ----------------------------------------------------------------

func test_marquee_falls_back_to_string_and_scrolls() -> void:
	var menu := _build_menu()
	var m := menu.find_child("Credits", true, false)
	assert_true(m is NovaMnuMarquee, "marquee becomes a NovaMnuMarquee")
	# No resource root -> DATASOURCE unresolved -> STRING fallback.
	assert_eq(m.get_content(), "Rolling credits", "STRING fallback content")
	assert_true(m.is_processing(), "scrolls at runtime")


func test_marquee_static_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var m := menu.find_child("Credits", true, false)
	assert_false(m.is_processing(), "static while authoring")


func test_marquee_datasource_resolves_with_root() -> void:
	var root := NovaResourceRoot.new()
	var dir := ProjectSettings.globalize_path("res://../fixtures/mnu")
	if root.set_root_dir(dir) != OK:
		pass_test("resource root unavailable: %s" % root.get_last_error())
		return
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.menu = _load_doc()
	var m := menu.find_child("Credits", true, false)
	assert_true(m.get_content().contains("OpenNova"), "DATASOURCE credits.txt resolved + read")


func test_marquee_set_content_runtime() -> void:
	var menu := _build_menu()
	var m := menu.find_child("Credits", true, false)
	m.set_content("Hello\nWorld")
	var lbl := m.find_child("Content", true, false) as Label
	assert_eq(lbl.text, "Hello\nWorld", "shell content drives the label")


# --- Faithful partial-POSITION layout ----------------------------------------
# [orig: CUIElement_ParseXMLDefinition @ 0x648120 - degenerate-axis fallback to
#  the largest appearance image, then adjust_rect_to_text_size @ 0x6575f0 for
#  text widgets, anchored per JUSTIFY/VJUSTIFY. NO per-type default sizes.]
# With no resolvable font the builder measures a nominal 8x16 glyph box, making
# these assertions deterministic in headless runs.

const LAYOUT_MENU := """<MENU>
<SCREEN name="MAIN">
<WINDOW type="WINDOW" name="Root"><POSITION left="0" top="0" right="640" bottom="480"></POSITION>
<WINDOW type="BUTTON" name="BLeft"><POSITION left="40" top="10"></POSITION><STRING>SOLO</STRING></WINDOW>
<WINDOW type="STATIC" name="SCenter"><POSITION left="320" top="200"></POSITION><STRING justify="CENTER" vjustify="CENTER">AB</STRING></WINDOW>
<WINDOW type="STATIC" name="SRight"><POSITION left="600" top="50"></POSITION><STRING justify="RIGHT" vjustify="BOTTOM">AB</STRING></WINDOW>
<WINDOW type="WINDOW" name="WEmpty"><POSITION left="5" top="6"></POSITION></WINDOW>
</WINDOW>
</SCREEN>
</MENU>"""


func _layout_widget(menu: NovaMnuMenu, wname: String) -> Control:
	return menu.find_child(wname, true, false) as Control


func _build_layout_menu() -> NovaMnuMenu:
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(LAYOUT_MENU.to_utf8_buffer()), OK, "layout menu parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(true)
	menu.menu = doc
	return menu


func test_layout_text_button_sizes_from_text() -> void:
	var menu := _build_layout_menu()
	var b := _layout_widget(menu, "BLeft")
	assert_eq(b.position, Vector2(40, 10), "authored point is the top-left anchor")
	assert_eq(b.size, Vector2(8 * 4, 16), "LEFT/TOP justify: rect grows right/down by the text box")


func test_layout_center_justify_anchors_at_point() -> void:
	var menu := _build_layout_menu()
	var s := _layout_widget(menu, "SCenter")
	assert_eq(s.position, Vector2(320 - 8, 200 - 8), "CENTER: the authored point is the text centre")
	assert_eq(s.size, Vector2(16, 16), "2-char nominal text box")


func test_layout_right_bottom_justify_anchor_edges() -> void:
	var menu := _build_layout_menu()
	var s := _layout_widget(menu, "SRight")
	assert_eq(s.position, Vector2(600 - 16, 50 - 16), "RIGHT/BOTTOM: the authored point is the far edge")
	assert_eq(s.size, Vector2(16, 16), "2-char nominal text box")


func test_layout_empty_window_has_no_default_size() -> void:
	var menu := _build_layout_menu()
	var w := _layout_widget(menu, "WEmpty")
	assert_eq(w.position, Vector2(5, 6), "authored position kept")
	assert_eq(w.size, Vector2(0, 0), "no image, no text, no authored size -> zero (no per-type defaults)")
