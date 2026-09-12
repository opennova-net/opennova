// The retail game-font text engine — structural translation of CGameFont's
// measurer and drawer over engine/formats/fnt. Witness: docs/fonts/fnt-re.md.
// [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0;
//  GText_ParseFormatTag @ 0x674200; CGameFont_ReadLine @ 0x676480]

#include <runtime/hud/game_font.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace opennova::fnt;

namespace opennova::hud {

namespace {

constexpr int kLineBufferMax = 255; // [orig: 256-byte line buffer @ 0x6752c0]

// The drawer's half-texel bottom-V bias [orig: +0.001953125 @ 0x6752c0].
constexpr float kBottomVBias = 0.5f / 256.0f;

bool byte_is_skipped(uint8_t byte) {
	// Controls below space plus the three retail control bytes; the range and
	// its witness live at fnt.h's fnt_byte_is_nonprinting.
	return fnt_byte_is_nonprinting(byte) != 0;
}

uint32_t double_channel_clamped(uint32_t argb) {
	// The underline color fold: 2x per channel clamped, alpha kept
	// [orig: @ 0x6752c0 the per-byte >= 0x80 ? 0xFF : 2x ladder].
	uint32_t out = argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t c = (argb >> shift) & 0xFFu;
		out |= (c >= 0x80u ? 0xFFu : c * 2u) << shift;
	}
	return out;
}

} // namespace

const fnt_glyph_t *GameFont::glyph_for_byte(uint8_t byte) const {
	if (font_ == nullptr || byte < FNT_FIRST_CHAR) {
		return nullptr;
	}
	return &font_->glyphs[byte - FNT_FIRST_CHAR];
}

float GameFont::char_height(uint8_t byte, float scale_y) const {
	// One glyph's v-extent in pixels [orig: GameFont_MeasureCharHeight
	// @ 0x580a80 — CGameFont_GetCharMetrics scaled by the fontDesc scale;
	// the friendly-tag drawer measures '0' @ 0x5a3a36].
	const fnt_glyph_t *glyph = glyph_for_byte(byte);
	if (glyph == nullptr) {
		return 0.0f;
	}
	return (glyph->uv.v1 - glyph->uv.v0) * 256.0f *
			fnt_design_scale(font_->design_width) * scale_y;
}

int GameFont::char_width(uint8_t byte, float scale_x, int tab_width) const {
	// [orig: CGameFont_GetCharExtent @0x674dc0] in the witnessed order: a tab
	// is the font's tab width when it carries one, else the SPACE glyph
	// (@0x674dd8); any other byte below 0x20 is 0 (@0x674e55); otherwise
	// fld u1; fsub u0; fmul 256.0; fiadd glyph_spacing; fsub 1.0;
	// fmul (800 / design_width); fadd 0.5; floor (@0x674de4..0x674e25) — the
	// (glyph_spacing - 1) pad INCLUDED, unlike the measurer's final width.
	if (font_ == nullptr) {
		return 0;
	}
	if (byte == '\t') {
		if (tab_width != 0) {
			return tab_width;
		}
		byte = ' ';
	}
	if (byte < 0x20) {
		return 0;
	}
	const fnt_glyph_t *glyph = glyph_for_byte(byte);
	if (glyph == nullptr) {
		return 0;
	}
	const float pad = static_cast<float>(font_->glyph_spacing - 1);
	return static_cast<int>(std::floor(
			((glyph->uv.u1 - glyph->uv.u0) * 256.0f + pad) *
					fnt_design_scale(font_->design_width) * scale_x +
			0.5f));
}

float GameFont::line_height(float scale_y) const {
	if (font_ == nullptr) {
		return 0.0f;
	}
	// The SPACE glyph's v-extent [orig: (this[95] - this[93]) * 256 * scale
	// @ 0x6752c0].
	const fnt_glyph_t *space = &font_->glyphs[0];
	return (space->uv.v1 - space->uv.v0) * 256.0f *
			fnt_design_scale(font_->design_width) * scale_y;
}

bool GameFont::parse_format_tag(const char *text, int *index,
		GameFontState *state) {
	// [orig: GText_ParseFormatTag @ 0x674200] — mutations land only on a
	// well-formed (terminated) tag, exactly like the original's local-copy
	// commit; the tags_disabled byte freezes everything but the scan.
	GameFontState local = *state;
	int i = *index;
	if (text[i] != '<') {
		return true;
	}
	++i;
	bool off = false;
	bool ok = false;
	for (;; ++i) {
		const uint8_t c = static_cast<uint8_t>(text[i]);
		if (c == 0) {
			break;
		}
		if (c == '>') {
			ok = true;
			break;
		}
		switch (c) {
			case '-':
				off = true;
				break;
			case 'B':
			case 'b':
				local.bold = !off;
				break;
			case 'I':
			case 'i':
				local.italic = !off;
				break;
			case 'U':
			case 'u':
				local.underline = !off;
				break;
			case 'C':
			case 'c': {
				uint32_t value = 0;
				for (++i;; ++i) {
					const uint8_t h = static_cast<uint8_t>(text[i]);
					if (h == 0 || h == '>') {
						break;
					}
					if (h >= '0' && h <= '9') {
						value = value * 16 + (h - '0');
					} else if (h >= 'A' && h <= 'F') {
						value = value * 16 + (h - 'A' + 10);
					} else if (h >= 'a' && h <= 'f') {
						value = value * 16 + (h - 'a' + 10);
					} else if (h == 'O' || h == 'o') {
						value = local.original_color;
					} else if (h == 'H' || h == 'h') {
						// Half-bright: 3/4 c + 0x40 per channel
						// [orig: 3 * (((c >> 2) & 0x3F3F3F) + 0x156B40)].
						value = 3u * (((local.color_xor ^ 0u) >> 2 & 0x3F3F3Fu) +
								0x156B40u);
					}
				}
				if (value != 0) {
					// The original XOR-folds the new color into the low 24
					// bits of the live color slot.
					local.color_xor ^= (value ^ local.color_xor) & 0xFFFFFFu;
				}
				if (text[i] == 0) {
					goto done;
				}
				ok = true;
				goto done;
			}
			case 'T':
			case 't': {
				int value = 0;
				for (++i;; ++i) {
					const uint8_t d = static_cast<uint8_t>(text[i]);
					if (d == 0 || d == '>') {
						break;
					}
					if (d >= '0' && d <= '9') {
						value = value * 10 + (d - '0');
					}
				}
				if (value != 0) {
					local.timer = value;
				}
				if (text[i] == 0) {
					goto done;
				}
				ok = true;
				goto done;
			}
			default:
				break;
		}
	}
done:
	if (state->tags_disabled) {
		*index = i;
	} else if (ok) {
		*index = i;
		*state = local;
	}
	return ok;
}

void GameFont::measure(const char *text, float scale_x, float scale_y,
		int *out_w, int *out_h, GameFontState *p_state) const {
	// [orig: CGameFont_MeasureText @ 0x674e70]
	if (out_w != nullptr) {
		*out_w = 0;
	}
	if (out_h != nullptr) {
		*out_h = 0;
	}
	if (font_ == nullptr || text == nullptr) {
		return;
	}
	GameFontState state = p_state != nullptr ? *p_state : GameFontState{};
	const float design = fnt_design_scale(font_->design_width);
	const float scaled_x = design * scale_x;
	const float pad = static_cast<float>(font_->glyph_spacing - 1);
	const int tab = state.tab_width > 0 ? state.tab_width : 0;
	const float line_h = (font_->glyphs[0].uv.v1 - font_->glyphs[0].uv.v0) *
			256.0f; // design-space; the final height multiplies scale in

	float cursor = 0.0f;
	float max_w = 0.0f;
	float height = line_h;
	for (int i = 0; text[i] != 0; ++i) {
		uint8_t byte = static_cast<uint8_t>(text[i]);
		if (byte == '\\') {
			byte = static_cast<uint8_t>(text[++i]);
			if (byte == 0) {
				break;
			}
		} else if (byte == '<') {
			if (!state.tags_disabled) {
				int idx = i;
				if (parse_format_tag(text, &idx, &state)) {
					i = idx;
					continue;
				}
			}
			// An unterminated/disabled tag measures '<' as a glyph.
		} else if (byte == '\n') {
			max_w = std::max(max_w, cursor);
			cursor = 0.0f;
			height += line_h;
			continue;
		} else if (byte == '\t') {
			if (tab > 0) {
				float stop = 0.0f;
				while (stop <= cursor) {
					stop += static_cast<float>(tab);
				}
				cursor = stop;
				continue;
			}
			byte = ' ';
		} else if (byte_is_skipped(byte)) {
			continue;
		}
		const fnt_glyph_t *glyph = glyph_for_byte(byte);
		if (glyph == nullptr) {
			continue;
		}
		cursor = std::floor(cursor +
				(glyph->uv.u1 - glyph->uv.u0) * 256.0f * scaled_x +
				pad * scaled_x + 0.5f);
		max_w = std::max(max_w, cursor);
	}
	max_w = std::max(max_w, cursor);
	if (max_w > 0.0f && out_w != nullptr) {
		// The final width strips the trailing spacing pad [orig: the
		// (spacing - 1) * scale subtraction at the tail].
		*out_w = static_cast<int>(max_w - pad * scale_x);
	}
	if (out_h != nullptr) {
		*out_h = static_cast<int>(height * design * scale_y);
	}
	if (p_state != nullptr) {
		*p_state = state;
	}
}

GameFontRun GameFont::layout(const char *text, float x, float y,
		float scale_x, float scale_y, uint32_t flags, uint32_t color,
		GameFontState *p_state) const {
	// [orig: CGameFont_DrawText @ 0x6752c0] — the same walk once per texture
	// page; the layout keeps the drawer's exact cursor arithmetic and vertex
	// offsets so quads land where retail rasterized.
	GameFontRun run;
	if (font_ == nullptr || text == nullptr) {
		return run;
	}
	GameFontState base_state = p_state != nullptr ? *p_state : GameFontState{};
	if (base_state.tab_width == 0) {
		base_state.tab_width = 0; // retail default rides this+360; state 0 = font tab
	}
	if ((flags & kFontStyleBold) != 0) {
		base_state.bold = true;
	}
	if ((flags & kFontStyleItalic) != 0) {
		base_state.italic = true;
	}
	if ((flags & kFontStyleUnderline) != 0) {
		base_state.underline = true;
	}
	if ((flags & kFontTagsDisabled) != 0) {
		base_state.tags_disabled = true;
	}
	base_state.original_color = color & 0xFFFFFFu;

	const float design = fnt_design_scale(font_->design_width);
	const float scaled_x = design * scale_x;
	const float scaled_y = design * scale_y;
	const float pad = static_cast<float>(font_->glyph_spacing - 1);
	const float line_h = (font_->glyphs[0].uv.v1 - font_->glyphs[0].uv.v0) *
			256.0f * scaled_y;

	GameFontState final_state = base_state;
	int measured_w = 0;
	int measured_h = 0;
	measure(text, scale_x, scale_y, &measured_w, &measured_h);
	run.width = static_cast<float>(measured_w);
	run.height = static_cast<float>(measured_h);

	for (uint32_t page = 0; page < font_->num_pages; ++page) {
		GameFontState state = base_state;
		float line_y = y;
		int i = 0;
		while (text[i] != 0) {
			// One line into the retail-sized buffer
			// [orig: CGameFont_ReadLine @ 0x676480].
			char line[kLineBufferMax + 1];
			int len = 0;
			while (text[i] != 0 && text[i] != '\n' && len < kLineBufferMax) {
				line[len++] = text[i++];
			}
			line[len] = 0;
			if (text[i] == '\n') {
				++i;
			}

			float cursor = x;
			if ((flags & kFontAlignCenter) != 0) {
				int w = 0;
				int h = 0;
				measure(line, 1.0f, 1.0f, &w, &h);
				cursor = x - static_cast<float>(w) * scaled_x * 0.5f;
			} else if ((flags & kFontAlignRight) != 0) {
				int w = 0;
				int h = 0;
				measure(line, 1.0f, 1.0f, &w, &h);
				cursor = x - static_cast<float>(w) * scaled_x;
			}

			for (int j = 0; line[j] != 0; ++j) {
				uint8_t byte = static_cast<uint8_t>(line[j]);
				if (byte == '\\') {
					byte = static_cast<uint8_t>(line[++j]);
					if (byte == 0) {
						break;
					}
				} else if (byte == '<') {
					if (!state.tags_disabled) {
						int idx = j;
						if (parse_format_tag(line, &idx, &state)) {
							j = idx;
							continue;
						}
					}
				} else if (byte == '\t') {
					const int tab = state.tab_width;
					if (tab > 0) {
						float stop = x;
						while (stop <= cursor) {
							stop += static_cast<float>(tab);
						}
						cursor = stop;
						continue;
					}
					byte = ' ';
				} else if (byte_is_skipped(byte)) {
					continue;
				}
				const fnt_glyph_t *glyph = glyph_for_byte(byte);
				if (glyph == nullptr) {
					continue;
				}
				const float glyph_w =
						(glyph->uv.u1 - glyph->uv.u0) * 256.0f * scaled_x;
				const float glyph_h =
						(glyph->uv.v1 - glyph->uv.v0) * 256.0f * scaled_y;
				const float next = std::floor(cursor + glyph_w +
						pad * scaled_x + 0.5f);
				if (glyph->page == page) {
					const uint32_t draw_color =
							(color & 0xFF000000u) |
							((color ^ state.color_xor) & 0xFFFFFFu);
					// Italic shears the TOP edge by h/8; bold double-strikes
					// at (+1, -1) [orig: the passArray skew + the second
					// strike pass].
					const float skew = state.italic ? glyph_h * 0.125f : 0.0f;
					const int strikes = state.bold ? 2 : 1;
					// Bold double-strikes at +1 x (same y); each strike's
					// underline rides one pixel higher [orig: the strike loop's
					// cursor+1 / underline-y-1 stepping].
					for (int strike = 0; strike < strikes; ++strike) {
						const float sx = cursor + static_cast<float>(strike);
						GameFontQuad quad;
						quad.page = page + page_base_;
						quad.x_top_left = sx + skew - 0.5f;
						quad.x_top_right = sx + glyph_w + skew - 0.5f;
						quad.x_bottom_left = sx - skew - 0.5f;
						quad.x_bottom_right = sx + glyph_w - skew - 0.5f;
						quad.y_top = line_y - 0.5f;
						quad.y_bottom = line_y + glyph_h - 0.5f;
						quad.u0 = glyph->uv.u0;
						quad.v0 = glyph->uv.v0;
						quad.u1 = glyph->uv.u1;
						quad.v1 = glyph->uv.v1 + kBottomVBias;
						quad.color = draw_color;
						run.quads.push_back(quad);
						if (state.underline) {
							GameFontUnderline seg;
							seg.x0 = sx - 0.5f;
							seg.x1 = sx + glyph_w - 0.5f;
							seg.y = line_y + glyph_h - 1.5f -
									static_cast<float>(strike);
							seg.color = double_channel_clamped(draw_color);
							run.underlines.push_back(seg);
						}
					}
				}
				cursor = next;
			}
			line_y += line_h;
		}
		final_state = state;
	}
	if (p_state != nullptr) {
		*p_state = final_state;
	}
	return run;
}

} // namespace opennova::hud
