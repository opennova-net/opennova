extends GutTest

# M2 gate: load a fixture .mnu into MnuDocument, assert the parsed tree, and
# verify a serialize round-trip plus the core mutation surface.

# The shipped style sheet comes from the reference fixture set; the legs that
# read it pend without it.
const MNS_REL := "mns/menu_style.mns"


func _real_mns_bytes() -> PackedByteArray:
	var path := RetailData.fixture(MNS_REL)
	if path.is_empty():
		pending(RetailData.fixture_pending_text(MNS_REL))
		return PackedByteArray()
	return FileAccess.get_file_as_bytes(path)


func test_mns_entries_expose_document_order() -> void:
	var bytes := _real_mns_bytes()
	if bytes.is_empty():
		return
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(bytes), OK, "real stylesheet loads")
	assert_true(sheet.is_runtime_valid(), "the game reads the whole shipped sheet")
	var entries := sheet.get_entries()
	assert_eq(entries.size(), 12, "12 defines in the shipped file")
	assert_eq(sheet.get_entry_count(), 12, "entry count matches")
	assert_eq(String((entries[0] as Dictionary).get("name", "")), "DEF_FONTNAME", "authored case, the reader's order")
	assert_eq(String((entries[11] as Dictionary).get("name", "")), "DEF_IMAGE_DEFAULT_BG", "last define")


# The shipped sheet written in the writer's form reads back as the same list.
func test_mns_written_sheet_reads_back() -> void:
	var original := _real_mns_bytes()
	if original.is_empty():
		return
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(original), OK)
	var written := sheet.to_byte_array()
	assert_gt(written.size(), 0, "the shipped list has a written form")
	var again := MnsStyleSheet.new()
	assert_eq(again.load_from_bytes(written), OK)
	assert_true(again.is_runtime_valid(), "the game reads the whole written sheet")
	assert_eq(again.get_entries(), sheet.get_entries(), "the written sheet reads back as the list")


func test_mns_set_variable_changes_one_line_and_emits_changed() -> void:
	var original_bytes := _real_mns_bytes()
	if original_bytes.is_empty():
		return
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(original_bytes), OK)
	var before := sheet.get_source_text().split("\r\n")
	watch_signals(sheet)
	sheet.set_variable("DEF_TEXT_FG", "11223344")
	assert_signal_emit_count(sheet, "changed", 1, "a real edit emits changed once")
	sheet.set_variable("DEF_TEXT_FG", "11223344")
	assert_signal_emit_count(sheet, "changed", 1, "an equal value is a no-op (no dirty flip)")
	var after := sheet.get_source_text().split("\r\n")
	assert_eq(after.size(), before.size(), "line count unchanged")
	var diffs := 0
	for i in before.size():
		if after[i] != before[i]:
			diffs += 1
	assert_eq(diffs, 1, "a value edit changes exactly its own line of the written form")
