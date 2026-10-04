#pragma once

#include <cstdint>
#include <cstdio>

namespace opennova::hud {

// THE AAS OBJECTIVE ("LFP") STATUS PANEL — retail's objective-status HUD
// element. One marker per contestable objective: the team-coloured icon, the
// objective's letter, the distance to it, and the two in-radius contest counts.
//
// "LFP" is the game's own name for an Objective Point (gametext
// `LFP = "Objective"`), not an abbreviation worth renaming.
//
// It is a HUD-SPACE panel, not a world billboard: retail scales fixed
// design-space coordinates and never projects the objective's world position.
// The distance label is simply how far that objective is from you.
//
// [orig: contestable list Entity_BuildSpawnZoneList @0x43EAE0 (walks pools 2/1
//        for the flag-0x40000 spawn zones and sorts them; iterated via
//        SpawnZoneList_GetCount @0x43B920 / SpawnZoneList_GetByIndex @0x43B930);
//        panel   HUD_DrawZoneStatusPanel @0x5A2480;
//        marker  HUD_DrawZoneMarker @0x5986F0 (x, y, zoneEntity, letterIndex)]
//
// The panel models the AAS (grouped-by-team) branch. Conquest mode
// (g_GameType == 0x50010 @0x5a24a1) takes the other arm: every marker is its
// own row (Y += 86 per marker @0x5a2781) with the status text drawn per
// marker — not modelled here.

// Team colours, ARGB — the three g_HUDColors entries the marker reads
// [orig: team 1 g_HUDColors.palette[3] @0x24C1844 = table[3], team 2
//  dword_24C184C = table[5], neutral / other dword_24C183C = table[1] —
//  selected @0x598746..0x598751 in HUD_DrawZoneMarker; the immediates written
//  by HUD_InitTeamColorTable @0x51f26d / @0x51f259 / @0x51f263].
inline constexpr uint32_t kLfpColorNeutral = 0xFF00FF00u;
inline constexpr uint32_t kLfpColorTeam1 = 0xFF80A0FFu;
inline constexpr uint32_t kLfpColorTeam2 = 0xFFFF5050u;

// Per-marker geometry, design px, as offsets from the marker origin (x, y)
// [orig: HUD_DrawZoneMarker @0x5986F0 — icon rect x..x+102 / y..y+102
//  @0x59886f..0x59888d; team quad 36x36 centred @(x+34, y+52) @0x5989de;
//  letter @(x+32, y+44) @0x598a1b; distance @(x+16, y+18) @0x59916f; own count
//  @(x+16, y+66) @0x599068; other count @(x+47, y+66) @0x5990a0].
inline constexpr int kLfpIconSize = 0x66;      // 102x102 team icon quad
inline constexpr int kLfpTileOffX = 0x22;      // letter tile CENTRE, not corner
inline constexpr int kLfpTileOffY = 0x34;
inline constexpr int kLfpTileSize = 0x24;      // 36x36
inline constexpr int kLfpLetterOffX = 0x20;    // the glyph, centred
inline constexpr int kLfpLetterOffY = 0x2C;
inline constexpr int kLfpDistOffX = 0x10;      // distance, left aligned
inline constexpr int kLfpDistOffY = 0x12;
inline constexpr int kLfpCountOwnOffX = 0x10;  // own-side count, left aligned
inline constexpr int kLfpCountOwnOffY = 0x42;
inline constexpr int kLfpCountEnemyOffX = 0x2F; // other side, RIGHT aligned
inline constexpr int kLfpCountEnemyOffY = 0x42;
// The capture progress bar's rect [orig: (x+56, y+30)..(x+76, y+82)
//  @0x598a41..0x598a5f] and its fraction: DWORD 8 / DWORD 10 of the timer
// entry while DWORD 12 is set, else 0 [orig: @0x598a27..0x598a3a].
inline constexpr int kLfpBarX1 = 0x38;
inline constexpr int kLfpBarY1 = 0x1E;
inline constexpr int kLfpBarX2 = 0x4C;
inline constexpr int kLfpBarY2 = 0x52;

// STEPPING [orig: HUD_DrawZoneStatusPanel @0x5A2480]. Markers step ACROSS
// within a team group (`x += 0x62` after each marker @0x5a27a3) and each new
// team group steps DOWN (`y += 0x56` @0x5a2667 / @0x5a2781). The two pitches
// differ, so one shared step would drift.
//
// A group is RIGHT-ANCHORED to the panel X: its first marker sits at
// `g_HUDZonePanelX - 98 * zonesInGroup` (`esi = g_HUDZonePanelX - 0x62*count`
// @0x5a2589..0x5a259d, where count is that team's zone count from the first
// pass @0x5a24c5..0x5a24ea), so the group ends one pitch short of the anchor.
// The panel anchor is the hudpos-authored pair g_HUDZonePanelX/Y
// [orig: the hudpos writes @0x5a0563 / @0x5a057b].
//
// Every marker row AND its status text sit 12 px below the group's row base:
// the Y cursor starts at `g_HUDZonePanelY + 12` (`x_position = x_base + 12`
// @0x5a25bd) and both the marker call (arg 1 @0x5a2799) and the text call take
// that cursor, while the 86 step advances both [orig: @0x5a2667..0x5a2676].
//
// (Hex-Rays names the X accumulator "y_position" and the Y "x_base" in this
// function; the pushes to HUD_DrawZoneMarker @0x5a2797..0x5a279b settle it:
// x first.)
inline constexpr int kLfpStepX = 0x62; // 98, marker pitch across a group
inline constexpr int kLfpStepY = 0x56; // 86, group pitch down
inline constexpr int kLfpRowDy = 12;   // the STATUS TEXT's +12 over its group's Y

// The group's status text ("!Under Attack!!" / "!Ready for Takeover!") sits
// 4 px left of the group's first marker, on the group's row cursor
// [orig: `esi - 4` @0x5a2601; drawn right-aligned via HUD_DrawTextRightAlignedScaled (ex sub_580BC0) @0x5a2652 /
//  @0x5a262b / @0x5a281a / @0x5a27f3].
inline constexpr int kLfpStatusTextDx = -4;

// The icon quad's constant modulate [orig: CEffect_Begin_Debug @0x67BB50, a
// thunk to Render_DrawTiledTextureStrip @0x67AED0 (effect, rect[4], colour, frame);
// the colour push 0xFF7F7F7F @0x598971, the call @0x59898B]. The three LFP
// textures already carry their own team colours, so this is a flat BRIGHTNESS
// term, not a tint — replacing it with the team colour would double-apply it.
// It is the raw diffuse: each tile's material 0x300631 (colour family 0x600,
// hud_texture_materials.h kLfpIconMaterialWord) doubles it on the device, so
// the icon lands at its texels' own brightness (D-HUD-49).
inline constexpr uint32_t kLfpIconModulate = 0xFF7F7F7Fu;

// The icon textures are VERTICAL FRAME ATLASES (retail ships 64x256 = four
// 64x64 frames) and the strip renderer selects one row by the strip's
// per-row pixel height [orig: Render_TiledImageStrip @0x67B540 —
//  row_start = row * this[5] @0x67B5C2, bounded against this[4] (the total
//  height) @0x67B5D8; this[5] equals the width only because the LFP frames
//  are square].
enum class LfpFrame : int {
	Default = 0,     // the viewer is outside this objective's capture cylinder
	InZone = 1,      // the viewer is INSIDE it
	UnderAttack = 2, // an own point being drained
	Ready = 3,       // ready for takeover
};
inline constexpr int kLfpFrameCount = 4;

// Frames 2 and 3 are NOT the pair an earlier reading assumed: retail is
// 2 = under-attack, 3 = ready [orig: @0x59891D..0x598934]. Getting them the
// wrong way round shows "ready for takeover" on a point you are losing.
//
// Their inputs come from the zone-timer record the 0x6F value handler writes
// [orig: NapiNPClientMsg_ZoneTimerValue @0x428D60 -> ZoneTimerList_SetEntryValue
//  @0x537EC0; read in HUD_DrawZoneMarker — Entry[9] control target, Entry[11]
//  signed step delta, Entry[1] team].

// READY: someone else's point whose control target has reached zero
// [orig: `player_diff != local_team && !EntryById[9]` @0x598825..0x598845].
inline bool lfp_zone_ready(int zone_team, int viewer_team, int control) {
	return zone_team != viewer_team && control == 0;
}

// UNDER ATTACK: your own point whose control is DRAINING — a negative rate.
// A zero or positive rate is not an attack, so the sign test is the whole
// condition [orig: `EntryById[11] < 0 && EntryById[1] == team` @0x598834].
inline bool lfp_zone_under_attack(int zone_team, int viewer_team, int rate) {
	return zone_team == viewer_team && rate < 0;
}

// THE BLINK PHASE. Retail's HUD frame counter advances once per main frame
// (gated on no suicide / no epilog) and the marker tests `counter & 0x18`
// [orig: g_HUDFrameCounter @0xA87064, ++ in Game_TickHudFrameCounters
//  @0x434C00 from Game_ProcessMainFrame @0x5265d5; the tests @0x5988E8 /
//  @0x5988FB / @0x59891D]. That mask is not a 50/50 blink: phase A is 8
// counts of every 32 and phase B the remaining 24.
inline bool lfp_blink_phase_a_counter(int frame_counter) {
	return (frame_counter & 0x18) == 0;
}
// The millisecond-clock form (>>4, one count per 16 ms) for a device that
// clocks the panel off wall time rather than the frame counter.
inline bool lfp_blink_phase_a(int64_t now_ms) {
	return lfp_blink_phase_a_counter(static_cast<int>(now_ms >> 4));
}

// IN THE CYLINDER: the 2D distance to the zone is within its capture radius
// (entity+0x15E, in whole units -> Q16) and the height difference within half
// of it [orig: @0x5987f4..0x598810 — `dist2d <= radius << 16`, then
//  `|dz| > (radius << 16) / 2` clears it]. The radius IS on the wire (the 0x0D
// record's u16), so a client computes this like the host.
inline bool lfp_in_cylinder(int32_t dist2d_q16, int32_t dz_q16, uint16_t radius) {
	const int32_t r_q16 = static_cast<int32_t>(radius) << 16;
	if (dist2d_q16 > r_q16) return false;
	const int32_t adz = dz_q16 < 0 ? -dz_q16 : dz_q16;
	return adz <= r_q16 / 2;
}

// Which frame a marker draws, given its state. `attacked` and `ready` are the
// CALLER'S two tests — the same pair the point colour ladder reads, computed
// once per marker: the attacked test carries the timer entry's team term
// (DWORD 1 == the zone's team byte) alongside the rate sign, so a frame and a
// colour never disagree about one zone [orig: HUD_DrawZoneMarker @0x5986f0 —
//  both tests are evaluated ahead of the colour ladder and the frame select
//  reads the same two locals].
inline LfpFrame lfp_frame(bool attacked, bool ready, bool in_cylinder,
		bool phase_a) {
	// The two blinking states alternate on the pulse; when neither applies the
	// in-cylinder frame wins over the default [orig: @0x598915..0x598934].
	if (attacked && phase_a) return LfpFrame::UnderAttack;
	if (ready && !phase_a) return LfpFrame::Ready;
	if (in_cylinder) return LfpFrame::InZone;
	return LfpFrame::Default;
}

// THE POINT COLOUR LADDER the marker's tile, bar, distance text and the
// caller's status text take, applied in this order to the team colour
// [orig: @0x5988b7..0x59890d]:
//   1. neither in the cylinder nor contested (the two counts equal) -> dimmed:
//      (c>>1 & 0x7F7F7F) + (c>>3 & 0x1F1F1F) + alpha   @0x5988d6
//   2. under attack on phase A -> pure yellow 0xFFFFFF00   @0x5988ea
//   3. else ready on phase A -> halved: (c>>1 & 0x7F7F7F) + alpha   @0x59890d
inline uint32_t lfp_point_color(uint32_t team_color, bool in_cylinder,
		bool contested, bool attacked, bool ready, bool phase_a) {
	uint32_t c = team_color;
	if (!in_cylinder && !contested) {
		c = ((c >> 1) & 0x7F7F7Fu) + ((c >> 3) & 0x1F1F1Fu) + (c & 0xFF000000u);
	}
	if (attacked) {
		if (phase_a) c = 0xFFFFFF00u;
	} else if (ready && phase_a) {
		c = ((c >> 1) & 0x7F7F7Fu) + (c & 0xFF000000u);
	}
	return c;
}

// The capture fraction, Q16: DWORD 8 / DWORD 10 while DWORD 12, else 0
// [orig: @0x598a27..0x598a3a].
inline int32_t lfp_bar_fraction_q16(int32_t value, int32_t limit, bool active) {
	if (!active || limit == 0) return 0;
	int64_t f = (static_cast<int64_t>(value) << 16) / limit;
	if (f < 0) f = 0;
	if (f > 0x10000) f = 0x10000;
	return static_cast<int32_t>(f);
}

// The two contest counts as the marker prints them: the OWN-side count is the
// owner's count (+0x220) when the zone is the viewer's, else the other side's
// (+0x221), and vice versa; printed only when either is nonzero
// [orig: @0x598ff5..0x599037].
inline void lfp_contest_counts(int zone_team, int viewer_team,
		uint8_t count_owner, uint8_t count_other, int &own, int &other) {
	if (zone_team == viewer_team) {
		own = count_owner;
		other = count_other;
	} else {
		own = count_other;
		other = count_owner;
	}
}

// The distance label: whole metres up to 1000, else kilometres to two places
// [orig: "%1dm" @0x59914b for <= 1000, "%01.2fk" with x 0.001 @0x599133].
inline int lfp_format_distance(char *buf, size_t len, int metres) {
	if (metres <= 1000) return std::snprintf(buf, len, "%1dm", metres);
	return std::snprintf(buf, len, "%01.2fk", static_cast<double>(metres) * 0.001);
}

// A marker's origin: the group is right-anchored to the panel X and steps
// across by kLfpStepX per marker; each group steps down by kLfpStepY from the
// PANEL Y. The marker takes the group's Y accumulator itself, NOT the status
// text's row: the drawer seeds the stack slot from g_HUDZonePanelY, adds 86
// per group and pushes that slot as the marker's y, while the +12 lives in a
// separate register that only the status-text call reads
// [orig: seed @0x5a249d; `add [esp+y], 56h` @0x5a2667 vs `add ebx, 56h`
//  @0x5a2676; `mov ebx, [esp+y]; add ebx, 0Ch` @0x5a25b9..0x5a25bd; the
//  marker pushes [esp+y] @0x5a2799, the text call @0x5a2652 takes ebx].
inline void lfp_marker_origin(int panel_x, int panel_y, int zones_in_group,
		int index_in_group, int group_index, int &out_x, int &out_y) {
	out_x = panel_x - zones_in_group * kLfpStepX + index_in_group * kLfpStepX;
	out_y = panel_y + group_index * kLfpStepY;
}

// Where a group's status text is drawn (right-aligned at this point): the
// group's first marker x - 4, on the group's row cursor.
inline void lfp_status_text_origin(int panel_x, int panel_y, int zones_in_group,
		int group_index, int &out_x, int &out_y) {
	out_x = panel_x - zones_in_group * kLfpStepX + kLfpStatusTextDx;
	out_y = panel_y + kLfpRowDy + group_index * kLfpStepY;
}

// The colour a team takes.
inline uint32_t lfp_team_color(int zone_team) {
	if (zone_team == 1) return kLfpColorTeam1;
	if (zone_team == 2) return kLfpColorTeam2;
	return kLfpColorNeutral;
}
// The OPPOSING side's colour as the marker resolves it for the other-side
// count [orig: @0x598781..0x598792 — local 1 -> dword_24C184C, local 2 ->
//  light blue, else neutral].
inline uint32_t lfp_opposing_team_color(int viewer_team) {
	if (viewer_team == 1) return kLfpColorTeam2;
	if (viewer_team == 2) return kLfpColorTeam1;
	return kLfpColorNeutral;
}

} // namespace opennova::hud
