// THE KEY-TOGGLED OVERLAY WINDOWS: the I briefing panel, the F1 key-binding
// help screen and the F12 map legend, the tail HUD_DrawGameplayOverlays draws
// after the big map [orig: HUD_DrawGameplayOverlays @0x5BDE60 — the briefing
// @0x5be133..0x5be145, HUD_DrawWinConditions @0x5be163, HelpScreen_Draw
// @0x5be179], plus the block wrapper the briefing lays its text out with
// [orig: HUD_DrawWrappedText @0x580C00].
// The overlay-panel pass's voice-macro menus and the SP pause text live here
// too (HUD_DrawOverlayPanels @0x5c0060).

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_medic_cross.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace opennova::hud {
namespace {

// The overlay safe area's top/bottom rows the help box hangs from
// [orig: dword_24C1900 / dword_24C1904 — 0 / 768 for the full frame,
//  stamped by Renderer_SetDisplayModeWithFallback @0x587634/@0x58760b].
constexpr float kSafeTop = 0.0f;
constexpr float kSafeBottom = 768.0f;

// One truncating float -> int step [orig: _ftol2_sse].
int ftol(float v) { return static_cast<int>(v); }

// The legend's pulse folds on the HUD frame counter [orig:
// HUD_DrawHelpScreenIcons @0x4976b0..0x497714].
uint32_t legend_icon_color(const char *name, uint32_t color, int frame_counter) {
	if (std::strcmp(name, "hud_icon_pwrmed") == 0 || std::strcmp(name, "hud_icon_pwrammo") == 0 ||
			std::strcmp(name, "hud_icon_pwrweap") == 0) {
		// A 128-frame triangle 0..64 on every channel, lifted by 63
		// [orig: @0x4976f8..0x497714].
		uint32_t q = static_cast<uint32_t>(frame_counter - 8) & 0x7Fu;
		if (q > 0x40u) q = 127u - q;
		const uint32_t c = (q + 63u) & 0xFFu;
		return (color & 0xFF000000u) | (c << 16) | (c << 8) | c;
	}
	if (std::strcmp(name, "hud_icon_psp") == 0 || std::strcmp(name, "hud_icon_waypoint") == 0 ||
			std::strcmp(name, "hud_icon_recentspeaker") == 0) {
		// A 64-frame triangle 0..32 scaling each channel by q/32
		// [orig: @0x4976b0..0x4976ea].
		uint32_t q = static_cast<uint32_t>(frame_counter - 8) & 0x3Fu;
		if (q > 0x20u) q = 63u - q;
		const auto scale = [q](uint32_t channel) { return ((q * channel) >> 5) & 0xFFu; };
		return (color & 0xFF000000u) | (scale((color >> 16) & 0xFFu) << 16) |
				(scale((color >> 8) & 0xFFu) << 8) | scale(color & 0xFFu);
	}
	return color;
}

} // namespace

int wrapped_text_layout(const GameFont &font, float scale_x, float scale_y,
		const std::string &text, int x_start, int y_start, int x_end, int y_limit,
		int skip_lines, std::vector<HudWrappedLine> &out) {
	out.clear();
	if (font.font() == nullptr || scale_x == 0.0f) return 0;
	const int budget = ftol(static_cast<float>(x_end - x_start) / scale_x);
	if (text.empty() || budget <= 0) return 0;
	const int line_h = ftol(font.char_height('I', scale_y));
	const int half_line = line_h >> 1;
	const size_t n = text.size();
	const auto at = [&](size_t i) -> char { return i < n ? text[i] : '\0'; };
	int pen_y = y_start;
	int line_index = 0;
	size_t line_start = 0;
	size_t last_space = 0;
	int width = 0;
	size_t i = 0;
	while (true) {
		const char c = at(i);
		if (c == ' ') last_space = i;
		if (c != '\0') {
			int h = 0;
			const std::string run = text.substr(line_start, i + 1 - line_start);
			font.measure(run.c_str(), 1.0f, 1.0f, &width, &h);
		}
		size_t line_end = i;
		if (width <= budget) {
			if (c != '\r' && c != '\0') {
				++i;
				continue;
			}
		} else if (last_space != 0) {
			line_end = last_space;
			i = last_space;
		}
		if (line_index >= skip_lines) {
			if (line_start != line_end) {
				out.push_back({line_start, line_end, pen_y});
				pen_y += line_h;
			} else {
				pen_y += half_line;
			}
		}
		++line_index;
		if (at(i) == '\0') return 0;
		if (y_start != y_limit && pen_y + line_h > y_limit) return line_index;
		line_start = line_end + 1;
		if (at(line_end + 1) == '\n') line_start = line_end + 2;
		width = 0;
		last_space = 0;
		++i;
	}
}

void HudBriefingPages::reset() {
	page = 0;
	for (int &start : starts) start = 0;
}

void HudBriefingPages::cycle(int direction, bool in_session) {
	if (direction == 0) {
		reset();
		return;
	}
	if (in_session) {
		page = direction <= 0 ? (page + direction + 4) % 4 : (direction + page) % 4;
		return;
	}
	if (direction == 1) {
		if (page <= 8 && starts[page + 1] != 0) ++page;
	} else if (direction == -1 && page != 0) {
		--page;
	}
}

void HudFrameCompiler::mark_top_layer() {
	draw_list_.top_begin.quads = draw_list_.quads.size();
	draw_list_.top_begin.tris = draw_list_.tris.size();
	draw_list_.top_begin.lines = draw_list_.lines.size();
	draw_list_.top_begin.glyphs = draw_list_.glyphs.size();
	draw_list_.top_begin.underlines = draw_list_.underlines.size();
}

void HudFrameCompiler::emit_label_box(float x1, float y1, float x2, float y2,
		const std::string &title, uint32_t title_color, float w, float h) {
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	float title_gap = 0.0f;
	if (!title.empty() && bf.font() != nullptr) {
		int tw = 0;
		int th = 0;
		bf.measure(title.c_str(), bscale, bscale, &tw, &th);
		title_gap = static_cast<float>(tw) + kBoxTitlePad;
		const float s = w / kBoxScaleRef;
		if (title_gap > kBoxTitleTrim * s) title_gap -= kBoxTitleTrim * s;
	}
	emit_stdbox(sx(x1, w), sy(y1, h), sx(x2, w), sy(y2, h), w, 0xFFu, title_gap);
	if (!title.empty()) {
		emit_slot_text(label_font_bold_, label_scale_, title.c_str(), sx(x1 + 15.0f, w),
				sy(y1 + 2.0f, h), half_bright_argb(title_color), 0u);
	}
}

void HudFrameCompiler::element_objectives(const HudFrameState &state, float w, float h) {
	// THE OBJECTIVES PANEL [orig: HUD_DrawWinConditions @0x5ba940, called
	// @0x5be163 with x = 15, y = dword_24C1900 + 0xF0 (the 0-top safe area), and
	// only while the panel alpha dword_24C18CC is nonzero @0x5be14a — the co-op
	// toggle flips it 0 <-> 0xFF @0x49b68b].
	if (state.objectives.empty()) return;
	const uint32_t alpha = state.objectives_alpha;
	if (alpha == 0) return;
	const uint32_t a24 = alpha << 24;
	const int surface_w = static_cast<int>(w);
	if (surface_w <= 0) return;
	// The slots: rows and every measure in the LARGE slot, the header drawn in
	// the BOLD slot [orig: HUD_MeasureTextWH(.., &g_HUDLabelFontLarge, ..)
	// @0x5baa2c; GameFont_MeasureTextWidth(header, &g_HUDLabelFontLarge)
	// @0x5baa68; Render_DrawTextScaled(.., &g_HUDLabelFontBold, ..) @0x5baae4;
	// the rows' Render_DrawTextScaled(.., &g_HUDLabelFontLarge, ..) @0x5bacb5].
	// An absent slot falls back to the HUD slot, as emit_slot_text draws.
	const bool have_large = label_font_large_.font() != nullptr;
	const GameFont &large = have_large ? label_font_large_ : font_;
	const float large_scale = have_large ? label_large_scale_ : hud_font_scale_;
	if (large.font() == nullptr) return;
	const float x = 15.0f;
	const float y = kSafeTop + 240.0f;
	// The measure pass: each row's measured width stays in screen pixels for
	// the max, its height folds through the WIDTH ratio (h << 10) / W and
	// sums into the panel height; the header's width joins the max last
	// [orig: the measure loop @0x5ba9c9; @0x5baa2c..0x5baa4c — `(*width_ptr
	// << 10) / g_OverlayCtx` @0x5baa37; the header max @0x5baa68..0x5baa78].
	std::vector<int> heights;
	heights.reserve(state.objectives.size());
	int max_w = 0;
	int sum_h = 0;
	for (const HudObjectiveRow &row : state.objectives) {
		int rw = 0;
		int rh = 0;
		large.measure(row.text.c_str(), large_scale, large_scale, &rw, &rh);
		const int folded = (rh << 10) / surface_w;
		heights.push_back(folded);
		if (rw > max_w) max_w = rw;
		sum_h += folded;
	}
	{
		int hw = 0;
		int hh = 0;
		large.measure(state.objectives_header.c_str(), large_scale, large_scale, &hw, &hh);
		if (hw > max_w) max_w = hw;
	}
	// The box: a TITLELESS HUD_DrawLabelBox, so the colour argument never
	// reaches it — the plain stdbox [orig: HUD_DrawLabelBox(ctx, x, y - 24,
	// (maxW << 10) / W + x + 72, y + sumH + 48, NULL, (a << 24) + 0xFFFFFF)
	// @0x5baaba -> the NULL-title arm Render_HUDBoxOverlay(stdbox, rect, NULL)
	// @0x51f099].
	emit_label_box(x, y - 24.0f, static_cast<float>((max_w << 10) / surface_w) + x + 72.0f,
			y + static_cast<float>(sum_h) + 48.0f, std::string(), 0xFFFFFFFFu, w, h);
	// Both text draws halve the RGB and keep the alpha [orig:
	// Render_DrawTextScaled @0x5d3fa0 -> sub_5D2FC0 mode 0 -> sub_580560
	// @0x580560 — (c & 0xFF000000) + ((c >> 1) & 0x7F7F7F)].
	emit_slot_text(label_font_bold_, label_scale_, state.objectives_header.c_str(),
			sx(x + 24.0f, w), sy(y, h), half_bright_keep_alpha(a24 | 0x00FFFFFFu), 0u);
	// Rows: y advances by the header's 0x18 first, then by each row's folded
	// height [orig: y_origin + 6 dwords @0x5baaf5; += heights[slot] @0x5bacc1].
	const float bx = x + 24.0f; // the checkbox x [orig: edi stays x + 0x18]
	float row_y = y + 24.0f;
	const auto line = [&](float x0, float y0, float x1, float y1, uint32_t color) {
		HudLine seg;
		seg.color = color;
		seg.width = 1.0f;
		seg.x0 = sx(x0, w);
		seg.y0 = sy(y0, h);
		seg.x1 = sx(x1, w);
		seg.y1 = sy(y1, h);
		draw_list_.lines.push_back(seg);
	};
	for (size_t i = 0; i < state.objectives.size(); ++i) {
		const HudObjectiveRow &row = state.objectives[i];
		// The 16x16 checkbox outline, light gray [orig: the four
		// Render_DrawClipped2DLine calls @0x5bab47..0x5bab9a, colour 0xFFE0E0E0
		// with the panel alpha as the separate modulate arg].
		const uint32_t box_c = a24 | 0x00E0E0E0u;
		line(bx, row_y, bx + 16.0f, row_y, box_c);
		line(bx + 16.0f, row_y, bx + 16.0f, row_y + 16.0f, box_c);
		line(bx + 16.0f, row_y + 16.0f, bx, row_y + 16.0f, box_c);
		line(bx, row_y + 16.0f, bx, row_y, box_c);
		if (row.done) {
			// The done mark is a RED X — both diagonals, each tripled with
			// one-pixel offsets [orig: the six calls @0x5babc1..0x5bac5b,
			// colour 0xFFFF0000].
			const uint32_t x_c = a24 | 0x00FF0000u;
			line(bx, row_y, bx + 16.0f, row_y + 16.0f, x_c);
			line(bx + 1.0f, row_y, bx + 16.0f, row_y + 15.0f, x_c);
			line(bx, row_y + 1.0f, bx + 15.0f, row_y + 16.0f, x_c);
			line(bx + 16.0f, row_y, bx, row_y + 16.0f, x_c);
			line(bx + 15.0f, row_y, bx, row_y + 15.0f, x_c);
			line(bx + 16.0f, row_y + 1.0f, bx + 1.0f, row_y + 16.0f, x_c);
		}
		// Gray once done [orig: (a << 24) + (done ? 0x808080 : 0xFFFFFF)
		// @0x5bac8c], at (x + 48, y - 2) in the LARGE slot.
		const uint32_t color = a24 + (row.done ? 0x00808080u : 0x00FFFFFFu);
		emit_slot_text(label_font_large_, label_large_scale_, row.text.c_str(),
				sx(x + 48.0f, w), sy(row_y - 2.0f, h), half_bright_keep_alpha(color), 0u);
		row_y += static_cast<float>(heights[i]);
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_briefing(const HudFrameState &state, float w, float h) {
	// [orig: sub_5BAD00 — HUD_DrawLabelBox(ctx, 200, 40, 824, 728, null, -1)
	//  @0x5bad4a; the text rect (235, 75)..(789, 693) through
	//  Viewport_ScaleToVirtualCoords @0x5bad73/@0x5bad87; HUD_DrawWrappedText
	//  in the bold slot, alignment 0, colour -1, skipping the page's start
	//  lines @0x5bade7; an overflow writes the next page's start @0x5badfa]
	if (!state.briefing.shown) return;
	emit_label_box(200.0f, 40.0f, 824.0f, 728.0f, std::string(), 0xFFFFFFFFu, w, h);
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	if (bf.font() == nullptr || state.briefing.text.empty()) return;
	const int x0 = ftol(sx(235.0f, w));
	const int y0 = ftol(sy(75.0f, h));
	const int x1 = ftol(sx(789.0f, w));
	const int y1 = ftol(sy(693.0f, h));
	HudBriefingPages &pages = briefing_pages_;
	if (pages.page < 0 || pages.page >= HudBriefingPages::kPages) pages.page = 0;
	std::vector<HudWrappedLine> lines;
	const int next = wrapped_text_layout(bf, bscale, bscale, state.briefing.text, x0, y0, x1,
			y1, pages.starts[pages.page], lines);
	// The wrapper halves the colour keeping its alpha [orig: @0x580c99..0x580ca8].
	const uint32_t color = half_bright_keep_alpha(0xFFFFFFFFu);
	// The wrapper threads one five-dword state block through every line: the
	// style bytes cleared, the live and original colours the halved colour,
	// the tab width the font's [orig: @0x580caa..0x580cce; &block passed
	// @0x580d9e]. Its second dword, the inert byte, is never written, so it
	// holds the panel's stack residue: on the authority path, the section
	// row TextResource_FindEntryBySectionAndKey pushed for its last call
	// (push edi @0x75d2e9, reached through MissionText_GetString @0x5bada2).
	// That pointer's low byte is not zero, so every tag is consumed and
	// changes nothing: the briefing draws plain, its markup hidden
	// [orig: GText_ParseFormatTag @0x6743b0] (D-HUD-50).
	GameFontState block;
	block.tags_inert = true;
	for (const HudWrappedLine &line : lines) {
		const std::string run = state.briefing.text.substr(line.begin, line.end - line.begin);
		const GameFontRun laid = bf.layout(run.c_str(), static_cast<float>(x0),
				static_cast<float>(line.y), bscale, bscale, 0u, color, &block);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), laid.quads.begin(), laid.quads.end());
		draw_list_.underlines.insert(draw_list_.underlines.end(), laid.underlines.begin(),
				laid.underlines.end());
	}
	if (next != 0 && pages.page + 1 < HudBriefingPages::kPages) pages.starts[pages.page + 1] = next;
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_help_screen(const HudFrameState &state, float w, float h) {
	// The map legend wins over the key list [orig: HelpScreen_Draw @0x497800 —
	// dword_24C18B4 tails into HUD_DrawHelpScreenIcons @0x497811].
	if (state.map_legend.shown) {
		element_map_legend(state, w, h);
		return;
	}
	const HudHelpScreenState &hs = state.help_screen;
	if (!hs.shown) return;
	// [orig: HelpScreen_Draw @0x497816 — HUD_DrawLabelBox(ctx, 16, top + 16,
	//  1008, bottom - 16, title, -1) @0x49784b; the title again, centred at
	//  (512, 40) @0x497864; 23 rows from y 90 in steps of 25, the help text
	//  left at x 512 @0x49788b and the key right-aligned at x 472 @0x49789f;
	//  the page line centred at x 472, ten below the rows @0x4978ce, the
	//  CHANGE_SCREEN hint 25 below it @0x4978f9; all in the bold slot in
	//  g_HUDColors.active, half-bright]
	emit_label_box(16.0f, kSafeTop + 16.0f, 1008.0f, kSafeBottom - 16.0f, hs.title, 0xFFFFFFFFu,
			w, h);
	const uint32_t color = half_bright_argb(active_color(state));
	emit_slot_text(label_font_bold_, label_scale_, hs.title.c_str(), sx(512.0f, w), sy(40.0f, h),
			color, kFontAlignCenter);
	float y = 90.0f;
	for (int row = 0; row < 23; ++row) {
		const size_t r = static_cast<size_t>(row);
		if (r < hs.texts.size())
			emit_slot_text(label_font_bold_, label_scale_, hs.texts[r].c_str(), sx(512.0f, w),
					sy(y, h), color, 0u);
		if (r < hs.keys.size())
			emit_slot_text(label_font_bold_, label_scale_, hs.keys[r].c_str(), sx(472.0f, w),
					sy(y, h), color, kFontAlignRight);
		y += 25.0f;
	}
	const float footer_y = y + 10.0f;
	emit_slot_text(label_font_bold_, label_scale_, hs.page_line.c_str(), sx(472.0f, w),
			sy(footer_y, h), color, kFontAlignCenter);
	emit_slot_text(label_font_bold_, label_scale_, hs.footer.c_str(), sx(472.0f, w),
			sy(footer_y + 25.0f, h), color, kFontAlignCenter);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_map_legend(const HudFrameState &state, float w, float h) {
	// [orig: HUD_DrawHelpScreenIcons @0x497480 — HUD_DrawLabelBox(ctx, 128,
	//  96, 896, 672, Hud/hud_map_legend, -1) @0x4974bd; the label offset
	//  (32, 8), column x 192 / 512, first row y 144 and the 40 row step, each
	//  through Viewport_ScaleToVirtualCoords @0x4974e1/@0x49750d/@0x49752f]
	const HudMapLegendState &ml = state.map_legend;
	emit_label_box(128.0f, 96.0f, 896.0f, 672.0f, ml.title, 0xFFFFFFFFu, w, h);
	const float label_dx = sx(32.0f, w);
	const float label_dy = sy(8.0f, h);
	const float col0_x = sx(192.0f, w);
	const float col1_x = sx(512.0f, w);
	const float row_step = sy(40.0f, h);
	float y = sy(144.0f, h);
	const uint32_t label_color = half_bright_argb(active_color(state));
	for (int i = 0; i < kHudMapLegendIconCount; ++i) {
		const HudMapLegendIcon &icon = kHudMapLegendIcons[i];
		// The icon's half extents: the size factor times 16, scaled per axis
		// [orig: `fmul flt_7C4870` (16.0) @0x49754a; Viewport_ScaleToVirtualCoords
		//  @0x49756c]. The pair walk: even entries in the left column, odd in
		//  the right, the row stepping after each odd one [orig: @0x497586..
		//  0x4975a1, @0x4977d3..0x4977d9].
		const int v = ftol(icon.size * 16.0f);
		const float half_w = sx(static_cast<float>(v), w);
		const float half_h = sy(static_cast<float>(v), h);
		const bool left = (2 * (i / 2)) == i;
		const float x = left ? col0_x : col1_x;
		if (std::strcmp(icon.name, "hud_icon_medic") == 0) {
			// The medic entry is the cross plate, a quarter of the extents
			// about (x + 0.5, y + 0.5) [orig: @0x4975b7..0x497620 ->
			// HUD_DrawMedicCrossQuad(x1, y1, x2, y2, 0, 0xFF)].
			const float qw = static_cast<float>(ftol(half_w) >> 2);
			const float qh = static_cast<float>(ftol(half_h) >> 2);
			for (const MedicCrossQuad &q : medic_cross_quads(x + 0.5f - qw, y + 0.5f - qh,
						 x + 0.5f + qw, y + 0.5f + qh, 0xFF))
				emit_rect(q.x0, q.y0, q.x1, q.y1, q.color, true);
		} else {
			// The rotated strip cell: the recent-speaker icon turns a quarter
			// [orig: @0x49771e..0x49772a], the colour forced opaque, the raw
			// diffuse the strip's material 0x651 doubles on the device [orig:
			// HUD_DrawRotatedTexturedQuad @0x497390 -> Render_DrawIconStripCell_Debug
			// @0x67bae0, `color | 0xFF000000`].
			const uint32_t color =
					legend_icon_color(icon.name, icon.color, ml.frame_counter) | 0xFF000000u;
			const int angle_fp =
					std::strcmp(icon.name, "hud_icon_recentspeaker") == 0 ? 0x40000000 : 0;
			// [orig: @0x497390 — angle (fp >> 16) * 0.000095873802, the four
			//  corners with halfHeight along x and halfWidth along y]
			const double rad = static_cast<double>(angle_fp >> 16) * 0.000095873802;
			const float s = static_cast<float>(std::sin(rad));
			const float c = static_cast<float>(std::cos(rad));
			const float hw = half_w;
			const float hh = half_h;
			float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
			hud_icon_strip_cell_uv(state.minimap, icon.cell, u0, v0, u1, v1);
			const HudTriVertex p0{x - hh * c + hw * s, y - hh * s - hw * c, u0, v0, color};
			const HudTriVertex p1{x + hh * c + hw * s, y + hh * s - hw * c, u1, v0, color};
			const HudTriVertex p2{x + hh * c - hw * s, y + hh * s + hw * c, u1, v1, color};
			const HudTriVertex p3{x - hh * c - hw * s, y - hh * s + hw * c, u0, v1, color};
			HudTri a;
			a.a = p0;
			a.b = p1;
			a.c = p2;
			a.color = color;
			a.texture = kHudTexMapIcons;
			HudTri b = a;
			b.a = p0;
			b.b = p2;
			b.c = p3;
			draw_list_.tris.push_back(a);
			draw_list_.tris.push_back(b);
		}
		// The label, bold, half-bright in the active colour
		// [orig: HUD_DrawTextLeft_HalfBright @0x4977c9].
		const size_t li = static_cast<size_t>(i);
		if (li < ml.labels.size() && !ml.labels[li].empty())
			emit_slot_text(label_font_bold_, label_scale_, ml.labels[li].c_str(), label_dx + x,
					y - label_dy, label_color, 0u);
		if (!left) y += row_step;
	}
	++draw_list_.elements_drawn;
}

// The F9 / F10 voice-macro menus: the stdbox with its title, then the ten
// numbered rows in the bold slot, half-bright, the context rows in palette
// entry 4 [orig: HUD_DrawEmotesMenu @0x5bff00 / HUD_DrawRadioTitleMenu
// @0x5bfb90 — HUD_DrawLabelBox(ctx, 128, 180, 896, 550, title, -1)
// @0x5bff5c / @0x5bfbec; `display = i <= 9 ? i : i - 10` @0x5bff89..0x5bff8e;
// sprintf "%i - %s" @0x5bffe3; the colour pick @0x5bfff7..0x5c0004 /
// @0x5bfc84..0x5bfc91; HUD_DrawTextLeftScaled(bold, 200, y, line, colour, 1)
// @0x5c001f, y 225..495 step 30 @0x5bff81].
void HudFrameCompiler::element_voice_macro_menu(const HudFrameState &state,
		const HudVoiceMacroMenuState &menu, int context_row, float w, float h) {
	if (!menu.shown) return;
	emit_label_box(128.0f, 180.0f, 896.0f, 550.0f, menu.title, 0xFFFFFFFFu, w, h);
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	char line[256];
	int y = 225;
	for (int i = 1; i <= 10; ++i, y += 30) {
		const int display = i <= 9 ? i : i - 10;
		std::snprintf(line, sizeof(line), "%i - %s", display,
				menu.texts[static_cast<size_t>(i - 1)].c_str());
		const uint32_t color = i >= context_row ? hud_palette(4) : active_color(state);
		emit_slot_text(bf, bscale, line, sx(200.0f, w), sy(static_cast<float>(y), h),
				half_bright_argb(color), 0u);
	}
	++draw_list_.elements_drawn;
}

// The SP pause word's text: Overlays/STROVER7 through HUD_DrawTextAtVirtualPos
// mode 1 (right-aligned, half-bright) in the Impact38 slot at PAUSEDPOS, or
// (1000, 4) when either field is zero, in g_HUDColors.active [orig:
// HUD_DrawPausedText @0x59d650 — the fallback @0x59d65c..0x59d670, the
// Impact38 push @0x59d678, mode 1 @0x59d675, HUD_DrawTextAtVirtualPos
// @0x59d699].
void HudFrameCompiler::element_paused_text(const HudFrameState &state, float w, float h) {
	if (!state.paused) return;
	const std::array<int, 2> pos = paused_text_pos(layout_.paused_x, layout_.paused_y);
	const int x = pos[0];
	const int y = pos[1];
	// The Impact38 slot falls back like the end-round overlay's.
	const bool have_impact = label_font_impact38_.font() != nullptr;
	const bool have_large = label_font_large_.font() != nullptr;
	const GameFont &lf = have_impact ? label_font_impact38_
			: have_large ? label_font_large_ : label_font_bold_;
	const float ls = (have_impact || have_large) ? label_large_scale_ : label_scale_;
	emit_text_at_virtual_pos(lf.font() != nullptr ? lf : font_, lf.font() != nullptr ? ls : 1.0f,
			state.paused_text.c_str(), x, y, w, h, active_color(state), 1);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
