extends GutTest

# M9.6 gate: the special view widgets (Map, Globe, Marquee). They are inherently
# runtime/data-driven, so the builder shows a styled background or labeled
# placeholder and a host drives them. Map pans/zooms a supplied image + markers,
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
	assert_eq(map.get_map_texture(), tex, "host map texture round-trips")
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
	assert_eq(lbl.text, "Hello\nWorld", "host content drives the label")
