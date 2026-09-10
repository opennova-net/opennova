extends GutTest

# AnimDefDocument: rows in, the canonical .adm form out, and the parse of
# what it wrote (and of an authored table) reads the same rows back.


func test_rows_write_the_canonical_form_and_read_back() -> void:
	var table := AnimDefDocument.new()
	assert_true(table.add_row("anim_reset", PackedStringArray(["person_rst"])), table.get_last_error())
	assert_true(table.add_row("anim_wpn_reload", PackedStringArray(["m4_1r", "m4_1r", "m4_1r2"])))
	var bytes := table.to_bytes()
	var expected := ("\r\nanim_reset\t\t\t\t\"person_rst\"\r\n" +
			"anim_wpn_reload\t\t\t\t\"m4_1r\" \"m4_1r\" \"m4_1r2\"\r\n\r\n\r\n").to_ascii_buffer()
	expected.append(0)
	assert_eq(bytes, expected, "one leading blank line, four-tab rows, the CRLF x3 + NUL trailer")
	var back := AnimDefDocument.new()
	assert_eq(back.load_from_bytes(bytes), OK)
	assert_eq(back.get_row_count(), 2)
	assert_eq(back.get_row_key(1), "anim_wpn_reload")
	assert_eq(back.get_row_variants(1), PackedStringArray(["m4_1r", "m4_1r", "m4_1r2"]))
	assert_eq(back.find_row("ANIM_RESET"), 0, "keys match case-insensitively")
	assert_eq(back.find_row("anim_idle"), -1)


func test_rows_the_parser_would_drop_are_refused() -> void:
	var table := AnimDefDocument.new()
	assert_false(table.add_row("reset", PackedStringArray(["x"])), "keys carry the anim_ prefix")
	assert_false(table.add_row("anim_reset", PackedStringArray()), "a row names at least one clip")
	assert_false(table.add_row("anim_reset", PackedStringArray(["a\"b"])), "no quotes inside a clip name")
	assert_eq(table.get_row_count(), 0)


func test_the_authored_soldier_table_parses() -> void:
	var table := AnimDefDocument.new()
	assert_eq(table.load_from_path("res://../fixtures/anim/soldier.adm"), OK, table.get_last_error())
	assert_eq(table.get_row_count(), 7)
	assert_eq(table.get_row_key(0), "anim_reset")
	assert_eq(table.get_row_variants(3), PackedStringArray(["walk.bad"]))
