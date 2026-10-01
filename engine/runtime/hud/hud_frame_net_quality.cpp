// THE CONNECTION INDICATORS element: the quality icon, the T/R link-error
// icon and the NovaWorld N in the HUD's top-left corner, drawn from the
// g_NetQuality state (net_quality_indicators.h carries the state's witness).
// [orig: CNetQuality_DrawIndicators @0x4c3200]

#include <runtime/hud/hud_frame.h>

namespace opennova::hud {

namespace {

// One design value onto the surface, in integers: (v * dim + extent / 2) /
// extent, the divide truncating toward zero [orig: Viewport_ScaleToVirtualCoords
// @0x5d2b20]. The drawer scales a corner and a size separately and adds them.
int scale_virtual(int v, int surface, int extent) {
	return static_cast<int>((static_cast<int64_t>(v) * surface + extent / 2) / extent);
}

// Every quad is one band of a vertical atlas, textured with its own colours
// and faded by the diffuse alpha: the drawer passes `alpha << 24` over the
// icons' shader mode 0x451 (blend SRCALPHA/INVSRCALPHA, colour
// SELECTARG1(TEXTURE), alpha MODULATE(TEXTURE, DIFFUSE)), so the zero RGB
// never reaches the screen; the white modulate here is that colour stage
// [orig: `shl eax, 18h` @0x4c32ce / @0x4c32e9 / @0x4c33ba / @0x4c34c1; the
//  0x300451 loads @0x4c2d53 / @0x4c2dcb / @0x4c2e41; Render_DrawTiledTextureStrip
//  @0x67aed0 selects the band `frame * rowH` @0x67af40].
uint32_t indicator_color(int32_t alpha) {
	return (static_cast<uint32_t>(alpha) << 24) | 0x00FFFFFFu;
}

} // namespace

void HudFrameCompiler::element_net_quality_indicators(const HudFrameState &state, float w,
		float h) {
	// The scene frame draws them last, after the overlay panels, only at the
	// full HUD level [orig: `cmp g_HUDDetailLevel, 0` @0x5cae4d, push 0 @0x5cae56].
	if (state.hud_detail_level != 0) return;
	emit_net_quality_indicators(state.net_quality, state.overlay_master, false, w, h);
}

void HudFrameCompiler::emit_net_quality_indicators(const HudNetQualityState &net,
		uint32_t overlay_master, bool force_default_pos, float w, float h) {
	// In a session, and not under /NOHUD [orig: `cmp is_in_session, 0`
	// @0x4c3210, `cmp dword_840B18, 0` @0x4c3224]. The debug-visible arm
	// (dword_24C1930 & 0x8000000 @0x4c320a) is dead: nothing sets that bit.
	if (!net.in_session || overlay_master == 0u) return;
	const NetQualityIndicators &q = net.indicators;
	const size_t quads_before = draw_list_.quads.size();
	const int sw = static_cast<int>(w);
	const int sh = static_cast<int>(h);
	// The status page's call pins the reset corners (the same literals), the
	// HUD's reads the layout [orig: `cmp forceDefaultPos, 0` @0x4c3246 /
	// @0x4c3317 / @0x4c3445].
	const std::array<int, 6> &pos =
			force_default_pos ? kNetIndicatorResetPos : layout_.net_indicator_pos;
	// One band of `bands` rows at a design corner and a design size.
	const auto emit_band = [&](int x, int y, int size_w, int size_h, int32_t texture,
			int bands, int band, int32_t alpha) {
		const int x0 = scale_virtual(x, sw, 1024);
		const int y0 = scale_virtual(y, sh, 768);
		const int iw = scale_virtual(size_w, sw, 1024);
		const int ih = scale_virtual(size_h, sh, 768);
		const float step = 1.0f / static_cast<float>(bands);
		const float v0 = static_cast<float>(band) * step;
		emit_rect_uv(static_cast<float>(x0), static_cast<float>(y0),
				static_cast<float>(x0 + iw), static_cast<float>(y0 + ih), 0.0f, v0, 1.0f,
				v0 + step, indicator_color(alpha), texture);
	};

	// The quality icon: neticon2, 12x12 at the first corner (4, 4 on the
	// status page) — the red band at the level-3 alpha, then the yellow band at
	// the level-2 alpha [orig: the texture test @0x4c3239, the corner
	// @0x4c324d / @0x4c325d, the size @0x4c323b, band 2 over +0x10
	// @0x4c32b6..0x4c32d8, band 1 over +0x0C @0x4c32e0..0x4c32f3].
	if (layout_.net_icon_texture_valid) {
		const int x = pos[0];
		const int y = pos[1];
		if (q.level3_alpha != 0) emit_band(x, y, 12, 12, kHudTexNetIcon, 4, 2, q.level3_alpha);
		if (q.level2_alpha != 0) emit_band(x, y, 12, 12, kHudTexNetIcon, 4, 1, q.level2_alpha);
	}

	// The T/R link-error icon: neticon1, 24x12 at the second corner (20, 4),
	// while its alpha holds; the error band shows only while 62 or more
	// frames remain and the blink is off, else band 0 (both green)
	// [orig: `cmp [esi+24h], 0` @0x4c32fb, the texture test @0x4c330f, the
	// corner @0x4c331e / @0x4c332e, the size @0x4c3340, the band
	// @0x4c3380..0x4c3397, the draw @0x4c33cc].
	if (q.link_error_alpha != 0 && layout_.net_link_icon_texture_valid) {
		const int x = pos[2];
		const int y = pos[3];
		int band = 0;
		if (q.link_error_countdown >= kNetLinkErrorBandFrames && q.link_error_blink == 0)
			band = static_cast<int>(q.link_error_bits & 3u);
		emit_band(x, y, 24, 12, kHudTexNetLinkIcon, 4, band, q.link_error_alpha);
	}

	// The NovaWorld N: neticon3, 8x8 at the third corner (52, 4), on a
	// NovaWorld session whose NWU session is in use; the red band while its
	// alpha is full and the blink is on. Without the neticon3 atlas the quality
	// icon's red band stands in [orig: the gate @0x4c33d4..0x4c33f0, the atlas
	// @0x4c33f6 and the fallback @0x4c3405..0x4c340b, the band @0x4c3412..0x4c341e,
	// the alpha gate @0x4c3437, the corner @0x4c344c / @0x4c345c, the size
	// @0x4c346c, the draw @0x4c34d3].
	const bool own_atlas = layout_.net_novaworld_icon_texture_valid;
	if (net.novaworld_icon && (own_atlas || layout_.net_icon_texture_valid) &&
			q.novaworld_alpha != 0) {
		const int x = pos[4];
		const int y = pos[5];
		if (own_atlas) {
			const int band = q.novaworld_alpha == 255 && q.novaworld_blink != 0 ? 1 : 0;
			emit_band(x, y, 8, 8, kHudTexNetNovaWorldIcon, 2, band, q.novaworld_alpha);
		} else {
			emit_band(x, y, 8, 8, kHudTexNetIcon, 4, 2, q.novaworld_alpha);
		}
	}
	if (draw_list_.quads.size() != quads_before) ++draw_list_.elements_drawn;
}

} // namespace opennova::hud
