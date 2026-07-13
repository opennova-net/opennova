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
#        glyphs keyed at raw byte codepoints 32..255)
#     -> the engine draw (HudText.draw_text / menus / credits).
#
# cp1252 zones pinned here:
#   0x20..0x7F  ASCII        — identity through every stage.
#   0xA0..0xFF  Latin-1      — cp1252 == Unicode; identity through every stage.
#   0x80..0x9F  specials     — decode maps to the cp1252 punctuation codepoints
#               (0x93 -> U+201C etc.); encode maps back losslessly. The GLYPH
#               stage does NOT follow (see the audited divergence below).
#   0x81/0x8D/0x8F/0x90/0x9D — cp1252 holes: decode passes the byte through as
#               its own codepoint, encode returns it; glyph stage matches
#               (whenever the font draws that slot at all).
#
# Intentional exclusions (format facts, not gaps):
#   - bytes < 0x20 never map to glyphs: the .fnt format carries 224 glyphs for
#     chars 32..255 only (libs/fnt fnt.h, FNT_FIRST_CHAR/FNT_GLYPH_COUNT).
#   - byte 0x20 (space) is advance-only in the FontFile view (no drawn rect).
#
# This is an AUDIT: the divergence found in the 0x80..0x9F glyph stage is
# pinned as it behaves today and reported, not fixed here.

const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"

# One section, two entries: text blob starts right after the 16-byte header and
# the two 16-byte entry rows [docs/interface/rtxt-strings-re.md, on-disk format].
const TEXT_BLOB_START := 16 + 2 * 16
const E_ACUTE := 0xE9        # cp1252 'é' — Latin-1 zone (cp1252 == Unicode)
const LEFT_QUOTE_BYTE := 0x93  # cp1252 left curly quote — specials zone
const LEFT_QUOTE_CODEPOINT := 0x201C  # what 0x93 decodes to
const HOLE_BYTE := 0x9D      # unmapped in cp1252 — passes through by byte value
                             # (0x9D rather than 0x81: the fixture draws 0x9D)


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


func test_glyph_stage_keys_the_fnt_slots_at_raw_byte_codepoints() -> void:
	# String -> glyphs: the .fnt view registers each glyph under its table BYTE
	# value (chars 32..255). For ASCII and the Latin-1 zone the decoded
	# codepoint IS the byte, so the engine draw picks the same glyph slot the
	# retail engine indexes for that byte.
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


func test_specials_zone_glyphs_are_keyed_at_bytes_not_decoded_codepoints() -> void:
	# AUDITED DIVERGENCE (reported, not fixed — this pin makes any fix a
	# conscious flip): the retail engine indexes glyphs by the TABLE BYTE
	# (glyph slot = byte - 32; the .fnt model in libs/fnt, drawn by
	# [orig: CGameFont_DrawText @ 0x6752c0]), so a table byte 0x93 draws the
	# font's curly-quote glyph. Our display decode (D-RTXT-8) maps 0x93 to
	# U+201C, but NovaFntResource.to_font_file() keys glyphs at raw byte
	# codepoints — U+201C finds NO .fnt glyph. Because the FontFile ships with
	# allow_system_fallback on (Godot's default), the engine-path draw of a
	# decoded cp1252 special (curly quotes, dashes: bytes 0x82..0x9F less the
	# holes) silently substitutes a HOST SYSTEM font's glyph (or a missing-glyph
	# box where no host font matches) for the .fnt glyph retail renders. Retail
	# tables hit this: 67/98 JO bins carry bytes >= 0x80
	# (docs/interface/rtxt-strings-re.md).
	var font := _fixture_font()
	if font == null:
		return
	var fs := font.get_fixed_size()
	var glyphs := font.get_glyph_list(0, Vector2i(fs, 0))
	assert_true(glyphs.has(LEFT_QUOTE_BYTE),
		"the .fnt DOES carry the curly-quote glyph — keyed at the byte value 0x93")
	assert_false(glyphs.has(LEFT_QUOTE_CODEPOINT),
		"…but nothing is keyed at the decoded U+201C (today's behavior, pinned)")

	# The .fnt slot renders with its own metric when addressed by byte
	# (advance = rect width + shadow_offset - 1: 10 + 0 - 1 in this fixture)…
	assert_eq(font.get_string_size(String.chr(LEFT_QUOTE_BYTE), HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x, 9.0,
		"the .fnt curly-quote slot renders with the .fnt metric when addressed by byte")
	# …while the decoded codepoint can only resolve outside the .fnt: the view
	# leaves Godot's system-font fallback on, so the substitution is host-font
	# dependent (today's behavior, pinned at the mechanism).
	assert_true(font.is_allow_system_fallback(),
		"the .fnt view leaves system-font fallback on — decoded specials draw host glyphs, not the .fnt's")
