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
	assert_eq(doc.get_screen_text_rsrc(main_id), "menutxt.BIN", "screen 0 text_rsrc")
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
	assert_eq(types[4], MnuDocument.TYPE_LABEL, "Version is label")


func test_widget_rect_and_properties() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id: int = doc.get_child_ids(root_id)[1]  # StartBtn

	var rect := doc.get_window_rect(start_id)
	assert_eq(rect, Rect2(270, 120, 100, 30), "StartBtn rect")
	assert_eq(doc.get_widget_text(start_id), "MM_Start", "StartBtn string value")
	assert_eq(doc.get_widget_string_type(start_id), "id", "StartBtn string is an id ref")
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
	assert_eq(doc.get_widget_datasource(marquee), "credits.txt", "parsed DATASOURCE")
	assert_eq(doc.get_widget_orientation(marquee), "VERTICAL", "parsed marquee ORIENTATION")
	assert_eq(doc.get_widget_orientation(scroll), "VERTICAL", "parsed scroll ORIENTATION")


func test_mns_stylesheet() -> void:
	var bytes := FileAccess.get_file_as_bytes("res://../fixtures/mns/test_style.mns")
	assert_gt(bytes.size(), 0, "mns fixture non-empty")
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(bytes), OK, "mns parses")
	assert_eq(sheet.get_variable("DEF_FONTNAME"), "Gunpl22b.fnt", "variable lookup")
	assert_eq(sheet.get_variable("def_fontname"), "Gunpl22b.fnt", "lookup is case-insensitive")
	assert_eq(sheet.substitute("font is %DEF_FONTNAME% ok"), "font is Gunpl22b.fnt ok", "substitution")
	assert_eq(sheet.substitute("%UNKNOWN%"), "%UNKNOWN%", "unknown var left as-is")


# --- MNS document surface (lossless model behind MnsStyleSheet, ADR 0014) -------

# The shipped style sheet's bytes, empty (after pending) without the reference set.
func test_mns_diagnostics_report_line_and_severity() -> void:
	var sheet := MnsStyleSheet.new()
	sheet.set_source_text("FOO a\nFOO b\n#if 2\nBAR c\n")
	var diagnostics := sheet.get_diagnostics()
	assert_gt(diagnostics.size(), 0, "problems surface as diagnostics")
	var codes := PackedStringArray()
	for diag_value in diagnostics:
		var diag := diag_value as Dictionary
		codes.append(String(diag.get("code", "")))
		assert_gt(int(diag.get("line", 0)), 0, "diagnostics carry 1-based lines")
		assert_true(String(diag.get("severity", "")) in ["error", "warning"], "severity is error|warning")
	assert_has(codes, "duplicate-name", "duplicate names are diagnosed")
	assert_has(codes, "noncanonical-if-arg",
		"a retail-truthy non-0/1 #if argument is diagnosed without rejection")
	assert_eq(sheet.get_variable("FOO"), "b", "the parse stays lenient: last duplicate wins")


func test_mns_runtime_evaluation_validity_is_distinct_from_repairable_load() -> void:
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes("#else\nOK value\n".to_utf8_buffer()), OK,
		"lossless editor load remains permissive")
	assert_false(sheet.is_runtime_valid(), "retail evaluator failure is runtime-visible")
	assert_eq(sheet.get_variable("OK"), "value", "partial evaluated view remains inspectable")
	var diagnostics := sheet.get_evaluation_diagnostics()
	assert_gt(diagnostics.size(), 0, "evaluation diagnostics are exposed")
	var codes := PackedStringArray()
	for value in diagnostics:
		codes.append(String((value as Dictionary).get("code", "")))
	assert_has(codes, "unbalanced-else", "runtime failure reports the evaluator code")
	sheet.set_source_text("OK value\n")
	assert_true(sheet.is_runtime_valid(), "repairing source refreshes runtime validity")


func test_mns_entry_add_rename_remove_move() -> void:
	var sheet := MnsStyleSheet.new()
	sheet.set_source_text("A 1\nB 2\nC 3\n")
	assert_true(sheet.add_variable("MID", "x", "A"), "insert after a named entry")
	assert_eq(String((sheet.get_entries()[1] as Dictionary).get("name", "")), "MID", "MID landed after A")
	assert_false(sheet.add_variable("mid", "y"), "duplicate names reject (case-insensitive)")
	assert_true(sheet.rename_variable("MID", "MIDDLE"), "rename")
	assert_false(sheet.rename_variable("MIDDLE", "B"), "rename collisions reject")
	assert_true(sheet.set_inline_comment("MIDDLE", "the middle one"), "comment attaches")
	assert_string_contains(String(sheet.get_source_text()), "// the middle one", "comment rendered")
	assert_true(sheet.move_variable("C", 0), "move to the front")
	assert_eq(String((sheet.get_entries()[0] as Dictionary).get("name", "")), "C", "C now first")
	assert_true(sheet.remove_variable("MIDDLE"), "safe standalone define removes")
	assert_false(sheet.has_variable("MIDDLE"), "removed")
	assert_true(sheet.is_valid_variable_name("DEF_TEXT_FG"), "name validation binds")
	assert_false(sheet.is_valid_variable_name("BAD NAME"), "whitespace rejects")
	assert_false(sheet.is_valid_variable_value("a//b"), "values cannot carry comment starts")


func test_mns_remove_rejects_control_flow_owning_entry_without_dirtying() -> void:
	var sheet := MnsStyleSheet.new()
	sheet.set_source_text("#if 1\nFOO bar \\\n#endif\nbaz\nQUX 7\n")
	var before := sheet.get_source_text()
	watch_signals(sheet)
	assert_false(sheet.remove_variable("FOO"),
		"a continuation crossing a directive cannot be structurally removed")
	assert_eq(sheet.get_source_text(), before, "rejected remove leaves source byte-stable")
	assert_true(sheet.has_variable("FOO"), "rejected entry remains in evaluated view")
	assert_signal_not_emitted(sheet, "changed", "rejected mutation does not dirty the editor")


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


func test_item_combo_top_level_items_with_empty_list_box() -> void:
	var src := """
<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">
  <WINDOW type="combo" name="ModeBox">
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
	assert_eq(doc.get_item_count(combo), 2, "reads the top-level ITEMS, not the empty LIST_BOX")
	assert_eq(doc.get_item_text(combo, 1), "Team", "second top-level row")


func test_widget_sounds_read_the_authored_row() -> void:
	# StartBtn carries <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>.
	var doc := _load_doc()
	var start := _find_widget(doc, "StartBtn")
	var sounds := doc.get_widget_sounds(start)
	assert_eq(sounds.size(), 1, "StartBtn has one authored sound")
	var sound: MnuSoundRow = sounds[0]
	assert_eq(sound.trigger, "MOUSE_OVER", "hover trigger read")
	assert_eq(sound.file, "menu.lwf", "sound file read")
	assert_eq(sound.state, "mousein", "sound state read")



func test_widget_actions_read_the_authored_row() -> void:
	# StartBtn carries <ACTION type="screen" target="OPTIONS">.
	var doc := _load_doc()
	var start := _find_widget(doc, "StartBtn")
	var actions := doc.get_widget_actions(start)
	assert_eq(actions.size(), 1, "StartBtn has one authored action")
	var action: MnuActionRow = actions[0]
	assert_eq(action.type, "screen", "screen verb read")
	assert_eq(action.target, "OPTIONS", "screen target read")
	assert_eq(action.file, "", "same-file action has no file")
	assert_eq(action.state, "", "screen action has no window state")



func test_explicit_zero_musicvar_reads_as_present() -> void:
	var src := """
<SCREEN><NAME>S</NAME><MUSICVAR>0</MUSICVAR>
<WINDOW type="window" name="ROOT"/>
</SCREEN>
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
	var src := '<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT"/></SCREEN>'
	var utf8_bom := PackedByteArray([0xEF, 0xBB, 0xBF])
	utf8_bom.append_array(src.to_utf8_buffer())
	for encoded in [utf8_bom, _ascii_utf16(src, false), _ascii_utf16(src, true)]:
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
