extends GutTest

# The .fnt format carries NO advance table. Retail advances the text cursor by
# `rect_width + (glyph_spacing - 1) * scale` and measures the same way
# [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0], so a glyph's RECT is
# its advance. Retail's own shipped fonts are the spec: Serpen24 gives 'i' 6 px and 'W' 17 px,
# and sizes its space like any other glyph (7x17) — those are advances, not ink boxes.
#
# The rasterizer used to emit each glyph's ink bounding box and NO cell at all for space, which
# yields a font that looks right in the editor's own preview and draws as run-together text in
# retail. These pin the three properties that stop that happening again.
#
# Rasterization needs a real text server to render glyph bitmaps; the headless dummy driver
# renders nothing, so the metric tests gate on it and report pending rather than asserting on
# degenerate output.

const PX := 16
const ASCII_SAMPLE := " ABCWXYZ0189ijl."

var _font: FntResource = null


func before_all() -> void:
	if _text_server_is_dummy():
		return
	_font = FntRasterizer.rasterize(ThemeDB.fallback_font, PX, 0)


func _text_server_is_dummy() -> bool:
	var ts := TextServerManager.get_primary_interface()
	return ts == null or String(ts.get_name()).to_lower().contains("dummy")


# Every metric test needs a rasterized font; returns null (and marks the test pending) when the
# driver cannot produce one.
func _require_font() -> FntResource:
	if _text_server_is_dummy():
		pending("no real text server (headless dummy driver renders no glyphs)")
		return null
	if _font == null:
		pending("rasterization returned null under this text server")
		return null
	return _font


func _rect(font: FntResource, ch: String) -> Rect2i:
	return font.get_glyph_rect(ch.unicode_at(0))


func test_space_gets_a_real_cell() -> void:
	# A 0x0 space advances by -1 and runs every word together. Retail's fonts size space like
	# any other glyph (9x19 in Gunpl22b, 7x17 in Serpen24).
	var font := _require_font()
	if font == null:
		return
	var space := _rect(font, " ")
	assert_gt(space.size.x, 0, "space has a non-zero advance width")
	assert_gt(space.size.y, 0, "space has a non-zero cell height")


func test_widths_are_proportional_advances() -> void:
	# Ink boxes would make 'i' a sliver; advances keep it a readable fraction of 'W'.
	var font := _require_font()
	if font == null:
		return
	var narrow := _rect(font, "i").size.x
	var wide := _rect(font, "W").size.x
	assert_gt(narrow, 0, "'i' has a width")
	assert_gt(wide, narrow, "'W' advances further than 'i' (%d vs %d)" % [wide, narrow])
	# An ink box for 'i' at 16 px would be 2-3 px; an advance is several times that.
	assert_gt(narrow, 3, "'i' carries its side bearings, not just its stem (%d px)" % narrow)


func test_cell_heights_are_uniform() -> void:
	# One height per font keeps baselines aligned; retail's fonts are uniform this way.
	var font := _require_font()
	if font == null:
		return
	var heights := {}
	for ch in ASCII_SAMPLE:
		var r := _rect(font, ch)
		if r.size.y > 0:
			heights[r.size.y] = true
	assert_eq(heights.size(), 1,
			"every sampled glyph shares one cell height, got %s" % [heights.keys()])


func test_descenders_are_not_clipped() -> void:
	# The cell is the full line height, so a 'g' fits without a taller-than-uniform rect.
	var font := _require_font()
	if font == null:
		return
	assert_eq(_rect(font, "g").size.y, _rect(font, "o").size.y,
			"a descender uses the same cell height as an x-height glyph")


func test_spacing_is_zero_so_the_baked_advances_are_what_retail_draws() -> void:
	var font := _require_font()
	if font == null:
		return
	assert_eq(font.get_glyph_spacing(), 0,
			"rasterized faces bake the advance into the rect and leave spacing neutral")


func test_build_font_source_rejects_a_missing_file() -> void:
	# Path branch: safe to assert without a text server.
	assert_null(FntRasterizer.build_font_source("C:/nope/does_not_exist.ttf"),
			"a .ttf path that is not there yields no font")
	assert_null(FntRasterizer.build_font_source(""), "an empty source yields no font")


func test_build_font_source_rejects_an_uninstalled_family() -> void:
	# SystemFont silently substitutes a default for an unknown family, which would hand back the
	# wrong typeface with no error, so the name is checked against the installed set.
	assert_null(FntRasterizer.build_font_source("Definitely Not An Installed Family 9000"),
			"an uninstalled family is refused rather than silently substituted")
