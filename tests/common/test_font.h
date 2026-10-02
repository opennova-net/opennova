// The uniform font the HUD and menu frame-compiler tests measure against. It
// needs no glyph art, so it stays a test header while the stroke font's art
// lives with the editor's blank factories (engine/editor/blank/blank_font_art.h).
// Header-only, infrastructure only (no retail counterpart to cite).
#pragma once

#include <formats/fnt/fnt.h>

#include <cstdint>

namespace test_font {

// One blank page, every glyph an 8x16 px UV rect on the 256-page grid, spacing
// 2, design width 800 (scale 1): glyph advance 9, and the measured width strips
// the trailing pad.
inline opennova::fnt::fnt_font_t uniform_test_font() {
	opennova::fnt::fnt_font_t font{};
	opennova::fnt::fnt_init_blank(&font, 1, 2);
	font.design_width = 800;
	for (uint32_t i = 0; i < opennova::fnt::FNT_GLYPH_COUNT; ++i) {
		font.glyphs[i].page = 0;
		font.glyphs[i].uv.u0 = 0.0f;
		font.glyphs[i].uv.v0 = 0.0f;
		font.glyphs[i].uv.u1 = 8.0f / 256.0f;
		font.glyphs[i].uv.v1 = 16.0f / 256.0f;
	}
	return font;
}

} // namespace test_font
