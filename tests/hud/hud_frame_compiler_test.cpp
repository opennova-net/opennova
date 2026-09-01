// HudFrameCompiler + GameFont pins: the witnessed element walk emits the
// typed draw list (quads/tris/glyphs) with the ported policy math intact.
// [orig: HUD_RenderOverlays @ 0x5a7bb0; CGameFont_MeasureText @ 0x674e70;
//  CGameFont_DrawText @ 0x6752c0]

#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_message_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using opennova::hud::GameFont;
using opennova::hud::GameFontState;
using opennova::hud::HudDrawList;
using opennova::hud::HudFrameCompiler;
using opennova::hud::HudFrameState;
using opennova::hud::HudLayout;
using opennova::hud::HudMinimapCompiler;
using opennova::hud::HudMinimapInput;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// A synthetic 1-page font: every glyph 8x16 px on the 256-page grid,
// spacing 2, design width 800 (scale 1).
fnt_font_t make_font() {
	fnt_font_t font{};
	fnt_init_blank(&font, 1, 2);
	font.design_width = 800;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		font.glyphs[i].page = 0;
		font.glyphs[i].uv.u0 = 0.0f;
		font.glyphs[i].uv.v0 = 0.0f;
		font.glyphs[i].uv.u1 = 8.0f / 256.0f;
		font.glyphs[i].uv.v1 = 16.0f / 256.0f;
	}
	return font;
}

void test_measure_advance_and_trailing_pad(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	int w = 0;
	int h = 0;
	// Each glyph advances floor(8 + (2-1) + 0.5) = 9; the final width strips
	// the trailing (spacing-1) pad: 3 glyphs -> 27 - 1 = 26.
	gf.measure("abc", 1.0f, 1.0f, &w, &h);
	CHECK(w == 26, "measure width = glyph advances minus the trailing pad");
	CHECK(h == 16, "measure height = the SPACE glyph's v-extent");

	// Controls and the three retail control bytes are skipped.
	int w2 = 0;
	gf.measure("a\x7f\x01"
			   "bc",
			1.0f, 1.0f, &w2, &h);
	CHECK(w2 == w, "0x7F and control bytes measure nothing");

	// Newline folds the max and adds a line.
	int w3 = 0;
	int h3 = 0;
	gf.measure("abc\nabcd", 1.0f, 1.0f, &w3, &h3);
	CHECK(w3 == 35, "multiline width is the widest line");
	CHECK(h3 == 32, "each newline adds one line height");
}

void test_layout_pages_bold_underline(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	const auto run = gf.layout("ab", 100.0f, 50.0f, 1.0f, 1.0f, 0u,
			0xFF102030u);
	CHECK(run.quads.size() == 2, "one quad per glyph on the one page");
	CHECK(run.quads[0].x_top_left == 99.5f,
			"the drawer's -0.5 vertex offset is kept");
	CHECK(run.quads[1].x_top_left == 108.5f,
			"the second glyph starts at the floored advance");
	CHECK(run.underlines.empty(), "no underline without the flag");

	const auto bold = gf.layout("a", 0.0f, 0.0f, 1.0f, 1.0f,
			opennova::hud::kFontStyleBold, 0xFF000000u);
	CHECK(bold.quads.size() == 2, "bold double-strikes each glyph");
	CHECK(bold.quads[1].x_top_left == bold.quads[0].x_top_left + 1.0f,
			"the second strike lands one pixel right");

	const auto lined = gf.layout("a", 0.0f, 0.0f, 1.0f, 1.0f,
			opennova::hud::kFontStyleUnderline, 0xFF404040u);
	CHECK(lined.underlines.size() == 1, "underline emits one segment");
	CHECK((lined.underlines[0].color & 0xFFFFFFu) == 0x808080u,
			"the underline color doubles the text color");
}

void test_format_tags(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	GameFontState state;
	int idx = 0;
	CHECK(GameFont::parse_format_tag("<B>", &idx, &state), "well-formed <B>");
	CHECK(state.bold, "<B> sets bold");
	idx = 0;
	CHECK(GameFont::parse_format_tag("<-B>", &idx, &state), "<-B> parses");
	CHECK(!state.bold, "<-B> clears bold");
	idx = 0;
	CHECK(!GameFont::parse_format_tag("<B", &idx, &state),
			"an unterminated tag reports failure");
	idx = 0;
	GameFontState color_state;
	CHECK(GameFont::parse_format_tag("<C102030>", &idx, &color_state),
			"hex color tag parses");
	CHECK((color_state.color_xor & 0xFFFFFFu) == 0x102030u,
			"the color XOR folds the tag color");
	// Measurement skips tags entirely.
	int w = 0;
	int h = 0;
	gf.measure("<B>ab", 1.0f, 1.0f, &w, &h);
	int w2 = 0;
	gf.measure("ab", 1.0f, 1.0f, &w2, &h);
	CHECK(w == w2, "tags measure zero width");
}

// The HUD declutter model: HUDDECLUT masks x hud_detail level -> visible[]
// [orig: HUD_ParseHudposToken @ 0x59F370 -> byte_2723CE0;
//  CRenderState_SetLayerVisibility @ 0x59B0F0 -> dword_2723C80; the huddetail
//  cycle @ 0x4E0601..0x4E0624].
void test_declutter_rebuild_and_cycle() {
	using namespace opennova::hud;

	// The token map, including the CTAPE dead token (no retail parse arm).
	CHECK(declutter_slot_from_token("SPINMAP") == kDeclutterSpinmap,
			"SPINMAP resolves to slot 17");
	CHECK(declutter_slot_from_token("chat") == kDeclutterChat,
			"token lookup is case-insensitive");
	CHECK(declutter_slot_from_token("CTAPE") == -1,
			"the dead JOX CTAPE token has no slot (no retail arm)");
	CHECK(std::string(declutter_token_name(kDeclutterMsnTitle)) == "MSNTITLE",
			"slot 0 names MSNTITLE");

	// The mask build: bit i set iff value i != 0.
	const int spinmap_flags[4] = {1, 1, 0, 0};
	CHECK(HudDeclutter::mask_from_flags(spinmap_flags) == 0x3,
			"SPINMAP 1 1 0 0 builds mask 0b0011");
	const int clock_flags[4] = {1, 0, 0, 0};
	CHECK(HudDeclutter::mask_from_flags(clock_flags) == 0x1,
			"CLOCK 1 0 0 0 builds mask 0b0001");

	HudDeclutter d;
	// The construction default (no hudpos): all-bits masks, visible at every
	// level.
	for (int level = 0; level <= kDeclutterLevelMax; ++level) {
		d.set_level(level);
		CHECK(d.visible()[kDeclutterDmgBar] &&
						d.visible()[kDeclutterSpinmap],
				"the harness default is visible at every level");
	}

	// The authored table: an UNAUTHORED slot is hidden at every level.
	d.begin_authoring();
	for (int level = 0; level <= kDeclutterLevelMax; ++level) {
		d.set_level(level);
		for (int slot = 0; slot < kDeclutterSlotCount; ++slot) {
			CHECK(!d.visible()[static_cast<size_t>(slot)],
					"an unauthored slot never draws");
		}
	}

	// The JOX-shaped fixture: SPINMAP `1 1 0 0` is visible at levels 0/1 and
	// hidden at 2/3; DMGBAR `1 1 1 0` flips only at 3.
	d.set_mask(kDeclutterSpinmap, 0x3);
	d.set_mask(kDeclutterDmgBar, 0x7);
	const bool spinmap_expect[4] = {true, true, false, false};
	const bool dmgbar_expect[4] = {true, true, true, false};
	for (int level = 0; level <= kDeclutterLevelMax; ++level) {
		d.set_level(level);
		CHECK(d.visible()[kDeclutterSpinmap] == spinmap_expect[level],
				"SPINMAP 1 1 0 0 follows (1 << level) & mask");
		CHECK(d.visible()[kDeclutterDmgBar] == dmgbar_expect[level],
				"DMGBAR 1 1 1 0 follows (1 << level) & mask");
	}

	// The huddetail cycle: 0 -> 1 -> 2 -> 3 -> 0.
	d.set_level(0);
	CHECK(d.cycle_level() == 1 && d.cycle_level() == 2 &&
					d.cycle_level() == 3 && d.cycle_level() == 0,
			"the level cycle wraps past 3 to 0");
}

// The per-element declutter gates at their compile sites, the CHAT double
// gate, the showhud bit-1 spinmap gate, and the level-3 whole-pass early-out.
// [orig: the slot cmps @ 0x5A7C99 (DMGBAR) / @ 0x5A7CC8..0x5A7D42 (WPNGRP) /
//  @ 0x592757 (XHAIRS) / @ 0x5A7DB8 (WAYPOINT) / @ 0x5A7DD2 (PWRBAR) /
//  @ 0x5A86E8 (SPINMAP, inside the showhud bit-1 test @ 0x5A8635) /
//  @ 0x59AD33 (CHAT) + the hard level >= 2 cull @ 0x59AD43; the early-out
//  @ 0x5A80C4]
void test_declutter_element_gates(const fnt_font_t *font) {
	using namespace opennova::hud;
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	layout.power_rect = {200.0f, 700.0f, 60.0f, 10.0f, true};
	layout.spinmap_rect = {810.0f, 552.0f, 210.0f, 210.0f, true};
	layout.crosshair_texture_valid = true;
	layout.crosshair_tex_w = 64;
	layout.crosshair_tex_h = 64;
	layout.ammo_count = {40, 700, 0, 0, true};
	layout.wpd_info = {512, 60, 1, 0, true};
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 10;
	state.health_fraction = 0.5f;
	state.weapon.active = true;
	state.weapon.clip = 12;
	state.weapon.reserve = 90;
	state.weapon.capacity = 30;
	state.windup_active = true;
	state.windup_held_ticks = 31;
	state.waypoint.present = true;
	state.waypoint.name = "Alpha";
	state.waypoint.distance_m = 120;

	// The default state (all-visible, level 0, showhud 3) draws everything.
	const HudDrawList &all_on = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!all_on.quads.empty() && !all_on.tris.empty() &&
					!all_on.glyphs.empty() && all_on.map.visible,
			"the all-visible default draws bars, reticle, text, and spinmap");

	// The JOX-shaped table at level 2: DMGBAR/WPNGRP (1 1 1 0) still draw,
	// SPINMAP (1 1 0 0) hides, CHAT (1 1 1 0) is slot-visible but the hard
	// level cull hides the feed anyway.
	HudDeclutter d;
	d.begin_authoring();
	d.set_mask(kDeclutterDmgBar, 0x7);
	d.set_mask(kDeclutterWpnGrp, 0x7);
	d.set_mask(kDeclutterXhairs, 0x7);
	d.set_mask(kDeclutterWaypoint, 0x7);
	d.set_mask(kDeclutterPwrBar, 0x7);
	d.set_mask(kDeclutterSpinmap, 0x3);
	d.set_mask(kDeclutterChat, 0x7);
	d.set_level(2);
	compiler.push_message("radio check", 10);
	state.declutter_visible = d.visible();
	state.hud_detail_level = d.level();
	const HudDrawList &level2 = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!level2.quads.empty(), "DMGBAR stays visible at level 2 (1 1 1 0)");
	CHECK(!level2.tris.empty(), "XHAIRS stays visible at level 2");
	bool saw_ammo_glyphs = !level2.glyphs.empty();
	CHECK(saw_ammo_glyphs, "WPNGRP text stays visible at level 2");
	CHECK(!level2.map.visible, "SPINMAP (1 1 0 0) hides at level 2");
	// The CHAT double gate: hold everything else constant and check that the
	// live message never lands despite slot 23 being visible at level 2.
	{
		HudFrameState quiet = state;
		quiet.weapon.active = false;
		quiet.windup_active = false;
		quiet.waypoint.present = false;
		quiet.health_fraction = -1.0f;
		HudDeclutter chat_only;
		chat_only.begin_authoring();
		chat_only.set_mask(kDeclutterChat, 0x7);
		chat_only.set_level(2);
		quiet.declutter_visible = chat_only.visible();
		quiet.hud_detail_level = chat_only.level();
		const HudDrawList &chat2 = compiler.compile(quiet, 1024.0f, 768.0f);
		CHECK(chat2.glyphs.empty(),
				"CHAT slot on + level 2 still hides the feed (the hard cull)");
		chat_only.set_level(1);
		quiet.declutter_visible = chat_only.visible();
		quiet.hud_detail_level = chat_only.level();
		const HudDrawList &chat1 = compiler.compile(quiet, 1024.0f, 768.0f);
		CHECK(!chat1.glyphs.empty(),
				"the same slot draws the feed at level 1");
	}

	// Per-slot isolation at level 2: masking DMGBAR out hides only the bar.
	{
		HudDeclutter no_bar = d;
		no_bar.set_mask(kDeclutterDmgBar, 0x3);
		state.declutter_visible = no_bar.visible();
		state.hud_detail_level = no_bar.level();
		const HudDrawList &bar_off = compiler.compile(state, 1024.0f, 768.0f);
		bool saw_health_fill = false;
		for (const HudQuad &q : bar_off.quads) {
			if (q.filled && q.color == layout.tag_middle) saw_health_fill = true;
		}
		CHECK(!saw_health_fill && !bar_off.tris.empty(),
				"a level-2-masked DMGBAR hides the bar and nothing else");
	}
	// WPNGRP off hides the ammo/name text and clip icons but NOT the
	// crosshair (its own XHAIRS slot stays on).
	{
		HudDeclutter no_grp = d;
		no_grp.set_mask(kDeclutterWpnGrp, 0x3);
		HudFrameState text_state = state;
		// Isolate the cluster text: no waypoint label, no power-bar "%d%".
		text_state.waypoint.present = false;
		text_state.windup_active = false;
		text_state.hud_detail_level = no_grp.level();
		text_state.declutter_visible = no_grp.visible();
		const HudDrawList &grp_off = compiler.compile(text_state, 1024.0f,
				768.0f);
		CHECK(grp_off.glyphs.empty() && !grp_off.tris.empty(),
				"a masked WPNGRP hides the ammo text while XHAIRS keeps the reticle");
	}

	// showhud bit 1 off hides the corner spinmap while visible[17] stays on.
	{
		HudDeclutter map_on = d;
		map_on.set_level(0);
		state.declutter_visible = map_on.visible();
		state.hud_detail_level = map_on.level();
		state.showhud_flags = 1; // gun only
		const HudDrawList &gun_only = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(!gun_only.map.visible,
				"showhud bit 1 off hides the spinmap with slot 17 visible");
		state.showhud_flags = 3;
		const HudDrawList &both = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(both.map.visible, "showhud 3 restores the corner spinmap");
	}

	// Level 3 blanks the whole gameplay overlay pass — nothing draws — while
	// the M-cycle big map (a separate retail pass) still compiles.
	{
		HudDeclutter max_declutter = d;
		max_declutter.set_level(3);
		state.declutter_visible = max_declutter.visible();
		state.hud_detail_level = max_declutter.level();
		const HudDrawList &blank = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(blank.quads.empty() && blank.tris.empty() &&
						blank.lines.empty() && blank.glyphs.empty() &&
						!blank.map.visible,
				"level 3 emits nothing for the gameplay pass");
		HudFrameState mode3 = state;
		mode3.minimap.map_mode = 3;
		const HudDrawList &big = compiler.compile(mode3, 1024.0f, 768.0f);
		CHECK(!big.map.visible && big.big_map.visible,
				"the M-cycle big map still compiles at level 3");
	}
}

void test_compiler_health_and_order(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	layout.heat_rect = {200.0f, 20.0f, 40.0f, 8.0f, true};
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 100;
	state.health_fraction = 0.5f;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Health at 0.5: the mid band fill + the wireframe border.
	CHECK(list.quads.size() == 2, "health emits fill + border");
	CHECK(list.quads[0].filled && !list.quads[1].filled,
			"fill first, border on top");
	CHECK(list.quads[0].color == layout.tag_middle,
			"0.5 health takes the middle band color");

	// Heat with an armed weapon.
	HudFrameState hot = state;
	hot.weapon.active = true;
	hot.weapon.heat = 0x8000;
	const HudDrawList &heat = compiler.compile(hot, 1024.0f, 768.0f);
	CHECK(heat.quads.size() == 4, "heat adds its border + fill");

	// The message ring: pushed lines draw until expiry.
	compiler.push_message("hello", 100);
	const HudDrawList &with_msg = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!with_msg.glyphs.empty(), "a live message emits glyphs");
	HudFrameState later = state;
	later.ticks = 100 + 930 + 1;
	const HudDrawList &expired = compiler.compile(later, 1024.0f, 768.0f);
	CHECK(expired.glyphs.empty(), "the 930-tick life expires the line");
}

void test_compiler_crosshair(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.crosshair_texture_valid = true;
	layout.crosshair_tex_w = 64;
	layout.crosshair_tex_h = 64;
	compiler.configure(layout, font);

	HudFrameState state;
	state.weapon.active = true;
	state.aimed_shot_available = false;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Five regions: four 5-vertex strips (3 tris) + one 4-vertex center
	// (2 tris) = 14 triangles.
	CHECK(list.tris.size() == 14, "the five tapered regions emit 14 triangles");

	HudFrameState aimed = state;
	aimed.aimed_shot_available = true;
	const HudDrawList &hidden = compiler.compile(aimed, 1024.0f, 768.0f);
	CHECK(hidden.tris.empty(), "a settled aimed shot hides the reticle");
}

// The two user crosshair options: colour reaches every reticle tri, and the
// spread toggle zeroes the arm offsets while still drawing all five regions
// [orig: the colour into the corner-quad params (dword_25510E0); the
// g_cfgCrossHairSpread arm @ 0x592b82 with the disabled fldz @ 0x592bcc].
void test_compiler_crosshair_user_options(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.crosshair_texture_valid = true;
	layout.crosshair_tex_w = 64;
	layout.crosshair_tex_h = 64;
	layout.crosshair_color = 0xFF20FF40u;
	compiler.configure(layout, font);

	HudFrameState state;
	state.weapon.active = true;
	state.hud_spread_fp16 = 0x400000; // a visibly nonzero spread input
	state.fov_deg = 80.0f;
	const HudDrawList &spread_on = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(spread_on.tris.size() == 14, "spread on draws the five regions");
	bool all_colored = !spread_on.tris.empty();
	for (const HudTri &tri : spread_on.tris) {
		if (tri.color != 0xFF20FF40u) all_colored = false;
	}
	CHECK(all_colored, "the user colour modulates every reticle tri");

	HudLayout no_spread = layout;
	no_spread.crosshair_spread_enabled = false;
	compiler.update_layout(no_spread);
	// compile() reuses the internal list; copy before compiling again.
	const HudDrawList spread_off = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(spread_off.tris.size() == 14,
			"spread off still draws all five regions");
	HudFrameState centered = state;
	centered.hud_spread_fp16 = 0;
	compiler.update_layout(layout);
	const HudDrawList &zero_input = compiler.compile(centered, 1024.0f, 768.0f);
	bool same_positions = spread_off.tris.size() == zero_input.tris.size();
	for (size_t i = 0; same_positions && i < spread_off.tris.size(); ++i) {
		const HudTri &a = spread_off.tris[i];
		const HudTri &b = zero_input.tris[i];
		same_positions = a.a.x == b.a.x && a.a.y == b.a.y &&
				a.b.x == b.b.x && a.b.y == b.b.y &&
				a.c.x == b.c.x && a.c.y == b.c.y;
	}
	CHECK(same_positions,
			"spread off collapses to the zero-spread geometry (arms centered)");
}

// The hud_color_index scheme swap (the D-HUD-20 good-tier residue + the
// master overlay color). [orig: HUD_InitTeamColorTable @ 0x51f240 table
// immediates; the good-tier index test @ 0x5a3c9e (== 2 -> tagcolor_good,
// else g_hudColorTable[index]); the attach labels' g_hudActiveColor reads
// @ 0x5a362d; table[2] sourced per frame from the hudpos hud_textcolor
// @ 0x5a8100]
void test_compiler_hud_color_schemes(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24;
	state.friendly_tags.push_back(tag);

	// Scheme 1 (green): the GOOD tier swaps off tagcolor_good onto table[1].
	state.hud_color_index = 1;
	const HudDrawList &green = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t green_tier = (217u << 24) | (0xFF00FF00u & 0xFFFFFFu);
	CHECK(!green.glyphs.empty() &&
					green.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(green_tier),
			"scheme 1 puts the good tier on the table green");

	// The middle tier never scheme-swaps.
	state.friendly_tags[0].health_ratio_fp16 = 0x8000;
	const HudDrawList &mid = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t middle = (217u << 24) | (layout.tag_middle & 0xFFFFFFu);
	CHECK(!mid.glyphs.empty() &&
					mid.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(middle),
			"the middle tier ignores the scheme");
	state.friendly_tags[0].health_ratio_fp16 = 0x10000;

	// Scheme 2 (the default) keeps tagcolor_good -- the retail identity.
	state.hud_color_index = 2;
	const HudDrawList &def = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t good = (217u << 24) | (layout.tag_good & 0xFFFFFFu);
	CHECK(!def.glyphs.empty() &&
					def.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(good),
			"scheme 2 keeps the hudpos tagcolor_good");

	// The attach labels ride the master overlay color: the wire box frames at
	// the raw scheme color (light blue, scheme 3).
	state.friendly_tags.clear();
	opennova::hud::HudAttachLabel label;
	label.screen_x = 300.0f;
	label.screen_y = 60.0f;
	label.text = "Sit";
	label.nearest = true;
	state.attach_labels.push_back(label);
	state.hud_color_index = 3;
	const HudDrawList &blue = compiler.compile(state, 1024.0f, 768.0f);
	bool saw_blue_box = false;
	for (const opennova::hud::HudQuad &q : blue.quads) {
		saw_blue_box = saw_blue_box || q.color == 0xFF80A0FFu;
	}
	CHECK(saw_blue_box, "scheme 3 draws the attach box light blue");

	// Scheme 2 tracks the LIVE hudpos hud_textcolor (the per-frame table[2]
	// refresh), alpha forced FF like the init path.
	HudLayout tinted = layout;
	tinted.hud_text = 0x00123456u;
	compiler.configure(tinted, font);
	state.hud_color_index = 2;
	const HudDrawList &track = compiler.compile(state, 1024.0f, 768.0f);
	bool saw_tinted_box = false;
	for (const opennova::hud::HudQuad &q : track.quads) {
		saw_tinted_box = saw_tinted_box || q.color == 0xFF123456u;
	}
	CHECK(saw_tinted_box, "scheme 2 sources hud_textcolor live, alpha-forced");
}

void test_spinmap_projection_zoom_and_clip() {
	HudMinimapInput input;
	input.rect_x1 = 810.0f;
	input.rect_y1 = 552.0f;
	input.rect_x2 = 1020.0f;
	input.rect_y2 = 762.0f;
	input.player_heading_bam = 0;
	float x = 0.0f;
	float y = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(input, 0, 0, false, x, y),
			"player projects inside spinmap");
	CHECK(x == 915.0f && y == 657.0f, "player projects at authored center");
	// World-per-pixel = zoom / (rect_w * 200) [orig: flt_7D2290 @0x5a5f40];
	// the default visible radius is 65536/400 = 163.84 world units.
	const float scale = 65536.0f / (210.0f * 200.0f);
	float fx = 0.0f;
	float fy = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(input, 100 << 16, 0, false,
			fx, fy), "a 100-unit-forward point projects inside");
	CHECK(std::fabs(fx - 915.0f) < 0.01f &&
			std::fabs(fy - (657.0f - 100.0f / scale)) < 0.01f,
			"heading zero puts +X (ahead) above center");
	// Facing +X, mission +Y (the player's LEFT) lands left of center — the
	// witnessed transform negates mission Y before the rotation.
	// [orig: Terrain_FixedPointToWorldFloat @0x607060]
	float lx = 0.0f;
	float ly = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(input, 0, 100 << 16, false,
			lx, ly), "a 100-unit-left point projects inside");
	CHECK(std::fabs(ly - 657.0f) < 0.01f &&
			std::fabs(lx - (915.0f - 100.0f / scale)) < 0.01f,
			"mission +Y draws on the player's left, not mirrored");
	// The backing/stencil fan sits TWO DESIGN pixels inside the scaled
	// half-height, taken through the width-axis scaler — the same
	// scaleX((flags >> 8) & 2) the waypoint-pointer radius subtracts
	// [orig: the map-setup inset @0x5a64c0..0x5a650d; the literal edi = 2
	// @0x5a60d8; Viewport_ScaleToVirtualCoords @0x5d2b20]. This input runs at
	// the DESIGN surface (1024x768), where scaleX(2) = 2: 105 - 2 = 103 px.
	// (At 1920 the same expression yields 4, which is the value an earlier
	// live probe read and a constant was written from.)
	CHECK(!opennova::hud::project_spinmap_point(input, 200 << 16, 0,
			false, x, y), "a point past the stencil's world radius is clipped");
	CHECK(!opennova::hud::project_spinmap_point(input, 200 << 16, 0,
			true, x, y) && std::fabs(y - (657.0f - 103.0f)) < 0.01f,
			"edge clamp lands on the stencil circle");
	// Zoom OUT grows the world-extent value x1.15 toward 0x100000; IN shrinks
	// x0.85 toward 4096. [orig: cases 361/360 @0x49beaf/@0x49bcb0]
	CHECK(opennova::hud::spinmap_zoom_step(65536, 1) == 75366,
			"zoom out applies the retail x1.15 truncation");
	CHECK(opennova::hud::spinmap_zoom_step(65536, -1) == 55705,
			"zoom in applies the retail x0.85 truncation");
	CHECK(opennova::hud::spinmap_zoom_step(4096, -1) == 4096,
			"zoom clamps at 4096");
	CHECK(opennova::hud::spinmap_zoom_step(0x100000, 1) == 0x100000,
			"zoom clamps at 0x100000");

	// The live retail probe observes 00TRa's authored 0.61 as the
	// complementary 0.39 Q16 value. The per-frame scale divides that value
	// by the independently scaled rect HEIGHT (281 px at 1920x1080), not its
	// 375 px width.
	opennova::hud::HudMapControl control;
	control.set_mission_map_zoom(0.61f);
	CHECK(control.spawn_zoom_q16 == 25559 && control.zoom_q16 == 25559,
			"00TRa map_zoom initializes the retail complementary Q16 zoom");
	CHECK(control.spawn_big_zoom_q16 == 204472 &&
			control.big_zoom_q16 == 204472,
			"the big-map spawn zoom uses the same complementary factor");

	HudMinimapInput widescreen;
	widescreen.rect_x1 = 840.0f;
	widescreen.rect_y1 = 20.0f;
	widescreen.rect_x2 = 1040.0f;
	widescreen.rect_y2 = 220.0f;
	widescreen.surface_w = 1920.0f;
	widescreen.surface_h = 1080.0f;
	widescreen.zoom_q16 = 25559;
	float center_x = 0.0f;
	float center_y = 0.0f;
	float ahead_x = 0.0f;
	float ahead_y = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(
			widescreen, 0, 0, false, center_x, center_y),
			"the widescreen player center remains visible");
	CHECK(opennova::hud::project_spinmap_point(
			widescreen, 50 << 16, 0, false, ahead_x, ahead_y),
			"a retail-probe comparison point remains visible");
	const float retail_scale = 25559.0f / (281.0f * 200.0f);
	CHECK(std::fabs(ahead_x - center_x) < 0.01f &&
			std::fabs((center_y - ahead_y) - 50.0f / retail_scale) < 0.01f,
			"widescreen projection uses the retail 281px-height scale");
	float edge_x = 0.0f;
	float edge_y = 0.0f;
	CHECK(!opennova::hud::project_spinmap_point(
			widescreen, 1000 << 16, 0, true, edge_x, edge_y),
			"a distant widescreen point clamps to the spinmap edge");
	CHECK(std::fabs(std::hypot(edge_x - center_x, edge_y - center_y) -
			136.5f) < 0.05f,
			"the corner stencil reaches the probed retail backing-ring radius");

	// THE DISC INSET IS WIDTH-DEPENDENT. It is two DESIGN pixels through the
	// width-axis integer scaler [orig: the literal edi = 2 @0x5a60d8; the apply
	// @0x5a64cd; Viewport_ScaleToVirtualCoords @0x5d2b20], i.e.
	// floor((2*W + 512) / 1024) — which is 4 at 1920 (the value an earlier live
	// probe read, and why a constant looked right) but 3 at 1280. A constant
	// would put the ring in the wrong place on every non-1920 display.
	HudMinimapInput narrow = widescreen;
	narrow.surface_w = 1280.0f;
	narrow.surface_h = 720.0f;
	float n_center_x = 0.0f;
	float n_center_y = 0.0f;
	float n_edge_x = 0.0f;
	float n_edge_y = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(
			narrow, 0, 0, false, n_center_x, n_center_y),
			"the 1280-wide player center remains visible");
	CHECK(!opennova::hud::project_spinmap_point(
			narrow, 1000 << 16, 0, true, n_edge_x, n_edge_y),
			"a distant 1280-wide point clamps to the spinmap edge");
	// rect_h at 720 = 206 - 19 = 187 -> base radius 93.5; inset 3 -> 90.5.
	CHECK(std::fabs(std::hypot(n_edge_x - n_center_x, n_edge_y - n_center_y) -
			90.5f) < 0.05f,
			"the 1280-wide disc insets by three pixels, not four");
}

// A mission-yaw -90 camera faces west. The registered retail full-frame
// witness for 00tra-fire-barrel-full-composite-retail (image sha256
// 6211fe22a5ebb51178ff8253941cfd98795afce370351d407a354015679c6306)
// consequently places W at twelve o'clock. Engine heading is 90-yaw, hence
// 0x80000000 here. HudMapSprite::rotation_rad is consumed in screen space
// (+Y down), so the left-side W glyph reaches the top at +pi/2.
void test_spinmap_compass_screen_rotation_matches_retail_heading() {
	HudMinimapInput input;
	input.rect_x1 = 810.0f;
	input.rect_y1 = 552.0f;
	input.rect_x2 = 1020.0f;
	input.rect_y2 = 762.0f;
	input.player_heading_bam = static_cast<int32_t>(0x80000000u);

	HudMinimapCompiler compiler;
	const auto &map = compiler.compile(input);
	const opennova::hud::HudMapSprite *compass = nullptr;
	for (const auto &sprite : map.sprites) {
		if (sprite.texture == 1) compass = &sprite;
	}
	CHECK(compass != nullptr, "the corner spinmap compiles its compass ring");
	CHECK(compass != nullptr &&
			std::fabs(compass->rotation_rad -
					static_cast<float>(3.14159265358979323846 / 2.0)) < 1e-4f,
			"mission yaw -90 puts W at twelve o'clock like retail");
}

void test_spinmap_mesh_layers_and_waypoint(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.spinmap_rect = {810.0f, 552.0f, 210.0f, 210.0f, true};
	layout.hud_text = 0xFF6080FFu;
	compiler.configure(layout, font);
	HudFrameState state;
	state.minimap.terrain.present = true;
	state.minimap.terrain.sector_count = 16;
	state.minimap.terrain.sector_rows = 16;
	state.minimap.terrain.sector_grid.fill(1);
	state.minimap.terrain.water_present = true;
	opennova::hud::HudMinimapMarker high;
	high.icon = 3;
	high.x = 32 << 16;
	high.color = 0xFF304080u;
	high.entity_known = 1;
	state.minimap.markers.push_back(high);
	opennova::hud::HudMinimapMarker low = high;
	low.icon = 10;
	low.x = -(32 << 16);
	state.minimap.markers.push_back(low);
	// An unknown-entity regular marker stays retained but does not draw.
	// [orig: render_minimap_slot_blip @0x5be4b8 entity[538] gate]
	opennova::hud::HudMinimapMarker hidden = high;
	hidden.entity_known = 0;
	state.minimap.markers.push_back(hidden);
	state.waypoint.present = true;
	state.waypoint.world_x = 1024 << 16;
	state.waypoint.world_z = 40 << 16; // 40 wu above -> "above" tricolor
	state.waypoint.distance_m = 1024;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(list.map.visible, "an authored HUDSPINMAP rect compiles the spinmap");
	CHECK(list.map.backing.size() == 32, "spinmap backing is a 32-sided fan");
	CHECK(!list.map.terrain.empty(), "sector routing emits clipped terrain triangles");
	CHECK(list.map.terrain_water.size() == list.map.terrain.size(),
			"depthspin redraws the same clipped sector geometry as the colormap");
	// The terrain tint bakes the forced-opaque alpha and the fixed-function
	// MODULATE2X doubling. [orig: color|0xFF000000 @0x6071C0; _FFP.fx
	//  TSSColor MODULATE2X]
	CHECK(!list.map.terrain.empty() &&
			list.map.terrain[0].color == 0xFFC0C0C0u,
			"terrain tiles carry the doubled opaque tint");
	for (const auto &tri : list.map.terrain_water) {
		CHECK(tri.color == 0xFF16476Bu,
				"depthspin carries the capture-matched opaque shore color");
		for (const auto *v : {&tri.a, &tri.b, &tri.c}) {
			CHECK(v->u >= -1e-6f && v->u <= 127.0f / 256.0f + 1e-6f &&
					v->v >= -1e-6f && v->v <= 127.0f / 256.0f + 1e-6f,
					"sector id 1 selects depthspin's exact 127px top-left quadrant");
		}
	}
	// Two live markers + ONE waypoint tip cell + the compass ring sprite;
	// the unknown-entity marker is culled. The far waypoint clamps at the
	// disc, so the tip is the rotated chevron (cell 7).
	// [orig: the single strip-cell submit @0x59953d, cell = lodLevel]
	CHECK(list.map.sprites.size() == 4,
			"two live markers, the waypoint tip cell, and the compass ring");
	CHECK(list.map.sprites[0].layer <= list.map.sprites[1].layer,
			"marker sprites retain four-layer ordering");
	CHECK(list.map.sprites[0].color == 0xFF6080FFu &&
			list.map.sprites[1].color == 0xFF6080FFu,
			"ordinary TSDicon markers bake the saturating MODULATE2X RGB stage");
	CHECK(list.map.sprites[2].color == 0xFFFEA000u,
			"the clamped chevron submits 2c-saturated (the outside branch never halves)");
	CHECK(list.map.sprites.back().texture == 1,
			"the compass ring rides texture slot 1");
	const auto &compass = list.map.sprites.back();
	CHECK(std::fabs(compass.u0 - 0.05f) < 1e-6f &&
			std::fabs(compass.v0 - 0.05f) < 1e-6f &&
			std::fabs(compass.u1 - 0.95f) < 1e-6f &&
			std::fabs(compass.v1 - 0.95f) < 1e-6f,
			"the compass crops five-percent texture padding like retail");
	CHECK(list.map.lines.size() == 1, "the waypoint state line is compiled");
	// Above-player waypoint, clamped OUTSIDE: the orange tricolor rides the
	// per-channel 2c saturate (the halve fires only in the inside-dot branch
	// on frames with counter bit 5 set — the 32-frame blink).
	// [orig: HUD_UpdateWaypointAltitudeColor @0x590970 0xFF7F5000; bit test @0x599353 inside-only; halve
	//  @0x599397; vertex 2c-saturate @0x5993c6]
	CHECK(list.map.lines[0].color == 0xFFFEA000u,
			"the state line carries the 2c-saturated above-tricolor");
	// The distance label is LIVE by default — the retail global is BSS
	// (zero) and only an authored NONZERO SPINMAPWPDISTOFF suppresses it.
	// [orig: dword_27237C0 (.data, no file bytes); read @0x5a7a6a]
	const opennova::hud::HudMapLabel *distance_label = nullptr;
	for (const auto &lab : list.map.labels) {
		if (std::string(lab.text) == "1.02k") distance_label = &lab;
	}
	CHECK(distance_label != nullptr,
			"the ring-edge distance label compiles by default");
	if (distance_label != nullptr) {
		// At design resolution: base 105 - compass inset 2 + scaled pointer
		// span 10 + integer half-span 5 = 118 pixels due north.
		CHECK(std::fabs(distance_label->x - list.map.center_x) < 0.01f &&
				std::fabs(distance_label->y -
						(list.map.center_y - 118.0f)) < 0.01f,
				"the distance label uses the retail pointer-slot anchor");
	}
	CHECK(!list.map_glyphs.empty() &&
			list.map_glyphs.front().color == 0xFF6080FEu,
			"map labels fold retail's half-bright then MODULATE2X color path");
	for (const auto &tri : list.map.terrain) {
		for (const auto *v : {&tri.a, &tri.b, &tri.c}) {
			const float nx = (v->x - list.map.center_x) / list.map.radius_x;
			const float ny = (v->y - list.map.center_y) / list.map.radius_y;
			CHECK(nx * nx + ny * ny <= 1.02f,
					"terrain vertices stay inside the 32-sided clip");
		}
	}
	// Pin the nonuniform widescreen scaling that exposed the old radius+6
	// approximation: base - scaleX(2) + scaleY(10) + half(scaleY(10))
	// becomes base + 17 at 1920x1080, or disc radius + 21.
	{
		const HudDrawList &wide = compiler.compile(state, 1920.0f, 1080.0f);
		const opennova::hud::HudMapLabel *wide_label = nullptr;
		for (const auto &lab : wide.map.labels) {
			if (std::string(lab.text) == "1.02k") wide_label = &lab;
		}
		CHECK(wide_label != nullptr &&
				std::fabs(std::hypot(wide_label->x - wide.map.center_x,
						wide_label->y - wide.map.center_y) -
						(wide.map.radius_y + 21.0f)) < 0.01f,
				"the widescreen label lands 17 pixels beyond the base radius");
	}
	// An authored NONZERO SPINMAPWPDISTOFF suppresses the distance text.
	layout.spinmap_wp_dist_off = 17;
	compiler.update_layout(layout);
	{
		const HudDrawList &off = compiler.compile(state, 1024.0f, 768.0f);
		bool suppressed_label = false;
		for (const auto &lab : off.map.labels) {
			if (std::string(lab.text) == "1.02k") suppressed_label = true;
		}
		CHECK(!suppressed_label,
				"authored nonzero SPINMAPWPDISTOFF suppresses the label");
	}
	layout.spinmap_wp_dist_off = 0;
	compiler.update_layout(layout);
	// The M-cycle big map: mode 3 compiles the fullscreen north-up pass —
	// grid rules and water appear, the compass ring does not (mask 0xAF937 has
	// neither bit9 nor bit6), and the view centers on the design screen.
	// [orig: HUD_BuildMapOverlayView @0x5a7e10; HUD_CycleMapMode @0x520bc0]
	{
		HudFrameState big_state = state;
		big_state.minimap.map_mode = 3;
		big_state.minimap.player_x = 1035 << 16;
		big_state.minimap.player_heading_bam = 0x10000000;
		big_state.minimap.terrain.present = true;
		big_state.minimap.terrain.water_present = true;
		big_state.minimap.terrain.sector_count = 16;
		big_state.minimap.terrain.sector_rows = 16;
		big_state.minimap.terrain.sector_grid.fill(1);
		// A live-entity marker facing world-east (the -90-fold base): on the
		// north-up map its facing must be world-stable, not player-relative.
		big_state.minimap.markers.clear();
		opennova::hud::HudMinimapMarker east = high;
		east.x = big_state.minimap.player_x + (32 << 16);
		east.heading_bam = 0x40000000;
		big_state.minimap.markers.push_back(east);
		const HudDrawList &big = compiler.compile(big_state, 1024.0f, 768.0f);
		CHECK(big.map.visible, "the corner spinmap still compiles under a map mode");
		CHECK(big.big_map.visible, "mode 3 compiles the big-map pass");
		CHECK(!big.big_map.terrain.empty() &&
				big.big_map.terrain_water.size() == big.big_map.terrain.size(),
				"mode 3 redraws every clipped big-map terrain triangle with "
				"depthspin water");
		CHECK(std::fabs(big.big_map.center_x - 511.0f) < 2.0f &&
				std::fabs(big.big_map.center_y - 383.0f) < 2.0f,
				"the fullscreen pass centers on the design screen");
		bool big_has_compass = false;
		for (const auto &sprite : big.big_map.sprites) {
			if (sprite.texture == 1) big_has_compass = true;
		}
		CHECK(!big_has_compass, "the big map draws no compass ring (no bit6)");
		CHECK(big.big_map.lines_under.size() > 4,
				"the axis-aligned big map rules its 300-wu grid lines under "
				"the marker layer");
		// The blip rotates against the fixed north-up base, not the player
		// heading; the corner pass keeps the player-relative form.
		CHECK(!big.big_map.sprites.empty() &&
				std::fabs(big.big_map.sprites[0].rotation_rad) < 1e-4f,
				"north-up blips hold world-stable facing");
		CHECK(!big.map.sprites.empty() &&
				std::fabs(big.map.sprites[0].rotation_rad) > 1.0f,
				"corner blips stay player-relative");
		bool letter_label = false;
		bool player_column_letter = false;
		bool player_readout = false;
		for (const auto &lab : big.big_map.labels) {
			if (lab.text[0] >= 'A' && lab.text[0] <= 'Z' && lab.text[1] == 0) {
				letter_label = true;
				// The player stands at 1035 wu: the M-map gap containing that
				// point is lettered by the SAME -150-fold formula the
				// MAPCOORDS readout uses -> 'C'. [orig: the @0x5a5f40 grid
				// branch letters line_x - origin on the -150 lattice]
				if (std::string(lab.text) == "C") player_column_letter = true;
			}
			if (lab.text[0] == '(') {
				// The grid leg closes with the on-map player readout in the
				// large font: "(C,-1)" for this pose at origin zero.
				player_readout = std::string(lab.text) == "(C,-1)" &&
						lab.font == 1;
			}
		}
		CHECK(letter_label, "grid column letters ride the big-map label list");
		CHECK(player_column_letter,
				"the player's gap letter agrees with the MAPCOORDS formula");
		CHECK(player_readout,
				"the big map draws the large-font player readout");
		// Every grid label rides the LARGE slot.
		// [orig: HUD_DrawTextCentered_HalfBright(g_hudLabelFontLarge, ...)]
		bool all_large = true;
		for (const auto &lab : big.big_map.labels) {
			if (lab.font != 1) all_large = false;
		}
		CHECK(all_large, "big-map grid labels select the large font slot");
		// Mode 2: the 400x400 north-up window at (20,20).
		big_state.minimap.map_mode = 2;
		const HudDrawList &win = compiler.compile(big_state, 1024.0f, 768.0f);
		CHECK(win.big_map.visible &&
				std::fabs(win.big_map.center_x - 219.0f) < 2.0f &&
				std::fabs(win.big_map.center_y - 219.0f) < 2.0f,
				"mode 2 compiles the 20,20..419,419 window");
		CHECK(!win.big_map.terrain.empty() &&
				win.big_map.terrain_water.size() == win.big_map.terrain.size(),
				"mode 2 redraws every clipped big-map terrain triangle with "
				"depthspin water");

		HudFrameState dry_big_state = big_state;
		dry_big_state.minimap.map_mode = 3;
		dry_big_state.minimap.terrain.water_present = false;
		const HudDrawList &dry_big = compiler.compile(
				dry_big_state, 1024.0f, 768.0f);
		CHECK(!dry_big.big_map.terrain.empty() &&
				dry_big.big_map.terrain_water.empty(),
				"a big map without a depthspin mask emits no water layer");
	}

	// Special-bank slots whose lifetime floored to zero stay claimed in the
	// bank but the render walk skips them.
	// [orig: MapOverlay_RenderAllByLayer @0x5be794]
	{
		HudFrameState expiry_state = state;
		expiry_state.minimap.markers.clear();
		expiry_state.waypoint.present = false;
		opennova::hud::HudMinimapMarker live_special;
		live_special.flags = 0x40;
		live_special.icon = 12;
		live_special.remaining_ticks = 5;
		expiry_state.minimap.markers.push_back(live_special);
		opennova::hud::HudMinimapMarker expired = live_special;
		expired.remaining_ticks = 0;
		expiry_state.minimap.markers.push_back(expired);
		const HudDrawList &pass = compiler.compile(expiry_state, 1024.0f,
				768.0f);
		int special_sprites = 0;
		for (const auto &sprite : pass.map.sprites) {
			if (sprite.half_w == 6.0f && sprite.texture == 0) ++special_sprites;
		}
		CHECK(special_sprites == 1,
				"an expired special slot is skipped by the draw pass");
	}

	// Footprint markers skip the icon quad for their polygon feed; markers
	// on the upright policy hold angle zero regardless of heading.
	// [orig: the Building leg @0x597a84 -> render_collision_wireframe
	//  @0x596800; the upright branches of draw_minimap_blip @0x597890]
	{
		HudFrameState fp_state = state;
		fp_state.minimap.markers.clear();
		fp_state.waypoint.present = false;
		opennova::hud::HudMinimapMarker building;
		building.handle = 0x2042;
		building.icon = 0;
		building.entity_known = 1;
		building.footprint = 1;
		fp_state.minimap.markers.push_back(building);
		opennova::hud::HudMinimapFootprint footprint;
		footprint.handle = 0x2042;
		footprint.fill_argb = 0xFFA0A0A0u;
		footprint.fill_xy_q16 = {20 << 16, -(4 << 16), 24 << 16,
				-(4 << 16), 22 << 16, 4 << 16};
		footprint.edge_xy_q16 = {20 << 16, -(4 << 16), 24 << 16, -(4 << 16)};
		opennova::hud::hud_minimap_finalize_footprint(footprint);
		fp_state.map_footprints.push_back(footprint);
		opennova::hud::HudMinimapMarker upright;
		upright.icon = 13;
		upright.x = -(10 << 16);
		upright.heading_bam = 0x30000000;
		upright.entity_known = 1;
		upright.rotate = 0;
		fp_state.minimap.markers.push_back(upright);
		const HudDrawList &fp_list = compiler.compile(fp_state, 1024.0f,
				768.0f);
		CHECK(!fp_list.map.overlays.empty(),
				"a footprint marker fills its polygon feed");
		CHECK(fp_list.map.sprites.size() == 2,
				"the footprint marker draws no icon sprite (badge + compass remain)");
		bool found_upright = false;
		for (const auto &sprite : fp_list.map.sprites) {
			if (sprite.texture == 0 &&
					std::fabs(sprite.rotation_rad) < 1e-4f) {
				found_upright = true;
				const float strip_w = 64.0f;
				const float strip_h = 1920.0f;
				CHECK(std::fabs(sprite.u0 - 0.5f / strip_w) < 1e-6f &&
						std::fabs(sprite.u1 - 64.5f / strip_w) < 1e-6f &&
						std::fabs(sprite.v0 - (13.0f * 64.0f + 0.5f) /
								strip_h) < 1e-6f &&
						std::fabs(sprite.v1 - (14.0f * 64.0f + 0.5f) /
								strip_h) < 1e-6f,
						"armory cell 13 uses retail's half-texel strip UVs");
			}
		}
		CHECK(found_upright, "an upright-policy marker holds angle zero");
		bool found_edge = false;
		for (const auto &line : fp_list.map.lines_under) {
			if (line.color == 0x80000000u) found_edge = true;
		}
		CHECK(!found_edge,
				"the completed retail footprint pass leaves no observable "
				"black silhouette stroke");
		// The half-texel insets ride the loaded strip's PHYSICAL size: a
		// stock JO install mounts the 16x480 strip, and the same cell must
		// derive its bounds from those dimensions. (compile() reuses the
		// internal list, so this re-compile ends fp_list's scope of use.)
		// [orig: render_tiled_image_strip @0x67b540 — 0.5 / tile_dim]
		fp_state.minimap.icon_strip_w_px = 16.0f;
		fp_state.minimap.icon_strip_h_px = 480.0f;
		const HudDrawList &stock_list = compiler.compile(fp_state, 1024.0f,
				768.0f);
		bool found_stock_upright = false;
		for (const auto &sprite : stock_list.map.sprites) {
			if (sprite.texture == 0 &&
					std::fabs(sprite.rotation_rad) < 1e-4f) {
				found_stock_upright = true;
				CHECK(std::fabs(sprite.u0 - 0.5f / 16.0f) < 1e-6f &&
						std::fabs(sprite.u1 - 16.5f / 16.0f) < 1e-6f &&
						std::fabs(sprite.v0 - (13.0f * 16.0f + 0.5f) /
								480.0f) < 1e-6f &&
						std::fabs(sprite.v1 - (14.0f * 16.0f + 0.5f) /
								480.0f) < 1e-6f,
						"the stock 16x480 strip derives its own half-texel UVs");
			}
		}
		CHECK(found_stock_upright,
				"the stock-dimension compile keeps the upright badge");
	}

	// The grid label compiles at the authored MAPCOORDS position once its
	// suppressor clears; the column letters ride the base-26 formatter.
	// [orig: HUD_DrawPlayerGridLabel @0x59cb40; HUD_FormatGridCoordinate @0x598600]
	layout.map_coords_x = 530.0f;
	layout.map_coords_y = 720.0f;
	layout.map_coords_off = 0;
	compiler.update_layout(layout);
	{
		HudFrameState grid_state = state;
		grid_state.minimap.player_x = 1035 << 16; // (1035-150)/300 -> C
		grid_state.minimap.player_y = -(180 << 16);
		const HudDrawList &grid = compiler.compile(grid_state, 1024.0f,
				768.0f);
		bool found = false;
		for (const auto &lab : grid.map.labels) {
			if (std::string(lab.text) == "(C,-1)" && lab.align == 1) {
				found = true;
			}
		}
		CHECK(found, "the grid label formats (C,-1) at origin zero, right-aligned");
	}
}

// Friendly tags (D-HUD-20) [orig: HUD_DrawEntityLabel @ 0x5a39b0]: the
// witnessed gates, tier colors, distance alpha, fallback name, medic plate,
// and the BRIEF tick form, over the default (retail) tag colors.
void test_compiler_friendly_tags(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24; // the fallback table's "SGT  Brown"
	state.friendly_tags.push_back(tag);

	// FULL (the boot default): centered half-bright text of the fallback name.
	const HudDrawList &full = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(full.glyphs.size() == 11,
			"the fallback '^SGT  Brown' lays out 11 glyphs");
	// 100 m -> alpha 255 - 192*50/250 = 217; good tier = tagcolor_good with
	// the alpha-preserving half-bright fold.
	const uint32_t good = (217u << 24) | (layout.tag_good & 0xFFFFFFu);
	CHECK(!full.glyphs.empty() &&
					full.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(good),
			"good tier rides tagcolor_good + the distance alpha");
	// Centered on x=200: 11 glyphs at advance 9 minus the trailing pad = 98
	// wide -> cursor 151 -> the -0.5 vertex offset.
	CHECK(!full.glyphs.empty() && full.glyphs[0].x_top_left == 150.5f,
			"the text centers on the projected x");
	CHECK(full.quads.empty(), "no medic plate without the medic flag");

	// The health tiers swap the color.
	state.friendly_tags[0].health_ratio_fp16 = 0x8000;
	const HudDrawList &mid = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t middle = (217u << 24) | (layout.tag_middle & 0xFFFFFFu);
	CHECK(!mid.glyphs.empty() &&
					mid.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(middle),
			"the middle tier rides tagcolor_middle");
	state.friendly_tags[0].health_ratio_fp16 = 0x10000;

	// The medic plate: white square + the two red cross bars, left of the text.
	state.friendly_tags[0].medic = true;
	const HudDrawList &medic = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(medic.quads.size() == 3, "the medic plate is three untextured quads");
	if (medic.quads.size() == 3) {
		CHECK(medic.quads[0].color == ((217u << 24) | 0xFFFFFFu),
				"the plate is white at the tag alpha");
		CHECK(medic.quads[1].color == ((217u << 24) | 0xFF0000u),
				"the cross bars are red at the tag alpha");
		// left = 200 - (98/2 + 16) - 0.5, a fontH/2 = 8 px square at the text top.
		CHECK(medic.quads[0].x0 == 134.5f && medic.quads[0].y0 == 91.5f,
				"the plate sits one fontH left of the text");
		CHECK(medic.quads[0].x1 - medic.quads[0].x0 == 8.0f,
				"the plate is a fontH/2 square");
	}
	state.friendly_tags[0].medic = false;

	// The DOWNED legs of the bad tier [orig: @0x5a3dc9..0x5a3e85]: dead with
	// a slot inside its revive window -> table[3] light blue, the name text
	// gains ": <seconds>" [orig: "%s: %ld" @0x5a400e]; a standing medic
	// request pulses the color toward white on the 64-frame triangle
	// [orig: @0x5a3dfb..0x5a3e6d]; dead without a slot -> table[8] gray.
	state.friendly_tags[0].health_ratio_fp16 = 0;
	state.friendly_tags[0].dead = true;
	state.friendly_tags[0].has_slot = true;
	state.friendly_tags[0].revive_seconds = 87;
	state.ticks = 8; // t = 0 -> no lift
	const HudDrawList &downed = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t light_blue =
			(217u << 24) | (opennova::hud::kFriendlyTagDownedLightBlue & 0xFFFFFFu);
	CHECK(!downed.glyphs.empty() &&
					downed.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(light_blue),
			"dead + slot + revive window rides table[3] light blue");
	CHECK(downed.glyphs.size() == 11 + 4,
			"the text form appends ': 87' to the name");
	state.friendly_tags[0].medic_request = true;
	state.ticks = 8 + 32; // t = 0x20 -> full lift = white
	const HudDrawList &pulse_peak = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!pulse_peak.glyphs.empty() &&
					pulse_peak.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(
									(217u << 24) | 0xFFFFFFu),
			"a medic request pulses to white at the triangle's peak");
	state.ticks = 8;
	const HudDrawList &pulse_base = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!pulse_base.glyphs.empty() &&
					pulse_base.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(light_blue),
			"the pulse returns to table[3] at the triangle's base");
	CHECK(opennova::hud::friendly_tag_revive_pulse(
					opennova::hud::kFriendlyTagDownedLightBlue, 8 + 47) ==
					opennova::hud::friendly_tag_revive_pulse(
							opennova::hud::kFriendlyTagDownedLightBlue, 8 + 16),
			"the triangle wave mirrors t -> 0x3F - t past the peak");
	state.friendly_tags[0].medic_request = false;
	state.friendly_tags[0].has_slot = false;
	const HudDrawList &gray = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t downed_gray =
			(217u << 24) | (opennova::hud::kFriendlyTagDownedGray & 0xFFFFFFu);
	CHECK(!gray.glyphs.empty() &&
					gray.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(downed_gray),
			"dead without a slot rides table[8] gray");
	CHECK(gray.glyphs.size() == 11, "no slot, no count");
	// The BRIEF ticks draw the bare count one fontH above the point
	// [orig: "%ld" @0x5a41f0 -> HUD_DrawTextHalfBrightF(x, y - fontH) @0x5a4453].
	state.friendly_tags[0].has_slot = true;
	state.friendly_tag_mode = 3;
	const HudDrawList &brief_count = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(brief_count.lines.size() == 3 && brief_count.glyphs.size() == 2,
			"BRIEF draws the ticks plus the bare two-digit count");
	CHECK(!brief_count.glyphs.empty() &&
					brief_count.glyphs[0].y_top < 100.0f - 8.0f,
			"the bare count sits a fontH above the projected point");
	state.friendly_tag_mode = 2;
	state.friendly_tags[0].dead = false;
	state.friendly_tags[0].has_slot = false;
	state.friendly_tags[0].revive_seconds = 0;
	state.friendly_tags[0].health_ratio_fp16 = 0x10000;
	state.ticks = 0;

	// Gates: too close, fogged, FARBRIEF far, OFF.
	state.friendly_tags[0].dist_q16 = 0x4000;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"under 0.5 u nothing draws");
	state.friendly_tags[0].dist_q16 = 100 << 16;
	state.fog_dist_q16 = 50 << 16;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"the fog cull hides the tag");
	state.fog_dist_q16 = INT32_MAX;
	state.friendly_tag_mode = 1;
	state.friendly_tags[0].dist_q16 = 400 << 16;
	state.fog_dist_q16 = 1000 << 16;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"FARBRIEF draws nothing past 300 m");
	state.friendly_tags[0].dist_q16 = 100 << 16;
	CHECK(!compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"FARBRIEF draws text under 300 m");
	state.friendly_tag_mode = 0;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"OFF draws nothing");

	// BRIEF: the three 1-px tick lines replace the text.
	state.friendly_tag_mode = 3;
	const HudDrawList &brief = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(brief.glyphs.empty(), "BRIEF draws no text");
	CHECK(brief.lines.size() == 3, "BRIEF draws the three tick lines");
	if (brief.lines.size() == 3) {
		CHECK(brief.lines[1].x0 == 200.0f && brief.lines[1].y0 == 96.0f &&
						brief.lines[1].y1 == 104.0f,
				"the tick spans +-fontH/4 around the projected point");
	}

	// A slot entry with an empty callsign draws the single bar.
	state.friendly_tag_mode = 2;
	state.friendly_tags[0].player = true;
	state.friendly_tags[0].name.clear();
	const HudDrawList &bar = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(bar.glyphs.empty() && bar.lines.size() == 1,
			"an empty player callsign draws the bar form");

	// The speaking pulse lifts the color.
	state.friendly_tags[0].player = false;
	state.friendly_tags[0].speaking = true;
	state.speaking_level255 = 255;
	const HudDrawList &speak = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t pulsed = (217u << 24) |
			(opennova::hud::friendly_tag_speaking_blend(
					 layout.tag_good, 255) &
					0xFFFFFFu);
	CHECK(!speak.glyphs.empty() &&
					speak.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(pulsed),
			"the speaking tag pulses toward white");
}

// The overlay label fonts: friendly tags draw with the NORMAL label font,
// attach labels with the BOLD one, both at the slot scale, each in its own
// draw-list page namespace. [orig: HUD_InitAllFonts @ 0x51ee20; tag font
// g_hudLabelFont @ 0x5a3a0c; attach font (the bold slot) @ 0x5a3680; the slot
// scales enter the draw/measure/char-height helpers @ 0x580680/@ 0x580ab0/
// @ 0x580a80]
void test_compiler_label_fonts(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 2.0f, 2.0f);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24; // "^SGT  Brown", 11 glyphs
	state.friendly_tags.push_back(tag);
	opennova::hud::HudAttachLabel label;
	label.screen_x = 300.0f;
	label.screen_y = 60.0f;
	label.text = "Sit";
	state.attach_labels.push_back(label);

	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	size_t normal_glyphs = 0;
	size_t bold_glyphs = 0;
	const opennova::hud::GameFontQuad *first_tag_glyph = nullptr;
	for (const opennova::hud::GameFontQuad &g : list.glyphs) {
		if (g.page ==
				static_cast<uint32_t>(opennova::hud::kHudFontSlotLabel) *
						FNT_MAX_PAGES) {
			if (first_tag_glyph == nullptr) {
				first_tag_glyph = &g;
			}
			++normal_glyphs;
		}
		if (g.page ==
				static_cast<uint32_t>(opennova::hud::kHudFontSlotLabelBold) *
						FNT_MAX_PAGES) {
			++bold_glyphs;
		}
	}
	CHECK(normal_glyphs == 11,
			"tag glyphs ride the normal label font's page namespace");
	CHECK(bold_glyphs == 3,
			"attach glyphs ride the bold label font's page namespace");
	// Scale 2: the 98-wide line centers as 200 - 98 -> 101.5 after the -0.5
	// offset; fontH = 16*2, text top = y - fontH/2 -> 83.5.
	CHECK(first_tag_glyph != nullptr &&
					first_tag_glyph->x_top_left == 101.5f &&
					first_tag_glyph->y_top == 83.5f,
			"the slot scale doubles the centered layout metrics");

	// Null label fonts fall back to the hudpos font at scale 1 (page 0).
	compiler.configure_label_fonts(nullptr, nullptr, nullptr, 1.0f, 1.0f);
	const HudDrawList &fallback = compiler.compile(state, 1024.0f, 768.0f);
	bool all_page0 = !fallback.glyphs.empty();
	for (const opennova::hud::GameFontQuad &g : fallback.glyphs) {
		all_page0 = all_page0 && g.page == 0;
	}
	CHECK(all_page0, "absent label fonts fall back to the hudpos font");
}

} // namespace

// The stdbox panel geometry: pieces and the fill inset scale with the surface,
// the fill tiles at a fixed 32 px screen period off the BORDER atlas's own
// cell (3,0), and the bottom row is cropped rather than squashed.
// [orig: render_hud_box_overlay @0x56b700; flt_7CFE3C = 0.000625; rec+0x180/+0x184;
//  the bottom-row crop flag1 @0x56b456 with flt_7C459C = 0.9]
void test_compiler_stdbox_geometry(const fnt_font_t *font) {
	using opennova::hud::HudQuad;
	HudFrameCompiler compiler;
	HudLayout layout;
	// The shipped border.tga is a 128 px 4x4 grid, so one source cell is 32 px.
	layout.box_texture_valid = true;
	layout.box_tex_w = 128;
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 100;
	state.scoreboard.shown = true;

	// Collect the panel's quads at two surface widths.
	auto box_quads = [&](float surface_w, float surface_h) {
		std::vector<HudQuad> out;
		const HudDrawList &list = compiler.compile(state, surface_w, surface_h);
		for (const HudQuad &q : list.quads) {
			if (q.texture == opennova::hud::kHudTexBoxBorder ||
					q.texture == opennova::hud::kHudTexBoxTile) {
				out.push_back(q);
			}
		}
		return out;
	};

	const std::vector<HudQuad> wide = box_quads(1600.0f, 1200.0f);
	CHECK(!wide.empty(), "the stdbox panel emits quads when its atlas loaded");

	// The fill comes off the BORDER atlas's cell (3,0) — boxtile.tga is a flat
	// camo sheet with no cell grid, so sampling a quarter-rect of it is wrong.
	bool any_tile_slot = false;
	for (const HudQuad &q : wide) {
		if (q.texture == opennova::hud::kHudTexBoxTile) any_tile_slot = true;
	}
	CHECK(!any_tile_slot,
			"every stdbox quad samples the border atlas, fill included");

	// The eight border pieces are the quads whose UV cell is not (3,0).
	// At surface 1600 the scale is exactly 1, so a piece is one full 32 px cell.
	auto corner_tl = [](const std::vector<HudQuad> &qs) {
		for (const HudQuad &q : qs) {
			if (q.u0 < 0.01f && q.v0 < 0.01f) return q;
		}
		return HudQuad{};
	};
	const HudQuad tl_wide = corner_tl(wide);
	CHECK(std::fabs((tl_wide.x1 - tl_wide.x0) - 32.0f) < 0.01f,
			"at surface 1600 the scale is 1, so a piece is one 32 px cell");

	// Halve the surface and the piece halves with it — the old hardcoded 32
	// stayed put, which is what this pins.
	const std::vector<HudQuad> narrow = box_quads(800.0f, 600.0f);
	const HudQuad tl_narrow = corner_tl(narrow);
	CHECK(std::fabs((tl_narrow.x1 - tl_narrow.x0) - 16.0f) < 0.01f,
			"surface 800 -> s = 0.5 -> a 16 px piece");

	// The bottom-left piece keeps its top 90% in BOTH the destination and the
	// source cell: cropped, not squashed.
	constexpr float kCell = 1.0f / 4.0f;
	bool saw_cropped = false;
	for (const HudQuad &q : wide) {
		const bool bottom_row = std::fabs(q.v0 - 2.0f * kCell) < 0.001f;
		const bool left_col = q.u0 < 0.01f;
		if (!bottom_row || !left_col) continue;
		saw_cropped = true;
		CHECK(std::fabs((q.v1 - q.v0) - kCell * 0.9f) < 0.001f,
				"the bottom row samples only the top 90% of its cell");
		CHECK(std::fabs((q.y1 - q.y0) - 32.0f * 0.9f) < 0.01f,
				"and draws into a correspondingly shorter rect");
	}
	CHECK(saw_cropped, "the bottom-left corner piece is emitted");

	// The fill tiles at the cell's own 32 px period, anchored to the ABSOLUTE
	// screen grid — retail's fill is one wrap-addressed quad, so every tile
	// boundary sits on a multiple of 32 regardless of where the inset rect
	// starts [orig: stdbox_draw_fill_wrap_tiled @0x56b5d0, UV=(dest+0.5)/32].
	std::vector<HudQuad> fill;
	float fill_min_x = 1e9f;
	float fill_max_x = -1e9f;
	for (const HudQuad &q : wide) {
		if (q.u0 > 3.0f * kCell - 0.01f) {
			fill.push_back(q);
			fill_min_x = std::min(fill_min_x, q.x0);
			fill_max_x = std::max(fill_max_x, q.x1);
		}
	}
	CHECK(fill.size() > 2, "the interior is tiled, not one stretched quad");
	int full_tiles = 0;
	bool grid_ok = true;
	bool slice_ok = true;
	for (const HudQuad &q : fill) {
		// Every edge is either the inset rect's edge or a 32-grid boundary.
		const auto on_grid = [](float v) {
			return std::fabs(v - std::floor(v / 32.0f + 0.5f) * 32.0f) < 0.01f;
		};
		if (!(on_grid(q.x0) || std::fabs(q.x0 - fill_min_x) < 0.01f)) grid_ok = false;
		if (!(on_grid(q.x1) || std::fabs(q.x1 - fill_max_x) < 0.01f)) grid_ok = false;
		// Every tile — clipped on either side — samples the slice of the
		// cell matching its screen-grid phase.
		const float frac = (q.x1 - q.x0) / 32.0f;
		if (std::fabs((q.u1 - q.u0) - kCell * frac) > 0.001f) slice_ok = false;
		if (std::fabs((q.x1 - q.x0) - 32.0f) < 0.01f) ++full_tiles;
	}
	CHECK(grid_ok, "every fill tile edge sits on the 32 px screen grid or the inset edge");
	CHECK(slice_ok, "every tile samples the cell slice matching its grid phase");
	CHECK(full_tiles > 0, "the interior contains full-period tiles");

	// The TITLED top row: a title swaps the top edge for the row-3 cells —
	// the stub, the title bar stretched to the measured gap, the end cap —
	// and the plain top edge resumes after [orig: the outTechnique arm
	// @0x56b93d-0x56ba52; the gap rule @0x51f0ea-0x51f114].
	state.scoreboard.title = "TEST";
	const std::vector<HudQuad> titled = box_quads(1600.0f, 1200.0f);
	GameFont measure_font;
	measure_font.set_font(font);
	int title_w = 0;
	int title_h = 0;
	measure_font.measure("TEST", 1.0f, 1.0f, &title_w, &title_h);
	float expected_gap = static_cast<float>(title_w) + 2.0f;
	if (expected_gap > 12.0f) expected_gap -= 12.0f; // s = 1 at surface 1600
	bool saw_stub = false;
	bool saw_bar = false;
	bool saw_cap = false;
	bool saw_plain_tl = false;
	for (const HudQuad &q : titled) {
		const bool row3 = std::fabs(q.v0 - 3.0f * kCell) < 0.001f;
		if (row3 && q.u0 < 0.01f) saw_stub = true;
		if (row3 && std::fabs(q.u0 - kCell) < 0.001f) {
			saw_bar = true;
			CHECK(std::fabs((q.x1 - q.x0) - expected_gap) < 0.01f,
					"the title bar stretches to the measured gap");
		}
		if (row3 && std::fabs(q.u0 - 2.0f * kCell) < 0.001f) saw_cap = true;
		if (q.u0 < 0.01f && q.v0 < 0.01f) saw_plain_tl = true;
	}
	CHECK(saw_stub && saw_bar && saw_cap,
			"the titled top row draws the row-3 stub/bar/cap cells");
	CHECK(!saw_plain_tl, "the plain (0,0) corner cell yields to the titled row");
	state.scoreboard.title.clear();
}

// The row walk's geometry: the spectator column is seeded below the LONGER
// player column plus two spacer rows, and the connection icon draws for every
// row with a live band — spectators included, quality outside 1..3 never
// [orig: the seed @0x423c3a; the icon block @0x4241e2-0x424244;
//  NetIcon_DrawConnectionQualityBand @0x4c2ee0].
void test_compiler_scoreboard_rows(const fnt_font_t *font) {
	using opennova::hud::HudQuad;
	using opennova::hud::ScoreboardEntry;
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.net_icon_texture_valid = true;
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 100;
	state.scoreboard.shown = true;
	state.scoreboard.game_type = 0x10000; // TDM — team columns
	const auto row = [](uint8_t slot, uint8_t team, bool spec, uint8_t quality) {
		ScoreboardEntry e;
		e.slot_id = slot;
		e.team = team;
		e.spectator = spec;
		e.has_entity = !spec;
		e.name = "P";
		e.quality = quality;
		return e;
	};
	state.scoreboard.rows.push_back(row(1, 1, false, 1));
	state.scoreboard.rows.push_back(row(2, 2, false, 3));
	state.scoreboard.rows.push_back(row(3, 1, false, 4)); // quality 4: no icon
	state.scoreboard.rows.push_back(row(4, 0, true, 2));  // spectator, icon
	// A team-mode row without a live entity vanishes entirely
	// [orig: the entity-null fallthrough @0x423d1b].
	ScoreboardEntry leaver = row(5, 2, false, 2);
	leaver.has_entity = false;
	state.scoreboard.rows.push_back(leaver);

	// 1024x768 keeps the design->output scale at identity.
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	std::vector<HudQuad> icons;
	for (const HudQuad &q : list.quads) {
		if (q.texture == opennova::hud::kHudTexNetIcon) icons.push_back(q);
	}
	// Rows with quality 1..3 draw an icon (the spectator too); quality 4 and
	// the vanished leaver do not.
	CHECK(icons.size() == 3, "three rows carry a drawable quality band");
	// The header has four rungs (no spectator line fed), so the list base is
	// 105 + 4*20 + 20 = 205 and the first row of each column sits at 223.
	// count_a = 2, count_b = 1 (the leaver dropped), spectators = 1 ->
	// the spectator cursor seeds at 205 + 18*(2+2), first row at 295.
	bool saw_team_first = false;
	bool saw_spec = false;
	for (const HudQuad &q : icons) {
		if (std::fabs(q.y0 - 223.0f) < 0.75f) saw_team_first = true;
		if (std::fabs(q.y0 - 295.0f) < 0.75f && std::fabs(q.x0 - 420.0f) < 0.75f)
			saw_spec = true;
	}
	CHECK(saw_team_first, "the first team rows sit one pitch below the list base");
	CHECK(saw_spec,
			"the spectator row seeds below the longer column plus two spacers");
	// The band is a quarter of the 4-row strip.
	CHECK(std::fabs((icons[0].v1 - icons[0].v0) - 0.25f) < 0.001f,
			"an icon samples exactly one band of the strip");
}

// The hudpos StaticFrame pick: retail keeps one static frame and each authored
// line overwrites the last, so the LAST entry draws. Our parser keeps them all
// (writer round-trip), which makes this the consumer-side half of that contract.
// [orig: HUD_ParseHudposToken @0x59F370 - the name copy @0x5a0a4e-0x5a0a62]
void test_static_frame_pick() {
	using opennova::hud::hud_static_frame_index;
	CHECK(hud_static_frame_index(0) == -1,
			"nothing authored draws no static frame");
	CHECK(hud_static_frame_index(1) == 0,
			"a single authored frame is the one that draws");
	CHECK(hud_static_frame_index(2) == 1,
			"the LAST authored frame wins, not the first");
	CHECK(hud_static_frame_index(5) == 4,
			"the rule is last-wins for any count");
}

// The mounted-vehicle panel element: what draws, in what order, and the
// occupied/empty split. [orig: HUD_DrawVehicleHealthBars @0x5A4FD0]
void test_vehicle_panel_element(const fnt_font_t *font) {
	using opennova::hud::HudVehicleSeat;
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 10;
	// Hidden by default: a player on foot must draw no panel at all.
	{
		const HudDrawList &none = compiler.compile(state, 1024.0f, 768.0f);
		size_t panel_quads = 0;
		for (const auto &q : none.quads)
			if (q.texture == opennova::hud::kHudTexVehiclePanel) ++panel_quads;
		CHECK(panel_quads == 0, "no panel while not mounted");
	}

	auto &vp = state.vehicle_panel;
	vp.shown = true;
	vp.anchor_x = 300; vp.anchor_y = 400;
	vp.stance_offset_x = -12; vp.stance_offset_y = 6;
	vp.hull_health = 100; vp.hull_max_health = 100;
	// No silhouette texture: the seats must still draw.
	vp.silhouette_valid = false;
	HudVehicleSeat driver; driver.x = 5; driver.y = 7;
	driver.occupied = true; driver.health = 100; driver.max_health = 100;
	driver.own_seat = true;
	HudVehicleSeat empty; empty.x = 40; empty.y = 7; empty.label = "2";
	vp.seats.push_back(driver);
	vp.seats.push_back(empty);

	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Exactly ONE filled seat box -- the occupied seat. The empty seat draws a
	// digit, not a box.
	size_t filled = 0;
	for (const auto &q : list.quads)
		if (q.filled && q.texture == opennova::hud::kHudTexNone) ++filled;
	CHECK(filled >= 1, "the occupied seat draws a filled box");

	// A missing silhouette texture emits no panel quad but does not suppress
	// the seats -- the panel degrades rather than disappearing.
	size_t panel_quads = 0;
	for (const auto &q : list.quads)
		if (q.texture == opennova::hud::kHudTexVehiclePanel) ++panel_quads;
	CHECK(panel_quads == 0, "no silhouette quad without its texture");

	// The digit and the X are CENTRED on their boxes: the text's top sits
	// above the box centre by half the label height (centre + 1 - h/2), not
	// AT the centre with the glyph hanging below it
	// [orig: @0x5a52f0..0x5a5322 -- HUD_MeasureTextWH then
	//  HUD_DrawTextCentered_HalfBright(&g_hudLabelFontBold, cx, cy + 1 - h/2)].
	// Both labels here share centre y 418 (base 406 + seat y 7 + 11/2).
	{
		opennova::hud::GameFont gf;
		gf.set_font(font);
		int tw = 0, th = 0;
		gf.measure("2", 1.0f, 1.0f, &tw, &th);
		CHECK(!list.glyphs.empty(), "the empty seat prints its digit");
		float min_top = 1e9f;
		for (const auto &g : list.glyphs) min_top = std::min(min_top, g.y_top);
		CHECK(min_top <= 419.0f - static_cast<float>(th / 2) + 0.01f,
				"the label's top rises half its height above the box centre");
		CHECK(min_top > 418.0f - static_cast<float>(th),
				"and no further than its own height");
	}

	// With the texture present the silhouette lands at the STANCE-SHIFTED
	// base (300-12, 400+6), not the raw anchor.
	vp.silhouette_valid = true;
	vp.silhouette_w = 64; vp.silhouette_h = 32;
	const HudDrawList &l2 = compiler.compile(state, 1024.0f, 768.0f);
	bool found = false;
	for (const auto &q : l2.quads) {
		if (q.texture != opennova::hud::kHudTexVehiclePanel) continue;
		found = true;
		CHECK(std::fabs(q.x0 - 288.0f) < 1.0f, "silhouette x rides the stance offset");
		CHECK(std::fabs(q.y0 - 406.0f) < 1.0f, "silhouette y rides the stance offset");
	}
	CHECK(found, "the silhouette draws when its texture is present");
}

// The Recent Messages (J) window: both rings listed in one titled stdbox,
// sixteen rows per column, NO expiry gate — a line that has faded off the
// feed is still listed. [orig: HUD_DrawMessageLog @0x5b9d70]
void test_message_log_element(const fnt_font_t *font) {
	using opennova::hud::HudQuad;
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.box_texture_valid = true;
	layout.box_tex_w = 128;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);

	compiler.push_chat_line("alpha: hi", 0xFF80A0FFu, 0);
	compiler.push_feed_line("alpha killed beta", 0xFFAFAFAFu, 0);
	HudFrameState state;
	state.ticks = 5000; // both lines are long expired on the feed
	state.hud_detail_level = 0;

	// Hidden: no box, no glyphs from either ring.
	{
		const HudDrawList &none = compiler.compile(state, 1024.0f, 768.0f);
		size_t box = 0;
		for (const HudQuad &q : none.quads)
			if (q.texture == opennova::hud::kHudTexBoxBorder) ++box;
		CHECK(box == 0, "no window box while the toggle is off");
		CHECK(none.glyphs.empty(), "expired lines draw nothing on the feed");
	}
	state.message_log_shown = true;
	state.message_log_title = "Recent Messages";
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	size_t box = 0;
	for (const HudQuad &q : list.quads)
		if (q.texture == opennova::hud::kHudTexBoxBorder) ++box;
	CHECK(box > 0, "the window draws its stdbox");
	// The expired lines are LISTED here even though the feed dropped them:
	// glyphs in the chat line's colour AND the system line's colour.
	size_t chat_glyphs = 0, sys_glyphs = 0;
	// The chat column sits at 32*w/1024 + 2 = 34 px; the system column is
	// right-aligned at 990 px, so its glyphs end left of 990.
	float chat_x = 1e9f, sys_right = -1e9f;
	for (const auto &g : list.glyphs) {
		if (g.color == 0xFF80A0FFu) {
			++chat_glyphs;
			chat_x = std::min(chat_x, g.x_top_left);
		}
		if (g.color == 0xFFAFAFAFu) {
			++sys_glyphs;
			sys_right = std::max(sys_right, g.x_top_left);
		}
	}
	CHECK(chat_glyphs > 0 && sys_glyphs > 0,
			"both rings list their lines with no expiry gate");
	CHECK(std::fabs(chat_x - 33.5f) < 1.0f, "the chat column starts at 32*w/1024 + 2");
	CHECK(sys_right < 990.0f && sys_right > 900.0f,
			"the system column is right-aligned at 990*w/1024");
	// One line per ring: both land in the BOTTOM row (row 15), the top rows
	// blank [orig: the walk paints slot 16 first; a short history pads at the top].
	const float expected_y = static_cast<float>(
			opennova::hud::message_log_text_top_px(1024) +
			15 * opennova::hud::message_log_step_px(1024));
	float chat_y = -1.0f;
	for (const auto &g : list.glyphs)
		if (g.color == 0xFF80A0FFu) chat_y = g.y_top;
	CHECK(std::fabs(chat_y - (expected_y - 0.5f)) < 1.0f,
			"a single line sits in the bottom row, not the top");
}

// The CHAT feed loop: three newest lines at HUDCHATTEXT, alpha folded from the
// remaining life (255 * timer / 186, clamped), skipped at zero
// [orig: HUD_DrawConsoleMessages @0x59ad30, the first loop @0x59adbc..0x59ae2e].
void test_chat_feed_loop(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.chat_text = {5, 40, 0, 0, true};
	compiler.configure(layout, font);

	compiler.push_chat_line("a", 0xFF102030u, 0);   // expires at 930
	HudFrameState state;
	// Fresh: 930 ticks left -> alpha clamps to 255.
	state.ticks = 0;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.size() == 1, "a fresh chat line draws");
		CHECK(!list.glyphs.empty() && (list.glyphs[0].color >> 24) == 0xFFu,
				"930 ticks of life clamp the alpha at 255");
		CHECK(!list.glyphs.empty() && (list.glyphs[0].color & 0xFFFFFFu) == 0x102030u,
				"the stored RGB survives the alpha fold");
	}
	// 93 ticks left -> 255 * 93 / 186 = 127.
	state.ticks = 930 - 93;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.size() == 1 && (list.glyphs[0].color >> 24) == 127u,
				"the last 186 ticks ramp the alpha down");
	}
	// Expired -> skipped, and no rung consumed.
	state.ticks = 930;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.empty(), "an expired chat line draws nothing");
	}
	// Only the newest three of the ring show, oldest at the anchor.
	for (int i = 0; i < 5; ++i) compiler.push_chat_line("b", 0xFFFFFFFFu, 2000);
	state.ticks = 2000;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.size() == 3, "three chat rows are walked");
	}
}

// The AAS zone status panel element: the first contested zone seeds the
// walk, groups step down by team, markers step across right-anchored, and the
// group's status text follows its markers.
// [orig: HUD_DrawZoneStatusPanel @0x5a2480; HUD_DrawZoneMarker @0x5986f0]
void test_lfp_panel_element(const fnt_font_t *font) {
	using opennova::hud::HudLfpZone;
	using opennova::hud::HudQuad;
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.lfp_anchor_present = true;
	layout.lfp_anchor_x = 1020;
	layout.lfp_anchor_y = 27;
	layout.lfp_icon_texture_valid = {true, true, true};
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);

	HudFrameState state;
	auto &lp = state.lfp_panel;
	lp.shown = true;
	lp.local_team = 1;
	lp.frame_counter = 0; // phase A
	const auto zone = [](int letter, int team, bool contested) {
		HudLfpZone z;
		z.letter_index = letter;
		z.team = team;
		z.timer_present = true;
		z.timer_team = team;
		z.capture_flags = contested ? 0x40 : 0;
		return z;
	};
	// Nothing contested -> nothing drawn at all.
	lp.zones = {zone(0, 1, false), zone(1, 2, false)};
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		size_t icons = 0;
		for (const HudQuad &q : list.quads)
			if (q.texture >= opennova::hud::kHudTexLfpTeam1 &&
					q.texture <= opennova::hud::kHudTexLfpNeutral) ++icons;
		CHECK(icons == 0, "no contested zone seeds the walk: the panel is empty");
	}
	// Team 1 holds A and B (B under attack), team 2 holds C (ready).
	HudLfpZone a = zone(0, 1, true);
	HudLfpZone b = zone(1, 1, true);
	b.rate = -3; // own point draining -> UNDER ATTACK
	HudLfpZone c = zone(2, 2, true);
	c.control = 0; // enemy point at zero control -> READY
	lp.zones = {a, b, c};
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	std::vector<HudQuad> icons;
	for (const HudQuad &q : list.quads)
		if (q.texture >= opennova::hud::kHudTexLfpTeam1 &&
				q.texture <= opennova::hud::kHudTexLfpNeutral) icons.push_back(q);
	CHECK(icons.size() == 3, "one icon per contested zone");
	if (icons.size() == 3) {
		// Group 1 (two zones) is right-anchored: A at 1020 - 196, B at 1020 - 98,
		// both ON the anchor row (the +12 belongs to the status text only);
		// group 2 (one zone) at 1020 - 98, one row (86) down.
		CHECK(std::fabs(icons[0].x0 - (1020.0f - 196.0f)) < 0.01f, "A right-anchors the group");
		CHECK(std::fabs(icons[1].x0 - (1020.0f - 98.0f)) < 0.01f, "B steps 98 across");
		CHECK(std::fabs(icons[0].y0 - 27.0f) < 0.01f, "the first row IS the anchor");
		CHECK(std::fabs(icons[2].y0 - (27.0f + 86.0f)) < 0.01f, "the next team steps 86 down");
		CHECK(std::fabs(icons[2].x0 - (1020.0f - 98.0f)) < 0.01f, "a one-zone group ends at the same edge");
		CHECK(icons[0].texture == opennova::hud::kHudTexLfpTeam1 &&
				icons[2].texture == opennova::hud::kHudTexLfpTeam2,
				"each marker samples its team's atlas");
		// Frames: B under attack on phase A -> frame 2 (v0 = 0.5); C ready on
		// phase A -> NOT the ready frame (that blinks on phase B) -> frame 0.
		CHECK(std::fabs(icons[1].v0 - 0.5f) < 1e-6f, "under attack draws frame 2 on phase A");
		CHECK(std::fabs(icons[2].v0 - 0.0f) < 1e-6f, "ready waits for phase B");
		CHECK(icons[0].color == 0xFF7F7F7Fu, "the icon modulate is the flat half-bright");
	}
	// Phase B flips C to frame 3 and clears B's attack frame.
	lp.frame_counter = 8;
	const HudDrawList &list_b = compiler.compile(state, 1024.0f, 768.0f);
	icons.clear();
	for (const HudQuad &q : list_b.quads)
		if (q.texture >= opennova::hud::kHudTexLfpTeam1 &&
				q.texture <= opennova::hud::kHudTexLfpNeutral) icons.push_back(q);
	if (icons.size() == 3) {
		CHECK(std::fabs(icons[2].v0 - 0.75f) < 1e-6f, "ready draws frame 3 on phase B");
		CHECK(std::fabs(icons[1].v0 - 0.0f) < 1e-6f, "the attack frame is a blink");
	}
	// The conquest arm draws nothing (unmodelled, never invented).
	lp.conquest_mode = true;
	const HudDrawList &list_c = compiler.compile(state, 1024.0f, 768.0f);
	size_t conquest_icons = 0;
	for (const HudQuad &q : list_c.quads)
		if (q.texture >= opennova::hud::kHudTexLfpTeam1 &&
				q.texture <= opennova::hud::kHudTexLfpNeutral) ++conquest_icons;
	CHECK(conquest_icons == 0, "the conquest arm stays unmodelled");
}

// The map medic marker: a local-team medic's blip is REPLACED by the
// red-cross plate — three overlay quads (six tris), no sprite.
// [orig: draw_entity_labels_and_markers @0x5a49e0 — the cross @0x5a4cd6..0x5a4d48]
void test_spinmap_medic_marker(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.spinmap_rect = {810.0f, 552.0f, 210.0f, 210.0f, true};
	compiler.configure(layout, font);
	HudFrameState state;
	opennova::hud::HudMinimapMarker plain;
	plain.icon = 3;
	plain.x = 8 << 16;
	plain.color = 0xFF304080u;
	plain.entity_known = 1;
	state.minimap.markers.push_back(plain);
	const HudDrawList &before = compiler.compile(state, 1024.0f, 768.0f);
	const size_t sprites_before = before.map.sprites.size();
	const size_t overlays_before = before.map.overlays.size();

	state.minimap.markers[0].medic = 1;
	const HudDrawList &after = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(after.map.sprites.size() == sprites_before - 1,
			"the medic's blip sprite is replaced, not added to");
	CHECK(after.map.overlays.size() == overlays_before + 6,
			"the cross is three quads = six overlay triangles");
	// White field first, then the two red bars.
	if (after.map.overlays.size() >= overlays_before + 6) {
		CHECK((after.map.overlays[overlays_before].color & 0xFFFFFFu) == 0xFFFFFFu,
				"the field is white");
		CHECK((after.map.overlays[overlays_before + 2].color & 0xFFFFFFu) == 0xFF0000u,
				"the bars are red");
		// The rect spans (px - 3.5)..(px + 4.5): 8 px wide.
		float min_x = 1e9f, max_x = -1e9f;
		for (size_t i = overlays_before; i < overlays_before + 2; ++i) {
			for (const auto *v : {&after.map.overlays[i].a, &after.map.overlays[i].b,
						 &after.map.overlays[i].c}) {
				min_x = std::min(min_x, v->x);
				max_x = std::max(max_x, v->x);
			}
		}
		CHECK(std::fabs((max_x - min_x) - 8.0f) < 0.01f,
				"the plate is the witnessed 8 px square");
	}
}

// The chat wrap + display-buffer slots: the test font's glyphs are 8 px and
// the walk adds 9 per character; a 44 px box breaks "aaaa bbbb cccc" after
// the first word, and the remainder (walked from 2 * 8 = 16) finds no space
// before overflowing, so it stays whole. The first segment is a continuation
// slot with timer 0; the last carries the 930 life.
// [orig: HUD_WordWrapText @0x580980; Chat_AddMessageChannel1 @0x4985d0]
void test_chat_wrap_slots(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	std::string buf = "aaaa bbbb cccc";
	CHECK(opennova::hud::chat_wrap_text(gf, 1.0f, buf, 44, 0) == 2,
			"a 44 px box wraps the line into two segments");
	CHECK(buf.size() == 14 && buf[4] == 0,
			"the break is a NUL written over the last space");
	std::string fits = "aaaa";
	CHECK(opennova::hud::chat_wrap_text(gf, 1.0f, fits, 44, 0) == 1,
			"a line narrower than the box is one segment");
	std::string no_space = "aaaaaaaaaa";
	CHECK(opennova::hud::chat_wrap_text(gf, 1.0f, no_space, 44, 0) == 1,
			"no space before the overflow means no break");

	HudFrameCompiler compiler;
	HudLayout layout;
	layout.chat_box_x1 = 4;
	layout.chat_box_x2 = 44;
	layout.chat_box_present = true;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	compiler.push_chat_line("aaaa bbbb cccc", 0xFF00FF00u, 100);
	const auto &ring = compiler.chat_lines();
	CHECK(ring.size() == 2, "one display slot per segment");
	if (ring.size() == 2) {
		CHECK(ring[0].text == "aaaa", "the first segment sits highest, unprefixed");
		CHECK(ring[1].text == "  bbbb cccc", "the last segment is prefixed two spaces");
		CHECK(ring[0].expire_tick == 100, "a continuation slot carries timer 0");
		CHECK(ring[1].expire_tick == 100 + 930, "slot 1 carries the 930 life");
		CHECK(ring[0].color == 0xFF00FF00u && ring[1].color == 0xFF00FF00u,
				"the colour lands on every slot");
	}
	// Without a chat box the line is never wrapped.
	HudFrameCompiler plain;
	plain.configure(HudLayout{}, font);
	plain.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	plain.push_chat_line("aaaa bbbb cccc", 0xFFFFFFFFu, 0);
	CHECK(plain.chat_lines().size() == 1, "no box, no wrap");
}

int main() {
	fnt_font_t font = make_font();
	test_measure_advance_and_trailing_pad(&font);
	test_layout_pages_bold_underline(&font);
	test_format_tags(&font);
	test_declutter_rebuild_and_cycle();
	test_declutter_element_gates(&font);
	test_compiler_health_and_order(&font);
	test_compiler_crosshair(&font);
	test_compiler_crosshair_user_options(&font);
	test_compiler_hud_color_schemes(&font);
	test_spinmap_projection_zoom_and_clip();
	test_spinmap_compass_screen_rotation_matches_retail_heading();
	test_spinmap_mesh_layers_and_waypoint(&font);
	test_compiler_friendly_tags(&font);
	test_compiler_label_fonts(&font);
	test_static_frame_pick();
	test_vehicle_panel_element(&font);
	test_compiler_stdbox_geometry(&font);
	test_compiler_scoreboard_rows(&font);
	test_message_log_element(&font);
	test_chat_feed_loop(&font);
	test_lfp_panel_element(&font);
	test_spinmap_medic_marker(&font);
	test_chat_wrap_slots(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_frame_compiler_test OK\n");
	return 0;
}
