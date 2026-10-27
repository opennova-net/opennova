// The loading screen's wrapped text block (loading_screen.h): the line breaker
// and the line placer of Render_DrawWrappedTextBlockEx, ported break rule
// for break rule, and the session block composited into the background as the
// game blends it. And the start-mission splash's continue line, laid out as
// the game draws it.
#include <runtime/hud/loading_screen.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <runtime/hud/hud_math.h>

namespace opennova::hud {

GameFontRun splash_continue_run(const GameFont &font, const char *text, int surface_w, int surface_h,
		bool phase_on) {
	if (font.font() == nullptr || text == nullptr || surface_w <= 0 || surface_h <= 0) return {};
	// The design point scaled with retail's integer rounding [orig: HUD_DrawTextAtVirtualPos @ 0x5d3ec0,
	// ((x * W + 512) / 1024, (y * H + 384) / 768)].
	const int64_t x = (static_cast<int64_t>(kSplashContinueX) * surface_w + 512) / 1024;
	const int64_t y = (static_cast<int64_t>(kSplashContinueY) * surface_h + 384) / 768;
	// The large slot's scale [orig: HUD_InitAllFonts @ 0x51ef62] and the centred drawer's halved colour,
	// alpha forced [orig: HUD_DrawTextCentered_HalfBright @ 0x580688..0x5806ab; the blink select @
	// 0x5209b0..0x5209be].
	const float scale = hud_label_font_choice(surface_w).large_scale;
	const uint32_t color = half_bright_argb(phase_on ? kSplashContinueColorOn : kSplashContinueColorOff);
	return font.layout(text, static_cast<float>(x), static_cast<float>(y), scale, scale, kFontAlignCenter, color);
}

// The line breaker [orig: Render_DrawWrappedTextBlockEx @ 0x580eb0 — the
// 32 / 13 / 10 tests @0x580f88, @0x580fdf, @0x581128]. The two kerning
// parameters offset the first (`use_kerning_start`) and the wrapped
// (`use_kerning_wrap`) lines by the font's tab-width field font+0x168; every
// loading-screen call site passes `use_kerning_start = 0` [orig: @0x521fee,
// @0x52201a, @0x52203c, @0x5220bd]. Render_LoadingScreen sets that field (the
// one GText_SetTabWidth @ 0x674710 also writes) to 120 on Arials18.fnt and
// 0 on Arial22.fnt [orig: CGameFont_SetTabWidth @0x6741c0, called @0x521f42 /
// @0x521fae], but only the large font's sites pass
// `use_kerning_wrap = 1` (the small font's pass 0 [orig: @0x5220bb,
// @0x5220f8]), so the offset folds out here. The 120 is the small font's tab
// stop (kLoadingFontSmallTabWidth), which the measure and the draw read.
std::vector<std::string> wrap_text_lines(const TextExtent &extent, const std::string &text,
		int max_width) {
	std::vector<std::string> out;
	if (!extent || text.empty() || max_width <= 0) return out;
	const std::size_t n = text.size();
	std::size_t line_start = 0;
	std::size_t i = 0;
	std::size_t last_space = 0; // absolute index; 0 means "no usable space on this line"
	int width = 0;              // loop-carried: a NUL/end step re-uses the last measure
	while (i <= n) {
		const unsigned char ch = i < n ? static_cast<unsigned char>(text[i]) : 0;
		const std::size_t ch_len = i < n ? 1 : 0;
		if (ch == kTextSpace) last_space = i;
		if (ch != 0) {
			// Measure the whole prefix up to and INCLUDING this character, the
			// way retail NUL-terminates one past it and re-measures the line.
			const std::size_t span = i - line_start + ch_len;
			width = span > 0 ? static_cast<int>(extent(text.substr(line_start, span))) : 0;
		}
		std::size_t break_at = i;
		std::size_t break_len = ch_len;
		if (width <= max_width) {
			// Only '\r' and the terminator break a line that still fits; a '\n'
			// is an ordinary glyph unless it trails a break.
			if (ch != kTextCarriageReturn && ch != 0) {
				i += ch_len;
				continue;
			}
		} else if (last_space != 0) {
			// Overflow with a space on this line: break at the LAST space.
			break_at = last_space;
			break_len = 1;
		}
		// Overflow with no space breaks at the current character, which is then
		// dropped with every other break character.
		out.push_back(text.substr(line_start, break_at - line_start));
		if (break_at >= n) return out;
		line_start = break_at + break_len;
		if (line_start < n && static_cast<unsigned char>(text[line_start]) == kTextLineFeed)
			++line_start; // a '\n' right after the break is swallowed
		i = line_start;
		last_space = 0;
		width = 0;
	}
	return out;
}

// The line placer, the drawing half of the same routine [orig:
// Render_DrawWrappedTextBlockEx @ 0x580eb0 — the alignment fold (4 =
// centred on left + width/2 @0x5810ab, 5 = right aligned on rect_right
// @0x581094, else left @0x58107f), the half advance `extent >> 1`
// @0x580f40/@0x581005 against the full advance @0x5810d7, the skip_lines jump
// @0x580ffb, and the ordering of the end-of-text return @0x581155 ahead of the
// bottom test @0x58111e; the line pitch is the 'I' character's own extent
// @0x580f2b].
TextBlock layout_text_block(const TextExtent &extent, int line_height, const std::string &text,
		int left, int top, int right, int bottom, TextBlockAlign align, int skip_lines) {
	TextBlock out;
	const int width = right - left;
	if (!extent || text.empty() || width <= 0) return out;
	const int line_h = line_height;
	const int half_h = line_h >> 1;
	int cursor = top;
	const std::vector<std::string> lines = wrap_text_lines(extent, text, width);
	for (std::size_t i = 0; i < lines.size(); ++i) {
		const std::string &line = lines[i];
		if (static_cast<int>(i) >= skip_lines) {
			const float line_w = line.empty() ? 0.0f : extent(line);
			if (line_w <= 0.0f) {
				// The drawer still walks a line of bytes that measures nothing (its tags);
				// an empty slice it skips [orig: @0x581003, @0x581033..0x581059].
				if (!line.empty())
					out.lines.push_back(TextBlockLine{line, static_cast<float>(left),
							static_cast<float>(cursor)});
				cursor += half_h;
			} else {
				float px = static_cast<float>(left);
				if (align == TextBlockAlign::kCenter)
					px = static_cast<float>(left + (width >> 1)) - line_w * 0.5f;
				else if (align == TextBlockAlign::kRight)
					px = static_cast<float>(right) - line_w;
				out.lines.push_back(TextBlockLine{line, px, static_cast<float>(cursor)});
				cursor += line_h;
			}
		}
		// The terminating break reports "consumed"; only a line that still has
		// text behind it can run the box out of room, and top == bottom disables
		// the vertical clip entirely.
		if (i + 1 == lines.size()) return out;
		if (bottom != top && cursor + line_h > bottom) {
			out.stopped_at = static_cast<int>(i) + 1;
			return out;
		}
	}
	return out;
}

LoadingSessionBoxes loading_session_boxes(int image_w, int image_h, int band_right) {
	LoadingSessionBoxes out;
	const LoadingTextBox band{kLoadingBandLeft, kLoadingBandTop, band_right - kLoadingBandLeft,
			kLoadingBandBottom, TextBlockAlign::kLeft, 0xFFFFFFFFu};
	out.title = band;
	out.mission = band;
	out.mission.align = TextBlockAlign::kCenter;
	out.game_type = band;
	out.game_type.align = TextBlockAlign::kRight;
	const int x = static_cast<int>(kLoadingMsgXFrac * image_w);
	const int width = static_cast<int>(kLoadingMsgRightFrac * image_w) - x;
	out.message_label = {x, static_cast<int>(kLoadingMsgLabelYFrac * image_h), width, image_h,
			TextBlockAlign::kLeft, 0xFF000000u | kLoadingMsgLabelRgb};
	out.message_body = {x, static_cast<int>(kLoadingMsgBodyYFrac * image_h), width, image_h,
			TextBlockAlign::kLeft, 0xFFFFFFFFu};
	return out;
}

void loading_blend_glyph(LoadingSurface &surface, const opennova::fnt::fnt_font_t &font,
		const opennova::fnt::fnt_glyph_t &glyph, int x, int y, uint32_t color) {
	if (surface.rgba == nullptr || font.pages == nullptr || glyph.page >= font.num_pages) return;
	const int width = static_cast<int>((glyph.uv.u1 - glyph.uv.u0) * 256.0f);
	const int height = static_cast<int>((glyph.uv.v1 - glyph.uv.v0) * 256.0f);
	const int col0 = -static_cast<int>(glyph.uv.u0 * -256.0f);
	const int row0 = -static_cast<int>(glyph.uv.v0 * -256.0f);
	if (height < 0) return;
	const uint32_t cr = (color >> 16) & 0xFFu;
	const uint32_t cg = (color >> 8) & 0xFFu;
	const uint32_t cb = color & 0xFFu;
	const uint8_t *page =
			font.pages + static_cast<size_t>(glyph.page) * opennova::fnt::FNT_TEXTURE_SIZE;
	const int page_w = static_cast<int>(opennova::fnt::FNT_TEXTURE_WIDTH);
	const int page_h = static_cast<int>(opennova::fnt::FNT_TEXTURE_HEIGHT);
	for (int r = 0; r <= height; ++r) {
		const int ty = row0 + r;
		const int dy = y + r;
		if (ty < 0 || ty >= page_h || dy < 0 || dy >= surface.height) continue;
		for (int c = 0; c < width; ++c) {
			const int tx = col0 + c;
			const int dx = x + c;
			if (tx < 0 || tx >= page_w || dx < 0 || dx >= surface.width) continue;
			const uint8_t *t = page + (static_cast<size_t>(ty) * page_w + tx) * 4;
			uint8_t *d = surface.rgba + (static_cast<size_t>(dy) * surface.width + dx) * 4;
			const uint32_t a = t[3];
			d[0] = static_cast<uint8_t>(((256u - a) * d[0] * 256u + a * cr * t[0]) >> 16);
			d[1] = static_cast<uint8_t>(((256u - a) * d[1] * 256u + a * cg * t[1]) >> 16);
			d[2] = static_cast<uint8_t>(((256u - a) * d[2] + a * ((cb * t[2]) >> 8)) >> 8);
		}
	}
}

void loading_render_text(LoadingSurface &surface, const opennova::fnt::fnt_font_t &font,
		int tab_width, const std::string &text, float x, float y, uint32_t flags,
		GameFontState &state) {
	// The lines CGameFont_ReadLine cuts: a '\n' ends one, a '\r' is dropped, and the end
	// closes a line that holds a byte.
	std::vector<std::string> lines;
	{
		std::string line;
		for (const char ch : text) {
			if (ch == '\n') {
				lines.push_back(line);
				line.clear();
			} else if (ch != '\r') {
				line.push_back(ch);
			}
		}
		if (!line.empty()) lines.push_back(line);
	}
	GameFont measurer;
	measurer.set_font(&font);
	const opennova::fnt::fnt_glyph_t &space = font.glyphs[0];
	const float line_step = (space.uv.v1 - space.uv.v0) * 256.0f;
	const float pad = static_cast<float>(font.glyph_spacing - 1);
	const GameFontState entry = state;
	for (uint32_t page = 0; page < font.num_pages; ++page) {
		GameFontState pass = entry;
		float line_y = y;
		for (const std::string &line : lines) {
			float cursor = x;
			if ((flags & 3u) != 0) {
				// The measure with no state: the font's own tab width.
				GameFontState fresh;
				fresh.tab_width = tab_width;
				int w = 0;
				int h = 0;
				measurer.measure(line.c_str(), 1.0f, 1.0f, &w, &h, &fresh);
				cursor = (flags & 1u) != 0 ? x - static_cast<float>(w) * 0.5f
											: x - static_cast<float>(w);
			}
			for (size_t i = 0; i < line.size(); ++i) {
				uint8_t byte = static_cast<uint8_t>(line[i]);
				if (byte == '\\') {
					++i;
					byte = i < line.size() ? static_cast<uint8_t>(line[i]) : 0;
				} else if (byte == '<') {
					if (!pass.tags_disabled) {
						// The parser moves the walk's own index: past a tag, and while
						// inert to where its scan stopped, the '<' then drawn.
						int idx = static_cast<int>(i);
						const bool ok = GameFont::parse_format_tag(line.c_str(), &idx, &pass);
						i = static_cast<size_t>(idx);
						if (ok) continue;
					}
				}
				// An escaped byte falls into the tab test too [orig: CGameFont_RenderText
				// @ 0x675e40 - the escape's `mov bl, [text+1]` @0x6760C7, then `cmp bl, 9` @0x6760D2].
				if (byte == '\t') {
					if (pass.tab_width > 0) {
						float stop = x;
						while (stop <= cursor) stop += static_cast<float>(pass.tab_width);
						cursor = stop;
						continue;
					}
					byte = ' ';
				}
				if (opennova::fnt::fnt_byte_is_nonprinting(byte)) continue;
				const opennova::fnt::fnt_glyph_t &glyph =
						font.glyphs[byte - opennova::fnt::FNT_FIRST_CHAR];
				const float next = std::floor(
						cursor + (glyph.uv.u1 - glyph.uv.u0) * 256.0f + pad + 0.5f);
				if (glyph.page == page) {
					const int gx = static_cast<int>(cursor);
					const int gy = static_cast<int>(line_y);
					loading_blend_glyph(surface, font, glyph, gx, gy, pass.color);
					if (pass.bold)
						loading_blend_glyph(surface, font, glyph, gx + 1, gy, pass.color);
					if (pass.underline && surface.rgba != nullptr) {
						const int w = static_cast<int>((glyph.uv.u1 - glyph.uv.u0) * 256.0f);
						const int row =
								gy - static_cast<int>((glyph.uv.v1 - glyph.uv.v0) * -256.0f);
						for (int c = 0; c < w && row >= 0 && row < surface.height; ++c) {
							const int px = gx + c;
							if (px < 0 || px >= surface.width) continue;
							uint8_t *d = surface.rgba +
									(static_cast<size_t>(row) * surface.width + px) * 4;
							d[0] = static_cast<uint8_t>(pass.color >> 16);
							d[1] = static_cast<uint8_t>(pass.color >> 8);
							d[2] = static_cast<uint8_t>(pass.color);
						}
					}
				}
				cursor = next;
			}
			line_y += line_step;
		}
		state = pass;
	}
}

void loading_draw_text_block(LoadingSurface &surface, const opennova::fnt::fnt_font_t &font,
		int tab_width, const std::string &text, const LoadingTextBox &box) {
	GameFont measurer;
	measurer.set_font(&font);
	const TextExtent extent = [&measurer, tab_width](const std::string &s) {
		GameFontState fresh;
		fresh.tab_width = tab_width;
		int w = 0;
		int h = 0;
		measurer.measure(s.c_str(), 1.0f, 1.0f, &w, &h, &fresh);
		return static_cast<float>(w);
	};
	const int pitch = static_cast<int>(measurer.char_height('I', 1.0f));
	const int right = box.x + box.width;
	const TextBlock block =
			layout_text_block(extent, pitch, text, box.x, box.y, right, box.bottom, box.align);
	GameFontState state;
	state.color = box.argb;
	state.original_color = box.argb;
	state.tab_width = tab_width;
	for (const TextBlockLine &line : block.lines) {
		float anchor = static_cast<float>(box.x);
		uint32_t flags = 0;
		if (extent(line.text) > 0.0f) {
			if (box.align == TextBlockAlign::kCenter) {
				anchor = static_cast<float>(box.x + (box.width >> 1));
				flags = 1;
			} else if (box.align == TextBlockAlign::kRight) {
				anchor = static_cast<float>(right);
				flags = 2;
			}
		}
		loading_render_text(surface, font, tab_width, line.text, anchor, line.y, flags, state);
	}
}

void composite_loading_session_text(LoadingSurface &surface,
		const opennova::fnt::fnt_font_t *large, const opennova::fnt::fnt_font_t *small,
		const std::string &title, const std::string &mission, const std::string &game_type,
		const std::string &label, const std::string &custom_text, int band_right) {
	if (surface.rgba == nullptr || surface.width <= 0 || surface.height <= 0) return;
	const LoadingSessionBoxes boxes =
			loading_session_boxes(surface.width, surface.height, band_right);
	if (large != nullptr) {
		loading_draw_text_block(surface, *large, kLoadingFontLargeTabWidth, title, boxes.title);
		loading_draw_text_block(surface, *large, kLoadingFontLargeTabWidth, mission, boxes.mission);
		loading_draw_text_block(surface, *large, kLoadingFontLargeTabWidth, game_type,
				boxes.game_type);
	}
	if (custom_text.empty() || small == nullptr) return;
	loading_draw_text_block(surface, *small, kLoadingFontSmallTabWidth,
			loading_message_label_text(label), boxes.message_label);
	loading_draw_text_block(surface, *small, kLoadingFontSmallTabWidth, custom_text,
			boxes.message_body);
}

LoadingBarRects loading_bar_rects(int surface_w, int surface_h, int displayed) {
	// The placement and the extent each through the integer scaler [orig:
	// LoadingScreen_UpdateAndPresent -- x, y stored @ 0x586c88 / @ 0x586c90 and scaled @ 0x586c98,
	// w, h stored @ 0x586c78 / @ 0x586c80 and scaled @ 0x586cac, each through
	// Viewport_ScaleToVirtualCoords @ 0x5d2b20].
	const int x = design_to_screen_x(kLoadingBarX, surface_w);
	const int y = design_to_screen_y(kLoadingBarY, surface_h);
	const int w = design_to_screen_x(kLoadingBarW, surface_w);
	const int h = design_to_screen_y(kLoadingBarH, surface_h);
	LoadingBarRects out;
	out.outer = {x, y, w + 6, h + 6};
	out.gray = {x + 1, y + 1, w + 4, h + 4};
	out.track = {x + 2, y + 2, w + 2, h + 2};
	const LoadingBarSpan span = loading_bar_fill_span(x, w, displayed);
	out.fill = {span.left, y + 3, span.right - span.left, h};
	return out;
}

} // namespace opennova::hud
