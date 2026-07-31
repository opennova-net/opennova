extends GutTest

# STR-2 (docs/oned/workspace-maturity-program.md): the encoding audit — pin the
# retail code-page behavior end to end, table bytes -> glyphs.
#
# The pipeline under audit:
#   .bin table bytes (cp1252, raw in libs/rtxt — no transcoding)
#     -> RtxtStringFile decode (cp1252-aware, D-RTXT-8 in
#        docs/interface/rtxt-strings-re.md; the strings editor's read path)
#     -> Godot String (Unicode)
#     -> NovaFntResource.to_font_file() FontFile (the runtime's .fnt view;
#        glyph slots keyed at their decoded cp1252 codepoints)
#     -> the engine draw (HudText.draw_text / menus / credits).
#
# cp1252 zones pinned here:
#   0x20..0x7E  ASCII        — identity through every stage.
#   0xA0..0xFF  Latin-1      — cp1252 == Unicode; identity through every stage.
#   0x82..0x9F  specials     — decode maps to the cp1252 punctuation codepoints
#               (0x93 -> U+201C etc.); encode maps back losslessly, and the
#               FontFile exposes each byte's retail glyph under that codepoint.
#   0x8D/0x8F/0x90/0x9D      — cp1252 holes: decode passes the byte through as
#               its own codepoint, encode returns it; glyph stage matches
#               (whenever the font draws that slot at all).
#
# Intentional exclusions (format facts, not gaps):
#   - bytes < 0x20 never map to glyphs: the .fnt format carries 224 glyphs for
#     chars 32..255 only (libs/fnt fnt.h, FNT_FIRST_CHAR/FNT_GLYPH_COUNT).
#   - byte 0x20 (space) is advance-only in the FontFile view (no drawn rect).
#   - retail's text drawer and measurer skip bytes 0x7F, 0x80, and 0x81 as
#     non-printing controls; the FontFile omits those slots and disables system-font
#     fallback so decoded U+20AC does not become a system-font Euro glyph.
#
# D-FNT-4 closed the audited 0x80..0x9F glyph-stage divergence: decoded text
# now selects the same bitmap slot and metric that retail selects by byte.

const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"

# One section, two entries: text blob starts right after the 16-byte header and
# the two 16-byte entry rows [docs/interface/rtxt-strings-re.md, on-disk format].
const TEXT_BLOB_START := 16 + 2 * 16
const E_ACUTE := 0xE9        # cp1252 'é' — Latin-1 zone (cp1252 == Unicode)
const LEFT_QUOTE_BYTE := 0x93  # cp1252 left curly quote — specials zone
const LEFT_QUOTE_CODEPOINT := 0x201C  # what 0x93 decodes to
const HOLE_BYTE := 0x9D      # unmapped in cp1252 — passes through by byte value
                             # (0x9D rather than 0x81: the fixture draws 0x9D)
const DELETE_CONTROL_CODEPOINT := 0x7F
const EURO_CODEPOINT := 0x20AC
const UNDEFINED_81_CONTROL_CODEPOINT := 0x81


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


func _fixture_font() -> FontFile:
	var res := ResourceLoader.load(FNT_PATH, "NovaFntResource", ResourceLoader.CACHE_MODE_IGNORE) as NovaFntResource
	assert_not_null(res, "Serpen24.fnt fixture should load as NovaFntResource.")
	if res == null:
		return null
	var font := res.to_font_file()
	assert_not_null(font, "the runtime FontFile view should build")
	return font


func test_identity_zones_key_fnt_slots_at_matching_codepoints() -> void:
	# For ASCII, Latin-1, and pass-through holes, the decoded codepoint IS the
	# table byte, so the engine draw picks the same glyph slot retail indexes.
	var font := _fixture_font()
	if font == null:
		return
	var fs := font.get_fixed_size()
	assert_gt(fs, 0, "the .fnt view carries its fixed pixel size")
	var glyphs := font.get_glyph_list(0, Vector2i(fs, 0))
	assert_true(glyphs.has(0x41), "ASCII: 'A' has its glyph")
	assert_true(glyphs.has(E_ACUTE),
		"Latin-1 zone: the decoded U+00E9 finds the glyph the retail engine draws for byte 0xE9")
	assert_true(glyphs.has(HOLE_BYTE),
		"cp1252 holes pass through decode by byte value, so they find their slots too")
	assert_gt(font.get_string_size(String.chr(E_ACUTE), HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x, 0.0,
		"the engine draw advances for the Latin-1 glyph — it renders")


func test_specials_zone_glyphs_follow_decoded_cp1252_codepoints() -> void:
	# Retail indexes the .fnt slot by the TABLE BYTE (glyph = byte - 32)
	# [orig: CGameFont_DrawText @ 0x6752c0]. OpenNova decodes table text to a
	# Unicode Godot String first, so the FontFile boundary must expose that same
	# slot under the byte's decoded cp1252 codepoint. Otherwise Godot substitutes
	# a system-font glyph for shipped curly quotes and dashes (D-FNT-4).
	var font := _fixture_font()
	if font == null:
		return
	var fs := font.get_fixed_size()
	var glyphs := font.get_glyph_list(0, Vector2i(fs, 0))
	assert_false(glyphs.has(LEFT_QUOTE_BYTE),
		"a defined cp1252 special is not exposed as the raw C1-control codepoint")
	assert_true(glyphs.has(LEFT_QUOTE_CODEPOINT),
		"byte 0x93's .fnt slot is exposed at its decoded U+201C codepoint")
	assert_false(glyphs.has(DELETE_CONTROL_CODEPOINT),
		"retail control byte 0x7F remains non-printing")
	assert_false(glyphs.has(EURO_CODEPOINT),
		"retail control byte 0x80 does not expose a Euro glyph")
	assert_false(glyphs.has(UNDEFINED_81_CONTROL_CODEPOINT),
		"retail control byte 0x81 remains non-printing")
	assert_false(font.is_allow_system_fallback(),
		"retail bitmap fonts never substitute glyphs from a system-font face")

	# The decoded codepoint must retain the source slot's own metric (advance =
	# rect width + shadow_offset - 1: 10 + 0 - 1 in this fixture). This proves
	# the engine draw resolves the bitmap font, not a system fallback face.
	assert_eq(font.get_string_size(String.chr(LEFT_QUOTE_CODEPOINT), HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x, 9.0,
		"decoded U+201C renders with byte 0x93's retail .fnt metric")
