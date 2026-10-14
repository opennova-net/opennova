extends GutTest

# M2 gate: load a fixture .mnu into MnuDocument, assert the parsed tree, and
# verify a serialize round-trip plus the core mutation surface.

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


func _load_doc() -> MnuDocument:
	# Read bytes directly so the test does not depend on the resource cache.
	var bytes := FileAccess.get_file_as_bytes(FIXTURE)
	assert_gt(bytes.size(), 0, "fixture bytes are non-empty")
	var doc := MnuDocument.new()
	var err := doc.load_from_bytes(bytes)
	assert_eq(err, OK, "load_from_bytes succeeds")
	return doc


func test_screen_structure() -> void:
	var doc := _load_doc()
	assert_eq(doc.get_screen_count(), 2, "two screens")
	var screens := doc.get_screen_ids()
	assert_eq(screens.size(), 2, "two screen ids")

	var main_id: int = screens[0]
	assert_eq(doc.get_screen_name(main_id), "MAIN", "screen 0 name")
	assert_eq(doc.get_screen_music_var(main_id), 3, "screen 0 music_var")
	# The text table is the root window's own: a SCREEN reads none.
	assert_eq(doc.get_widget_text_rsrc(doc.get_screen_root_id(main_id)), "menutxt.BIN", "root TEXT_RSRC")
	assert_true(doc.is_screen(main_id), "screen id is a screen")


func test_root_window_and_children() -> void:
	var doc := _load_doc()
	var main_id: int = doc.get_screen_ids()[0]
	var root_id := doc.get_screen_root_id(main_id)
	assert_gt(root_id, 0, "root id valid")
	assert_eq(doc.get_widget_name(root_id), "ROOT", "root window name")
	assert_eq(doc.get_widget_type(root_id), MnuDocument.TYPE_WINDOW, "root is a window")
	assert_eq(doc.get_parent_id(root_id), main_id, "root's parent is its screen")

	var children := doc.get_child_ids(root_id)
	assert_eq(children.size(), 5, "root has 5 child widgets")

	var names := []
	var types := []
	for cid in children:
		names.append(doc.get_widget_name(cid))
		types.append(doc.get_widget_type(cid))
	assert_eq(names, ["Title", "StartBtn", "SoundChk", "Difficulty", "Version"], "child names in order")
	assert_eq(types[0], MnuDocument.TYPE_STATIC, "Title is static")
	assert_eq(types[1], MnuDocument.TYPE_BUTTON, "StartBtn is button")
	assert_eq(types[2], MnuDocument.TYPE_CHECKBOX, "SoundChk is checkbox")
	assert_eq(types[3], MnuDocument.TYPE_SPINLIST, "Difficulty is spinlist")
	assert_eq(types[4], MnuDocument.TYPE_STATIC, "Version is static")


func test_widget_rect_and_properties() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id: int = doc.get_child_ids(root_id)[1]  # StartBtn

	var rect := doc.get_window_rect(start_id)
	assert_eq(rect, Rect2(270, 120, 100, 30), "StartBtn rect")
	assert_eq(doc.get_widget_text(start_id), "MM_Start", "StartBtn string value")
	assert_eq(doc.get_widget_string_type(start_id), "ID", "StartBtn string is an id ref (the parse table's spelling)")
	assert_eq(doc.get_widget_texture(start_id, MnuDocument.TEX_DEFAULT), "btn_up.tga", "default appearance")
	assert_eq(doc.get_widget_texture(start_id, MnuDocument.TEX_MOUSEOVER), "btn_over.tga", "mouseover appearance")
	assert_eq(doc.get_widget_font(root_id), "%DEF_FONTNAME%", "font %VAR% preserved raw")
	assert_eq(doc.get_widget_color(root_id, MnuDocument.COLOR_DEFAULT_FG), "%DEF_TEXT_FG%", "color %VAR% preserved raw")

	var chk_id: int = doc.get_child_ids(root_id)[2]  # SoundChk
	assert_true((doc.get_widget_flags(chk_id) & MnuDocument.FLAG_CHECKED) != 0, "SoundChk is checked")


func test_serialize_roundtrip() -> void:
	var doc := _load_doc()
	var bytes := doc.to_byte_array()
	assert_gt(bytes.size(), 0, "serialize produces bytes")

	var doc2 := MnuDocument.new()
	assert_eq(doc2.load_from_bytes(bytes), OK, "re-parse serialized bytes")
	assert_eq(doc2.get_screen_count(), 2, "round-trip keeps 2 screens")

	var root2 := doc2.get_screen_root_id(doc2.get_screen_ids()[0])
	assert_eq(doc2.get_child_ids(root2).size(), 5, "round-trip keeps 5 children")
	assert_eq(doc2.get_screen_music_var(doc2.get_screen_ids()[0]), 3, "round-trip keeps music_var")


func _walk_for_name(doc: MnuDocument, id: int, wname: String) -> int:
	if doc.get_widget_name(id) == wname:
		return id
	for cid in doc.get_child_ids(id):
		var f := _walk_for_name(doc, cid, wname)
		if f != -1:
			return f
	return -1


func _find_widget(doc: MnuDocument, wname: String) -> int:
	for sid in doc.get_screen_ids():
		var f := _walk_for_name(doc, doc.get_screen_root_id(sid), wname)
		if f != -1:
			return f
	return -1


func test_datasource_orientation_round_trip() -> void:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(
		"res://../fixtures/mnu/all_widgets.mnu")), OK, "all_widgets loads")
	var marquee := _find_widget(doc, "Credits")
	var scroll := _find_widget(doc, "VolumeBar")
	assert_eq(doc.get_widget_type(marquee), MnuDocument.TYPE_MARQUEE, "Credits is a marquee")
	assert_eq(doc.get_widget_datasources(marquee), PackedStringArray(["credits.txt"]), "parsed DATASOURCE")
	# A marquee reads no ORIENTATION (a SCROLL's parse alone does), and a SCROLL's is HORIZONTAL or
	# nothing: VERTICAL reads as vertical, the default [orig: @ 0x64c745 / 0x64c755].
	assert_eq(doc.get_widget_orientation(marquee), "", "a marquee's ORIENTATION is left out")
	assert_eq(doc.get_widget_orientation(scroll), "", "a VERTICAL ORIENTATION is the default, nothing held")


func test_mns_stylesheet() -> void:
	var bytes := FileAccess.get_file_as_bytes("res://../fixtures/mns/test_style.mns")
	assert_gt(bytes.size(), 0, "mns fixture non-empty")
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(bytes), OK, "mns parses")
	assert_eq(sheet.get_variable("DEF_FONTNAME"), "Gunpl22b.fnt", "variable lookup")
	assert_eq(sheet.get_variable("def_fontname"), "Gunpl22b.fnt", "lookup is case-insensitive")
	assert_eq(sheet.substitute("font is %DEF_FONTNAME% ok"), "font is Gunpl22b.fnt ok", "substitution")
	assert_eq(sheet.substitute("%UNKNOWN%"), "%UNKNOWN%", "unknown var left as-is")


# --- MNS list surface (the reader's list behind MnsStyleSheet) -------------------

# A sheet's text is what the game reads of it: its directives decided, its comments,
# blanks and switched-off lines read as nothing; the writer puts the list down alone.
func test_mns_source_text_is_the_writers_form() -> void:
	var sheet := MnsStyleSheet.new()
	sheet.set_source_text("// a comment\r\nFOO a\r\nFOO b\r\n#if 0\r\nBAR c\r\n#endif\r\nBAZ   d   // e\r\n")
	assert_true(sheet.is_runtime_valid(), "the game reads the whole sheet")
	assert_eq(sheet.get_variable("FOO"), "b", "a later definition replaces the value")
	assert_false(sheet.has_variable("BAR"), "a switched-off line is read as nothing")
	assert_eq(sheet.get_source_text(), "FOO\tb\r\nBAZ\td\r\n", "the writer's form: no comment, blank or directive")
	assert_eq(sheet.to_byte_array(), "FOO\tb\r\nBAZ\td\r\n".to_utf8_buffer(), "the bytes are the text")


func test_mns_runtime_evaluation_validity_is_distinct_from_repairable_load() -> void:
	var sheet := MnsStyleSheet.new()
	# The game's reader stops at a name holding '%' and keeps what it read before.
	assert_eq(sheet.load_from_bytes("OK value\r\nBAD%NAME x\r\nLATER 1\r\n".to_utf8_buffer()), OK,
		"the load keeps what the game reads")
	assert_false(sheet.is_runtime_valid(), "the reader's stop is runtime-visible")
	assert_eq(sheet.get_variable("OK"), "value", "what the game read before the stop stays")
	assert_false(sheet.has_variable("LATER"), "nothing after the stop is read")
	var diagnostics := sheet.get_evaluation_diagnostics()
	assert_eq(diagnostics.size(), 1, "the reader's stop is exposed")
	if diagnostics.size() == 1:
		var stop := diagnostics[0] as Dictionary
		assert_eq(String(stop.get("code", "")), "stops", "the stop's code")
		assert_eq(int(stop.get("line", 0)), 2, "the stop's line")
	sheet.set_source_text("OK value\r\n")
	assert_true(sheet.is_runtime_valid(), "repairing source refreshes runtime validity")
	# A stray #else is no failure: the game switches the lines after it off.
	sheet.set_source_text("#else\r\nOFF value\r\n")
	assert_true(sheet.is_runtime_valid(), "a stray #else reads to the end")
	assert_false(sheet.has_variable("OFF"), "the lines after a stray #else are off")


# The shell's stylesheets: menu_style.mns, then brand.mns onto it, as the game
# loads them; a later definition wins, and a brand.mns the reader stops in keeps
# what it read before the stop (the game ignores the result).
func test_mns_load_shell_layers_brand_over_menu_style() -> void:
	var dir := OS.get_temp_dir().path_join("mns_load_shell_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	var style_file := FileAccess.open(dir.path_join("menu_style.mns"), FileAccess.WRITE)
	style_file.store_buffer("DEF_TEXT_FG FFFFFFFF\r\nTRIM_COLOR FF808080\r\n".to_utf8_buffer())
	style_file.close()
	var brand_file := FileAccess.open(dir.path_join("brand.mns"), FileAccess.WRITE)
	brand_file.store_buffer("TRIM_COLOR FF102030\r\nBAD%NAME x\r\nLATER 1\r\n".to_utf8_buffer())
	brand_file.close()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK, "the loose root mounts")
	var sheet: MnsStyleSheet = MnsStyleSheet.load_shell(root)
	assert_not_null(sheet, "the shell's stylesheets load")
	if sheet != null:
		assert_eq(sheet.get_variable("TRIM_COLOR"), "FF102030", "brand.mns overrides menu_style.mns")
		assert_eq(sheet.get_variable("DEF_TEXT_FG"), "FFFFFFFF", "menu_style.mns keeps the rest")
		assert_false(sheet.has_variable("LATER"), "the reader stops in brand.mns")
		assert_false(sheet.is_runtime_valid(), "a sheet the reader stops in is reported")
	assert_null(MnsStyleSheet.load_shell(null), "no root: null")
	var empty := ResourceRoot.new()
	var empty_dir := dir.path_join("none")
	DirAccess.make_dir_recursive_absolute(empty_dir)
	if empty.set_root_dir(empty_dir) == OK:
		assert_null(MnsStyleSheet.load_shell(empty), "no stylesheet: null, every %VAR% literal")
	root.clear()
	empty.clear()
	DirAccess.remove_absolute(empty_dir)
	DirAccess.remove_absolute(dir.path_join("menu_style.mns"))
	DirAccess.remove_absolute(dir.path_join("brand.mns"))
	DirAccess.remove_absolute(dir)


func test_mns_entry_add_rename_remove_move() -> void:
	var sheet := MnsStyleSheet.new()
	sheet.set_source_text("A 1\r\nB 2\r\nC 3\r\n")
	watch_signals(sheet)
	assert_true(sheet.add_variable("MID", "x", "A"), "insert after a named entry")
	assert_eq(String((sheet.get_entries()[1] as Dictionary).get("name", "")), "MID", "MID landed after A")
	assert_false(sheet.add_variable("mid", "y"), "duplicate names reject (case-insensitive)")
	assert_true(sheet.rename_variable("MID", "MIDDLE"), "rename")
	assert_false(sheet.rename_variable("MIDDLE", "B"), "rename collisions reject")
	assert_true(sheet.move_variable("C", 0), "move to the front")
	assert_eq(String((sheet.get_entries()[0] as Dictionary).get("name", "")), "C", "C now first")
	assert_true(sheet.remove_variable("MIDDLE"), "a variable removes")
	assert_false(sheet.has_variable("MIDDLE"), "removed")
	assert_false(sheet.remove_variable("MIDDLE"), "removing an absent variable changes nothing")
	assert_signal_emit_count(sheet, "changed", 4, "each change emits changed once, a refused one never")
	assert_eq(sheet.get_source_text(), "C\t3\r\nA\t1\r\nB\t2\r\n", "the list in the writer's form")
	assert_true(sheet.is_valid_variable_name("DEF_TEXT_FG"), "name validation binds")
	assert_false(sheet.is_valid_variable_name("BAD NAME"), "whitespace rejects")
	assert_false(sheet.is_valid_variable_value("a//b"), "values cannot carry comment starts")
	assert_false(sheet.is_valid_variable_value("a\\b"), "a lone backslash continues the value")


# --- M10.1: item-row authoring (list / multi / spinlist / combo) ----------------

func _load_all_widgets() -> MnuDocument:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(
		"res://../fixtures/mnu/all_widgets.mnu")), OK, "all_widgets loads")
	return doc


func test_item_accessors_read_list() -> void:
	var doc := _load_all_widgets()
	var list := _find_widget(doc, "MissionList")
	assert_eq(doc.get_item_count(list), 2, "MissionList has 2 items")
	assert_eq(doc.get_item_text(list, 0), "MM_Alpha", "first row text")
	assert_eq(doc.get_item_value(list, 0), "0", "first row value")
	assert_eq(doc.get_item_text(list, 1), "MM_Bravo", "second row text")
	assert_eq(doc.get_item_value(list, 1), "1", "second row value")
	assert_eq(doc.get_item_text(list, 2), "", "past the end reads empty")


func test_item_accessors_non_list_widget() -> void:
	var doc := _load_all_widgets()
	var edit := _find_widget(doc, "NameEdit")
	assert_eq(doc.get_item_count(edit), 0, "a non-list widget has no items")


func test_item_combo_uses_list_box_and_round_trips() -> void:
	var doc := _load_all_widgets()
	var combo := _find_widget(doc, "ServerList")
	assert_eq(doc.get_widget_type(combo), MnuDocument.TYPE_COMBO, "ServerList is a combo")
	assert_eq(doc.get_item_count(combo), 3, "combo reads its LIST_BOX rows")
	assert_eq(doc.get_item_text(combo, 2), "LAN Server", "third LIST_BOX row")


func test_item_combo_reads_its_list_box_rows_alone() -> void:
	var src := """
<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">
  <WINDOW type="combobox" name="ModeBox">
    <ITEMS justify="LEFT">
      <ITEM value="0">Solo</ITEM>
      <ITEM value="1">Team</ITEM>
    </ITEMS>
    <LIST_BOX>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>40</BOTTOM></POSITION>
      <APPEARANCE state="default" type="color">101820</APPEARANCE>
    </LIST_BOX>
  </WINDOW>
</WINDOW></SCREEN>
"""
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK, "inline combo parses")
	var combo := _find_widget(doc, "ModeBox")
	# A combo reads ITEMS in its LIST_BOX's list alone [orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0],
	# as the shipped menus author them: the top-level ITEMS is left out.
	assert_eq(doc.get_item_count(combo), 0, "the top-level ITEMS is not the combo's rows")


func test_widget_sounds_read_the_authored_row() -> void:
	# StartBtn carries <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>.
	var doc := _load_doc()
	var start := _find_widget(doc, "StartBtn")
	var sounds := doc.get_widget_sounds(start)
	assert_eq(sounds.size(), 1, "StartBtn has one authored sound")
	var sound: MnuSoundRow = sounds[0]
	assert_eq(sound.trigger, "MOUSE_OVER", "hover trigger read")
	assert_eq(sound.file, "menu.lwf", "sound file read")
	assert_eq(sound.state, "MOUSEIN", "sound state read (the parse table's spelling)")



func test_widget_actions_read_the_authored_row() -> void:
	# StartBtn carries <ACTION type="screen" file="widgets.mnu">OPTIONS</ACTION>: the
	# target is the element text (retail reads no TARGET attribute).
	var doc := _load_doc()
	var start := _find_widget(doc, "StartBtn")
	var actions := doc.get_widget_actions(start)
	assert_eq(actions.size(), 1, "StartBtn has one authored action")
	var action: MnuActionRow = actions[0]
	assert_eq(action.type, "SCREEN", "screen verb read (the parse table's spelling)")
	assert_eq(action.target, "OPTIONS", "screen target read")
	assert_eq(action.file, "widgets.mnu", "a SCREEN action names its file")
	assert_eq(action.state, "", "screen action has no window state")



func test_explicit_zero_musicvar_reads_as_present() -> void:
	var src := """
<SCREEN><NAME>S</NAME><MUSICVAR>0</MUSICVAR>
<WINDOW type="window" name="ROOT"></WINDOW></SCREEN>
"""
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK)
	var screen := int(doc.get_screen_ids()[0])
	assert_true(doc.get_screen_has_music_var(screen), "explicit MUSICVAR=0 presence is exposed")
	assert_eq(doc.get_screen_music_var(screen), 0)


func _ascii_utf16(text: String, big_endian: bool) -> PackedByteArray:
	var out := PackedByteArray([0xFE, 0xFF] if big_endian else [0xFF, 0xFE])
	for byte in text.to_ascii_buffer():
		if big_endian:
			out.append(0)
			out.append(byte)
		else:
			out.append(byte)
			out.append(0)
	return out


func test_document_bridge_preserves_bom_and_utf16_source_encoding() -> void:
	var src := '<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT"><POSITION><LEFT>0</LEFT></POSITION></WINDOW></SCREEN>'
	var utf8_bom := PackedByteArray([0xEF, 0xBB, 0xBF])
	utf8_bom.append_array(src.to_utf8_buffer())
	# Retail reads a big-endian file as little-endian: it loads nothing.
	assert_ne(MnuDocument.new().load_from_bytes(_ascii_utf16(src, true)), OK, "UTF-16 BE does not load")
	for encoded in [utf8_bom, _ascii_utf16(src, false)]:
		var doc := MnuDocument.new()
		assert_eq(doc.load_from_bytes(encoded), OK)
		var saved := doc.to_byte_array()
		assert_gt(saved.size(), 3)
		assert_eq(saved[0], encoded[0], "first encoding marker byte survives")
		assert_eq(saved[1], encoded[1], "second encoding marker byte survives")
		var reparsed := MnuDocument.new()
		assert_eq(reparsed.load_from_bytes(saved), OK, "preserved encoding remains parseable")


func test_is_widget_multiselect_reads_the_items_flag() -> void:
	# The window-level <ITEMS MULTISELECT> flag is the driver's CTRL-select gate.
	var src := """
<SCREEN><NAME>S</NAME><MUSICVAR>0</MUSICVAR>
<WINDOW type="window" name="ROOT">
  <WINDOW type="list" name="Multi">
    <ITEMS MULTISELECT justify="LEFT" vjustify="TOP"><ITEM value="0">A</ITEM></ITEMS>
  </WINDOW>
  <WINDOW type="list" name="Single">
    <ITEMS><ITEM value="0">A</ITEM></ITEMS>
  </WINDOW>
</WINDOW>
</SCREEN>
"""
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK, "inline document loads")
	assert_true(doc.is_widget_multiselect(_find_widget(doc, "Multi")),
		"an ITEMS MULTISELECT list reads true")
	assert_false(doc.is_widget_multiselect(_find_widget(doc, "Single")),
		"a plain ITEMS list reads false")
	assert_false(doc.is_widget_multiselect(-1), "an unknown id reads false")
