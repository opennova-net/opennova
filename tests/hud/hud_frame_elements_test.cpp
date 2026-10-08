// The flat HUD elements ported from the HUD residuals: the GAMEINFO/ZONEINFO
// overlay and its snapshot-twin restamp, the CLOCK slot's timer + player
// count, the TEAMID line, the HUDLS slot bar, the objectives panel's slots
// and width-ratio fold, the /NOHUD master word, the friendly-tag squad
// colours and the clip-flash restamp key.
// [orig: HUD_DrawGameTimerOverlay @0x59cc80; HUD_DrawGameTimer @0x593d40;
//  HUD_DrawScoreOverlay @0x593e50; HUD_DrawTeamIdLine @0x59aa30;
//  HUD_DrawWeaponSlotBar @0x599cd0; HUD_DrawWinConditions @0x5ba940;
//  HUD_RenderAllOverlays @0x5A81CE / HUD_DrawGameplayOverlays @0x5BDE9B;
//  HUD_DrawEntityLabel @0x5A3CBB..0x5A3DBC; HUD_DrawAmmoIndicator @0x599A90]
#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_math.h>

#include "common/test_font.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::hud;
using opennova::fnt::FNT_MAX_PAGES;
using opennova::fnt::fnt_font_t;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

constexpr uint32_t page_of(int slot) { return static_cast<uint32_t>(slot) * FNT_MAX_PAGES; }

// Glyphs on one font slot's pages, in draw order.
std::vector<GameFontQuad> glyphs_on(const HudDrawList &list, int slot) {
	std::vector<GameFontQuad> out;
	for (const GameFontQuad &g : list.glyphs)
		if (g.page == page_of(slot)) out.push_back(g);
	return out;
}

void configure(HudFrameCompiler &compiler, const HudLayout &layout, const fnt_font_t *font) {
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
}

// THE GAMEINFO OVERLAY: the TKOTH team timers (bold, GAMEINFO, one '0'
// height apart, the pre-restamp snapshot colour) and "In the Zone" (large,
// ZONEINFO, the score-diff colour).
void test_game_info(const fnt_font_t *font) {
	HudLayout layout;
	layout.game_info = {1013, 410, 0, 1, true};
	layout.zone_info = {1013, 386, 0, 1, true};
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	state.session.game_type = 0x10001u;
	state.session.time_limit_minutes = 20;
	state.session.team_score1 = {100, 250};
	state.session.team_koth = {3, 0};
	state.session.text.team_names[1] = "J";
	state.session.text.team_names[2] = "R";
	state.session.text.in_the_zone = "IZ";
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		// "18:20 J (3)" (60*20 - 100 = 1100 s) and "15:50 R" (950 s, no hold).
		const std::vector<GameFontQuad> bold = glyphs_on(list, kHudFontSlotLabelBold);
		CHECK(bold.size() == 11 + 7);
		// The snapshot twin seeded from the hudpos text colour, half-bright.
		const uint32_t hb = half_bright_argb(layout.hud_text);
		bool colors = !bold.empty();
		for (const GameFontQuad &g : bold) colors = colors && g.color == hb;
		CHECK(colors);
		// The second line one bold '0' height (16 px -> 16 design) lower.
		CHECK(bold.size() == 18 && bold[11].y_top - bold[0].y_top == 16.0f);
		// No coverage: no "In the Zone".
		CHECK(glyphs_on(list, kHudFontSlotLabelLarge).empty());
	}
	// Inside the hill: team 1 trails -> red; leads -> blue; tie -> the twin.
	state.session.zone_coverage = 40;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		const std::vector<GameFontQuad> large = glyphs_on(list, kHudFontSlotLabelLarge);
		CHECK(large.size() == 2 && large[0].color == half_bright_argb(0x00FF2020u));
	}
	state.session.team_score1 = {300, 250};
	{
		const std::vector<GameFontQuad> large =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelLarge);
		CHECK(large.size() == 2 && large[0].color == 0xFF20307Fu);
	}
	state.session.team_score1 = {250, 250};
	{
		const std::vector<GameFontQuad> large =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelLarge);
		CHECK(large.size() == 2 && large[0].color == half_bright_argb(layout.hud_text));
	}
	// The zone line needs the TRGTCNT slot; the timers do not.
	state.declutter_visible[kDeclutterTrgtCnt] = false;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(glyphs_on(list, kHudFontSlotLabelLarge).empty());
		CHECK(glyphs_on(list, kHudFontSlotLabelBold).size() == 18);
	}
	state.declutter_visible[kDeclutterTrgtCnt] = true;
	// Solo KOTH draws the zone line but no team timers.
	state.session.game_type = 1u;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(glyphs_on(list, kHudFontSlotLabelLarge).size() == 2);
		CHECK(glyphs_on(list, kHudFontSlotLabelBold).empty());
	}
	// GAMEINFO's hidden dword ends the drawer before either line.
	state.session.game_type = 0x10001u;
	layout.game_info.hidden = 1;
	compiler.update_layout(layout);
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
}

// THE CLOCK SLOT: the session gate, the -1 untimed gate, the h:mm:ss split,
// the player count's two forms and its spectator line one '0' height lower.
void test_clock(const fnt_font_t *font) {
	HudLayout layout;
	layout.time_clock = {1020, 27, 0, 0, true};
	layout.player_count = {1015, 49, 0, 1, true};
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	state.session.text.timer = "T";
	state.session.text.players = "P";
	state.session.text.players_remaining = "PR";
	state.session.text.spectators = "S";
	state.session.row_count = 9;
	state.session.spectator_count = 2;
	state.session.round_time_remaining = 62 * 3725; // 1 h 02 min 05 s
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty()); // not in session
	state.session.in_session = true;
	{
		const std::vector<GameFontQuad> bold =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold);
		// "T 1:02:05" + "P 7" + "S 2"
		CHECK(bold.size() == 9 + 3 + 3);
		const uint32_t frame = half_bright_argb(layout.hud_text | 0xFF000000u);
		CHECK(!bold.empty() && bold[0].color == frame);
		CHECK(bold.size() == 15 && bold[12].y_top - bold[9].y_top == 16.0f);
	}
	// Untimed: the timer drops, the count stays; no spectators: one line.
	state.session.round_time_remaining = -1;
	state.session.spectator_count = 0;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 3);
	// Permanent death counts the live players.
	state.session.permanent_death = true;
	state.session.remaining_count = 4;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 4);
	// The count's hidden dword; the CLOCK slot gates both.
	layout.player_count.hidden = 1;
	compiler.update_layout(layout);
	state.session.round_time_remaining = 62;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 9);
	state.declutter_visible[kDeclutterClock] = false;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
}

// THE TEAMID LINE: team-mode session gate, the per-team colours, the A&D
// suffix, and the death screen's spectate arm.
void test_team_id_line(const fnt_font_t *font) {
	HudLayout layout;
	layout.team_xy = {1015, 5, 0, 1, true};
	layout.health_rect = {2.0f, 739.0f, 139.0f, 18.0f, true};
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	// The CLOCK slot's player count would draw at the unauthored (0, 0)
	// record in a session too — this test isolates the TEAMID line.
	state.declutter_visible[kDeclutterClock] = false;
	for (size_t i = 0; i < state.session.text.team_names.size(); ++i)
		state.session.text.team_names[i] = std::string("T") + std::to_string(i);
	state.session.text.attacking = "A";
	state.session.text.defending = "D";
	state.session.game_type = 0x10000u;
	state.session.team = 1;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty()); // not in session
	state.session.in_session = true;
	{
		const std::vector<GameFontQuad> bold =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold);
		CHECK(bold.size() == 2 && bold[0].color == half_bright_argb(0xFF80A0FFu)); // palette[3]
	}
	const auto color_for = [&](int team) {
		state.session.team = team;
		const std::vector<GameFontQuad> bold =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold);
		return bold.empty() ? 0u : bold[0].color;
	};
	CHECK(color_for(2) == half_bright_argb(0xFFFF5050u)); // palette[5]
	CHECK(color_for(3) == half_bright_argb(0xFFFFFF00u));
	CHECK(color_for(4) == half_bright_argb(0xFFFF027Fu));
	CHECK(color_for(0) == half_bright_argb(layout.hud_text));
	CHECK(color_for(7) == half_bright_argb(layout.hud_text | 0xFF000000u)); // the frame colour
	// Attack & Defend appends the side's word to the team name.
	state.session.team = 1;
	state.session.game_type = 0x10002u;
	state.session.attack_defend = 2;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 3);
	state.session.attack_defend = 3; // bit 1 wins
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 3);
	state.session.attack_defend = 0;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 2);
	// Non-team modes draw nothing; a hidden HUDTEAMXY neither.
	state.session.game_type = 0x00001u;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
	state.session.game_type = 0x10000u;
	// The death screen: nothing outside the spectate arm; inside it, the
	// spectated name one measured line above the team line, both 20 design
	// px above the health bar's top.
	state.combat.death_screen = true;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
	state.session.spectating = true;
	state.session.spectated_name = "NM";
	{
		const std::vector<GameFontQuad> bold =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold);
		CHECK(bold.size() == 4);
		CHECK(bold.size() == 4 && bold[2].y_top - bold[0].y_top == 16.0f);
	}
	layout.team_xy.hidden = 1;
	compiler.update_layout(layout);
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
}

// THE DMGBAR ON THE DEATH SCREEN: the bar draws only inside the spectate
// arm, off the spectated entity's ratio [orig: HUD_RenderOverlays
// @0x5a7bc5..0x5a7c04; the living arm @0x5a7ca0].
void test_health_bar_death_arm(const fnt_font_t *font) {
	HudLayout layout;
	layout.health_rect = {2.0f, 739.0f, 140.0f, 18.0f, true};
	layout.tag_good = 0xFF00FF00u;
	layout.tag_middle = 0xFFFFFF00u;
	layout.tag_bad = 0xFFFF0000u;
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	state.health_fraction = 1.0f;
	const auto fill_width = [&]() -> float {
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		for (const HudQuad &q : list.quads)
			if (q.color == layout.tag_good || q.color == layout.tag_middle ||
					q.color == layout.tag_bad)
				return q.x1 - q.x0;
		return -1.0f;
	};
	CHECK(fill_width() > 100.0f); // the living bar
	state.combat.death_screen = true;
	CHECK(fill_width() < 0.0f); // no spectate arm: no bar
	state.session.spectating = true;
	state.session.spectated_health_fraction = 0.5f;
	const float half = fill_width();
	CHECK(half > 60.0f && half < 72.0f); // the target's ratio, not the local one
}

// THE HUDLS SLOT BAR: the HUDLS_SYSTEM gate, the 6,7,8,9,0 visit packing
// from HUDLS_SLOT 6, the (0,0) stall, the MOREAV marker, the OR'd colour.
void test_slot_bar(const fnt_font_t *font) {
	HudLayout layout;
	layout.alpha_fade_base_alpha = 102; // alphafade 40
	layout.stance_tint = 0x00123456u;
	layout.hudls.key_ofst_x = 2;
	layout.hudls.key_ofst_y = 20;
	layout.hudls.moreav_dx = 30;
	layout.hudls.moreav_dy = -4;
	for (int i = 0; i < 10; ++i) {
		layout.hudls.slot_x[static_cast<size_t>(i)] = 100 + 50 * i;
		layout.hudls.slot_y[static_cast<size_t>(i)] = 700;
	}
	layout.hudls.bracket_texture_valid = true;
	layout.hudls.bracket_tex_w = 40;
	layout.hudls.bracket_tex_h = 30;
	layout.hudls.moreav_texture_valid = true;
	layout.hudls.moreav_tex_w = 8;
	layout.hudls.moreav_tex_h = 8;
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	for (const int c : {6, 9, 0}) {
		HudSlotBarSlot &s = state.slot_bar[static_cast<size_t>(c)];
		s.present = true;
		s.count = c == 9 ? 2 : 1;
		s.icon_valid = true;
		s.icon_w = 32;
		s.icon_h = 16;
		s.key_label = "K";
	}
	// HUDLS_SYSTEM 0 (every shipped hudpos.def): nothing.
	CHECK(compiler.compile(state, 1024.0f, 768.0f).quads.empty());
	layout.hudls.system = 1;
	compiler.update_layout(layout);
	const auto quads_of = [](const HudDrawList &list, int32_t texture) {
		std::vector<HudQuad> out;
		for (const HudQuad &q : list.quads)
			if (q.texture == texture) out.push_back(q);
		return out;
	};
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		const std::vector<HudQuad> brackets = quads_of(list, kHudTexSlotBarBracket);
		// Categories 6, 9, 0 pack into HUDLS_SLOT 6, 7, 8 (x 350, 400, 450).
		CHECK(brackets.size() == 3 && brackets[0].x0 == 350.0f && brackets[1].x0 == 400.0f &&
				brackets[2].x0 == 450.0f && brackets[0].x1 == 390.0f);
		// The colour ORs the fade base alpha over the stance colour.
		CHECK(!brackets.empty() && brackets[0].color == (0x00123456u | (102u << 24)));
		// Only category 9 holds two: its marker at the signed offset.
		const std::vector<HudQuad> more = quads_of(list, kHudTexSlotBarMoreAv);
		CHECK(more.size() == 1 && more[0].x0 == 430.0f && more[0].y0 == 696.0f);
		// Each category's own icon slot.
		CHECK(quads_of(list, kHudTexSlotBarIcon0 + 6).size() == 1);
		CHECK(quads_of(list, kHudTexSlotBarIcon0 + 9).size() == 1);
		CHECK(quads_of(list, kHudTexSlotBarIcon0 + 0).size() == 1);
		// Three key labels, bold, at slot + key offset.
		const std::vector<GameFontQuad> keys = glyphs_on(list, kHudFontSlotLabelBold);
		CHECK(keys.size() == 3);
	}
	// A (0,0) HUDLS_SLOT 7 stalls the cursor: category 9 and 0 skip too.
	layout.hudls.slot_x[6] = 0;
	layout.hudls.slot_y[6] = 0;
	compiler.update_layout(layout);
	CHECK(quads_of(compiler.compile(state, 1024.0f, 768.0f), kHudTexSlotBarBracket).size() == 1);
	// The death screen never draws the bar; neither does a hidden HUDLS slot.
	state.combat.death_screen = true;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).quads.empty());
	state.combat.death_screen = false;
	state.declutter_visible[kDeclutterHudLs] = false;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).quads.empty());
}

// THE OBJECTIVES PANEL: the untitled stdbox, the bold header, the large rows
// in half-bright-keep-alpha, and both measured dims folded through the WIDTH
// ratio.
void test_objectives_panel(const fnt_font_t *font) {
	HudLayout layout;
	layout.box_texture_valid = true;
	layout.box_tex_w = 128;
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	state.objectives_header = "HDR";
	state.objectives.push_back({"AB", false});
	state.objectives.push_back({"CD", true});
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		const std::vector<GameFontQuad> bold = glyphs_on(list, kHudFontSlotLabelBold);
		const std::vector<GameFontQuad> large = glyphs_on(list, kHudFontSlotLabelLarge);
		CHECK(bold.size() == 3 && large.size() == 4);
		CHECK(!bold.empty() && bold[0].color == 0xFF7F7F7Fu);
		CHECK(large.size() == 4 && large[0].color == 0xFF7F7F7Fu && large[2].color == 0xFF404040u);
		// Rows one folded height apart: (16 << 10) / 1024 = 16.
		CHECK(large.size() == 4 && large[2].y_top - large[0].y_top == 16.0f);
		// The box: stdbox pieces from x 15 to (maxW << 10)/W + 87, no title bar
		// (no row-3 cell).
		float right = 0.0f;
		bool titled = false;
		bool raw_diffuse = true;
		for (const HudQuad &q : list.quads) {
			if (q.texture != kHudTexBoxBorder) continue;
			if (q.x1 > right) right = q.x1;
			if (q.v0 >= 0.75f && q.u0 < 0.75f) titled = true;
			if (q.color != 0xFF7F7F7Fu) raw_diffuse = false;
		}
		// Every box quad carries retail's alpha<<24 | 0x7F7F7F, the raw diffuse
		// the box material's MODULATE2X doubles on the device (D-HUD-49).
		CHECK(raw_diffuse);
		GameFont measure;
		measure.set_font(font);
		int hw = 0;
		int hh = 0;
		measure.measure("HDR", 1.0f, 1.0f, &hw, &hh);
		CHECK(right == static_cast<float>(hw + 15 + 72));
		CHECK(!titled);
	}
	// Twice as wide: the folded row height halves.
	{
		const std::vector<GameFontQuad> large =
				glyphs_on(compiler.compile(state, 2048.0f, 768.0f), kHudFontSlotLabelLarge);
		CHECK(large.size() == 4 && large[2].y_top - large[0].y_top == 8.0f);
	}
	// A zero panel alpha draws nothing.
	state.objectives_alpha = 0;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelLarge).empty());
}

// /NOHUD: the overlay pass and the gameplay-overlay windows go; the frame
// drawer's panels stay.
void test_no_hud(const fnt_font_t *font) {
	HudLayout layout;
	layout.health_rect = {2.0f, 739.0f, 139.0f, 18.0f, true};
	layout.box_texture_valid = true;
	layout.box_tex_w = 128;
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	state.objectives.push_back({"AB", false});
	state.message_log_shown = true;
	const size_t with_hud = compiler.compile(state, 1024.0f, 768.0f).elements_drawn;
	state.overlay_master = hud_overlay_master(true);
	CHECK(state.overlay_master == 0u);
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Only the message log (frame drawer) survives; health and objectives go.
	CHECK(with_hud == 3 && list.elements_drawn == 1);
	CHECK(hud_overlay_master(false) == 3u);
}

// THE SQUAD COLOURS [orig: g_SquadColors @0x83B450].
void test_squad_colors() {
	CHECK(kHudSquadColors[1] == 0xFFA000F0u && kHudSquadColors[13] == 0xFFFF99CCu);
	// Good tier: the whole squad colour; middle: each channel x0.7, chopped;
	// bad tier, no slot, index 0: the tier colour.
	CHECK(friendly_tag_squad_color(0, true, 1, 0xFF010203u) == 0xFFA000F0u);
	CHECK(friendly_tag_squad_color(1, true, 2, 0xFF010203u) == 0xFFB26B6Bu);
	CHECK(friendly_tag_squad_color(2, true, 2, 0xFF010203u) == 0xFF010203u);
	CHECK(friendly_tag_squad_color(0, false, 2, 0xFF010203u) == 0xFF010203u);
	CHECK(friendly_tag_squad_color(0, true, 0, 0xFF010203u) == 0xFF010203u);
}

// THE CLIP-FLASH KEY (D-HUD-5): the bucket, the reserve and the class byte.
void test_flash_key(const fnt_font_t *font) {
	HudLayout layout;
	layout.clip_pos = {32, 700, 0, 0, true};
	layout.alpha_fade_ramp_ticks = 62; // alphafade 25 100 1
	layout.alpha_fade_base_alpha = 63;
	layout.alpha_fade_max_alpha = 255;
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	state.weapon.active = true;
	state.weapon.capacity = 30;
	state.weapon.clip = 3;
	state.weapon.reserve = 60;
	state.weapon.ammo_bucket = 4;
	state.weapon.ammo_class_id = 1;
	state.weapon.round_texture_valid = true;
	state.weapon.round_tex_w = 8;
	state.weapon.round_tex_h = 16;
	const auto round_alpha = [&](int ticks) {
		state.ticks = ticks;
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		for (const HudQuad &q : list.quads)
			if (q.texture == kHudTexRoundGfx) return static_cast<int>(q.color >> 24);
		return -1;
	};
	round_alpha(0);                  // the first key restamps at 0
	const int settled = round_alpha(200);
	CHECK(settled == 63);            // the base alpha
	state.weapon.ammo_bucket = 5;    // a bucket change restamps
	round_alpha(300);
	CHECK(round_alpha(301) > settled);
	round_alpha(500);
	state.weapon.ammo_class_id = 2;  // a class change restamps
	round_alpha(600);
	CHECK(round_alpha(601) > settled);
	round_alpha(800);
	state.weapon.clip = 2;           // the clip is not in the key
	round_alpha(900);
	CHECK(round_alpha(901) == settled);
}

} // namespace


// THE OVERLAY-PANEL MENUS AND THE PAUSE TEXT: the F9 emotes menu's ten
// "%i - %s" rows (the tenth numbered 0) in the bold slot, the context rows
// from 8 in palette entry 4, none on the death screen; the pause word's
// STROVER7 right-aligned in the Impact38 slot at (1000, 4) unless PAUSEDPOS
// authors both fields [orig: HUD_DrawEmotesMenu @0x5bff00;
// HUD_DrawRadioTitleMenu @0x5bfb90; HUD_DrawOverlayPanels @0x5c00cf /
// @0x5c0120; HUD_DrawPausedText @0x59d650].
void test_overlay_panel_menus(const fnt_font_t *font) {
	HudLayout layout;
	HudFrameCompiler compiler;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f, font);
	// The first glyph of each drawn line, in draw order (one per distinct y).
	const auto line_heads = [](const std::vector<GameFontQuad> &glyphs) {
		std::vector<GameFontQuad> out;
		for (const GameFontQuad &g : glyphs)
			if (out.empty() || out.back().y_top != g.y_top) out.push_back(g);
		return out;
	};
	HudFrameState state;
	state.emotes_menu.shown = true;
	for (size_t i = 0; i < 10; ++i) state.emotes_menu.texts[i] = "e";
	{
		const std::vector<GameFontQuad> rows = line_heads(
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold));
		// Ten rows 30 design px apart from y 225 (an untitled box draws no title).
		CHECK(rows.size() == 10);
		CHECK(rows.size() == 10 && rows[1].y_top - rows[0].y_top == 30.0f &&
				rows[9].y_top - rows[0].y_top == 270.0f);
		// Rows 1..7 in the active colour, 8..10 in palette entry 4.
		CHECK(rows.size() == 10 && rows[6].color == rows[0].color &&
				rows[7].color != rows[0].color && rows[9].color == rows[7].color);
	}
	state.combat.death_screen = true;
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).empty());
	state.combat.death_screen = false;
	state.emotes_menu.shown = false;
	state.radio_menu.shown = true;
	for (size_t i = 0; i < 10; ++i) state.radio_menu.texts[i] = "r";
	{
		const std::vector<GameFontQuad> rows = line_heads(
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold));
		// The radio menu's context rows start at 9.
		CHECK(rows.size() == 10 && rows[7].color == rows[0].color &&
				rows[8].color != rows[0].color);
	}
	state.radio_menu.shown = false;
	state.paused = true;
	state.paused_text = "P";
	const auto pause_glyphs = [&]() {
		return glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotImpact38);
	};
	float right0 = 0.0f;
	float top0 = 0.0f;
	{
		// Right-aligned on design x 1000 (the half-pixel quad bias and the
		// glyph's advance keep the edge within a few pixels), at y 4.
		const std::vector<GameFontQuad> text = pause_glyphs();
		CHECK(text.size() == 1);
		if (!text.empty()) {
			right0 = text[0].x_top_right;
			top0 = text[0].y_top;
		}
		CHECK(right0 > 990.0f && right0 <= 1000.0f);
	}
	layout.paused_x = 900;
	layout.paused_y = 0; // a zero field falls back to (1000, 4) too
	compiler.update_layout(layout);
	{
		const std::vector<GameFontQuad> text = pause_glyphs();
		CHECK(text.size() == 1 && text[0].x_top_right == right0 && text[0].y_top == top0);
	}
	layout.paused_y = 40;
	compiler.update_layout(layout);
	{
		// Both fields authored: the anchor moves by (-100, +36).
		const std::vector<GameFontQuad> text = pause_glyphs();
		CHECK(text.size() == 1 && text[0].x_top_right == right0 - 100.0f &&
				text[0].y_top == top0 + 36.0f);
	}
	state.paused = false;
	CHECK(pause_glyphs().empty());
}

// THE SQUAD ORDER LINES: the two S2C 0x72 lines right-aligned at the
// HUDORDERS anchor in the bold slot, 18 design px apart (an empty line keeps
// its rung), palette[4] less one alpha step through the half-bright drawer;
// a detail level above 1 hides them, and no HUDDECLUT slot does (D-HUD-54:
// the CHAT slot is the console rings' alone).
// [orig: sub_59AEE0 @0x59aee0]
void test_squad_orders(const fnt_font_t *font) {
	HudLayout layout;
	layout.squad_orders = {1000, 300, 0, 0, true};
	HudFrameCompiler compiler;
	configure(compiler, layout, font);
	HudFrameState state;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
	state.squad_orders[1] = "GO";
	{
		const std::vector<GameFontQuad> bold =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold);
		CHECK(bold.size() == 2);
		// Right-aligned: the run ends at the anchor x.
		CHECK(bold.size() == 2 && bold[1].x_top_right <= 1000.0f &&
				bold[1].x_top_right > 990.0f);
		CHECK(bold.size() == 2 &&
				bold[0].color == half_bright_argb(0xFFF0F000u - 0x01000000u));
	}
	state.squad_orders[0] = "HOLD";
	{
		const std::vector<GameFontQuad> bold =
				glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold);
		CHECK(bold.size() == 6);
		// The second rung sits 18 design px (768 high) below the first.
		CHECK(bold.size() == 6 && bold[4].y_top - bold[0].y_top == 18.0f);
	}
	state.hud_detail_level = 2;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty());
	state.hud_detail_level = 1;
	state.declutter_visible.fill(false);
	CHECK(glyphs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotLabelBold).size() == 6);
}

// THE MESSAGE FEEDS AND THE POWER-THROW LABEL ride the BOLD label slot at its
// own scale even when hudpos names a HUD font, and the feeds' anchors and
// 18-unit step scale to the surface once and add in surface pixels; the
// power bar draws in the per-frame HUD colour.
// [orig: HUD_DrawConsoleMessages @0x59ad30 -- g_HUDLabelFontBold @0x59ae09 /
//  @0x59aea6, the step scaled alone @0x59adb0, added @0x59ae1a / @0x59aeb7;
//  HUD_DrawPowerThrowChargeBar @0x599830 -- g_HUDFrameOverlayColor @0x599955 /
//  @0x599982 / @0x5999f4, g_HUDLabelFontBold @0x599a0f]
void test_feed_and_power_label_slots(const fnt_font_t *font) {
	fnt_font_t hud_font = test_font::uniform_test_font();
	HudLayout layout;
	layout.chat_text = {122, 684, 0, 0, true};
	layout.sys_text = {5, 22, 0, 0, true};
	layout.power_rect = {14.0f, 576.0f, 160.0f, 6.0f, true};
	HudFrameCompiler compiler;
	compiler.configure(layout, &hud_font);
	compiler.configure_label_fonts(font, font, font, 1.25f, 1.6f);
	const float w = 1280.0f, h = 720.0f;
	HudFrameState state;
	state.ticks = 100;
	compiler.push_chat_line("AB", 0xFFFFFFFFu, state.ticks);
	compiler.push_chat_line("CD", 0xFFFFFFFFu, state.ticks);
	compiler.push_message("EF", state.ticks);
	{
		const HudDrawList &list = compiler.compile(state, w, h);
		CHECK(glyphs_on(list, kHudFontSlotHud).empty());
		const std::vector<GameFontQuad> bold = glyphs_on(list, kHudFontSlotLabelBold);
		CHECK(bold.size() == 6);
		if (bold.size() == 6) {
			// Two chat lines one scaled step apart, at the scaled HUDCHATTEXT.
			const float step = static_cast<float>(design_to_screen_y(kFeedLineStepDesign, 720));
			CHECK(bold[2].y_top - bold[0].y_top == step);
			CHECK(bold[0].y_top - static_cast<float>(design_to_screen_y(684, 720)) < 1.0f &&
					bold[0].y_top - static_cast<float>(design_to_screen_y(684, 720)) > -1.0f);
			CHECK(bold[0].x_top_left - static_cast<float>(design_to_screen_x(122, 1280)) < 1.0f &&
					bold[0].x_top_left - static_cast<float>(design_to_screen_x(122, 1280)) > -1.0f);
			// The bold slot's 1.25 scale: a 16-px cell and the drawer's half-texel
			// bottom bias draw 16.5 x 1.25 px tall (D-FNT-6).
			CHECK(bold[0].y_bottom - bold[0].y_top == 16.5f * 1.25f);
			// The system line at the scaled HUDSYSTEXT.
			CHECK(bold[4].y_top - static_cast<float>(design_to_screen_y(22, 720)) < 1.0f &&
					bold[4].y_top - static_cast<float>(design_to_screen_y(22, 720)) > -1.0f);
		}
	}
	// The power-throw label: bold slot, the outline in the per-frame colour.
	HudFrameState power;
	power.ticks = 100000;
	power.windup_active = true;
	power.windup_held_ticks = 31;
	power.hud_color_index = 0;
	const HudDrawList &list = compiler.compile(power, w, h);
	CHECK(glyphs_on(list, kHudFontSlotHud).empty());
	const std::vector<GameFontQuad> bold = glyphs_on(list, kHudFontSlotLabelBold);
	CHECK(bold.size() == 2); // "0%"
	HudFrameState scheme2 = power;
	scheme2.hud_color_index = 2;
	const uint32_t outline0 = [&] {
		for (const HudQuad &q : list.quads)
			if (!q.filled) return q.color;
		return 0u;
	}();
	const HudDrawList &list2 = compiler.compile(scheme2, w, h);
	uint32_t outline2 = 0;
	for (const HudQuad &q : list2.quads)
		if (!q.filled) outline2 = q.color;
	CHECK(outline0 != 0u && outline2 == (0xFF000000u | layout.hud_text) && outline0 != outline2);
	opennova::fnt::fnt_free(&hud_font);
}

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_feed_and_power_label_slots(&font);
	test_game_info(&font);
	test_clock(&font);
	test_team_id_line(&font);
	test_health_bar_death_arm(&font);
	test_slot_bar(&font);
	test_objectives_panel(&font);
	test_no_hud(&font);
	test_squad_colors();
	test_flash_key(&font);
	test_overlay_panel_menus(&font);
	test_squad_orders(&font);
	opennova::fnt::fnt_free(&font);
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_frame_elements_test OK\n");
	return 0;
}
