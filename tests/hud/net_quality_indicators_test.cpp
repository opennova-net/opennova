// The connection indicators (D-HUD-38): the g_NetQuality display state's
// frame step, flags, level and reset, and the three icons' draw — gates,
// corners, sizes, bands, alphas, the status page's reset corners and the
// NovaWorld fallback.
// [orig: CNetQuality_SetLevel @0x4c3060; CNetQuality_UpdateIndicators
//  @0x4c3090; CNetQuality_DrawIndicators @0x4c3200; CNetQuality_SetLinkErrorFlag
//  @0x4c34f0; CNetQuality_Reset @0x4c58c0]
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/net_quality_indicators.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// [orig: @0x4c3060] the 0..4 clamp and the change byte.
void test_set_level() {
	NetQualityIndicators q;
	CHECK(net_quality_set_level(q, 3) == 3);
	CHECK(q.level == 3 && q.level_changed);
	q.level_changed = false;
	CHECK(net_quality_set_level(q, 3) == 3);
	CHECK(!q.level_changed);
	CHECK(net_quality_set_level(q, 9) == 4);
	CHECK(net_quality_set_level(q, -2) == 0);
	CHECK(q.level == 0);
}

// [orig: @0x4c309e..0x4c3143] the selected channel climbs 4 a frame to 255;
// the others fall 4 a frame to 0; levels 0 and 4 select none.
void test_level_channels() {
	NetQualityIndicators q;
	const NovaWorldLinkFacts nw;
	net_quality_set_level(q, 3);
	for (int i = 0; i < 10; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.level3_alpha == 40);
	CHECK(q.level2_alpha == 0 && q.level1_alpha == 0);
	for (int i = 0; i < 60; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.level3_alpha == 255);
	CHECK(q.ramp_target == 255);
	net_quality_set_level(q, 2);
	net_quality_update_indicators(q, nw);
	CHECK(q.level3_alpha == 251 && q.level2_alpha == 4);
	net_quality_set_level(q, 1);
	for (int i = 0; i < 64; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.level1_alpha == 255);
	CHECK(q.level3_alpha == 0 && q.level2_alpha == 0);
	net_quality_set_level(q, 4);
	net_quality_update_indicators(q, nw);
	CHECK(q.level1_alpha == 251);
	// 1 at the bottom snaps to 0, not -3.
	q.level1_alpha = 3;
	net_quality_update_indicators(q, nw);
	CHECK(q.level1_alpha == 0);
}

// [orig: @0x4c34f0] flags 1..3 OR in and restart 155 frames once the clock
// has reached the cooldown; 4 clears and holds new flags off 10 s.
void test_flags() {
	NetQualityIndicators q;
	net_quality_reset(q, 1000);
	CHECK(q.flag_cooldown_until_ms == 1000);
	net_quality_set_flag(q, kNetLinkErrorOutgoing, 999);
	CHECK(q.link_error_bits == 0 && q.link_error_countdown == 0);
	net_quality_set_flag(q, kNetLinkErrorOutgoing, 1000);
	CHECK(q.link_error_bits == 1 && q.link_error_countdown == kNetLinkErrorFrames);
	q.link_error_countdown = 40;
	net_quality_set_flag(q, kNetLinkErrorIncoming, 1500);
	CHECK(q.link_error_bits == 3 && q.link_error_countdown == 155);
	q.link_error_alpha = 200;
	q.link_error_blink = 1;
	net_quality_set_flag(q, kNetLinkErrorClear, 2000);
	CHECK(q.link_error_bits == 0 && q.link_error_countdown == 0 && q.link_error_alpha == 0);
	CHECK(q.flag_cooldown_until_ms == 12000);
	CHECK(q.link_error_blink == 1); // the blink pair is kept
	net_quality_set_flag(q, kNetLinkErrorIncoming, 11999);
	CHECK(q.link_error_bits == 0);
	net_quality_set_flag(q, kNetLinkErrorIncoming, 12000);
	CHECK(q.link_error_bits == 2);
	net_quality_set_flag(q, 5, 20000); // not a flag: nothing
	CHECK(q.link_error_bits == 2 && q.flag_cooldown_until_ms == 12000);
}

// [orig: @0x4c3146..0x4c31c1] the link-error icon's countdown, alpha and blink.
void test_link_error_frames() {
	NetQualityIndicators q;
	const NovaWorldLinkFacts nw;
	net_quality_set_flag(q, kNetLinkErrorIncoming, 0);
	net_quality_update_indicators(q, nw);
	CHECK(q.link_error_countdown == 154 && q.link_error_alpha == 64);
	// The first step reloads the blink timer and flips the blink on.
	CHECK(q.link_error_blink == 1 && q.link_error_blink_timer == 10);
	for (int i = 0; i < 3; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.link_error_alpha == 255 && q.link_error_blink_timer == 7);
	// The timer runs down to 0; the next frame reloads it and flips the blink.
	for (int i = 0; i < 7; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.link_error_blink == 1 && q.link_error_blink_timer == 0);
	net_quality_update_indicators(q, nw);
	CHECK(q.link_error_blink == 0 && q.link_error_blink_timer == 10);
	// Down to 32 remaining the alpha holds; at 31 and below it falls 10.
	while (q.link_error_countdown > 32) net_quality_update_indicators(q, nw);
	CHECK(q.link_error_alpha == 255);
	net_quality_update_indicators(q, nw);
	CHECK(q.link_error_countdown == 31 && q.link_error_alpha == 245);
	while (q.link_error_countdown > 1) net_quality_update_indicators(q, nw);
	CHECK(q.link_error_alpha == 0); // 245 falls 10 a frame and floors at 0
	CHECK(q.link_error_bits == 2);
	net_quality_update_indicators(q, nw);
	CHECK(q.link_error_countdown == 0 && q.link_error_bits == 0 && q.link_error_alpha == 0);
	// With the countdown spent the blink stops too.
	const int blink = q.link_error_blink;
	const int timer = q.link_error_blink_timer;
	net_quality_update_indicators(q, nw);
	CHECK(q.link_error_blink == blink && q.link_error_blink_timer == timer);
}

// [orig: @0x4c3177..0x4c31eb] the NovaWorld N's fade and blink.
void test_novaworld_frames() {
	NetQualityIndicators q;
	NovaWorldLinkFacts nw;
	nw.novaworld = true;
	nw.nwu_session_flags = 0x02; // state 2: not ready
	net_quality_update_indicators(q, nw);
	CHECK(q.novaworld_alpha == 255);
	CHECK(q.novaworld_blink == 1 && q.novaworld_blink_timer == 10);
	for (int i = 0; i < 11; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.novaworld_blink == 0);
	// Ready (CGameSession states 4..8 carry 2 and 8): fade 10 a frame, no blink.
	nw.nwu_session_flags = 0x1A;
	CHECK(nwu_session_ready(nw.nwu_session_flags));
	net_quality_update_indicators(q, nw);
	CHECK(q.novaworld_alpha == 245 && q.novaworld_blink == 0);
	for (int i = 0; i < 30; ++i) net_quality_update_indicators(q, nw);
	CHECK(q.novaworld_alpha == 0);
	// Off NovaWorld both clear.
	nw.nwu_session_flags = 0;
	net_quality_update_indicators(q, nw);
	CHECK(q.novaworld_alpha == 255);
	nw.novaworld = false;
	net_quality_update_indicators(q, nw);
	CHECK(q.novaworld_alpha == 0 && q.novaworld_blink == 0);
}

// [orig: @0x4c58c0] the reset raises the change byte only over a held level.
void test_reset() {
	NetQualityIndicators q;
	net_quality_reset(q, 7);
	CHECK(!q.level_changed);
	net_quality_set_level(q, 2);
	q.level_changed = false;
	q.level2_alpha = 100;
	q.link_error_bits = 3;
	q.novaworld_alpha = 255;
	net_quality_reset(q, 50);
	CHECK(q.level_changed && q.level == 0);
	CHECK(q.level2_alpha == 0 && q.link_error_bits == 0 && q.novaworld_alpha == 0);
	CHECK(q.ramp_target == 255 && q.flag_cooldown_until_ms == 50);
}

HudLayout icon_layout() {
	HudLayout layout;
	layout.net_icon_texture_valid = true;
	layout.net_link_icon_texture_valid = true;
	layout.net_novaworld_icon_texture_valid = true;
	return layout;
}

std::vector<HudQuad> icon_quads(const HudDrawList &list) {
	std::vector<HudQuad> out;
	for (const HudQuad &q : list.quads)
		if (q.texture == kHudTexNetIcon || q.texture == kHudTexNetLinkIcon ||
				q.texture == kHudTexNetNovaWorldIcon)
			out.push_back(q);
	return out;
}

HudFrameState live_state() {
	HudFrameState state;
	state.net_quality.in_session = true;
	state.net_quality.novaworld_icon = true;
	NetQualityIndicators &q = state.net_quality.indicators;
	q.level3_alpha = 200;
	q.level2_alpha = 55;
	q.link_error_alpha = 255;
	q.link_error_countdown = 100;
	q.link_error_bits = 2;
	q.novaworld_alpha = 255;
	q.novaworld_blink = 1;
	return state;
}

// [orig: @0x4c3200] the corners, the separately scaled sizes, the bands, the
// alphas and the draw order, at 2x.
void test_draw_layout() {
	HudFrameCompiler compiler;
	compiler.configure(icon_layout(), nullptr);
	const HudFrameState state = live_state();
	const std::vector<HudQuad> quads = icon_quads(compiler.compile(state, 2048.0f, 1536.0f));
	CHECK(quads.size() == 4);
	if (quads.size() != 4) return;
	// The quality icon: red (band 2) first, then yellow (band 1), 12x12 at (4, 4).
	CHECK(quads[0].texture == kHudTexNetIcon && near(quads[0].x0, 8) && near(quads[0].y0, 8));
	CHECK(near(quads[0].x1, 32) && near(quads[0].y1, 32));
	CHECK(near(quads[0].v0, 0.5f) && near(quads[0].v1, 0.75f));
	CHECK(quads[0].color == 0xC8FFFFFFu);
	CHECK(quads[1].texture == kHudTexNetIcon && near(quads[1].v0, 0.25f));
	CHECK(quads[1].color == 0x37FFFFFFu);
	// The link-error icon: 24x12 at (20, 4), the R band while the blink is off.
	CHECK(quads[2].texture == kHudTexNetLinkIcon && near(quads[2].x0, 40) && near(quads[2].y0, 8));
	CHECK(near(quads[2].x1, 88) && near(quads[2].y1, 32));
	CHECK(near(quads[2].v0, 0.5f) && near(quads[2].v1, 0.75f));
	CHECK(quads[2].color == 0xFFFFFFFFu);
	// The N: 8x8 at (52, 4), two bands, the red one while blinking at full alpha.
	CHECK(quads[3].texture == kHudTexNetNovaWorldIcon && near(quads[3].x0, 104));
	CHECK(near(quads[3].x1, 120) && near(quads[3].y1, 24));
	CHECK(near(quads[3].v0, 0.5f) && near(quads[3].v1, 1.0f));
}

// The corner and the size each round through (v * dim + extent/2) / extent
// before they are added [orig: Viewport_ScaleToVirtualCoords @0x5d2b20 twice].
void test_draw_rounding() {
	HudFrameCompiler compiler;
	HudLayout layout = icon_layout();
	layout.net_indicator_pos = {5, 3, 20, 4, 52, 4};
	compiler.configure(layout, nullptr);
	HudFrameState state = live_state();
	state.net_quality.indicators.level2_alpha = 0;
	state.net_quality.indicators.link_error_alpha = 0;
	state.net_quality.novaworld_icon = false;
	const std::vector<HudQuad> quads = icon_quads(compiler.compile(state, 1280.0f, 960.0f));
	CHECK(quads.size() == 1);
	if (quads.size() != 1) return;
	// x = (5*1280 + 512) / 1024 = 6, w = (12*1280 + 512) / 1024 = 15;
	// y = (3*960 + 384) / 768 = 4, h = (12*960 + 384) / 768 = 15.
	CHECK(near(quads[0].x0, 6) && near(quads[0].x1, 21));
	CHECK(near(quads[0].y0, 4) && near(quads[0].y1, 19));
}

// The band rules: the link icon shows band 0 while blinking or once fewer
// than 62 frames remain [orig: @0x4c3380..0x4c3397]; the N shows band 0 below
// full alpha [orig: @0x4c3412..0x4c341e].
void test_draw_bands() {
	HudFrameCompiler compiler;
	compiler.configure(icon_layout(), nullptr);
	HudFrameState state = live_state();
	NetQualityIndicators &q = state.net_quality.indicators;
	q.level3_alpha = 0;
	q.level2_alpha = 0;
	q.link_error_blink = 1;
	std::vector<HudQuad> quads = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(quads.size() == 2 && near(quads[0].v0, 0.0f));
	q.link_error_blink = 0;
	q.link_error_countdown = 61;
	q.link_error_bits = 3;
	quads = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(quads.size() == 2 && near(quads[0].v0, 0.0f));
	q.link_error_countdown = 62;
	quads = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(quads.size() == 2 && near(quads[0].v0, 0.75f));
	q.novaworld_alpha = 254;
	quads = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(quads.size() == 2 && near(quads[1].v0, 0.0f) && quads[1].color == 0xFEFFFFFFu);
	// Zero alphas draw nothing.
	q.link_error_alpha = 0;
	q.novaworld_alpha = 0;
	quads = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(quads.empty());
}

// The gates: the session, /NOHUD, the HUD level (the HUD caller's own), the
// N icon's NovaWorld gate and each atlas [orig: @0x4c3210 / @0x4c3224,
// @0x5cae4d, @0x4c33d4..0x4c33f0, @0x4c3239 / @0x4c330f].
void test_draw_gates() {
	HudFrameCompiler compiler;
	compiler.configure(icon_layout(), nullptr);
	HudFrameState state = live_state();
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).size() == 4);
	state.net_quality.in_session = false;
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).empty());
	state.net_quality.in_session = true;
	state.overlay_master = 0;
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).empty());
	state.overlay_master = kHudOverlayMasterDefault;
	state.hud_detail_level = 1;
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).empty());
	state.hud_detail_level = 3;
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).empty());
	state.hud_detail_level = 0;
	state.net_quality.novaworld_icon = false;
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).size() == 3);
	HudLayout layout = icon_layout();
	layout.net_icon_texture_valid = false;
	layout.net_link_icon_texture_valid = false;
	compiler.configure(layout, nullptr);
	state.net_quality.novaworld_icon = true;
	const std::vector<HudQuad> only_n = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(only_n.size() == 1 && only_n[0].texture == kHudTexNetNovaWorldIcon);
}

// Without neticon3 the quality icon's red band stands in for the N
// [orig: @0x4c3405..0x4c340b].
void test_novaworld_fallback() {
	HudFrameCompiler compiler;
	HudLayout layout = icon_layout();
	layout.net_novaworld_icon_texture_valid = false;
	compiler.configure(layout, nullptr);
	HudFrameState state = live_state();
	state.net_quality.indicators.level3_alpha = 0;
	state.net_quality.indicators.level2_alpha = 0;
	state.net_quality.indicators.link_error_alpha = 0;
	const std::vector<HudQuad> quads = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(quads.size() == 1);
	if (quads.size() != 1) return;
	CHECK(quads[0].texture == kHudTexNetIcon && near(quads[0].x0, 52) && near(quads[0].x1, 60));
	CHECK(near(quads[0].v0, 0.5f) && near(quads[0].v1, 0.75f));
}

// The server-status page's call pins the reset corners whatever hudpos
// authored, and ignores the HUD level [orig: Server_DrawStatusScreen
// @0x50b2a8; the literals @0x4c325d / @0x4c332e / @0x4c345c].
void test_status_page_corners() {
	HudFrameCompiler compiler;
	HudLayout layout = icon_layout();
	layout.net_indicator_pos = {100, 200, 300, 400, 500, 600};
	compiler.configure(layout, nullptr);
	HudFrameState state = live_state();
	state.hud_detail_level = 2;
	CHECK(icon_quads(compiler.compile(state, 1024.0f, 768.0f)).empty());
	compiler.emit_net_quality_indicators(state.net_quality, state.overlay_master, true, 1024.0f,
			768.0f);
	const std::vector<HudQuad> quads = icon_quads(compiler.last_draw_list());
	CHECK(quads.size() == 4);
	if (quads.size() != 4) return;
	CHECK(near(quads[0].x0, 4) && near(quads[0].y0, 4));
	CHECK(near(quads[2].x0, 20) && near(quads[2].y0, 4));
	CHECK(near(quads[3].x0, 52) && near(quads[3].y0, 4));
	// The HUD's own call reads the authored corners.
	state.hud_detail_level = 0;
	const std::vector<HudQuad> hud = icon_quads(compiler.compile(state, 1024.0f, 768.0f));
	CHECK(hud.size() == 4 && near(hud[0].x0, 100) && near(hud[2].y0, 400) && near(hud[3].x0, 500));
}

} // namespace

int main() {
	test_set_level();
	test_level_channels();
	test_flags();
	test_link_error_frames();
	test_novaworld_frames();
	test_reset();
	test_draw_layout();
	test_draw_rounding();
	test_draw_bands();
	test_draw_gates();
	test_novaworld_fallback();
	test_status_page_corners();
	if (failures != 0) {
		std::printf("net_quality_indicators: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("net_quality_indicators: ok\n");
	return 0;
}
