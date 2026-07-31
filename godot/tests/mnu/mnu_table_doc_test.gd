extends GutTest

# M10.2: table COLUMN authoring (count / spacing / header + body definitions) on
# NovaMnuDocument, and that edits survive a serialize round-trip. The fixture's
# MissionTable carries 3 columns (Name/Players/Ping) with one bitmap BODY.

const FIXTURE := "res://../fixtures/mnu/all_widgets.mnu"


func _load() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE)), OK, "all_widgets loads")
	return doc


func _walk_for_name(doc: NovaMnuDocument, id: int, wname: String) -> int:
	if doc.get_widget_name(id) == wname:
		return id
	for cid in doc.get_child_ids(id):
		var f := _walk_for_name(doc, cid, wname)
		if f != -1:
			return f
	return -1


func _find_widget(doc: NovaMnuDocument, wname: String) -> int:
	for sid in doc.get_screen_ids():
		var f := _walk_for_name(doc, doc.get_screen_root_id(sid), wname)
		if f != -1:
			return f
	return -1


func test_table_columns_parsed() -> void:
	var doc := _load()
	var table := _find_widget(doc, "MissionTable")
	assert_eq(doc.get_widget_type(table), NovaMnuDocument.TYPE_TABLE, "MissionTable is a table")
	assert_eq(doc.get_table_column_count(table), 3, "3 columns")
	assert_eq(doc.get_table_column_spacing(table), 4, "spacing 4")

	var headers := doc.get_table_headers(table)
	assert_eq(headers.size(), 3, "3 header definitions")
	assert_eq(headers[0], {
		"column": 0, "has_column": true,
		"width": 120, "has_width": true,
		"justify": "LEFT", "vjustify": "", "sort": "A", "type": "", "text": "Name",
	},
		"Name header")
	assert_eq(int(headers[2]["width"]), 60, "Ping header width")
	assert_eq(String(headers[2]["justify"]), "RIGHT", "Ping header justify")

	var bodies := doc.get_table_bodies(table)
	assert_eq(bodies.size(), 1, "1 body definition")
	assert_eq(int(bodies[0]["column"]), 2, "body targets column 2")
	assert_true(bool(bodies[0]["bitmap_draw"]), "body draws a bitmap")
	assert_eq(String(bodies[0]["bitmap_flags"]), "STANDARD_TRANSPARENT", "body bitmap flags")
	assert_false(bool(bodies[0]["custom_draw"]), "body exposes the preserved custom-draw flag")


func test_table_accessors_non_table_widget() -> void:
	var doc := _load()
	var list := _find_widget(doc, "MissionList")
	assert_eq(doc.get_table_column_count(list), 0, "non-table has no column count")
	assert_eq(doc.get_table_headers(list).size(), 0, "non-table has no headers")
	assert_eq(doc.add_table_header(list, {"text": "x"}), -1, "add_table_header on a non-table is rejected")


func test_table_header_edit_round_trip() -> void:
	var doc := _load()
	var table := _find_widget(doc, "MissionTable")

	watch_signals(doc)
	doc.set_table_header(table, 0, {"column": 0, "width": 140, "justify": "LEFT", "sort": "A", "text": "Mission"})
	assert_signal_emitted(doc, "changed", "set_table_header emits changed")
	var added := doc.add_table_header(table, {"column": 3, "width": 50, "justify": "CENTER", "text": "Mode"})
	assert_eq(added, 3, "new header appended at index 3")
	doc.set_table_column_count(table, 4)
	doc.set_table_column_spacing(table, 6)

	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(doc.to_byte_array()), OK, "table round-trip re-parses")
	var table2 := _find_widget(doc2, "MissionTable")
	assert_eq(doc2.get_table_column_count(table2), 4, "column count persisted")
	assert_eq(doc2.get_table_column_spacing(table2), 6, "spacing persisted")
	var headers2 := doc2.get_table_headers(table2)
	assert_eq(headers2.size(), 4, "4 headers after round-trip")
	assert_eq(String(headers2[0]["text"]), "Mission", "renamed header persisted")
	assert_eq(int(headers2[0]["width"]), 140, "edited width persisted")
	assert_eq(String(headers2[3]["text"]), "Mode", "added header persisted")


func test_table_body_add_remove() -> void:
	var doc := _load()
	var table := _find_widget(doc, "MissionTable")
	assert_eq(doc.get_table_bodies(table).size(), 1, "starts with 1 body")
	var idx := doc.add_table_body(table, {"column": 1, "bitmap_draw": false, "scale_bitmap": true})
	assert_eq(idx, 1, "body appended at index 1")
	assert_eq(doc.get_table_bodies(table).size(), 2, "2 bodies")
	doc.remove_table_body(table, 0)
	var bodies := doc.get_table_bodies(table)
	assert_eq(bodies.size(), 1, "body removed")
	assert_eq(int(bodies[0]["column"]), 1, "remaining body reindexed")
	assert_true(bool(bodies[0]["scale_bitmap"]), "remaining body keeps its flag")


func test_table_compound_edit_preserves_header_type_and_body_custom_draw() -> void:
	var src := """
<SCREEN><NAME>MAIN</NAME><WINDOW type="window" name="MAIN">
  <WINDOW type="table" name="TypedTable">
    <COLUMN count="1">
      <HEADER column="0" width="100" type="id">MM_NAME</HEADER>
      <BODY column="0" CUSTOM_DRAW justify="LEFT"/>
    </COLUMN>
  </WINDOW>
</WINDOW></SCREEN>
"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK, "typed table parses")
	var table := _find_widget(doc, "TypedTable")
	var headers := doc.get_table_headers(table)
	var bodies := doc.get_table_bodies(table)
	assert_eq(String(headers[0]["type"]), "id", "header type crosses the document seam")
	assert_true(bool(bodies[0]["custom_draw"]), "body custom_draw crosses the document seam")

	# This is exactly how the GUI and MCP edit one cell: read the complete row,
	# replace an unrelated key, then write the row back.
	var header: Dictionary = headers[0]
	header["width"] = 140
	doc.set_table_header(table, 0, header)
	var body: Dictionary = bodies[0]
	body["justify"] = "RIGHT"
	doc.set_table_body(table, 0, body)

	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(doc.to_byte_array()), OK, "edited typed table re-parses")
	var table2 := _find_widget(doc2, "TypedTable")
	assert_eq(String(doc2.get_table_headers(table2)[0]["type"]), "id",
		"unrelated header edit does not clear type")
	assert_true(bool(doc2.get_table_bodies(table2)[0]["custom_draw"]),
		"unrelated body edit does not clear custom_draw")
