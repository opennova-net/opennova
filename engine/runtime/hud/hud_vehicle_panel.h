#pragma once

#include <cstdint>

namespace opennova::hud {

// THE MOUNTED-VEHICLE PANEL: the silhouette plus one 11x11 marker per seat,
// drawn while the local player rides a vehicle
// [orig: HUD_DrawVehicleHealthBars @0x5A4FD0]. The layout comes from the
// item's VEHICLE_HUD block (engine/formats/def/def.h); this header carries the
// per-marker POLICY -- which colour a seat takes and where its label sits.

// The marker box, set once as constants rather than authored
// [orig: dword_27237FC = 11 / dword_2723800 = 11 @0x5A3...].
inline constexpr int kSeatMarkerW = 11;
inline constexpr int kSeatMarkerH = 11;

// A seat marker's colour band. The panel reuses the STANCE colour triple
// rather than owning its own [orig: dword_2723ADC / g_stanceColorMiddle /
// g_stanceColorBad at the three arms].
enum class SeatHealthBand { Good, Middle, Bad };

// Health -> band. The ratio is Q16: (health << 16) / maxHealth, with a zero
// maxHealth forced to 1 before the divide so the panel cannot divide by zero
// on an entity whose def gives no health [orig: the `if (!max) max = 1` guard].
//
// THE TWO COMPARISONS DIFFER IN SIGNEDNESS, and that is witnessed, not a
// transcription slip: the Good test is UNSIGNED against 0xC000 while the
// Middle test is SIGNED against 0x6FFF. A negative ratio -- which a negative
// health produces -- therefore wraps large under the first test and reads as
// GOOD, never reaching the second. Normalising both to signed would quietly
// change that edge, so the port keeps the asymmetry.
SeatHealthBand seat_health_band(int32_t health, int32_t max_health);

// The seat label's anchor: the marker box's CENTRE, floor-divided
// [orig: baseX + w/2 + slotX, baseY + slotY + h/2 -- note the x form adds the
//  half-extent before the slot offset and the y form after, which lands the
//  same place but is kept in the witnessed order].
inline int seat_label_x(int base_x, int slot_x) {
	return base_x + kSeatMarkerW / 2 + slot_x;
}
inline int seat_label_y(int base_y, int slot_y) {
	return base_y + slot_y + kSeatMarkerH / 2;
}

// THE PANEL'S BASE CORNER. Every seat offset in the VEHICLE_HUD block is
// relative to this, and it is NOT a fixed screen position: it is the authored
// HUDVEHSTANCEPOS anchor plus the offset for the rider's CURRENT STANCE
// [orig: baseX = dword_2723AF4 + dword_2723B24[byte_27235C0],
//        baseY = dword_2723AF8 + dword_2723B44[byte_27235C0]].
//
// Both halves already exist on our side and are simply joined here: the
// anchor is hudpos's HUDVEHSTANCEPOS pair (def.h `veh_stance_pos`) and the
// per-stance offsets are the same HUDSTANCE tables the stance icon uses
// (hud_frame.h `stance_offset_x`/`stance_offset_y`). Sharing them is the
// witnessed behaviour, not a convenience -- the panel MOVES WITH the stance
// icon, so a reimplementation that pinned the panel to a fixed corner would
// drift apart from it the moment the rider changed stance.
inline void vehicle_panel_base(int anchor_x, int anchor_y, int stance_offset_x,
		int stance_offset_y, int &out_x, int &out_y) {
	out_x = anchor_x + stance_offset_x;
	out_y = anchor_y + stance_offset_y;
}

// The marker's filled rect, as corners.
inline void seat_marker_rect(int base_x, int base_y, int slot_x, int slot_y,
		int &x0, int &y0, int &x1, int &y1) {
	x0 = base_x + slot_x;
	y0 = base_y + slot_y;
	x1 = x0 + kSeatMarkerW;
	y1 = y0 + kSeatMarkerH;
}

} // namespace opennova::hud
