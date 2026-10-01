// THE HUDLS WEAPON SLOT BAR: per carried weapon category a bracket, a "more
// available" marker when the category holds more than one weapon, the first
// weapon's hud_loadout_select icon and its key label, packed into the
// authored HUDLS_SLOT positions from slot 6 on. The per-category scan is the
// feed's (world/hud_slot_bar_feed.h). Neither shipped hudpos.def authors
// HUDLS_SYSTEM, so on retail the bar never draws; a def that does gets it.
// [orig: HUD_DrawWeaponSlotBar @0x599cd0, the dispatcher's last call
//  @0x5A7DF7 behind the HUDLS slot (dword_2723CD8)]

#include <runtime/hud/hud_frame.h>

namespace opennova::hud {

void HudFrameCompiler::element_weapon_slot_bar(const HudFrameState &state, float w, float h) {
	if (!state.declutter_visible[kDeclutterHudLs]) return;
	const HudSlotBarLayout &ls = layout_.hudls;
	// HUDLS_SYSTEM set and not on the death screen [orig: `dword_2723700 &&
	// !g_DeathScreenActive` @0x599CF8; the death-screen arm returns before
	// the call anyway @0x5A7C68].
	if (ls.system == 0 || state.combat.death_screen) return;
	// One colour for every quad: the stance icon colour OR'd with the
	// ALPHAFADE base alpha (an OR, not a replace) [orig: `dword_2723AE8 |
	// (dword_2723614 << 24)` @0x599DA7].
	const int base_alpha = static_cast<int>(layout_.alpha_fade_base *
			static_cast<float>(kPercentToAlpha));
	const uint32_t color = layout_.stance_tint | (static_cast<uint32_t>(base_alpha) << 24);
	// The bordered quad at a design rect [orig: HUD_DrawTexturedQuadWithBorder
	// @0x590C40 — the corners through Viewport_ScaleToVirtualCoords, the
	// texture window off the authored extent].
	const auto quad = [&](int x, int y, int qw, int qh, int32_t texture) {
		const float x0 = sx(static_cast<float>(x), w);
		const float y0 = sy(static_cast<float>(y), h);
		const float x1 = sx(static_cast<float>(x + qw), w);
		const float y1 = sy(static_cast<float>(y + qh), h);
		const BorderedQuadUv uv = bordered_quad_uv(qw, qh, x0, y0, x1, y1);
		emit_rect_uv(x0, y0, x1, y1, uv.u0, uv.v0, uv.u1, uv.v1, color, texture);
	};
	// The visit order is categories 1..9 then 0 — 1..5 never hold an entry, so
	// 6, 7, 8, 9, 0 — and the position cursor starts at HUDLS_SLOT 6, stepping
	// one entry per DRAWN category; a (0, 0) entry skips the category without
	// stepping, so every later category skips too [orig: `draw_slot` 1..10
	// with 10 -> 0 @0x599DC4..0x599DCD; the cursor unk_2723764 @0x599DAF,
	// advanced +8 @0x599EDE; the zero test @0x599DE1..0x599DEA].
	int cursor = 5;
	bool drew = false;
	for (int draw = 1; draw < 11; ++draw) {
		const int category = draw == 10 ? 0 : draw;
		const HudSlotBarSlot &slot = state.slot_bar[static_cast<size_t>(category)];
		if (!slot.present || cursor >= 10) continue;
		const int x = ls.slot_x[static_cast<size_t>(cursor)];
		const int y = ls.slot_y[static_cast<size_t>(cursor)];
		if (x == 0 && y == 0) continue;
		// The bracket [orig: @0x599DF2..0x599E0E, dword_272378C/90/94].
		if (ls.bracket_texture_valid)
			quad(x, y, ls.bracket_tex_w, ls.bracket_tex_h, kHudTexSlotBarBracket);
		// More than one weapon in the category: the marker at the signed
		// offset [orig: `cmp count, 1; jle` @0x599E1A; movsx byte_2723733/34
		// @0x599E30 / @0x599E42].
		if (slot.count > 1 && ls.moreav_texture_valid)
			quad(x + ls.moreav_dx, y + ls.moreav_dy, ls.moreav_tex_w, ls.moreav_tex_h,
					kHudTexSlotBarMoreAv);
		// The first weapon's icon, when its def loaded one [orig: `cmp
		// [eax+1BCh], 0` @0x599E5A; the w/h pair @0x599E67..0x599E6D].
		if (slot.icon_valid)
			quad(x, y, slot.icon_w, slot.icon_h, kHudTexSlotBarIcon0 + category);
		// The key label at the key offset, bold, the snapshot twin's colour,
		// left [orig: KeyBinding_FormatDisplayString @0x599E9F;
		// HUD_DrawTextAtVirtualPos(.., x + dword_2723718, y + dword_272371C, 0,
		// text, &g_HUDLabelFontBold, g_HUDColors.active, 0) @0x599EA4..0x599ED2].
		emit_text_at_virtual_pos(label_font_bold_, label_scale_, slot.key_label.c_str(),
				x + ls.key_ofst_x, y + ls.key_ofst_y, w, h, hud_colors_active_, 0);
		++cursor;
		drew = true;
	}
	if (drew) ++draw_list_.elements_drawn;
}

} // namespace opennova::hud
