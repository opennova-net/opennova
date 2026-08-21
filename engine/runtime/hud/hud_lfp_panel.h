#pragma once

#include <cstdint>

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

// Team colours, ARGB. The marker reads three runtime HUD colour globals
// [orig: team 1 g_hudColorLightBlue @0x24C1844, team 2 dword_24C184C, neutral /
//  other dword_24C183C — selected @0x598746..0x598751 in HUD_DrawZoneMarker].
// Those globals are BSS (config-loaded; they read 0 in the image), so the
// literal ARGB below are UNVERIFIED placeholders, not witnessed values — a live
// read is owed before this panel is wired.
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

// STEPPING [orig: HUD_DrawZoneStatusPanel @0x5A2480]. Markers step ACROSS
// within a team group (`x += 0x62` after each marker @0x5a27a3) and each new
// team group steps DOWN (`y += 0x56` @0x5a2667 / @0x5a2781). The two pitches
// differ, so one shared step would drift.
//
// A group is RIGHT-ANCHORED to the panel X: its first marker sits at
// `g_hudZonePanelX - 98 * zonesInGroup` (`esi = g_hudZonePanelX - 0x62*count`
// @0x5a2589..0x5a259d, where count is that team's zone count from the first
// pass @0x5a24c5..0x5a24ea), so the group ends one pitch short of the anchor.
// The panel anchor is the hudpos-authored pair g_hudZonePanelX/Y
// [orig: the hudpos writes @0x5a0563 / @0x5a057b].
//
// (Hex-Rays names the X accumulator "y_position" and the Y "x_base" in this
// function; the pushes to HUD_DrawZoneMarker @0x5a2797..0x5a279b settle it:
// x first.)
inline constexpr int kLfpStepX = 0x62; // 98, marker pitch across a group
inline constexpr int kLfpStepY = 0x56; // 86, group pitch down

// The group's status text ("!Under Attack!!" / "!Ready for Takeover!") sits
// 4 px left of the group's first marker and 12 px below the group's row
// [orig: `esi - 4` @0x5a2601, `ebx = y + 0xC` @0x5a25bd; drawn right-aligned
//  via sub_580BC0 @0x5a2652 / @0x5a262b / @0x5a281a / @0x5a27f3].
inline constexpr int kLfpStatusTextDx = -4;
inline constexpr int kLfpStatusTextDy = 12;

// The icon quad's constant modulate [orig: CEffect_Begin_Debug @0x67BB50, a
// thunk to draw_tiled_texture_strip @0x67AED0 (effect, rect[4], colour, frame);
// the colour push 0xFF7F7F7F @0x598975, the call @0x59898B]. The three LFP
// textures already carry their own team colours, so this is a flat BRIGHTNESS
// term, not a tint — replacing it with the team colour would double-apply it.
inline constexpr uint32_t kLfpIconModulate = 0xFF7F7F7Fu;

// The icon textures are VERTICAL FRAME ATLASES (retail ships 64x256 = four
// 64x64 frames) and the strip renderer selects one row by the strip's
// per-row pixel height [orig: render_tiled_image_strip @0x67B540 —
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
// [orig: g_hudFrameCounter @0xA87064, ++ in Game_TickHudFrameCounters
//  @0x434C00 from Game_ProcessMainFrame @0x5265d5; the tests @0x5988E8 /
//  @0x5988FB / @0x59891D]. That mask is not a 50/50 blink: phase A is 128
// counts of every 512 and phase B the remaining 384. This port drives it off
// a millisecond clock (>>4, one count per 16 ms) as a device approximation of
// the 62 Hz frame counter.
inline bool lfp_blink_phase_a(int64_t now_ms) {
	return ((now_ms >> 4) & 0x18) == 0;
}

// Which frame a marker draws, given its state. `in_cylinder` is held false by
// a client that cannot see the capture radius (entity+0x15E is not on the
// wire) — a recorded gap, not a guess.
inline LfpFrame lfp_frame(int zone_team, int viewer_team, int control, int rate,
		bool in_cylinder, int64_t now_ms) {
	const bool ready = lfp_zone_ready(zone_team, viewer_team, control);
	const bool attacked = lfp_zone_under_attack(zone_team, viewer_team, rate);
	// The two blinking states alternate on the pulse; when neither applies the
	// in-cylinder frame wins over the default.
	if (attacked && lfp_blink_phase_a(now_ms)) return LfpFrame::UnderAttack;
	if (ready && !lfp_blink_phase_a(now_ms)) return LfpFrame::Ready;
	if (in_cylinder) return LfpFrame::InZone;
	return LfpFrame::Default;
}

// A marker's origin: the group is right-anchored to the panel X and steps
// across by kLfpStepX per marker; each group steps down by kLfpStepY.
inline void lfp_marker_origin(int panel_x, int panel_y, int zones_in_group,
		int index_in_group, int group_index, int &out_x, int &out_y) {
	out_x = panel_x - zones_in_group * kLfpStepX + index_in_group * kLfpStepX;
	out_y = panel_y + group_index * kLfpStepY;
}

// Where a group's status text is drawn (right-aligned at this point).
inline void lfp_status_text_origin(int panel_x, int panel_y, int zones_in_group,
		int group_index, int &out_x, int &out_y) {
	out_x = panel_x - zones_in_group * kLfpStepX + kLfpStatusTextDx;
	out_y = panel_y + group_index * kLfpStepY + kLfpStatusTextDy;
}

// The colour a marker's objective takes.
inline uint32_t lfp_team_color(int zone_team) {
	if (zone_team == 1) return kLfpColorTeam1;
	if (zone_team == 2) return kLfpColorTeam2;
	return kLfpColorNeutral;
}

} // namespace opennova::hud
