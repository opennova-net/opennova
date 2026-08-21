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
// [orig: list/sort Objectives_BuildSortedList @0x43EAE0;
//        layout    HUD_DrawLfpObjectivePanel  @0x5A2480;
//        marker    HUD_DrawLfpObjectiveMarker @0x5986F0;
//        team cols @0x51F240]

// Team colours, ARGB [orig: @0x51F240]. Neutral is GREEN — an un-owned
// objective is not "no colour".
inline constexpr uint32_t kLfpColorNeutral = 0xFF00FF00u;
inline constexpr uint32_t kLfpColorTeam1 = 0xFF80A0FFu;
inline constexpr uint32_t kLfpColorTeam2 = 0xFFFF5050u;

// Per-marker geometry, design px, as offsets from the marker origin
// [orig: @0x5986F0].
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

// Stepping [orig: @0x5A2480 — +0x62 per marker, +0x56 per group]. Markers step
// horizontally WITHIN a zone group and the group steps vertically; the two
// pitches differ, so one shared step would drift.
inline constexpr int kLfpStepX = 0x62; // 98, marker pitch inside a group
inline constexpr int kLfpStepY = 0x56; // 86, row pitch between groups

// The icon quad's constant modulate [orig: FUN_0067BB50(tex, rect, 0xff7f7f7f)
// @0x59898B]. The three LFP textures already carry their own team colours, so
// this is a flat BRIGHTNESS term, not a tint — replacing it with the team
// colour would double-apply it.
inline constexpr uint32_t kLfpIconModulate = 0xFF7F7F7Fu;

// The icon textures are VERTICAL FRAME ATLASES (retail ships 64x256 = four
// 64x64 frames) and the drawer selects one square frame using the texture
// WIDTH as the stride [orig: FUN_0067B540 @0x67B5A4 — frame * texture.width].
enum class LfpFrame : int {
	Default = 0,     // the viewer is outside this objective's capture cylinder
	InZone = 1,      // the viewer is INSIDE it
	UnderAttack = 2, // an own point being drained
	Ready = 3,       // ready for takeover
};
inline constexpr int kLfpFrameCount = 4;

// Frames 2 and 3 are NOT the pair an earlier reading assumed: retail is
// 2 = under-attack, 3 = ready [orig: @0x59890F..0x598934]. Getting them the
// wrong way round shows "ready for takeover" on a point you are losing.
//
// Their inputs come from the zone-timer record the 0x6F value handler writes
// [orig: NapiNPClientMsg_ZoneTimerValue @0x428D60 -> SetEntryValue @0x537EC0:
//  Entry[9] = control target, Entry[0xb] = signed step delta, Entry[1] = team].

// READY: someone else's point whose control target has reached zero.
inline bool lfp_zone_ready(int zone_team, int viewer_team, int control) {
	return zone_team != viewer_team && control == 0;
}

// UNDER ATTACK: your own point whose control is DRAINING — a negative rate.
// A zero or positive rate is not an attack, so the sign test is the whole
// condition.
inline bool lfp_zone_under_attack(int zone_team, int viewer_team, int rate) {
	return zone_team == viewer_team && rate < 0;
}

// THE BLINK PHASE. Retail's pulse counter increments once per 16 ms and the
// marker tests `counter & 0x18` [orig: DAT_00A87064 @FUN_00434C00; the test
// @0x5986F0]. That mask is not a 50/50 blink: phase A is 128 ms of every 512
// and phase B the remaining 384. Phase A shows the under-attack frame and the
// colour overrides; phase B shows the ready frame.
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

// A marker's origin within the panel, from its index in the group and the
// group's row.
inline void lfp_marker_origin(int anchor_x, int anchor_y, int index_in_group,
		int group_row, int &out_x, int &out_y) {
	out_x = anchor_x + index_in_group * kLfpStepX;
	out_y = anchor_y + group_row * kLfpStepY;
}

// The colour a marker's objective takes.
inline uint32_t lfp_team_color(int zone_team) {
	if (zone_team == 1) return kLfpColorTeam1;
	if (zone_team == 2) return kLfpColorTeam2;
	return kLfpColorNeutral;
}

} // namespace opennova::hud
