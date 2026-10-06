#pragma once

// The retail game-font text engine (ADR 0033 R2): measurement and glyph-quad
// layout over a parsed .fnt, ported structurally from CGameFont. The embedder
// keeps only texture upload and quad rasterization; alignment, per-page
// passes, bold double-strike, italic shear, the underline pass, tab stops,
// inline format tags, and the byte-indexed glyph walk all live here.
// [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0;
//  GText_ParseFormatTag @ 0x674200; CGameFont_ReadLine @ 0x676480]
// Witness record: docs/fonts/fnt-re.md.

#include <formats/fnt/fnt.h>

#include <cstdint>
#include <vector>

namespace opennova::hud {

// The material word every font page draws with, whichever loader made the font:
// colour family 0x600, MODULATE2X(TEXTURE, DIFFUSE) on a modulate-2x device and
// alpha MODULATE(TEXTURE, DIFFUSE) (renderer::material_color_stage). The drawer
// binds the page's material for each page run, so every glyph of every caller
// draws at twice its texel times its vertex colour, saturated; the callers halve
// their colour first (the HUD's half-bright drawers, the menus' text sink), so
// a half-bright colour reads at full brightness. The underline pass is
// untextured and doubles its colour on the CPU instead (GameFontUnderline).
// [orig: GameFont_LoadFromBlob @0x674740 — sub_676D50(page, 0x651) @0x674825,
//  kept at font+0xA0[page] @0x67483B; CGameFont_Create @0x674b9c / @0x674d1e,
//  the same word; CGameFont_DrawText @0x6752c0 binds font+0xA0[page] per run,
//  GfxShader_ApplyPassChecked @0x675a9e / @0x675d10]
inline constexpr uint32_t kFontPageMaterialWord = 0x651u;

// Layout flags — the drawer's packed flag word (v164) [orig: @ 0x6752c0].
enum GameFontFlags : uint32_t {
	kFontAlignCenter = 0x1,
	kFontAlignRight = 0x2,
	kFontStyleBold = 0x10,
	kFontStyleItalic = 0x20,
	kFontStyleUnderline = 0x40,
	// A '<' is not parsed and draws as a glyph [orig: HIBYTE(textBuffer[0])
	// @0x67538e; the '<' arm's test @0x675619].
	kFontTagsDisabled = 0x100,
	// Each tag is parsed and consumed but commits nothing: no colour, style
	// or tab change [orig: LOBYTE(textBuffer[1]) @0x67539e; the parser's
	// cursor-only write @0x6743b0].
	kFontTagsInert = 0x200,
};

// The persistent inline-format state a draw call threads (retail's 5-dword
// textBuffer block; the drawer persists it at this+4860.. and mirrors it back
// through the optional state pointer) [orig: @ 0x6752c0 tail]. The dwords:
// [0] the bold / underline / italic / tags-disabled bytes, [1] the
// tags-inert byte, [2] the live colour, [3] the original colour, [4] the tab
// width [orig: GText_ParseFormatTag @0x674200 reads and commits all five].
struct GameFontState {
	bool bold = false;
	bool underline = false;
	bool italic = false;
	bool tags_disabled = false;  // [0] byte 3 (kFontTagsDisabled)
	bool tags_inert = false;     // [1] byte 0 (kFontTagsInert)
	uint32_t color = 0;          // [2] the live draw colour (ARGB)
	uint32_t original_color = 0; // [3] the <CO> restore target and the <CH> source (ARGB)
	int tab_width = 0;           // [4] 0 = the font's own tab stop; <Tnnn> sets it
};

// One glyph quad in output space. The four corners are explicit because the
// italic pass shears the TOP edge by height/8 [orig: @ 0x6752c0 the 0.125
// skew]; UVs carry the drawer's half-texel bottom-V bias.
struct GameFontQuad {
	uint32_t page = 0;
	float x_top_left = 0.0f;
	float x_top_right = 0.0f;
	float x_bottom_left = 0.0f;
	float x_bottom_right = 0.0f;
	float y_top = 0.0f;
	float y_bottom = 0.0f;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
	// 0xAARRGGBB as the caller supplied / tag-adjusted: the raw diffuse, which
	// the page's material doubles on the device (kFontPageMaterialWord).
	uint32_t color = 0;
};

// The underline pass draws LINE segments under the run, at DOUBLED text color
// (2x per channel, clamped) [orig: @ 0x6752c0 the line-list buffer +
// per-channel 2x fold].
struct GameFontUnderline {
	float x0 = 0.0f;
	float x1 = 0.0f;
	float y = 0.0f;
	uint32_t color = 0;
};

struct GameFontRun {
	std::vector<GameFontQuad> quads;
	std::vector<GameFontUnderline> underlines;
	float width = 0.0f;  // the measured extent actually laid out
	float height = 0.0f;
};

class GameFont {
public:
	// Borrow a parsed font (not owned; the caller keeps it alive).
	void set_font(const opennova::fnt::fnt_font_t *font) { font_ = font; }
	const opennova::fnt::fnt_font_t *font() const { return font_; }

	// Emitted glyph quads carry page indices offset by this base, giving each
	// font its own page namespace inside one mixed draw list (the compiler
	// assigns slot * FNT_MAX_PAGES; the device leg indexes its page-texture
	// table the same way).
	void set_page_base(uint32_t base) { page_base_ = base; }

	// Pixel measurement [orig: CGameFont_MeasureText @ 0x674e70]: byte-indexed
	// glyphs (controls < 0x20 and 0x7F/0x80/0x81 skipped), '\\' escapes,
	// '<' tags via the shared parser, '\t' to the next tab stop, '\n' folds
	// width into the max and adds the SPACE glyph's v-extent as line height;
	// the final width strips the trailing (glyph_spacing - 1) pad.
	void measure(const char *text, float scale_x, float scale_y,
			int *out_w, int *out_h, GameFontState *state = nullptr) const;

	// Glyph-quad layout [orig: CGameFont_DrawText @ 0x6752c0]: one pass per
	// texture page over each 255-byte line [orig: CGameFont_ReadLine
	// @ 0x676480]; center/right alignment re-measures per line; bold emits a
	// second strike at (+1, -1); italic shears the top edge by height/8;
	// underline emits doubled-color segments; every glyph advance is
	// floor(w * scale + (spacing - 1) * scale + 0.5) from the same cursor walk
	// the measurer uses. (x, y) is the anchor the alignment resolves against.
	// The live and original colours start as `color`, as for retail's
	// null-state callers; a passed state carries its styles, tag switches and
	// tab width.
	GameFontRun layout(const char *text, float x, float y, float scale_x,
			float scale_y, uint32_t flags, uint32_t color,
			GameFontState *state = nullptr) const;

	// The shared inline-tag parser [orig: GText_ParseFormatTag @ 0x674200]:
	// <B>/<I>/<U> (with '-' prefix off), <Cxxxxxx> hex colour into the live
	// colour's low 24 bits, <CO> the original colour, <CH> the original's
	// half-bright, <Tnnn> the tab width. Returns true when the tag was
	// well-formed. *index lands on its '>' and the state commits only then;
	// while tags_inert, *index lands where the scan stopped, '>' or the
	// terminator, and nothing commits.
	static bool parse_format_tag(const char *text, int *index,
			GameFontState *state);

	// The engine's per-font line height: the SPACE glyph's v-extent in pixels
	// [orig: @ 0x6752c0 (this[95] - this[93]) * 256].
	float line_height(float scale_y) const;

	// GameFont_MeasureCharHeight: the extent's height times the slot's
	// scale_y, truncated. The extent's height is the SPACE glyph's v-extent
	// whatever the byte, trunc((v1 - v0) * (800 / design_width) * 256); a
	// control byte other than tab measures 0 [orig: GameFont_MeasureCharHeight
	// @ 0x580a80; CGameFont_GetCharExtent @0x674dc0 -- the height
	// @0x674e2c..0x674e44 off the first glyph record, the control-byte zeroing
	// @0x674e57]. The friendly-tag line metric measures '0'.
	float char_height(uint8_t byte, float scale_y) const;

	// One glyph's pixel extent WITH the spacing pad, rounded — the
	// per-character extent the chat wrapper walks with:
	// floor(((u1 - u0) * 256 + glyph_spacing - 1) * (800 / design_width) + 0.5)
	// (scale_x multiplied into the design fold; the original takes no scale).
	// Bytes below 0x20 measure 0; a tab measures `tab_width` when nonzero
	// (retail's per-font tab width, this+0x168) and the SPACE glyph otherwise
	// [orig: CGameFont_GetCharExtent @0x674dc0 — tab @0x674dd8, control bytes
	//  @0x674e55, the fold @0x674de4..0x674e25; read as charSize[0] by
	//  HUD_WordWrapText @0x5809db, which adds +1 per byte on top @0x5809e4].
	int char_width(uint8_t byte, float scale_x, int tab_width = 0) const;

private:
	struct Cursor;
	const opennova::fnt::fnt_glyph_t *glyph_for_byte(uint8_t byte) const;

	const opennova::fnt::fnt_font_t *font_ = nullptr;
	uint32_t page_base_ = 0;
};

} // namespace opennova::hud
