// The hudpos.def parse applied to the HUD layout (hud/hud_layout_from_hudpos.h),
// pinned where it used to live in the HudOverlay binding and the HudPos GUT
// suite (ADR 0040 ladder E3b): the corner rects against the one x,y,w,h rect,
// the 4-field positioned records, the packed colours, the spinmap extent gate,
// the stance slots by id (later wins), the last-authored static frame and the
// font fallback — on a synthetic file, then on the reference fixture's hudpos.def
// (an earlier build's layout, not JO:CA's: hud-re.md "Render pipeline"); and the
// HUD an empty, a key-less and a partial file make (D-HUD-54).
// [orig: HUD_ParseHudposToken @0x59f370; HUD_DrawHealthBar @0x5a2e50;
//  HUD_DrawPowerThrowChargeBar @0x599830]
#include <runtime/hud/hud_layout_from_hudpos.h>

#include <formats/def/def.h>
#include <runtime/hud/hud_declutter.h>
#include <runtime/hud/hud_frame.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/retail_paths.h"
#include "common/test_font.h"

using namespace opennova::def;
using namespace opennova::hud;
using opennova::fnt::fnt_font_t;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

static bool parse(const char *text, DefHudPosFile &file) {
	std::memset(&file, 0, sizeof(file));
	return def_parse_hudpos_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text),
				   &file) == 0;
}

static void synthetic() {
	static const char text[] =
			"fonthud1_lo\tfontlo\r\n"
			"HUDHEALTH\t2,739,141,757\r\n"
			"HUDHEAT\t10,20,30,50\r\n"
			"HUDPOWERBAR\t20,720,72,11\r\n"
			"AMMOCOUNTPOS\t128,597,0,right\r\n"
			"HUDWEAPONNAME\t11,630,1,left\r\n"
			"GAMEINFO\t1013,430\r\n"
			"HUDCHATTEXT\t8,600\r\n"
			"HUDSYSTEXT\t8,640\r\n"
			"LFP_FLAGS\t1020 , 27\r\n"
			"HUDVEHSTANCEPOS\t0 272\r\n"
			"HUDCLIP\t900,700\r\n"
			"HUDSTANCEPOS\t5,760\r\n"
			"HUDSCOPERANGEXY\t100,101\r\n"
			"HUDSCOPEZEROXY\t102,103\r\n"
			"HUDSCOPEMAGXY\t104,105\r\n"
			"SHOWIMPACTDISTPOS\t200,201\r\n"
			"HUDWPNICON\t202,203\r\n"
			"HUDGEARTEXT\t204,205\r\n"
			"CARGOPOS\t206,207\r\n"
			"HUDAGLRADIUS\t7\r\n"
			"HUDAGLTLRX\t300,301\r\n"
			"HUDAGLYLEN\t302,303\r\n"
			"AGLCOLOR\t1,2,3,4\r\n"
			"hud_textcolor\t251,213,5\r\n"
			"stancecolor_good\t200,5,249,12\r\n"
			"tagcolor_bad\t300,-1,10\r\n"
			"HUDSPINMAPX1\t810\r\n"
			"HUDSPINMAPX2\t1020\r\n"
			"HUDSPINMAPY1\t552\r\n"
			"HUDSPINMAPY2\t762\r\n"
			"SPINMAPWPDISTOFF\t17\r\n"
			"MAPCOORDS\t530,720,1\r\n"
			"alphafade\t30 50 3\r\n"
			"HUDSTANCE\t0 1 2 stance_1.tga STAND\r\n"
			"HUDSTANCE\t3 4 5 stance_4.tga PRONE\r\n"
			"HUDSTANCE\t0 6 7 stance_x.tga STAND2\r\n"
			"HUDSTANCE\t9 8 8 nope.tga NOPE\r\n"
			"StaticFrame\tH_BlkHLin.tga  512,720\r\n"
			"StaticFrame\tCompMark.tga  508,685\r\n"
			"PARACHUTEICON\tchute.tga 400,401\r\n"
			"ARMORICON\tarmor.tga 402,403\r\n"
			"BREATHTIME\t\t512,70,center\r\n"
			"HUDTIMECLOCK\t1020,27,0,right\r\n"
			"HUDPLAYERCOUNT\t1015,49,0,right\r\n"
			"HUDTEAMXY\t1015,5,1,center\r\n"
			"ZONEINFO\t1013,386,Right\r\n"
			"HUDLS_SYSTEM\t1\r\n"
			"HUDLS_BRACKET\tbrack.tga\r\n"
			"HUDLS_KEYOFST\t3,18\r\n"
			"HUDLS_MOREAV\tmore.tga 20 -5\r\n"
			"HUDLS_SLOT\t6 300 700\r\n";
	DefHudPosFile file;
	if (!parse(text, file)) {
		std::printf("FAIL: synthetic parse\n");
		++failures;
		return;
	}
	HudLayout layout;
	HudLayoutAssets assets;
	hud_layout_from_hudpos(file, layout, assets);

	// The corner rects against the one x,y,w,h rect.
	CHECK(layout.health_rect.present && layout.health_rect.x == 2.0f &&
			layout.health_rect.y == 739.0f && layout.health_rect.w == 139.0f &&
			layout.health_rect.h == 18.0f);
	CHECK(layout.heat_rect.w == 20.0f && layout.heat_rect.h == 30.0f);
	CHECK(layout.power_rect.x == 20.0f && layout.power_rect.y == 720.0f &&
			layout.power_rect.w == 72.0f && layout.power_rect.h == 11.0f);
	// The 4-field positioned records: x, y, hidden, align; a 2-field line reads
	// visible / left.
	CHECK(layout.ammo_count.present && layout.ammo_count.x == 128 && layout.ammo_count.y == 597 &&
			layout.ammo_count.hidden == 0 && layout.ammo_count.align == 1);
	CHECK(layout.weapon_name.hidden == 1 && layout.weapon_name.align == 0);
	CHECK(layout.game_info.x == 1013 && layout.game_info.y == 430 && layout.game_info.hidden == 0 &&
			layout.game_info.align == 0);
	CHECK(layout.wpd_info.present && layout.wpd_info.x == 0);
	CHECK(layout.chat_text.x == 8 && layout.chat_text.y == 600 && layout.chat_text.present);
	CHECK(layout.sys_text.y == 640);
	CHECK(!layout.chat_box_present && layout.chat_box_x1 == 0 && layout.chat_box_x2 == 0);
	CHECK(layout.lfp_anchor_present && layout.lfp_anchor_x == 1020 && layout.lfp_anchor_y == 27);
	CHECK(layout.veh_stance_pos.present && layout.veh_stance_pos.x == 0 &&
			layout.veh_stance_pos.y == 272);
	CHECK(layout.clip_pos.x == 900 && layout.stance_pos.y == 760);
	// BREATHTIME is x, y, align: the third field is the alignment word.
	CHECK(layout.breath_time.x == 512 && layout.breath_time.y == 70 &&
			layout.breath_time.align == 2);
	CHECK(layout.scope_range.x == 100 && layout.scope_zero.y == 103 && layout.scope_mag.x == 104);
	// The session anchors: HUDTIMECLOCK keeps x, y only; ZONEINFO's third
	// field is its alignment [orig: @0x59FDB8 / @0x5A0676].
	CHECK(layout.time_clock.x == 1020 && layout.time_clock.y == 27 &&
			layout.time_clock.hidden == 0 && layout.time_clock.align == 0);
	CHECK(layout.player_count.x == 1015 && layout.player_count.y == 49 &&
			layout.player_count.hidden == 0 && layout.player_count.align == 1);
	CHECK(layout.team_xy.hidden == 1 && layout.team_xy.align == 2);
	CHECK(layout.zone_info.x == 1013 && layout.zone_info.y == 386 &&
			layout.zone_info.hidden == 0 && layout.zone_info.align == 1);
	// The HUDLS block and its two texture names.
	CHECK(layout.hudls.system == 1 && layout.hudls.key_ofst_x == 3 &&
			layout.hudls.key_ofst_y == 18 && layout.hudls.moreav_dx == 20 &&
			layout.hudls.moreav_dy == -5 && layout.hudls.slot_x[5] == 300 &&
			layout.hudls.slot_y[5] == 700 && layout.hudls.slot_x[0] == 0);
	CHECK(assets.hudls_bracket == "brack.tga" && assets.hudls_moreav == "more.tga");
	// The combat anchors and the AGL colour.
	CHECK(layout.combat.impact_x == 200 && layout.combat.impact_y == 201);
	CHECK(layout.combat.icon_x == 202 && layout.combat.gear_y == 205 && layout.combat.cargo_x == 206);
	CHECK(layout.combat.parachute_x == 400 && layout.combat.parachute_y == 401);
	CHECK(layout.combat.armor_x == 402 && layout.combat.armor_y == 403);
	CHECK(layout.combat.agl_tick_width == 7 && layout.combat.agl_left == 300 &&
			layout.combat.agl_right == 301 && layout.combat.agl_y == 302 &&
			layout.combat.agl_height == 303);
	// The stance and AGL colours are authored a,r,g,b (formats/def parse_hud_color_argb).
	CHECK(layout.combat.agl_color == 0x01020304u);
	CHECK(assets.parachute_icon == "chute.tga" && assets.armor_icon == "armor.tga");
	// Colours pack 0xAARRGGBB: a 3-value line is opaque, every channel clamps
	// to a byte.
	CHECK(layout.hud_text == 0xFFFBD505u);
	CHECK(layout.stance_good == 0xC805F90Cu);
	CHECK(layout.tag_bad == 0xFFFF000Au);
	// The spinmap rect from its corner pair, present with a positive extent.
	CHECK(layout.spinmap_rect.present && layout.spinmap_rect.x == 810.0f &&
			layout.spinmap_rect.y == 552.0f && layout.spinmap_rect.w == 210.0f &&
			layout.spinmap_rect.h == 210.0f);
	CHECK(layout.spinmap_wp_dist_off == 17);
	CHECK(layout.map_coords_x == 530.0f && layout.map_coords_y == 720.0f &&
			layout.map_coords_off == 1);
	CHECK(layout.alpha_fade_base_alpha == 76 && layout.alpha_fade_max_alpha == 127 &&
			layout.alpha_fade_ramp_ticks == 186);
	// An unauthored HUDCHLINE keeps the 8-line default.
	CHECK(layout.chat_lines == 8);
	// HUDSTANCE by id: a later record for the same id replaces the earlier
	// one, an out-of-range id authors nothing.
	CHECK(assets.stance_textures[0] == "stance_x.tga" && layout.stance_offset_x[0] == 6 &&
			layout.stance_offset_y[0] == 7);
	CHECK(assets.stance_textures[3] == "stance_4.tga" && layout.stance_offset_x[3] == 4);
	CHECK(assets.stance_textures[1].empty() && layout.stance_offset_x[1] == 0);
	CHECK(assets.stance_textures[5].empty());
	// The static frame: the LAST authored line draws.
	CHECK(layout.frame_pos.present && layout.frame_pos.x == 508 && layout.frame_pos.y == 685);
	CHECK(assets.static_frame == "CompMark.tga");
	// Each name's loader mode [orig: HUD_LoadAllTextures @0x59DED8..0x59DFC9]: the
	// static frame and the HUDLS pair in colour, the stances and the two icons
	// alpha only, a VEHICLE_HUD block's interface art alpha only (@0x59E26A).
	{
		using opennova::renderer::TextureLoader;
		using opennova::renderer::texture_role;
		CHECK(texture_role(HudLayoutAssets::kStaticFrameRole).loader == TextureLoader::HudColor);
		CHECK(texture_role(HudLayoutAssets::kHudlsRole).loader == TextureLoader::HudColor);
		CHECK(texture_role(HudLayoutAssets::kStanceRole).loader == TextureLoader::HudAlpha);
		CHECK(texture_role(HudLayoutAssets::kParachuteIconRole).loader == TextureLoader::HudAlpha);
		CHECK(texture_role(HudLayoutAssets::kArmorIconRole).loader == TextureLoader::HudAlpha);
		CHECK(texture_role(kVehicleHudInterfaceRole).loader == TextureLoader::HudAlpha);
	}
	// The fonts: each key fills its own name, and the width pick has no
	// fallback from one to the other [orig: HUD_SelectHudposFont @0x591890].
	CHECK(assets.font_lo == "fontlo" && assets.font_hi.empty());
	CHECK(hudpos_font_for_width(assets, 640) == "fontlo");
	CHECK(hudpos_font_for_width(assets, 641).empty());
	def_free_hudpos(&file);

	// A bare `fonthud1` line (the JOX hudpos.def's) is no key: HUD_ParseHudposToken
	// compares whole tokens with _stricmp, so both names stay empty and the HUD
	// slot takes the bold label font.
	static const char bare_font[] = "fonthud1\tGunpb18b.fnt\r\nHUDCHLINE\t9\r\n";
	if (!parse(bare_font, file)) {
		std::printf("FAIL: bare fonthud1 parse\n");
		++failures;
		return;
	}
	layout = HudLayout();
	assets = HudLayoutAssets();
	hud_layout_from_hudpos(file, layout, assets);
	CHECK(assets.font_lo.empty() && assets.font_hi.empty() && layout.chat_lines == 9);
	def_free_hudpos(&file);

	// A minimal file: no frame, no spinmap extent, a chline, the hi font.
	static const char minimal[] =
			"fonthud1_hi\tfonthi\r\n"
			"fonthud1_lo\tfontlo\r\n"
			"HUDCHLINE\t12\r\n"
			"HUDSPINMAPX1\t100\r\n"
			"HUDSPINMAPX2\t100\r\n"
			"HUDSPINMAPY1\t50\r\n"
			"HUDSPINMAPY2\t50\r\n";
	if (!parse(minimal, file)) {
		std::printf("FAIL: minimal parse\n");
		++failures;
		return;
	}
	layout = HudLayout();
	assets = HudLayoutAssets();
	hud_layout_from_hudpos(file, layout, assets);
	CHECK(!layout.frame_pos.present && assets.static_frame.empty());
	CHECK(!layout.spinmap_rect.present);
	CHECK(layout.chat_lines == 12);
	CHECK(assets.font_hi == "fonthi" && assets.font_lo == "fontlo");
	CHECK(hudpos_font_for_width(assets, 1024) == "fonthi");
	CHECK(hudpos_font_for_width(assets, 640) == "fontlo");
	CHECK(layout.spinmap_wp_dist_off == 0 && layout.map_coords_off == 0);
	// An unauthored BREATHTIME keeps the zero anchor (0, 0, left): the bar has
	// no presence gate.
	CHECK(layout.breath_time.x == 0 && layout.breath_time.y == 0 && layout.breath_time.align == 0);
	// The device-owned fields are untouched by the fill.
	CHECK(!layout.frame_texture_valid && layout.stance_frame0_w == 0 && !layout.box_texture_valid);
	// No NETWORKINDICATOR line: the connection indicators keep the reset corners.
	CHECK(layout.net_indicator_pos == kNetIndicatorResetPos);
	def_free_hudpos(&file);
}

// NETWORKINDICATOR authors the connection indicators' three corners; a file
// without the line restores the reset's, as the mission-start reset runs
// before every parse [orig: HUD_ParseHudposToken @0x59F981..0x59FA0C;
// CNetQuality_Reset @0x4C5908..0x4C591E via Game_StartMission @0x5243B4].
// ALPHAFADE's converts are the original's: atof times dbl_7D9A20 (2.55 rounded
// up) for the two alphas and times 62.0 for the ramp, each through _ftol2_sse
// [orig: @0x5A0882..0x5A08C2].
static void alphafade_converts() {
	DefHudPosFile file;
	if (!parse("alphafade 20 100.5 0.5\r\n", file)) {
		std::printf("FAIL: alphafade parse\n");
		++failures;
		return;
	}
	HudLayout layout;
	HudLayoutAssets assets;
	hud_layout_from_hudpos(file, layout, assets);
	CHECK(layout.alpha_fade_base_alpha == 51 && layout.alpha_fade_max_alpha == 256 &&
			layout.alpha_fade_ramp_ticks == 31);
	def_free_hudpos(&file);
}

static void network_indicator() {
	DefHudPosFile file;
	if (!parse("NETWORKINDICATOR\t6,7 30,7 70,8\r\n", file)) {
		std::printf("FAIL: NETWORKINDICATOR parse\n");
		++failures;
		return;
	}
	HudLayout layout;
	HudLayoutAssets assets;
	hud_layout_from_hudpos(file, layout, assets);
	CHECK((layout.net_indicator_pos == std::array<int, 6>{6, 7, 30, 7, 70, 8}));
	def_free_hudpos(&file);
	if (!parse("PAUSEDPOS\t980 12\r\n", file)) {
		std::printf("FAIL: second parse\n");
		++failures;
		return;
	}
	hud_layout_from_hudpos(file, layout, assets);
	CHECK(layout.net_indicator_pos == kNetIndicatorResetPos);
	def_free_hudpos(&file);
}

// An armed soldier on foot: the state the gated elements would all draw for.
static HudFrameState armed_soldier() {
	HudFrameState state;
	state.ticks = 10;
	state.health_fraction = 0.5f;
	state.weapon.active = true;
	state.weapon.clip = 12;
	state.weapon.reserve = 90;
	state.weapon.capacity = 30;
	state.weapon.display_name = "M16";
	state.windup_active = true;
	state.windup_held_ticks = 31;
	state.waypoint.present = true;
	state.waypoint.name = "Alpha";
	state.waypoint.distance_m = 120;
	return state;
}

static bool drew(const HudDrawList &list, HudElement element) {
	for (const HudElementSpan &span : list.element_spans)
		if (span.element == element) return true;
	return false;
}

// The layout and the declutter table a file authors, through the compiler
// with every font slot set and the crosshair art stamped (the device's leg).
struct Compiled {
	HudLayout layout;
	HudDeclutter table;
	HudFrameCompiler compiler;
};

static void compile_from(const DefHudPosFile &file, const fnt_font_t *font, Compiled &out) {
	HudLayoutAssets assets;
	hud_layout_from_hudpos(file, out.layout, assets);
	out.layout.crosshair_texture_valid = true;
	out.layout.crosshair_tex_w = out.layout.crosshair_tex_h = 64;
	out.table = declutter_from_hudpos(file);
	out.compiler.configure(out.layout, font);
	out.compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
}

// D-HUD-54: an empty, a key-less and a partial hudpos.def. Retail's HUD tables
// are BSS-zero and only a parse arm writes them, so a file that authors no
// HUDDECLUT row hides every gated element at every level (the ammo count drew
// at (0, 0) through the old all-visible fallback, where retail draws nothing);
// a partial file shows what its rows show, a later row for a slot replacing an
// earlier one; an unauthored HUDORDERS is (0, 0).
// [orig: HUD_ParseHudposToken @0x59F370 -> byte_2723CE0 (the MSNTITLE arm's
//  store @0x5A1683); CRenderState_SetLayerVisibility @0x59B0F0; the WPNGRP
//  cmp HUD_RenderOverlays @0x5A7CC8; dword_2723D84 / dword_2723D88]
static void unauthored_hud() {
	fnt_font_t font = test_font::uniform_test_font();
	for (const char *text : { "", "// nothing but a comment\r\n", "NOSUCHKEY\t1\r\n" }) {
		DefHudPosFile file;
		if (!parse(text, file)) {
			std::printf("FAIL: key-less parse\n");
			++failures;
			continue;
		}
		Compiled hud;
		compile_from(file, &font, hud);
		CHECK(hud.layout.squad_orders.x == 0 && hud.layout.squad_orders.y == 0);
		for (int level = 0; level < 4; ++level) {
			hud.table.set_level(level);
			bool any = false;
			for (bool shown : hud.table.visible()) any = any || shown;
			CHECK(!any);
		}
		hud.table.set_level(0);
		HudFrameState state = armed_soldier();
		hud.compiler.push_message("radio check", 10);
		// Contrast: the embedder's all-visible default draws the ammo count at
		// the unauthored (0, 0), so the hidden run below is not vacuous.
		{
			const HudDrawList &shown = hud.compiler.compile(state, 1024.0f, 768.0f);
			CHECK(drew(shown, HudElement::AmmoCount) && drew(shown, HudElement::Crosshair));
		}
		state.declutter_visible = hud.table.visible();
		const HudDrawList &list = hud.compiler.compile(state, 1024.0f, 768.0f);
		for (HudElement gated : { HudElement::AmmoCount, HudElement::WeaponName, HudElement::ClipIndicator,
				 HudElement::Stance, HudElement::Health, HudElement::Crosshair, HudElement::Power,
				 HudElement::Waypoint, HudElement::Spinmap, HudElement::Feed, HudElement::Clock,
				 HudElement::TeamIdLine, HudElement::WeaponSlotBar, HudElement::BreathBar })
			CHECK(!drew(list, gated));
		// Nothing else of the walk draws for this soldier either: retail's
		// screen with an empty hudpos.def carries no HUD element.
		CHECK(list.element_spans.empty());
		def_free_hudpos(&file);
	}

	// A partial file: WPNGRP at level 0 only, no DMGBAR row, and two XHAIRS
	// rows of which the later (all off) stands; a row without an arm
	// (HUDDECLUT_CTAPE) authors nothing.
	DefHudPosFile file;
	if (!parse("AMMOCOUNTPOS\t40,700,0,left\r\n"
	           "HUDWEAPONNAME\t40,720,0,left\r\n"
	           "HUDHEALTH\t2,739,141,757\r\n"
	           "HUDDECLUT_WPNGRP\t1 0 0 0\r\n"
	           "HUDDECLUT_XHAIRS\t1 1 1 1\r\n"
	           "HUDDECLUT_CTAPE\t1 1 1 1\r\n"
	           "HUDDECLUT_XHAIRS\t0 0 0 0\r\n",
	           file)) {
		std::printf("FAIL: partial parse\n");
		++failures;
		opennova::fnt::fnt_free(&font);
		return;
	}
	Compiled hud;
	compile_from(file, &font, hud);
	CHECK(hud.table.mask(kDeclutterWpnGrp) == 0x1 && hud.table.mask(kDeclutterXhairs) == 0 &&
			hud.table.mask(kDeclutterDmgBar) == 0);
	HudFrameState state = armed_soldier();
	state.declutter_visible = hud.table.visible();
	{
		const HudDrawList &list = hud.compiler.compile(state, 1024.0f, 768.0f);
		CHECK(drew(list, HudElement::AmmoCount) && drew(list, HudElement::WeaponName));
		CHECK(!drew(list, HudElement::Health) && !drew(list, HudElement::Crosshair));
	}
	hud.table.set_level(1);
	state.declutter_visible = hud.table.visible();
	state.hud_detail_level = 1;
	{
		const HudDrawList &list = hud.compiler.compile(state, 1024.0f, 768.0f);
		CHECK(!drew(list, HudElement::AmmoCount) && !drew(list, HudElement::WeaponName));
	}
	def_free_hudpos(&file);
	opennova::fnt::fnt_free(&font);
}

static void retail_leg() {
	const std::string fixture = retail::reference_fixture("def/hudpos.def");
	if (fixture.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/hudpos.def (an earlier build's HUD layout, not JO:CA's)");
		return;
	}
	DefHudPosFile file;
	std::memset(&file, 0, sizeof(file));
	if (def_parse_hudpos(fixture.c_str(), &file) != 0) {
		std::printf("FAIL: def_parse_hudpos %s\n", fixture.c_str());
		++failures;
		return;
	}
	HudLayout layout;
	HudLayoutAssets assets;
	hud_layout_from_hudpos(file, layout, assets);
	// The values the HudPos GUT suite used to pin through its getters.
	CHECK(layout.health_rect.x == 2.0f && layout.health_rect.y == 739.0f &&
			layout.health_rect.w == 139.0f && layout.health_rect.h == 18.0f);
	CHECK(layout.hud_text == 0xFFFBD505u);
	CHECK(layout.spinmap_rect.present && layout.spinmap_rect.x == 810.0f &&
			layout.spinmap_rect.y == 552.0f && layout.spinmap_rect.w == 210.0f &&
			layout.spinmap_rect.h == 210.0f);
	CHECK(layout.spinmap_wp_dist_off == 0);
	CHECK(assets.stance_textures[0] == "stance_1.tga");
	CHECK(layout.game_info.x == 1013 && layout.game_info.y == 430 && layout.game_info.hidden == 0 &&
			layout.game_info.align == 0);
	CHECK(layout.alpha_fade_base_alpha == 76 && layout.alpha_fade_max_alpha == 127 &&
			layout.alpha_fade_ramp_ticks == 186);
	CHECK(layout.chat_lines == 8);
	CHECK(layout.veh_stance_pos.x == 0 && layout.veh_stance_pos.y == 272);
	CHECK(layout.lfp_anchor_x == 1020 && layout.lfp_anchor_y == 27);
	// The fixture authors no NETWORKINDICATOR: the reset corners stand.
	CHECK(layout.net_indicator_pos == kNetIndicatorResetPos);
	// Its HUDDECLUT rows decide the soldier's panel level by level: each gated
	// element draws exactly at the levels its row shows, and each draws at one
	// level at least (D-HUD-54 leaves an authored layout as it drew; this file
	// shows the ammo count and the health bar at levels 1 and 2, `0 1 1 0`,
	// where JO:CA's two hudpos.def files author `1 1 1 0`).
	{
		fnt_font_t font = test_font::uniform_test_font();
		Compiled hud;
		compile_from(file, &font, hud);
		const struct {
			HudElement element;
			int slot;
		} gated[] = { { HudElement::AmmoCount, kDeclutterWpnGrp }, { HudElement::Health, kDeclutterDmgBar },
			{ HudElement::Crosshair, kDeclutterXhairs }, { HudElement::Spinmap, kDeclutterSpinmap } };
		bool ever[4] = {};
		for (int level = 0; level < 3; ++level) {
			hud.table.set_level(level);
			HudFrameState state = armed_soldier();
			state.declutter_visible = hud.table.visible();
			state.hud_detail_level = level;
			const HudDrawList &list = hud.compiler.compile(state, 1024.0f, 768.0f);
			for (size_t i = 0; i < 4; ++i) {
				const bool shown = hud.table.visible()[static_cast<size_t>(gated[i].slot)];
				CHECK(drew(list, gated[i].element) == shown);
				ever[i] = ever[i] || shown;
			}
		}
		CHECK(ever[0] && ever[1] && ever[2] && ever[3]);
		opennova::fnt::fnt_free(&font);
	}
	def_free_hudpos(&file);
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
	synthetic();
	alphafade_converts();
	network_indicator();
	unauthored_hud();
	retail_leg();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_layout_test OK\n");
	return 0;
}
