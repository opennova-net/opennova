// THE TIP PANEL ("MrClippy"): the box sized to the measured text, the icon,
// the Tips header and the expanded body, all fading with the countdown
// (tip_system.h carries the state and its producers).
// [orig: CTipSystem_Draw @0x5b6d60; the box style CTipSystem_Init @0x5b6970 ->
//  BoxTexture_LoadAndSetupUVRegions("border3.tga", no secondary, arg 1)
//  @0x5b698c; Render_HUDBoxOverlay @0x56b700]

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/tip_system.h>

#include <algorithm>
#include <array>

namespace opennova::hud {

namespace {

// [orig: Viewport_ScaleToVirtualCoords @0x5d2b20 / Viewport_ScaleRectByDimensions
// @0x5d2ba0 — (v * dim + extent/2) / extent]
int to_screen(int v, int surface, int extent) {
	return static_cast<int>((static_cast<int64_t>(v) * surface + extent / 2) / extent);
}

// [orig: Viewport_ScreenToVirtual @0x5d2c70 — (dim/2 + v * extent) / dim]
int to_virtual(int v, int surface, int extent) {
	if (surface <= 0) return 0;
	return static_cast<int>((static_cast<int64_t>(surface >> 1) + static_cast<int64_t>(v) * extent) /
			surface);
}

// A MODULATE2X material's diffuse folded into the quad colour: each channel
// doubled, clamped (the icon and the box ride flag word 0x651, colour op
// MOD2X(TEXTURE, DIFFUSE), alpha op MODULATE(TEXTURE, DIFFUSE)).
uint32_t fold_modulate2x(uint32_t argb) {
	const auto doubled = [](uint32_t c) { return std::min(c * 2u, 255u); };
	return (argb & 0xFF000000u) | (doubled((argb >> 16) & 0xFFu) << 16) |
			(doubled((argb >> 8) & 0xFFu) << 8) | doubled(argb & 0xFFu);
}

} // namespace

void HudFrameCompiler::element_tip(const HudFrameState &state, bool alternate, float w,
		float h) {
	// [orig: CTipSystem_Draw @0x5b6d60 — countdown > 0 @0x5b6d6e]
	if (state.tip_countdown <= 0) return;
	TipText keys;
	if (!tip_text_keys(state.tip, keys)) return;
	const int sw = static_cast<int>(w);
	const int sh = static_cast<int>(h);
	// [orig: @0x5b6d76..0x5b6db1 — alpha = min(4 * countdown, 255); the header
	//  orange 0xEEA400, the icon grey 0x808080, the body white]
	const uint32_t a24 = static_cast<uint32_t>(tip_alpha(state.tip_countdown)) << 24;
	const uint32_t orange = a24 | 0xEEA400u;
	const uint32_t grey = a24 | 0x808080u;
	const uint32_t white = a24 | 0xFFFFFFu;

	// The large label slot, its tab stop 192 design px on the surface: the
	// body lays its key columns out on it [orig: Viewport_ScaleToVirtualCoords
	// (192) @0x5b6eaa, sub_6741C0 (the font's tab width, this+0x168)
	// @0x5b6ebd].
	const GameFont &font = label_font_large_.font() != nullptr ? label_font_large_ : font_;
	const float scale = label_font_large_.font() != nullptr ? label_large_scale_ : hud_font_scale_;
	if (font.font() == nullptr) return;
	GameFontState tabs;
	tabs.tab_width = std::max(0, to_screen(192, sw, 1024));

	// The measures in surface pixels, back to design space; the width is the
	// body's or the header's plus 52, whichever is wider, the height the
	// body's with a 32 floor [orig: HUD_MeasureTextWH @0x5b6ed2 / @0x5b6ee7,
	// Viewport_ScreenToVirtual @0x5b6efb / @0x5b6f0f, the +52 compare
	// @0x5b6f1c..0x5b6f2c, the floor @0x5b6f30..0x5b6f3b].
	int body_w = 0, body_h = 0, head_w = 0, head_h = 0;
	font.measure(state.tip_body.c_str(), scale, scale, &body_w, &body_h, &tabs);
	font.measure(state.tip_header.c_str(), scale, scale, &head_w, &head_h, &tabs);
	body_w = to_virtual(body_w, sw, 1024);
	body_h = to_virtual(body_h, sh, 768);
	head_w = to_virtual(head_w, sw, 1024);
	const int max_w = std::max(body_w, head_w + 52);
	const int text_h = std::max(body_h, 32);

	// The panel rect from the hudpos slot: x, y, then the width pad and the
	// height pad past the text [orig: @0x5b6f44..0x5b6f85 — alternate
	// +0x50..+0x5C, normal +0x40..+0x4C; the rect +0xC..+0x18].
	const std::array<int, 4> &slot = alternate ? layout_.tip_alternate : layout_.tip_normal;
	const int x0 = slot[0];
	const int y0 = slot[1];
	const int x1 = max_w + slot[0] + slot[2];
	const int y1 = text_h + slot[1] + slot[3];

	// The box: the rect scaled with rounding, the box scale W/1024, the
	// diffuse alpha<<24 | 0x7F7F7F [orig: Viewport_ScaleRectByDimensions
	// @0x5b6fae, Viewport_ScaleOptionalXY(1.0) @0x5b6fbf, Render_HUDBoxOverlay
	// @0x5b6fdf; the diffuse @0x56b70e..0x56b713]. The MrClippy style takes
	// the rec+0x3C != 0 arm: ONE fill quad of atlas cell (1,1) inset a cell
	// on every side, then the eight pieces [orig: @0x56b72a..0x56b7aa — the
	// region rec+0x160, the insets rec+0x40/+0x44 and rec+0xE8/+0xEC].
	if (layout_.tip_box_texture_valid && layout_.tip_box_tex_w > 0) {
		const float bx0 = static_cast<float>(to_screen(x0, sw, 1024));
		const float by0 = static_cast<float>(to_screen(y0, sh, 768));
		const float bx1 = static_cast<float>(to_screen(x1, sw, 1024));
		const float by1 = static_cast<float>(to_screen(y1, sh, 768));
		const float box_scale = static_cast<float>(static_cast<double>(sw) * 1.0 * 0.0009765625);
		const float cell = static_cast<float>(layout_.tip_box_tex_w >> 2) * box_scale;
		const uint32_t box_color = fold_modulate2x(a24 | 0x7F7F7Fu);
		emit_stdbox_piece(bx0 + cell, by0 + cell, bx1 - cell, by1 - cell, 1, 1, false, box_color,
				kHudTexTipBox);
		emit_box_pieces(bx0, by0, bx1, by1, cell, cell, box_color, 0.0f, kHudTexTipBox);
	}

	// The icon: 48 design px square at (x + 28, y + 32), the whole texture,
	// the grey diffuse under the icon's MODULATE2X material [orig:
	// @0x5b6fe6..0x5b7088 — Viewport_ScaleRectToScreen, GfxShader_ApplyPassChecked
	// (k_tip +0x24 / g_tip +0x28), CGfxDevice_SetQuadDiffuse(grey)].
	const bool icon_valid = keys.gameplay ? layout_.tip_gameplay_texture_valid
										  : layout_.tip_keyboard_texture_valid;
	if (icon_valid) {
		const float ix = static_cast<float>(x0) + 28.0f;
		const float iy = static_cast<float>(y0) + 32.0f;
		const double kx = 0.0009765625, ky = 0.0013020834;
		emit_rect_uv(static_cast<float>(static_cast<double>(sw) * ix * kx),
				static_cast<float>(static_cast<double>(sh) * iy * ky),
				static_cast<float>(kx * (static_cast<double>(sw) * (ix + 48.0f))),
				static_cast<float>(ky * (static_cast<double>(sh) * (iy + 48.0f))), 0.0f, 0.0f,
				1.0f, 1.0f, fold_modulate2x(grey),
				keys.gameplay ? kHudTexTipGameplay : kHudTexTipKeyboard);
	}

	// The header at (x + 80, y + 42) and the body at (x + 28, y + 92), both
	// left-aligned through the alpha-keeping half-bright drawer
	// [orig: Render_DrawTextScaled @0x5b70af / @0x5b710d -> mode 0
	//  HUD_DrawTextLeft_HalfBrightKeepAlpha (ex sub_580560) @0x580560:
	//  (color & 0xFF000000) + ((color >> 1) & 0x7F7F7F)].
	const auto emit_line = [&](const std::string &text, int dx, int dy, uint32_t argb) {
		if (text.empty()) return;
		GameFontState st = tabs;
		const GameFontRun run = font.layout(text.c_str(),
				static_cast<float>(to_screen(dx, sw, 1024)),
				static_cast<float>(to_screen(dy, sh, 768)), scale, scale, 0u,
				half_bright_keep_alpha(argb), &st);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
		draw_list_.underlines.insert(draw_list_.underlines.end(), run.underlines.begin(),
				run.underlines.end());
	};
	emit_line(state.tip_header, x0 + 80, y0 + 42, orange);
	emit_line(state.tip_body, x0 + 28, y0 + 92, white);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
