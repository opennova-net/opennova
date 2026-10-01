// The capture-point labels: the walk and the element -- see hud_capture_labels.h.

#include <runtime/hud/hud_capture_labels.h>

#include <cmath>
#include <cstdio>
#include <string>

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_math.h>

namespace opennova::hud {

namespace {

// Team 1 palette[3], team 2 palette[5], else palette[1] -- the marker's team
// byte and the bar's entry[2] pick alike [orig: @0x5a2a13..0x5a2a2f;
// @0x5a2b14..0x5a2b32].
uint8_t team_palette(int32_t team) {
	return team == 1 ? 3 : (team == 2 ? 5 : 1);
}

} // namespace

void capture_point_label_walk(const std::vector<HudMinimapMarker> &markers, uint8_t local_team,
		const GameTextLookup &gametext, std::vector<HudCaptureLabel> &out) {
	out.clear();
	// The 1160 slots from word_28E5620 in address order: transient (328),
	// persistent (328), special (504) [orig: @0x5a2862 / count 0x488
	// @0x5a2888, stride 32 @0x5a2bc9].
	for (const HudMinimapBank bank : { HudMinimapBank::kTransient, HudMinimapBank::kPersistent,
				 HudMinimapBank::kSpecial }) {
		for (const HudMinimapMarker &m : markers) {
			if (static_cast<HudMinimapBank>(m.bank) != bank)
				continue;
			// [orig: the free handle @0x5a2895, `test byte [edi+3], 40h`
			//  @0x5a289f, the pool-0..4 handle and its capacity
			//  @0x5a28b2..0x5a28db, the pool entity @0x5a28f0 and its def
			//  +0x20 @0x5a28f8]
			if (m.handle == 0xFFFF || (m.flags & 0x40u) != 0 || !m.entity_known)
				continue;
			// The SpawnPoint def attrib, a zone number, and a capture bit in
			// the slot's source byte [orig: `test [eax+54h], 40000h`
			//  @0x5a2903, `cmp [esi+21Ah], bl` @0x5a2910, `test byte [edi+4],
			//  0C0h` @0x5a291c].
			if ((m.entity_bits & kMarkerEntityZoneDef) == 0 || m.zone_number == 0 ||
					(m.source & 0xC0u) == 0)
				continue;
			HudCaptureLabel label;
			// label_above starts " "; name and label_below start empty
			// [orig: the word store 0x0020 @0x5a2935, @0x5a293f, @0x5a2943].
			label.letter = " ";
			if (m.timer_known) {
				// With a zone-timer entry: the spawn-zone index names the zone
				// (WPNames/STRWPNAME%03d of index + 1, folded through
				// Overlays/STROVER_OBJECTIVEPOINT_SHORT) and letters it 'A' +
				// index; a zone off the list indexes -1 [orig:
				//  SpawnZoneList_IndexOf @0x43b990 (miss -1) @0x5a294d;
				//  sprintf @0x5a2962, GameText_GetString @0x5a2971 / @0x5a2984,
				//  sprintf @0x5a2992; sprintf("%c", index + 'A') @0x5a29a8].
				const int32_t index = m.zone_index;
				char key[32];
				std::snprintf(key, sizeof key, "STRWPNAME%03d", index + 1);
				HudTextArg name;
				name.text = game_text(gametext, "WPNames", key, "");
				label.name = hud_sprintf(
						game_text(gametext, "Overlays", "STROVER_OBJECTIVEPOINT_SHORT", ""),
						std::vector<HudTextArg>{ name });
				// %c prints the int as one byte; a NUL byte ends the string.
				const char letter = static_cast<char>(static_cast<uint8_t>(index + 'A'));
				label.letter = letter != '\0' ? std::string(1, letter) : std::string();
				label.timer = true;
				// The bar's colour by entry[2] and its fill entry[8] / entry[10]
				// while entry[12], else 0 [orig: @0x5a2b14..0x5a2b32;
				//  `cmp [ebp+30h], ebx` @0x5a2b8b, fild / fidiv @0x5a2b94..0x5a2b97,
				//  fldz @0x5a2b9c]. A stock server's value entry always carries
				//  the limit 0x10000 s (x62), never zero [orig:
				//  Server_UpdateCaptureZoneEntities `push 10000h` @0x51979d].
				label.bar_palette = team_palette(m.timer_bar_team);
				label.bar_fraction = m.timer_active
						? static_cast<float>(double(m.timer_value) / double(m.timer_limit))
						: 0.0f;
			}
			// (Retail rebuilds the zone entity's cached placement matrix
			// entity+0xB4 here unless entity+0x24 & 0x20000 [orig:
			// @0x5a29b0..0x5a2a0b]; the label reads only entity+4..+0xC and the
			// port derives placements from the live pose, so nothing ports.)
			label.palette = team_palette(m.team);
			// The viewer's own zone whose value drains while entry[1] names
			// its team flashes [orig: @0x5a2a35..0x5a2a5b -- the team byte,
			//  the entry, `cmp [ebp+2Ch], ebx; jge`, movsx team vs entry[1]].
			label.flash = m.team == local_team && m.timer_known && m.timer_rate < 0 &&
					m.timer_team == static_cast<int32_t>(static_cast<int8_t>(m.team));
			label.type = m.team == local_team ? kMarkerTypeZoneOwn : kMarkerTypeZoneOther;
			label.position = { m.entity_x, m.entity_y,
				int32_t(uint32_t(m.entity_z) + uint32_t(kCaptureLabelLiftQ16)) };
			out.push_back(std::move(label));
		}
	}
}

// Each label through HUD_DrawEntityMarker(pos, type, 1.0, colour, 0, letter,
// name, "") and, in binoculars with a timer entry, the progress bar under it.
// [orig: Render_CapturePointLabels @0x5a2a62..0x5a2bbd -> HUD_DrawEntityMarker
//  @0x593140 (types 8/9 @0x5936a1..0x593742, the labels @0x59374a..0x5937cb),
//  HUD_DrawProgressBar @0x59B340]
void HudFrameCompiler::element_capture_point_labels(const HudFrameState &s, float w, float h) {
	const auto &labels = s.combat.capture_labels;
	if (w <= 0 || h <= 0 || labels.empty())
		return;
	const int32_t surface_w = int32_t(w), surface_h = int32_t(h);
	// Outside binoculars the name is cleared before the draw [orig:
	// `cmp g_BinocularsViewActive, bl` @0x5a2a62..0x5a2a6a].
	const bool binoculars = s.binoculars_view_active;
	// Every primitive keeps retail's submission order: the stem, then the
	// tile under the letter and the name, then the bar, label by label.
	mark_order_break();
	for (const HudCaptureLabel &label : labels) {
		uint32_t color = hud_palette(label.palette);
		// [orig: `test byte g_HUDFrameCounter, 18h; jnz` @0x5a2a54]
		if (label.flash && (s.ticks & 0x18) == 0)
			color = kCaptureLabelFlashColor;
		// Behind the near plane, or clipped by any frustum plane, the marker
		// draws nothing and returns 0, which also skips the bar [orig: the
		//  near test @0x593176..0x593189; HUD_ClipPointToFrustumAndProject
		//  @0x593194 nonzero -> return 0; `cmp eax, ebx; jz` @0x5a2ac1].
		if (!label.point.valid || label.point.clip != 0 || label.point.depth_q16 <= 0)
			continue;
		// The projected pixel is integral (the projection's `>> 16`).
		const int32_t x = int32_t(std::floor(label.point.x));
		const int32_t y = int32_t(std::floor(label.point.y));
		// The marker's half size: the letter's height in the HUD slot times
		// the 1.0 scale; an empty letter takes scale * 20.0 [orig:
		//  GameFont_MeasureCharHeight(label_above[0], slot 0x2723C74)
		//  @0x5931bf, fmul scale @0x5931cf; flt_7D8E60 @0x5931e4].
		const int32_t half = label.letter.empty()
				? int32_t(1.0f * 20.0f)
				: int32_t(font_.char_height(uint8_t(label.letter[0]), hud_font_scale_) * 1.0f);
		// The stem, straight up in the full colour [orig:
		//  Render_ClipAndDrawLine2DToRect @0x59321d].
		draw_list_.lines.push_back(
				{ float(x), float(y), float(x), float(y - half), 1, color });
		mark_order_break();
		// The tile: the stem's top and the 2 * half extent into design
		// space, centred half its height above that top, the half-bright
		// colour; type 8 lfp_alf, type 9 lfp_dlf. An unloaded tile draws no
		// quad [orig: Viewport_ScreenToVirtual @0x5936cc / @0x5936e0; the
		//  pick @0x5936f6..0x593724; HUD_DrawTexturedQuadCentered(x, y - h / 2,
		//  w, h, tex, (c >> 1 & 0x7F7F7F) | 0xFF000000, 0) @0x593742].
		const bool own = label.type == kMarkerTypeZoneOwn;
		if (own ? layout_.lfp_tile_own_texture_valid : layout_.lfp_tile_other_texture_valid) {
			const int32_t vx = screen_to_design_x(x, surface_w);
			const int32_t vy = screen_to_design_y(y - half, surface_h);
			const int32_t vw = screen_to_design_x(2 * half, surface_w);
			const int32_t vh = screen_to_design_y(2 * half, surface_h);
			emit_textured_quad_centered(vx, vy - vh / 2, vw, vh,
					own ? kHudTexLfpTileOwn : kHudTexLfpTileOther,
					((color >> 1) & 0x7F7F7Fu) | 0xFF000000u, w, h);
		}
		// The labels two pixels left, centred and half-bright in the HUD slot:
		// the letter at trunc(y - half * 2.3f) (the types 8/9 arm replaces the
		// 2.6f row), the name at the ground point [orig: `sub
		//  [esp+50h+isReflectionPass], 2` @0x59374e; flt_7D8E50 = 2.6f
		//  @0x593764, flt_7D8E4C = 2.3f @0x593781..0x593789;
		//  HUD_DrawTextCentered_HalfBright(&byte_2723C74, ..., 1) @0x5937a3 /
		//  @0x5937cb].
		const float text_x = float(x - 2);
		if (!label.letter.empty()) {
			const int32_t above = int32_t(double(y) - double(half) * double(2.3f));
			emit_half_bright_text(font_, hud_font_scale_, label.letter.c_str(), text_x,
					float(above), color, 2);
		}
		if (binoculars && !label.name.empty())
			emit_half_bright_text(font_, hud_font_scale_, label.name.c_str(), text_x, float(y),
					color, 2);
		mark_order_break();
		// The bar, in binoculars with a timer entry: 72 x 8 design units
		// about the point, its top 20 design-WIDTH units below it (the 10
		// scaled beside it is never read) [orig: @0x5a2ac9..0x5a2ad7;
		//  Viewport_ScaleToVirtualCoords(36, 8) @0x5a2b67, (20, 10)
		//  @0x5a2b7b, `add eax, [esp+1E8h+var_1A4]` @0x5a2b84;
		//  HUD_DrawProgressBar(x - w, y', x + w, y' + h, c, c, f, 0) @0x5a2bbd].
		if (binoculars && label.timer) {
			const int32_t bar_w = design_to_screen_x(36, surface_w);
			const int32_t bar_h = design_to_screen_y(8, surface_h);
			const int32_t top = y + design_to_screen_x(20, surface_w);
			const uint32_t bar = hud_palette(label.bar_palette);
			emit_progress_bar(x - bar_w, top, x + bar_w, top + bar_h, bar, bar,
					label.bar_fraction, false);
			mark_order_break();
		}
	}
}

} // namespace opennova::hud
