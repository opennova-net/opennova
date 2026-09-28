// The hudpos.def parse applied to the HUD layout (hud/hud_layout_from_hudpos.h),
// pinned where it used to live in the HudOverlay binding and the HudPos GUT
// suite (ADR 0040 ladder E3b): the corner rects against the one x,y,w,h rect,
// the 4-field positioned records, the packed colours, the spinmap extent gate,
// the stance slots by id (later wins), the last-authored static frame and the
// font fallback — on a synthetic file, then on the shipped hudpos.def.
// [orig: HUD_ParseHudposToken @0x59f370; HUD_DrawHealthBar @0x5a2e50;
//  HUD_DrawPowerThrowChargeBar @0x599830]
#include <runtime/hud/hud_layout_from_hudpos.h>

#include <formats/def/def.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common/retail_paths.h"

using namespace opennova::def;
using namespace opennova::hud;

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
			"fonthud1_lo\tfontlo\n"
			"HUDHEALTH\t2,739,141,757\n"
			"HUDHEAT\t10,20,30,50\n"
			"HUDPOWERBAR\t20,720,72,11\n"
			"AMMOCOUNTPOS\t128,597,0,right\n"
			"HUDWEAPONNAME\t11,630,1,left\n"
			"GAMEINFO\t1013,430\n"
			"HUDCHATTEXT\t8,600\n"
			"HUDSYSTEXT\t8,640\n"
			"LFP_FLAGS\t1020 , 27\n"
			"HUDVEHSTANCEPOS\t0 272\n"
			"HUDCLIP\t900,700\n"
			"HUDSTANCEPOS\t5,760\n"
			"HUDSCOPERANGEXY\t100,101\n"
			"HUDSCOPEZEROXY\t102,103\n"
			"HUDSCOPEMAGXY\t104,105\n"
			"SHOWIMPACTDISTPOS\t200,201\n"
			"HUDWPNICON\t202,203\n"
			"HUDGEARTEXT\t204,205\n"
			"CARGOPOS\t206,207\n"
			"HUDAGLRADIUS\t7\n"
			"HUDAGLTLRX\t300,301\n"
			"HUDAGLYLEN\t302,303\n"
			"AGLCOLOR\t1,2,3,4\n"
			"hud_textcolor\t251,213,5\n"
			"stancecolor_good\t200,5,249,12\n"
			"tagcolor_bad\t300,-1,10\n"
			"HUDSPINMAPX1\t810\n"
			"HUDSPINMAPX2\t1020\n"
			"HUDSPINMAPY1\t552\n"
			"HUDSPINMAPY2\t762\n"
			"SPINMAPWPDISTOFF\t17\n"
			"MAPCOORDS\t530,720,1\n"
			"alphafade\t30 50 3\n"
			"HUDSTANCE\t0 1 2 stance_1.tga STAND\n"
			"HUDSTANCE\t3 4 5 stance_4.tga PRONE\n"
			"HUDSTANCE\t0 6 7 stance_x.tga STAND2\n"
			"HUDSTANCE\t9 8 8 nope.tga NOPE\n"
			"StaticFrame\tH_BlkHLin.tga  512,720\n"
			"StaticFrame\tCompMark.tga  508,685\n"
			"PARACHUTEICON\tchute.tga 400,401\n"
			"ARMORICON\tarmor.tga 402,403\n"
			"BREATHTIME\t\t512,70,center\n";
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
	CHECK(layout.alpha_fade_base == 30.0f && layout.alpha_fade_max == 50.0f &&
			layout.alpha_fade_seconds == 3.0f);
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
	// The fonts: each key fills its own name, and the width pick has no
	// fallback from one to the other [orig: HUD_SelectHudposFont @0x591890].
	CHECK(assets.font_lo == "fontlo" && assets.font_hi.empty());
	CHECK(hudpos_font_for_width(assets, 640) == "fontlo");
	CHECK(hudpos_font_for_width(assets, 641).empty());
	def_free_hudpos(&file);

	// A bare `fonthud1` line (the JOX hudpos.def's) is no key: HUD_ParseHudposToken
	// compares whole tokens with _stricmp, so both names stay empty and the HUD
	// slot takes the bold label font.
	static const char bare_font[] = "fonthud1\tGunpb18b.fnt\nHUDCHLINE\t9\n";
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
			"fonthud1_hi\tfonthi\n"
			"fonthud1_lo\tfontlo\n"
			"HUDCHLINE\t12\n"
			"HUDSPINMAPX1\t100\n"
			"HUDSPINMAPX2\t100\n"
			"HUDSPINMAPY1\t50\n"
			"HUDSPINMAPY2\t50\n";
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
	def_free_hudpos(&file);
}

static void retail_leg() {
	const std::string fixture = retail::reference_fixture("def/hudpos.def");
	if (fixture.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/hudpos.def (the shipped HUD layout table)");
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
	CHECK(layout.alpha_fade_base == 30.0f && layout.alpha_fade_max == 50.0f &&
			layout.alpha_fade_seconds == 3.0f);
	CHECK(layout.chat_lines == 8);
	CHECK(layout.veh_stance_pos.x == 0 && layout.veh_stance_pos.y == 272);
	CHECK(layout.lfp_anchor_x == 1020 && layout.lfp_anchor_y == 27);
	def_free_hudpos(&file);
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
	synthetic();
	retail_leg();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_layout_test OK\n");
	return 0;
}
