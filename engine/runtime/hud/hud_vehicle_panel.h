#pragma once

#include <cstdint>

namespace opennova::hud {

// THE MOUNTED-VEHICLE PANEL: the silhouette plus one 11x11 marker per seat,
// drawn while the local player rides a vehicle
// [orig: HUD_DrawVehicleHealthBars @0x5A4FD0]. The layout comes from the
// item's VEHICLE_HUD block (engine/formats/def/def.h); this header carries the
// per-marker POLICY -- which colour a seat takes and where its label sits.

// The marker box, set once as constants rather than authored: both slots
// take 11 at HUD init [orig: g_hudVehSeatMarkerW / g_hudVehSeatMarkerH = 11,
//  stored from eax=0Bh @0x5A47B0 / @0x5A47B5 in HUD_InitOverlaySystem
//  @0x5A4620; read throughout HUD_DrawVehicleHealthBars @0x5A4FD0].
inline constexpr int kSeatMarkerW = 11;
inline constexpr int kSeatMarkerH = 11;

// A seat marker's colour band. The panel reuses the STANCE colour triple
// rather than owning its own [orig: g_stanceColorGood / g_stanceColorMiddle /
// g_stanceColorBad at the three arms @0x5A5130 / @0x5A513B / @0x5A5095].
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

// The EMPLACEMENT occupant's band takes the other road: the ratio is clamped
// to 1.0 and classified through the shared HUD_ClassifyHealthBand (signed
// > 0xC000 Good, > 0x6FFF Middle) rather than the inline unsigned/signed pair
// above [orig: the emplacement arm @0x5a54d3..0x5a54e3 — min(ratio, 0x10000)
//  then HUD_ClassifyHealthBand @0x59c1f0, result 2 Good / 1 Middle / else Bad].
// Behaviourally equal on every non-negative ratio; a negative ratio reads BAD
// here where the rider arm reads Good — the asymmetry retail carries.
SeatHealthBand emplacement_health_band(int32_t health, int32_t max_health);

// THE SEAT-SELECT DIGIT. Every empty marker prints ONE digit through "%1d"
// [orig: off_7D8E64 = "%1d"], and the number is the seat's 1-based position
// in Entity_BuildWeaponSlotList's order with 10 folded to 0 — the key the
// seat1..seat10 binding rows select with
// [orig: passenger k -> (listPos + 1) % 10 @0x5a5283; the emplacement arm
//  prints i + 2 @0x5a5602 (= its list position 1 + i, plus one); the driver
//  arm prints 1 @0x5a57ee — its list position is always 0].
// The list order itself [orig: Entity_BuildWeaponSlotList @0x434c60]: slot 0
// the vehicle's control seat (type 8), then every attached gun child in the
// def's gun-slot order (type 9), then the present passenger seats 0..7 by
// seat index, capped at 10 entries.
inline int seat_label_digit(int list_pos) {
	return (list_pos + 1) % 10;
}
inline int emplace_label_digit(int gun_slot_index) {
	return gun_slot_index + 2; // at most 4 gun slots, so never folds
}
inline constexpr int kDriverLabelDigit = 1;

// The emplacement digit's draw count. The label arm walks the WHOLE slot
// list counting type-9 entries from -1 and draws the gun slot's digit at every
// list position whose running count equals the gun slot index: once for the
// matching emplacement entry, again for each later non-emplacement entry up to
// the next emplacement, and never when the list holds fewer emplacements than
// index + 1 [orig: HUD_DrawVehicleHealthBars -- count -1 @0x5A5593, the type-9
// increment @0x5A55A0..0x5A55A7, `cmp count,edi` @0x5A55AC, the walk's end
// @0x5A5670].
inline int emplace_label_draws(const int *slot_types, int count, int gun_slot_index) {
	int draws = 0;
	int emplacements = -1;
	for (int i = 0; i < count; ++i) {
		if (slot_types[i] == 9) ++emplacements;
		if (emplacements == gun_slot_index) ++draws;
	}
	return draws;
}

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
// [orig: @0x5A509B..0x5A50B9 — baseX = dword_2723AF4 + dword_2723B24[byte_27235C0],
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
