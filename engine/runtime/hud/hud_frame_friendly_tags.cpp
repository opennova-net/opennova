// THE FRIENDLY TAGS: the per-entity label pass over the gathered tags (world/friendly_tags.h,
// inmatch::collect_friendly_tags): the squad-colour override, the name and its distance fade,
// the revive count, the medic cross and the radio-request icon, drawn in screen pixels at the
// anchors the presenter projects. Split off hud_frame.cpp (one responsibility per TU).
// [orig: HUD_DrawFriendlyTagsPass @0x5A4480 -> HUD_DrawEntityLabel @0x5A39B0]

#include <base/title/title.h> // Title::hud_tag_ranges
#include <runtime/hud/hud_frame.h>

#include <runtime/hud/hud_medic_cross.h>

#include <string>

using namespace opennova::fnt;

namespace opennova::hud {

uint32_t friendly_tag_squad_color(int band, bool has_slot, uint8_t squad_index,
		uint32_t tier_rgb) {
	// [orig: HUD_DrawEntityLabel — the slot's +0x33 byte, `test al,al; jbe`
	//  @0x5A3CCA / @0x5A3D04; the table read g_SquadColors[idx] @0x5A3CD5 /
	//  @0x5A3D0D]. Band 0 is retail's classifier value 2 (good), band 1 its 1.
	if (!has_slot || squad_index == 0 || band > 1 || squad_index >= kHudSquadColors.size())
		return tier_rgb;
	const uint32_t squad = kHudSquadColors[squad_index];
	if (band == 0) return squad;
	// Each channel times 0.7 (dbl_7D9DE8) through fistp under the chop control
	// word, the byte stored back; the alpha byte stays the table's
	// [orig: @0x5A3D18..0x5A3DBC].
	const auto chop = [](uint32_t channel) {
		return static_cast<uint32_t>(static_cast<int32_t>(static_cast<double>(channel) * 0.7)) &
				0xFFu;
	};
	return (squad & 0xFF000000u) | (chop((squad >> 16) & 0xFFu) << 16) |
			(chop((squad >> 8) & 0xFFu) << 8) | chop(squad & 0xFFu);
}

void HudFrameCompiler::element_friendly_tags(const HudFrameState &state, float surface_w) {
	// D-HUD-20 [orig: HUD_DrawEntityLabel @ 0x5a39b0]. Screen-pixel anchors
	// like the attach labels — the presenter projects, the compiler draws.
	// Friendly tags draw with the NORMAL Arial label font at the slot scale
	// [orig: @0x5a3a0c loads g_HUDLabelFont @0xB4C388 (slot +0)], the death
	// screen's spectate target with the large one below.
	const bool have_label = label_font_.font() != nullptr;
	const GameFont &normal_font = have_label ? label_font_ : font_;
	const float normal_scale = have_label ? label_scale_ : hud_font_scale_;
	if (state.friendly_tag_mode == 0 || state.friendly_tags.empty() ||
			normal_font.font() == nullptr) {
		return;
	}
	for (HudFriendlyTag tag : state.friendly_tags) {
		// The tracked spectate target (g_DeathScreenTarget), whatever the screen.
		const bool target = state.session.spectate_target != 0xFFFF &&
				tag.handle == state.session.spectate_target;
		// The target's slot under a death-screen sub-mode takes the large label
		// font [orig: @0x5a3a05..0x5a3a2e — g_DeathScreenTarget, g_DeathScreenSubMode, the
		// slot's +0x24 entity against the target; g_HUDLabelFontLarge @0xB4C3A0
		// (slot +0x18, Impac22b) loaded @0x5a3a29].
		const bool large = target && tag.has_slot && state.session.spectate_submode != 0 &&
				label_font_large_.font() != nullptr;
		const GameFont &lf = large ? label_font_large_ : normal_font;
		const float ls = large ? label_large_scale_ : normal_scale;
		// The line metric is the '0' glyph's height at the slot scale
		// [orig: GameFont_MeasureCharHeight ('0', font) @ 0x5a3a36 — the helper
		// multiplies by the slot's scale_y @ 0x580a80].
		const float font_h = lf.char_height('0', ls);
		// Too close to draw, unless it is the target [orig: dist >= 0x8000
		// gate @ 0x5a3b0c, `cmp g_DeathScreenTarget, ebx; jnz` @0x5a3b0e..0x5a3b14],
		// and the fog cull [orig: dist <= g_EnvFogDistCurrent @ 0x5a3b28].
		if (tag.dist_q16 < kFriendlyTagMinDistQ16 && !target) {
			continue;
		}
		if (tag.dist_q16 > state.fog_dist_q16) {
			continue;
		}
		// On the death screen the target's tag is pinned at (surface width / 2,
		// 2 x fontH), past the clip test; any other clipped tag bails
		// [orig: @0x5a3b59..0x5a3b7c (dword_A78394 sar 1, `lea ecx, [edi+edi]`),
		// the clip test @0x5a3b7e..0x5a3b80].
		if (state.combat.death_screen && target) {
			tag.screen_x = static_cast<float>(static_cast<int32_t>(surface_w) >> 1);
			tag.screen_y = 2.0f * font_h;
		} else if (!tag.projected) {
			continue;
		}
		// Health tier -> the hudpos tag colors. The GOOD tier is scheme-
		// swapped: tagcolor_good only under hud_color_index == 2, else the
		// scheme table entry; middle/bad never swap [orig:
		// HUD_ClassifyHealthBand @ 0x5a3c2b; the index test + colors
		// @ 0x5a3c9e..0x5a3cf5; cfg default 2 @ 0x54d2a6].
		// On the death screen the colour is the team's, past the health tiers, the
		// squad override and the downed legs [orig: @0x5a3c33..0x5a3c66 (team 1
		// g_HudposTagcolorBlueteam, team 2 g_HudposTagcolorRedteam, else
		// dword_840A40 = 0xFF208020), the jump to @0x5a3e85].
		const int band = health_color_band_fp16(tag.health_ratio_fp16);
		uint32_t rgb = 0;
		if (state.combat.death_screen) {
			rgb = tag.team == 1 ? layout_.tag_blueteam
					: (tag.team == 2 ? layout_.tag_redteam : kFriendlyTagDeathScreenNeutral);
		} else {
			rgb = band == 0 ? (state.hud_color_index == 2 ? layout_.tag_good : active_color(state))
					: band == 1 ? layout_.tag_middle
								: layout_.tag_bad;
			// The squad colour override of the good and middle tiers.
			rgb = friendly_tag_squad_color(band, tag.has_slot, tag.squad_color_index, rgb);
			// The bad tier's DOWNED legs: a dead entity with a slot still inside
			// its revive window is light blue (table[3]), pulsing toward white
			// while a medic request stands; dead without that is gray (table[8]);
			// alive-but-bad keeps tagcolor_bad [orig: @0x5a3dc9..0x5a3e85 —
			// `Flags & 2` -> slot && slot+0x10 ? (slot+0x2C ? pulse : light blue)
			// : gray, else tagcolor_bad].
			if (band == 2 && tag.dead) {
				if (tag.has_slot && tag.revive_seconds != 0) {
					rgb = tag.medic_request
							? friendly_tag_revive_pulse(kFriendlyTagDownedLightBlue,
									  static_cast<int>(state.hud_frame))
							: kFriendlyTagDownedLightBlue;
				} else {
					rgb = kFriendlyTagDownedGray;
				}
			}
		}
		// The speaking pulse rides the mixer meter's first stage [orig: @ 0x5a3e8f..0x5a3e91].
		if (tag.speaking) {
			rgb = friendly_tag_speaking_blend(rgb, state.speaking_level);
		}
		// Distance alpha 255 -> 63 over 50..300 m [orig: @ 0x5a3eeb..0x5a3f18].
		const uint32_t argb =
				(static_cast<uint32_t>(friendly_tag_alpha(tag.dist_q16)) << 24) |
				(rgb & 0xFFFFFFu);
		const float top_y = tag.screen_y - font_h * 0.5f; // [orig: @ 0x5a4264]
		// The revive count rides the label while the entity is dead with a
		// slot inside its window [orig: the gate `dead && slot && slot+0x10`
		// @0x5a3fdc..0x5a3ff8 (text), @0x5a41c0..0x5a41df (ticks),
		// @0x5a4407..0x5a441b (bar)].
		const bool show_count = tag.dead && tag.has_slot && tag.revive_seconds != 0;
		// The tick and bar forms draw the bare count centered one fontH ABOVE
		// the projected point in the tag color, at the x the icon arm below
		// may have shifted [orig: sprintf("%ld") @0x5a41f0 / @0x5a4428 ->
		// HUD_DrawTextHalfBrightF(x, y - fontH) @0x5a4453].
		auto emit_bare_count = [&](float x) {
			if (!show_count) return;
			const std::string count = std::to_string(tag.revive_seconds);
			const GameFontRun run = lf.layout(count.c_str(), x,
					tag.screen_y - font_h, ls, ls, kFontAlignCenter,
					half_bright_keep_alpha(argb));
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
		};
		// The radio-request icon arm, gated on the tag's +885 fold AND the
		// viewer gate [orig: HUD_DrawEntityLabel `test ebp, ebp; jz` +
		// `cmp var_DC, 0; jz` @0x5a415a..0x5a4163 (tick), @0x5a4270..0x5a427d
		// (text), @0x5a43a0..0x5a43a9 (bar)]: the TSDicon cell 0x17 in table[3]
		// light blue at forced full alpha, half-size fontH*0.5 centered on
		// (cx, cy) [orig: HUD_DrawRotatedIconQuad @0x599630]. The raw diffuse,
		// as for the map blips: the strip's material 0x651 runs its MODULATE2X
		// stage on the device [orig: Render_DrawIconStripCell_Debug @0x67bae0].
		const bool request_icon = tag.radio_request && state.radio_request_icon_viewer;
		const float half_h = font_h * 0.5f;
		auto emit_request_icon = [&](float cx, float cy) {
			HudQuad icon;
			icon.x0 = cx - half_h;
			icon.y0 = cy - half_h;
			icon.x1 = cx + half_h;
			icon.y1 = cy + half_h;
			hud_icon_strip_cell_uv(state.minimap, kFriendlyTagRadioRequestIcon,
					icon.u0, icon.v0, icon.u1, icon.v1);
			icon.color = 0xFF000000u | (kFriendlyTagDownedLightBlue & 0xFFFFFFu);
			icon.texture = kHudTexMapIcons;
			draw_list_.quads.push_back(icon);
		};
		// The tick/bar forms: a medic tag pre-shifts x by -fontH/2, the icon
		// centers at (x - fontH/2, y - fontH/2), then a medic tag shifts x by
		// +fontH for the count [orig: @0x5a4165..0x5a41bc (tick) /
		// @0x5a43ab..0x5a4403 (bar): `cdq; sub; sar 1; neg; add axis`,
		// `fisubr axis` / `fild cosVal; fsub` at the icon, `add axis, ebx`].
		auto tick_form_icon_x = [&](float x) {
			if (!request_icon) return x;
			if (tag.medic) x -= half_h;
			emit_request_icon(x - half_h, tag.screen_y - half_h);
			if (tag.medic) x += font_h;
			return x;
		};

		// A slot entry with an empty callsign draws the bar form
		// [orig: the empty-name leg @ 0x5a4398 — y unadjusted, height fontH].
		std::string resolved = tag.name;
		if (resolved.empty()) {
			if (tag.player) {
				HudLine bar;
				bar.color = argb;
				bar.x0 = tag.screen_x;
				bar.y0 = tag.screen_y;
				bar.x1 = tag.screen_x;
				bar.y1 = tag.screen_y + font_h;
				draw_list_.lines.push_back(bar);
				emit_bare_count(tick_form_icon_x(tag.screen_x));
				continue;
			}
			// '^' + the compiled-in name table [orig: @ 0x5a4047..0x5a40cd].
			resolved = friendly_tag_fallback_name(tag.entity_id);
		}

		// The text form and the BRIEF tick by mode: Jointops.exe's FULL names every tag, its
		// FAR/BRIEF within 300 u (friendly_tag_text_visible), its BRIEF ticks. dfx2.exe's
		// FAR/BRIEF names every tag, its FULL names a tag within its reach or the HUD target
		// cursor's and ticks the rest, its BRIEF ticks (Title::hud_tag_ranges, DFX2-87)
		// [orig: HUD_DrawEntityLabel @0x5a27f0 - the switch's case 1 @0x5a2f04, case 2's `v76 >=
		//  v77 && a1 != g_HUDTargetCursorEntity` @0x5a2e47 into the tick @0x5a30a0, case 3
		//  @0x5a309a (dfx2.exe)].
		bool text_form = false;
		bool tick_form = false;
		if (state.title != nullptr && state.title->hud_tag_ranges) {
			const bool in_reach = tag.dist_q16 < friendly_tag_reach_q16(tag.player, tag.same_account);
			text_form = state.friendly_tag_mode == kFriendlyTagModeFarBrief ||
					(state.friendly_tag_mode == kFriendlyTagModeFull &&
							(in_reach || tag.cursor_target));
			tick_form = !text_form && (state.friendly_tag_mode == kFriendlyTagModeFull ||
										state.friendly_tag_mode == kFriendlyTagModeBrief);
		} else {
			text_form = friendly_tag_text_visible(state.friendly_tag_mode, tag.dist_q16);
			tick_form = !text_form && state.friendly_tag_mode == kFriendlyTagModeBrief;
		}
		if (text_form) {
			// The text form appends the count to the name
			// [orig: sprintf("%s: %ld", name, slot+0x10) @0x5a400e, else
			//  sprintf("%s", name) @0x5a422e].
			const std::string label = show_count
					? resolved + ": " + std::to_string(tag.revive_seconds)
					: resolved;
			// Centered text at the projected point, half-bright with the
			// distance alpha kept [orig: HUD_DrawTextHalfBrightF @ 0x5a4268 ->
			// CGameFont_DrawText flags 1, the slot scales pushed @ 0x580720].
			const GameFontRun run = lf.layout(label.c_str(), tag.screen_x,
					top_y, ls, ls, kFontAlignCenter,
					half_bright_keep_alpha(argb));
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
			int text_w = 0;
			int text_h = 0;
			if (request_icon || tag.medic)
				lf.measure(label.c_str(), ls, ls, &text_w, &text_h);
			// The text form's icon: x shifts by -(fontH/2 + textW/2) and STAYS
			// shifted for the plate below; the icon centers at (x - fontH/2,
			// top_y) [orig: @0x5a4288..0x5a42ee -- `sar 1; neg; sar ecx, 1;
			//  sub; add axis` @0x5a429b..0x5a42aa, `fild cosVal; fsub; fiadd
			//  fontH/2` @0x5a42cb..0x5a42da, `fisubr axis` @0x5a42e2].
			float plate_x = tag.screen_x;
			if (request_icon) {
				plate_x += -half_h - static_cast<float>(text_w) * 0.5f;
				emit_request_icon(plate_x - half_h, top_y);
			}
			if (tag.medic) {
				// The red-cross-on-white medic plate, a fontH/2 square left of
				// the text at the tag alpha, measured from the (possibly
				// icon-shifted) x [orig: rect @ 0x5a4309..0x5a436c reading
				// `axis` @0x5a430e -> HUD_DrawMedicCrossQuad @ 0x59bcb0]. The
				// quad itself is the shared primitive in hud/hud_medic_cross.h
				// (the map medic marker and the help icons draw the same
				// routine): the white field, then the two red bars, in the
				// witnessed order.
				const float x0 = plate_x -
						(static_cast<float>(text_w) * 0.5f + font_h) - 0.5f;
				const float y0 = top_y - 0.5f;
				const float x1 = x0 + font_h * 0.5f;
				const float y1 = y0 + font_h * 0.5f;
				for (const MedicCrossQuad &q : medic_cross_quads(x0, y0, x1, y1,
							 static_cast<int>(argb >> 24)))
					emit_rect(q.x0, q.y0, q.x1, q.y1, q.color, true);
			}
		} else if (tick_form) {
			// The BRIEF tick: three 1-px vertical lines at x-1/x/x+1 spanning
			// +-fontH/4 around the projected point [orig: @ 0x5a40eb..0x5a4160].
			const float half = font_h * 0.25f;
			for (int dx = -1; dx <= 1; ++dx) {
				HudLine seg;
				seg.color = argb;
				seg.x0 = tag.screen_x + static_cast<float>(dx);
				seg.y0 = tag.screen_y - half;
				seg.x1 = seg.x0;
				seg.y1 = tag.screen_y + half;
				draw_list_.lines.push_back(seg);
			}
			emit_bare_count(tick_form_icon_x(tag.screen_x));
		}
	}
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
