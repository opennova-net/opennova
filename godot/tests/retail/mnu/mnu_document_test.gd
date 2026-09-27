extends GutTest

# M2 gate: load a fixture .mnu into MnuDocument, assert the parsed tree, and
# verify a serialize round-trip plus the core mutation surface.

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"
# The shipped menus and style sheet come from the reference fixture set; the
# legs that read them pend without it.
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
	var entries := sheet.get_entries()
	assert_eq(entries.size(), 12, "12 defines in the shipped file")
	assert_eq(sheet.get_entry_count(), 12, "entry count matches")
	var first := entries[0] as Dictionary
	assert_eq(String(first.get("name", "")), "DEF_FONTNAME", "authored case, document order")
	assert_eq(int(first.get("line", 0)), 40, "1-based physical line after the 38-line header + blank")
	assert_eq(int(first.get("group", -1)), 0, "first blank-separated group")
	var last := entries[11] as Dictionary
	assert_eq(String(last.get("name", "")), "DEF_IMAGE_DEFAULT_BG", "last define")
	assert_eq(int(last.get("group", -1)), 4, "five groups in the shipped file")


func test_mns_source_text_round_trip_byte_faithful() -> void:
	var original := _real_mns_bytes()
	if original.is_empty():
		return
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(original), OK)
	assert_eq(sheet.to_byte_array(), original, "untouched load -> serialize is byte-identical")
	sheet.set_source_text(sheet.get_source_text())
	assert_eq(sheet.to_byte_array(), original, "source get -> set round-trips byte-identically")


func test_mns_set_variable_preserves_layout_and_emits_changed() -> void:
	var original_bytes := _real_mns_bytes()
	if original_bytes.is_empty():
		return
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(original_bytes), OK)
	watch_signals(sheet)
	sheet.set_variable("DEF_TEXT_FG", "11223344")
	assert_signal_emit_count(sheet, "changed", 1, "a real edit emits changed once")
	sheet.set_variable("DEF_TEXT_FG", "11223344")
	assert_signal_emit_count(sheet, "changed", 1, "an equal value is a no-op (no dirty flip)")

	var original := original_bytes.get_string_from_utf8().split("\n")
	var edited := sheet.to_byte_array().get_string_from_utf8().split("\n")
	assert_eq(edited.size(), original.size(), "line count unchanged")
	var diffs := 0
	for i in original.size():
		if edited[i] != original[i]:
			diffs += 1
	assert_eq(diffs, 1, "a value edit changes exactly its own line (tabs + comments survive)")
