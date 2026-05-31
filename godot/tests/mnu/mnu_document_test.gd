extends GutTest

# M2 gate: load a fixture .mnu into NovaMnuDocument, assert the parsed tree, and
# verify a serialize round-trip plus the core mutation surface.

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


func _load_doc() -> NovaMnuDocument:
	# Read bytes directly so the test does not depend on the resource cache.
	var bytes := FileAccess.get_file_as_bytes(FIXTURE)
	assert_gt(bytes.size(), 0, "fixture bytes are non-empty")
	var doc := NovaMnuDocument.new()
	var err := doc.load_from_bytes(bytes)
	assert_eq(err, OK, "load_from_bytes succeeds")
	return doc


func test_resource_loader_returns_document() -> void:
	var res := load(FIXTURE)
	assert_not_null(res, "loader returns non-null for " + FIXTURE)
	assert_true(res is NovaMnuDocument, "loader returns NovaMnuDocument")


func test_screen_structure() -> void:
	var doc := _load_doc()
	assert_eq(doc.get_screen_count(), 2, "two screens")
	var screens := doc.get_screen_ids()
	assert_eq(screens.size(), 2, "two screen ids")

	var main_id: int = screens[0]
	assert_eq(doc.get_screen_name(main_id), "MAIN", "screen 0 name")
	assert_eq(doc.get_screen_music_var(main_id), 3, "screen 0 music_var")
	assert_eq(doc.get_screen_text_rsrc(main_id), "menutxt.BIN", "screen 0 text_rsrc")
	assert_true(doc.is_screen(main_id), "screen id is a screen")


func test_root_window_and_children() -> void:
	var doc := _load_doc()
	var main_id: int = doc.get_screen_ids()[0]
	var root_id := doc.get_screen_root_id(main_id)
	assert_gt(root_id, 0, "root id valid")
	assert_eq(doc.get_widget_name(root_id), "ROOT", "root window name")
	assert_eq(doc.get_widget_type(root_id), NovaMnuDocument.TYPE_WINDOW, "root is a window")
	assert_eq(doc.get_parent_id(root_id), main_id, "root's parent is its screen")

	var children := doc.get_child_ids(root_id)
	assert_eq(children.size(), 5, "root has 5 child widgets")

	var names := []
	var types := []
	for cid in children:
		names.append(doc.get_widget_name(cid))
		types.append(doc.get_widget_type(cid))
	assert_eq(names, ["Title", "StartBtn", "SoundChk", "Difficulty", "Version"], "child names in order")
	assert_eq(types[0], NovaMnuDocument.TYPE_STATIC, "Title is static")
	assert_eq(types[1], NovaMnuDocument.TYPE_BUTTON, "StartBtn is button")
	assert_eq(types[2], NovaMnuDocument.TYPE_CHECKBOX, "SoundChk is checkbox")
	assert_eq(types[3], NovaMnuDocument.TYPE_SPINLIST, "Difficulty is spinlist")
	assert_eq(types[4], NovaMnuDocument.TYPE_LABEL, "Version is label")


func test_widget_rect_and_properties() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id: int = doc.get_child_ids(root_id)[1]  # StartBtn

	var rect := doc.get_window_rect(start_id)
	assert_eq(rect, Rect2(270, 120, 100, 30), "StartBtn rect")
	assert_eq(doc.get_widget_text(start_id), "MM_Start", "StartBtn string value")
	assert_eq(doc.get_widget_string_type(start_id), "id", "StartBtn string is an id ref")
	assert_eq(doc.get_widget_texture(start_id, NovaMnuDocument.TEX_DEFAULT), "btn_up.tga", "default appearance")
	assert_eq(doc.get_widget_texture(start_id, NovaMnuDocument.TEX_MOUSEOVER), "btn_over.tga", "mouseover appearance")
	assert_eq(doc.get_widget_font(root_id), "%DEF_FONTNAME%", "font %VAR% preserved raw")
	assert_eq(doc.get_widget_color(root_id, NovaMnuDocument.COLOR_DEFAULT_FG), "%DEF_TEXT_FG%", "color %VAR% preserved raw")

	var chk_id: int = doc.get_child_ids(root_id)[2]  # SoundChk
	assert_true((doc.get_widget_flags(chk_id) & NovaMnuDocument.FLAG_CHECKED) != 0, "SoundChk is checked")


func test_serialize_roundtrip() -> void:
	var doc := _load_doc()
	var bytes := doc.to_byte_array()
	assert_gt(bytes.size(), 0, "serialize produces bytes")

	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(bytes), OK, "re-parse serialized bytes")
	assert_eq(doc2.get_screen_count(), 2, "round-trip keeps 2 screens")

	var root2 := doc2.get_screen_root_id(doc2.get_screen_ids()[0])
	assert_eq(doc2.get_child_ids(root2).size(), 5, "round-trip keeps 5 children")
	assert_eq(doc2.get_screen_music_var(doc2.get_screen_ids()[0]), 3, "round-trip keeps music_var")


func test_mutation_emits_changed_and_persists() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id: int = doc.get_child_ids(root_id)[1]

	watch_signals(doc)
	doc.set_widget_name(start_id, "PlayBtn")
	doc.set_window_rect(start_id, Rect2(10, 20, 80, 24))
	assert_signal_emitted(doc, "changed", "mutations emit Resource.changed")
	assert_eq(doc.get_widget_name(start_id), "PlayBtn", "name mutated")
	assert_eq(doc.get_window_rect(start_id), Rect2(10, 20, 80, 24), "rect mutated")

	# The id stays stable across a serialize round-trip is NOT guaranteed (ids are
	# per-instance), but the mutation must survive serialize -> parse by content.
	var doc2 := NovaMnuDocument.new()
	doc2.load_from_bytes(doc.to_byte_array())
	var root2 := doc2.get_screen_root_id(doc2.get_screen_ids()[0])
	assert_eq(doc2.get_widget_name(doc2.get_child_ids(root2)[1]), "PlayBtn", "renamed widget persisted")


func test_add_and_delete_widget() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var before := doc.get_child_ids(root_id).size()

	var new_id := doc.add_widget(root_id, NovaMnuDocument.TYPE_BUTTON, Rect2(0, 0, 50, 20))
	assert_gt(new_id, 0, "add_widget returns a valid id")
	assert_eq(doc.get_child_ids(root_id).size(), before + 1, "child added")
	assert_eq(doc.get_parent_id(new_id), root_id, "new widget parented to root")
	assert_eq(doc.get_widget_type(new_id), NovaMnuDocument.TYPE_BUTTON, "new widget is a button")

	doc.delete_widget(new_id)
	assert_eq(doc.get_child_ids(root_id).size(), before, "child removed")
	assert_false(doc.widget_exists(new_id), "deleted id no longer exists")


func test_create_empty() -> void:
	var doc := NovaMnuDocument.new()
	doc.create_empty()
	assert_eq(doc.get_screen_count(), 1, "empty doc has one screen")
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	assert_eq(doc.get_widget_type(root), NovaMnuDocument.TYPE_WINDOW, "empty root is a window")


func test_mns_stylesheet() -> void:
	var bytes := FileAccess.get_file_as_bytes("res://../fixtures/mns/test_style.mns")
	assert_gt(bytes.size(), 0, "mns fixture non-empty")
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(bytes), OK, "mns parses")
	assert_eq(sheet.get_variable("DEF_FONTNAME"), "Gunpl22b.fnt", "variable lookup")
	assert_eq(sheet.get_variable("def_fontname"), "Gunpl22b.fnt", "lookup is case-insensitive")
	assert_eq(sheet.substitute("font is %DEF_FONTNAME% ok"), "font is Gunpl22b.fnt ok", "substitution")
	assert_eq(sheet.substitute("%UNKNOWN%"), "%UNKNOWN%", "unknown var left as-is")
