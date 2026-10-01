// THE AAS ZONE STATUS PANEL element: the spawn-zone list grouped by owning
// team, one marker per contested zone, the group's status text
// [orig: HUD_DrawZoneStatusPanel @0x5a2480 -> HUD_DrawZoneMarker @0x5986f0].
// hud/hud_lfp_panel.h owns the policy (frames, colours, stepping); this is
// the walk that puts it on screen. The feed (world/lfp_feed.h) supplies one
// HudLfpZone per list entry.

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_lfp_panel.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace opennova::hud {

namespace {

constexpr int kLfpTeamMax = 30; // [orig: the 30-dword count table @0x5a24b9]

} // namespace

void HudFrameCompiler::element_lfp_panel(const HudFrameState &state, float w,
		float h) {
	const HudLfpPanelState &lp = state.lfp_panel;
	if (!lp.shown || !layout_.lfp_anchor_present) return;
	// The conquest arm (one row per marker, text per marker) is unmodelled
	// [orig: g_GameType == 0x50010 @0x5a24a1]; nothing draws under it here.
	if (lp.conquest_mode) return;
	if (font_.font() == nullptr) return;

	// Pass 1: zones per team [orig: @0x5a24c5..0x5a24ea — team byte & 0x1F,
	// nonzero and < 30].
	std::array<int, kLfpTeamMax> per_team{};
	for (const HudLfpZone &z : lp.zones) {
		if (z.team != 0 && z.team < kLfpTeamMax) ++per_team[static_cast<size_t>(z.team)];
	}
	// The seed and the walk test the team byte and the transient slot's 0xC0
	// flags; the timer entry gates only the marker itself, never the seed
	// [orig: the seed search @0x5a2501..0x5a2575 reads the team and the slot
	//  flags only; in the walk CProximityList_FindEntryById @0x5a2689 skips
	//  past the marker and the flags test @0x5a26ef follows it]. Without a
	// seed the panel draws nothing at all.
	const auto flagged = [](const HudLfpZone &z) {
		return z.team != 0 && (z.capture_flags & 0xC0u) != 0;
	};
	const auto contested = [&](const HudLfpZone &z) {
		return flagged(z) && z.timer_present;
	};
	size_t start = lp.zones.size();
	for (size_t i = 0; i < lp.zones.size(); ++i) {
		if (flagged(lp.zones[i])) {
			start = i;
			break;
		}
	}
	if (start >= lp.zones.size()) return;

	const bool phase_a = lfp_blink_phase_a_counter(lp.frame_counter);
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	const auto bold_text = [&](const char *t, float design_x, float design_y,
			uint32_t argb, uint32_t flags) {
		if (t == nullptr || t[0] == 0) return;
		const GameFontRun run = bf.layout(t, sx(design_x, w), sy(design_y, h),
				bscale, bscale, flags, argb);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
				run.quads.end());
	};
	const uint32_t viewer_color = lfp_team_color(lp.local_team);
	const uint32_t opposing_color = lfp_opposing_team_color(lp.local_team);

	// The group cursor [orig: x = g_HUDZonePanelX - 98 * count[team]
	// @0x5a2589..0x5a259d; the markers' y = g_HUDZonePanelY + 86 * group
	// (@0x5a249d, @0x5a2667), the status text's y = that + 12 (@0x5a25bd)].
	int current_team = lp.zones[start].team;
	int group = 0;
	int index_in_group = 0;
	int status_kind = 0;          // [orig: g_HUDZoneStatusKind, 0 at group start]
	uint32_t status_color = 0;    // [orig: g_HUDZoneStatusColor]
	char text[32];

	const auto flush_status = [&]() {
		// The group's text, right-aligned 4 px left of its first marker on
		// the group's row [orig: @0x5a2601..0x5a2652 / @0x5a27c6..0x5a27f3].
		if (status_kind == 0) return;
		int tx = 0, ty = 0;
		lfp_status_text_origin(layout_.lfp_anchor_x, layout_.lfp_anchor_y,
				per_team[static_cast<size_t>(current_team)], group, tx, ty);
		const std::string &s = status_kind == 1 ? lp.under_attack_text
												 : lp.ready_text;
		const char *fallback = status_kind == 1 ? "!Under\nAttack!!"
												 : "!Ready for\nTakeover!";
		emit_text(s.empty() ? fallback : s.c_str(), static_cast<float>(tx),
				static_cast<float>(ty), w, h, status_color, kFontAlignRight);
	};

	for (size_t i = start; i < lp.zones.size(); ++i) {
		const HudLfpZone &z = lp.zones[i];
		if (z.team == 0) continue;
		// A team change closes the group: its text, then the next row
		// [orig: @0x5a25f6..0x5a2679].
		if (z.team != current_team) {
			flush_status();
			++group;
			current_team = z.team;
			index_in_group = 0;
			status_kind = 0;
		}
		if (!contested(z)) continue;

		// ---- HUD_DrawZoneMarker @0x5986f0 ----
		int mx = 0, my = 0;
		lfp_marker_origin(layout_.lfp_anchor_x, layout_.lfp_anchor_y,
				per_team[static_cast<size_t>(current_team)], index_in_group, group,
				mx, my);
		++index_in_group;

		const bool ready = lfp_zone_ready(z.team, lp.local_team, z.control);
		const bool attacked = lfp_zone_under_attack(z.timer_team, lp.local_team,
				z.rate) && z.team == lp.local_team;
		const bool contested_counts = z.count_owner != z.count_other;
		const uint32_t point = lfp_point_color(lfp_team_color(z.team),
				z.in_cylinder, contested_counts, attacked, ready, phase_a);
		const LfpFrame frame = lfp_frame(attacked, ready, z.in_cylinder, phase_a);

		// 1. The icon: one 102x102 frame of the team's vertical atlas at the
		// flat half-bright modulate [orig: @0x59886f..0x59898b].
		{
			const int team_slot = z.team == 1 ? 0 : (z.team == 2 ? 1 : 2);
			if (layout_.lfp_icon_texture_valid[static_cast<size_t>(team_slot)]) {
				const float band = 1.0f / static_cast<float>(kLfpFrameCount);
				const float v0 = static_cast<float>(frame) * band;
				emit_rect_uv(sx(static_cast<float>(mx), w),
						sy(static_cast<float>(my), h),
						sx(static_cast<float>(mx + kLfpIconSize), w),
						sy(static_cast<float>(my + kLfpIconSize), h), 0.0f, v0,
						1.0f, v0 + band, kLfpIconModulate,
						kHudTexLfpTeam1 + team_slot);
			}
		}
		// 2. The 36x36 tile behind the letter, half-bright point colour, own
		// vs other texture [orig: @0x5989b9..0x5989de — HUD_DrawTexturedQuadCentered
		//  (x+34, y+52, 36, 36, tex, (point>>1 & 0x7F7F7F) | 0xFF000000)].
		{
			const bool own = z.team == lp.local_team;
			const bool valid = own ? layout_.lfp_tile_own_texture_valid
								   : layout_.lfp_tile_other_texture_valid;
			if (valid) {
				const float cx = static_cast<float>(mx + kLfpTileOffX);
				const float cy = static_cast<float>(my + kLfpTileOffY);
				const float half = static_cast<float>(kLfpTileSize) * 0.5f;
				emit_rect(sx(cx - half, w), sy(cy - half, h), sx(cx + half, w),
						sy(cy + half, h), ((point >> 1) & 0x7F7F7Fu) | 0xFF000000u,
						true, own ? kHudTexLfpTileOwn : kHudTexLfpTileOther);
			}
		}
		// 3. The letter, centred [orig: sprintf("%c", 'A' + idx) @0x5989fe;
		//  HUD_DrawTextCenteredScaled (ex sub_580B80)(&dword_2723C74, x+32, y+44) @0x598a1b]. The
		//  font object at dword_2723C74 is the hudpos HUD slot [orig: HUD_SelectHudposFont
		//  @0x591890], the HUD font here.
		std::snprintf(text, sizeof(text), "%c", 'A' + z.letter_index);
		emit_text(text, static_cast<float>(mx + kLfpLetterOffX),
				static_cast<float>(my + kLfpLetterOffY), w, h, 0xFFFFFFFFu,
				kFontAlignCenter);
		// 4. The capture bar: the rect outline in the point colour and the
		// fraction filled from the bottom [orig: the rect (x+56,y+30)..(x+76,y+82)
		//  @0x598a41..0x598a5f; fraction entry[8]/entry[10] when entry[12]
		//  @0x598a27..0x598a3a]. WITNESS PENDING: the vertex buffer
		//  @0x598aa2..0x598db5 — a 6-vertex line list from vertex 0 and a
		//  12-vertex triangle list from vertex 6; vertices 0-1 and 12-17 carry
		//  the point colour, 2-11 the quarter-bright half-alpha track colour;
		//  the FPU position writes are unread, so the fill direction is the
		//  natural reading.
		{
			const int32_t frac = lfp_bar_fraction_q16(z.value, z.limit, z.active);
			const float bx0 = sx(static_cast<float>(mx + kLfpBarX1), w);
			const float by0 = sy(static_cast<float>(my + kLfpBarY1), h);
			const float bx1 = sx(static_cast<float>(mx + kLfpBarX2), w);
			const float by1 = sy(static_cast<float>(my + kLfpBarY2), h);
			// The quarter-bright half-alpha track [orig: (c>>2 & 0x3F3F3F) |
			// 0x80000000 @0x598a96].
			emit_rect(bx0, by0, bx1, by1, ((point >> 2) & 0x3F3F3Fu) | 0x80000000u,
					true);
			if (frac > 0) {
				const float fill_h = (by1 - by0) * static_cast<float>(frac) /
						65536.0f;
				emit_rect(bx0, by1 - fill_h, bx1, by1, point | 0xFF000000u, true);
			}
			emit_wire_rect(bx0, by0, bx1, by1, point | 0xFF000000u);
		}
		// WITNESS PENDING: the count-differential arrows [orig: @0x598dc0..0x598fe4
		//  — n = min(|own - other|, 5) @0x598dcb..0x598dde, the column
		//  x+76..x+90 @0x598de5/@0x598e02, rows of 7 px from y+57-(7n>>1)
		//  @0x598df2..0x598e1c, pointing up for a positive difference
		//  @0x598e69..0x598e7f, six vertices per arrow alternating the track
		//  and point colours @0x598f0c..0x598f2f]; the FPU vertex positions
		//  @0x598eb1..0x598f98 are unread, so the arrows are not drawn.
		// 5. The contest counts, only when either is nonzero: own side left at
		// (x+16, y+66) in the viewer's colour, the other side right-aligned at
		// (x+47, y+66) in the opposing colour [orig: @0x598ff5..0x5990a0].
		{
			int own = 0, other = 0;
			lfp_contest_counts(z.team, lp.local_team, z.count_owner, z.count_other,
					own, other);
			if (own != 0 || other != 0) {
				std::snprintf(text, sizeof(text), "%d", own);
				bold_text(text, static_cast<float>(mx + kLfpCountOwnOffX),
						static_cast<float>(my + kLfpCountOwnOffY), viewer_color, 0u);
				std::snprintf(text, sizeof(text), "%d", other);
				bold_text(text, static_cast<float>(mx + kLfpCountEnemyOffX),
						static_cast<float>(my + kLfpCountEnemyOffY), opposing_color,
						kFontAlignRight);
			}
		}
		// 6. The distance, in the point colour [orig: @0x59914b..0x59916f].
		lfp_format_distance(text, sizeof(text), z.distance_m);
		bold_text(text, static_cast<float>(mx + kLfpDistOffX),
				static_cast<float>(my + kLfpDistOffY), point | 0xFF000000u, 0u);
		// 7. The marker publishes the group's status: attack wins over ready
		// [orig: @0x59917c..0x59919f].
		if (attacked) {
			status_kind = 1;
			status_color = point | 0xFF000000u;
		} else if (ready && status_kind != 1) {
			status_kind = 2;
			status_color = point | 0xFF000000u;
		}
	}
	flush_status();
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
