extends GutTest

# STR-2, the maturity program's string-encoding audit (see git history of
# docs/oned/workspace-maturity-program.md, removed 2026-09-28): pin the
# retail code-page behavior of the string tables, table bytes <-> Unicode.
#
# The pipeline under audit:
#   .bin table bytes (cp1252, raw in engine/formats/rtxt — no transcoding)
#     -> RtxtStringFile decode (cp1252-aware, D-RTXT-8 in
#        docs/interface/rtxt-strings-re.md; the strings editor's read path)
#     -> Godot String (Unicode)
#     -> the runtime HUD, menu, and credits draws, which encode the text back
#        to the code page's bytes (cp1252_encode_codepoint) for the engine's
#        byte-indexed glyph walk (hud::GameFont; its glyph stage, D-FNT-4, is
#        ctest hud_game_font).
#
# cp1252 zones pinned here:
#   0x20..0x7E  ASCII        — identity through every stage.
#   0xA0..0xFF  Latin-1      — cp1252 == Unicode; identity through every stage.
#   0x82..0x9F  specials     — decode maps to the cp1252 punctuation codepoints
#               (0x93 -> U+201C etc.); encode maps back losslessly.
#   0x8D/0x8F/0x90/0x9D      — cp1252 holes: decode passes the byte through as
#               its own codepoint, encode returns it.

# One section, two entries: text blob starts right after the 16-byte header and
# the two 16-byte entry rows [docs/interface/rtxt-strings-re.md, on-disk format].
const TEXT_BLOB_START := 16 + 2 * 16
const E_ACUTE := 0xE9        # cp1252 'é' — Latin-1 zone (cp1252 == Unicode)
const LEFT_QUOTE_BYTE := 0x93  # cp1252 left curly quote — specials zone
const LEFT_QUOTE_CODEPOINT := 0x201C  # what 0x93 decodes to
const HOLE_BYTE := 0x9D      # unmapped in cp1252 — passes through by byte value


func _two_entry_table() -> RtxtStringFile:
	var table := RtxtStringFile.new()
	table.add_section("menu")
	table.add_entry("K1", "X" + String.chr(E_ACUTE), 0, Vector2i())
	table.add_entry("K2", "AB", 0, Vector2i())  # 2 ASCII bytes to patch below
	return table


func test_latin1_zone_encodes_to_single_cp1252_bytes_on_disk() -> void:
	# String -> table bytes: 'é' (U+00E9) must land as ONE cp1252 byte 0xE9,
	# not a UTF-8 pair — retail tables are cp1252 and the game reads bytes.
	var bytes := _two_entry_table().to_byte_array()
	assert_gt(bytes.size(), TEXT_BLOB_START + 3, "the crafted table has a text blob")
	assert_eq(bytes[TEXT_BLOB_START + 0], 0x58, "entry 0 text starts at the blob start ('X')")
	assert_eq(bytes[TEXT_BLOB_START + 1], E_ACUTE, "'é' is the single cp1252 byte 0xE9 on disk")
	assert_eq(bytes[TEXT_BLOB_START + 2], 0, "entry texts are null-terminated")


func test_cp1252_bytes_decode_to_the_code_pages_unicode() -> void:
	# Table bytes -> String: patch raw retail-style bytes into the blob and
	# reload. 0x93 (specials zone) decodes to U+201C; 0x81 (a cp1252 hole)
	# passes through as its own codepoint.
	var bytes := _two_entry_table().to_byte_array()
	var e1_text := TEXT_BLOB_START + 3  # entry 1's text follows "Xé\0"
	assert_eq(bytes[e1_text], 0x41, "entry 1 text ('AB') sits after entry 0's terminator")
	bytes[e1_text] = LEFT_QUOTE_BYTE
	bytes[e1_text + 1] = HOLE_BYTE

	var reloaded := RtxtStringFile.new()
	assert_eq(reloaded.load_from_byte_array(bytes), OK, "the patched table parses")
	assert_eq(reloaded.get_entry_text(0), "X" + String.chr(E_ACUTE),
		"Latin-1 zone: byte 0xE9 decodes to U+00E9 (identity)")
	var expected := String.chr(LEFT_QUOTE_CODEPOINT) + String.chr(HOLE_BYTE)
	assert_eq(reloaded.get_entry_text(1), expected,
		"specials zone: byte 0x93 -> U+201C; hole byte 0x81 passes through")

	# String -> table bytes, back again: rewriting the decoded text must
	# reproduce the raw cp1252 bytes so edited retail tables stay game-readable
	# (D-RTXT-8's lossless guarantee, pinned here on the exact byte cells).
	reloaded.set_entry_text(1, reloaded.get_entry_text(1))
	var rewritten := reloaded.to_byte_array()
	assert_eq(rewritten[e1_text], LEFT_QUOTE_BYTE, "U+201C re-encodes to the cp1252 byte 0x93")
	assert_eq(rewritten[e1_text + 1], HOLE_BYTE, "the hole byte survives the String round trip")
	assert_eq(rewritten, bytes, "the whole patched table survives a decode/encode cycle byte-for-byte")
