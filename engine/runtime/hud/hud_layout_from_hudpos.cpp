// The hudpos.def parse applied to the HUD layout globals — see
// hud_layout_from_hudpos.h for the witness map.
// [orig: HUD_ParseHudposToken @0x59f370]

#include <runtime/hud/hud_layout_from_hudpos.h>

#include <formats/def/def.h>

#include <algorithm>

using namespace opennova::def;

namespace opennova::hud {

namespace {

// Positioned text tokens keep the original's 4-dword layout: x, y, hidden
// (0 = draw), alignment (0=left 1=right 2=center). [orig: AMMOCOUNTPOS parse
// @0x59fc3d; the draws gate on the hidden dword @0x5939f3]
HudPosRecord pos_record4(const int v[4]) {
	HudPosRecord r;
	r.x = v[0];
	r.y = v[1];
	r.hidden = v[2];
	r.align = v[3];
	r.present = true;
	return r;
}

HudPosRecord pos_record2(int x, int y) {
	HudPosRecord r;
	r.x = x;
	r.y = y;
	r.present = true;
	return r;
}

// hudpos.def [4] rects are stored as corners (x1,y1,x2,y2); the witnessed draws
// read them that way. [orig: HUD_DrawHealthBar @0x5a2e50 reads
// dword_27237C8/CC/D0/D4, see docs/interface/hud-re.md]
HudRectRecord rect_from_corners(int x1, int y1, int x2, int y2) {
	HudRectRecord r;
	r.x = static_cast<float>(x1);
	r.y = static_cast<float>(y1);
	r.w = static_cast<float>(x2 - x1);
	r.h = static_cast<float>(y2 - y1);
	r.present = true;
	return r;
}

// HUDPOWERBAR alone is authored x,y,w,h — its witnessed consumer adds the third
// and fourth dwords to the anchor (JOX authors "20,720,72,11": as corners the
// height would be negative). [orig: HUD_DrawPowerThrowChargeBar @0x599830 draws
// (x, y)-(x+w, y+h) from dword_27237EC..F8, see docs/interface/hud-re.md]
HudRectRecord rect_from_xywh(const int v[4]) {
	HudRectRecord r;
	r.x = static_cast<float>(v[0]);
	r.y = static_cast<float>(v[1]);
	r.w = static_cast<float>(v[2]);
	r.h = static_cast<float>(v[3]);
	r.present = true;
	return r;
}

uint32_t clamp_byte(int v) {
	return static_cast<uint32_t>(std::clamp(v, 0, 255));
}

} // namespace

uint32_t hud_color_argb(const DefHudColor &c) {
	return (clamp_byte(c.a) << 24) | (clamp_byte(c.r) << 16) | (clamp_byte(c.g) << 8) |
			clamp_byte(c.b);
}

void hud_layout_from_hudpos(const DefHudPosFile &file, HudLayout &out,
		HudLayoutAssets &assets) {
	const DefHudPosDef &hud = file.hud;

	out.ammo_count = pos_record4(hud.ammo_count_pos);
	out.weapon_name = pos_record4(hud.weapon_name_pos);
	out.game_info = pos_record4(hud.game_info);
	out.wpd_info = pos_record4(hud.wpd_info);
	out.chat_text = pos_record2(hud.chat_text[0], hud.chat_text[1]);
	// The chat box's coordinate rows (the chat wrap width `x2 - (x1 - 4)`)
	// are NOT the HUDCHATTEXT anchor: retail's g_HUDChatBoxCoords are written
	// by a separate hud.def `chat_message x1 y1 x2 y2` / `sys_message` parser
	// [orig: File_ParseASCIIFile("hud.def", cb, 0x2A5A8EAD) @0x5be210..0x5be228,
	// the callback @0x5bb7a0, stores @0x5bb7d1/@0x5bb7ed/@0x5bb825/@0x5bb841],
	// and JO:CA ships no hud.def — the rows stay 0, the width is 4, and the
	// wrapper returns 1 at the first character, so a retail chat line never
	// wraps. chat_box_present stays false for the same result; a hud.def-equipped
	// title needs a formats/def reader (the file is SCR-encoded, key 0x2A5A8EAD).
	out.sys_text = pos_record2(hud.sys_text[0], hud.sys_text[1]);
	// BREATHTIME x, y, align: three fields, no hidden dword; an unauthored
	// line leaves the zero record (0, 0, left) the bar still draws at
	// [orig: HUD_ParseHudposToken @0x59FB3B..0x59FB84 -> dword_2723810/14/18].
	out.breath_time = pos_record2(hud.breath_time[0], hud.breath_time[1]);
	out.breath_time.align = hud.breath_time[2];
	// LFP_FLAGS — the AAS zone status panel's anchor (retail g_HUDZonePanelX/Y,
	// written by the hudpos parse @0x5a0563/@0x5a057b).
	out.lfp_anchor_x = hud.lfp_flags[0];
	out.lfp_anchor_y = hud.lfp_flags[1];
	out.lfp_anchor_present = true;
	// HUDVEHSTANCEPOS — the vehicle panel's base before the stance offset.
	out.veh_stance_pos = pos_record2(hud.veh_stance_pos[0], hud.veh_stance_pos[1]);
	out.clip_pos = pos_record2(hud.clip_pos[0], hud.clip_pos[1]);
	out.stance_pos = pos_record2(hud.stance_pos[0], hud.stance_pos[1]);

	// The targeting and instrument anchors (hud_combat.h): the sprites' pixel
	// sizes are the device's once it resolves the names below.
	HudCombatLayout &combat = out.combat;
	combat.impact_x = hud.impact_dist_pos[0];
	combat.impact_y = hud.impact_dist_pos[1];
	combat.icon_x = hud.wpn_icon[0];
	combat.icon_y = hud.wpn_icon[1];
	combat.gear_x = hud.gear_text[0];
	combat.gear_y = hud.gear_text[1];
	combat.cargo_x = hud.cargo_pos[0];
	combat.cargo_y = hud.cargo_pos[1];
	combat.parachute_x = hud.parachute_icon.x;
	combat.parachute_y = hud.parachute_icon.y;
	combat.armor_x = hud.armor_icon.x;
	combat.armor_y = hud.armor_icon.y;
	combat.agl_tick_width = hud.agl_radius;
	combat.agl_left = hud.agl_tlrx[0];
	combat.agl_right = hud.agl_tlrx[1];
	combat.agl_y = hud.agl_ylen[0];
	combat.agl_height = hud.agl_ylen[1];
	combat.agl_color = hud_color_argb(hud.agl_color);
	assets.parachute_icon = hud.parachute_icon.texture;
	assets.armor_icon = hud.armor_icon.texture;

	out.scope_range = pos_record2(hud.scope_range[0], hud.scope_range[1]);
	out.scope_zero = pos_record2(hud.scope_zero[0], hud.scope_zero[1]);
	out.scope_mag = pos_record2(hud.scope_mag[0], hud.scope_mag[1]);
	out.health_rect = rect_from_corners(hud.health[0], hud.health[1], hud.health[2], hud.health[3]);
	out.heat_rect = rect_from_corners(hud.heat[0], hud.heat[1], hud.heat[2], hud.heat[3]);
	out.power_rect = rect_from_xywh(hud.powerbar);
	// The spinmap's corner pair; the map draws only with a positive extent.
	out.spinmap_rect = rect_from_corners(hud.spinmap_x1, hud.spinmap_y1, hud.spinmap_x2,
			hud.spinmap_y2);
	out.spinmap_rect.present = out.spinmap_rect.w > 0.0f && out.spinmap_rect.h > 0.0f;
	out.spinmap_wp_dist_off = hud.spinmap_wp_dist_off;
	out.map_coords_x = static_cast<float>(hud.map_coords[0]);
	out.map_coords_y = static_cast<float>(hud.map_coords[1]);
	out.map_coords_off = hud.map_coords[2];

	out.health_border = hud_color_argb(hud.health_border);
	out.tag_good = hud_color_argb(hud.tagcolor_good);
	out.tag_middle = hud_color_argb(hud.tagcolor_middle);
	out.tag_bad = hud_color_argb(hud.tagcolor_bad);
	out.hud_text = hud_color_argb(hud.hud_textcolor);
	out.weapon_text = hud_color_argb(hud.weapon_textcolor);
	out.stance_tint = hud_color_argb(hud.stanceicon_color);
	out.heat_border = hud_color_argb(hud.heat_border);
	// The whole stance colour triple: the vehicle panel bands its seats with the
	// same three the stance bar reads [orig: the good/middle/bad arms of the
	// seat loop in HUD_DrawVehicleHealthBars @0x5a4fd0, see docs/interface/hud-re.md].
	out.stance_good = hud_color_argb(hud.stancecolor_good);
	out.stance_middle = hud_color_argb(hud.stancecolor_middle);
	out.stance_bad = hud_color_argb(hud.stancecolor_bad);

	out.alpha_fade_base = hud.alpha_fade[0];
	out.alpha_fade_max = hud.alpha_fade[1];
	out.alpha_fade_seconds = hud.alpha_fade[2];
	out.chat_lines = hud.hud_chline > 0 ? hud.hud_chline : 8;

	// HUDSTANCE's explicit id addresses the retail slot arrays; file order is
	// irrelevant and a later record for the same id replaces the earlier one.
	for (size_t i = 0; i < hud.stances_count; ++i) {
		const DefHudStance &stance = hud.stances[i];
		if (stance.id < 0 || stance.id >= 6) continue;
		const size_t slot = static_cast<size_t>(stance.id);
		assets.stance_textures[slot] = stance.texture;
		out.stance_offset_x[slot] = stance.offset_x;
		out.stance_offset_y[slot] = stance.offset_y;
	}

	// Static HUD frame background. Retail keeps ONE static frame and the LAST
	// authored line wins, so the index comes from the engine-side policy rather
	// than being assumed here [orig: HUD_ParseHudposToken @0x59F370, see
	// hud_static_frame_index in hud_frame.h].
	const int frame_index = hud_static_frame_index(static_cast<int>(hud.static_frames_count));
	if (frame_index >= 0) {
		const DefHudGraphic &frame = hud.static_frames[frame_index];
		out.frame_pos = pos_record2(frame.x, frame.y);
		assets.static_frame = frame.texture;
	}

	// The HUD font named by hudpos (hi first, lo fallback).
	assets.font = hud.font_hi[0] != '\0' ? hud.font_hi : hud.font_lo;
}

} // namespace opennova::hud
