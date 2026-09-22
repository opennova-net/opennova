#pragma once

#include <cstdint>

#include <runtime/world/angle.h>
#include <runtime/world/weapon_table.h>

namespace opennova::world {

// The emplaced turret clamp window and its source selection.
//
// Retail resolves the window in Entity_GetWeaponTurretLimits @0x540d70: the
// per-seat leg @0x540db5..0x540e27 reads the CARRIER def's addeweap arc
// arrays (carrierDef+540 down / +556 up / +572 right / +588 left, authored as
// `addeweap[C|G] <bone> <itemid> [down up right left]` degrees x11930464 BAM
// with the mins stored negated by the parser, ItemDef_ParseProperty
// @0x49eb00), indexed by the child's seat (subType, entity+532) and gated on
// the child def ATTR_EWeap (0x20, ItemDef+84). The all-zero test @0x540e27
// then falls to the weapon-def leg @0x540e35..0x540e58 (targetyawrange
// +324 symmetric, targetpitchmax/min +316/+320). Any nonzero per-seat value
// selects the WHOLE quartet verbatim — an authored zero pair inside a live
// quartet pins that axis at zero, exactly like retail's unconditional
// Math_ClampAngleToBounds calls on the returned values.
struct TurretWindow {
	// Upper/lower clamp bounds per axis, ready for the emplaced clamp
	// (upper >= value >= lower in wrapped BAM). All four zero = no window.
	int32_t yaw_upper = 0;
	int32_t yaw_lower = 0;
	int32_t pitch_upper = 0;
	int32_t pitch_lower = 0;
	// True when the per-seat authored quartet was selected (the addeweap arc);
	// false = the weapon-def fallback.
	bool per_seat = false;
	// Whether a window exists at all: the per-seat quartet, or a weapon def
	// to read the fallback from. An active window clamps BOTH axes every
	// time, so a zero bound pins its axis.
	bool active = false;
	bool empty() const {
		return yaw_upper == 0 && yaw_lower == 0 && pitch_upper == 0 &&
				pitch_lower == 0;
	}
};

// Degrees -> BAM bound exactly as the weapon-def parser stores it: the
// integer multiply by 11930464 that wraps, so 180 stops 128 BAM short of the
// half circle and 0 is a real bound. The fallback's consumers clamp to it
// unconditionally. [orig: WeaponDefs_ParseLineCallback targetpitchmax
//  imul 0xB60B60 @0x5443EC, targetpitchmin imul 0xFF49F4A0 (negated)
//  @0x544424, targetyawrange store @0x54446E; Entity_GetWeaponTurretLimits
//  fallback @0x540E2C..0x540E58]
inline int32_t turret_window_limit_bam(int16_t degrees) {
	return static_cast<int32_t>(
			static_cast<uint32_t>(static_cast<int32_t>(degrees)) * 11930464u);
}

// Select the clamp window: the per-seat authored quartet (down/up/right/left,
// signed BAM as parsed) wins when any value is nonzero; otherwise the
// fallback window (symmetric yaw range, +max/-min pitch), all in BAM, which
// exists only when the gun has a weapon def to read it from.
// [orig: Entity_GetWeaponTurretLimits @0x540d70 — the all-zero test
// @0x540e27 selects between the two legs]
inline TurretWindow select_turret_window_bam(int32_t down_limit_bam,
		int32_t up_limit_bam, int32_t right_limit_bam, int32_t left_limit_bam,
		bool fallback_valid, int32_t fallback_yaw_range_bam,
		int32_t fallback_pitch_max_bam, int32_t fallback_pitch_min_bam) {
	TurretWindow window;
	if ((down_limit_bam | up_limit_bam | right_limit_bam | left_limit_bam) !=
			0) {
		window.per_seat = true;
		window.active = true;
		window.yaw_upper = right_limit_bam;
		window.yaw_lower = left_limit_bam;
		window.pitch_upper = down_limit_bam;
		window.pitch_lower = up_limit_bam;
		return window;
	}
	window.active = fallback_valid;
	window.yaw_upper = fallback_yaw_range_bam;
	window.yaw_lower = -fallback_yaw_range_bam;
	window.pitch_upper = fallback_pitch_max_bam;
	window.pitch_lower = -fallback_pitch_min_bam;
	return window;
}

// The weapon-table flavor of the fallback: degree fields through the
// parser's integer conversion (a null entry has no fallback window).
inline TurretWindow select_turret_window(int32_t down_limit_bam,
		int32_t up_limit_bam, int32_t right_limit_bam, int32_t left_limit_bam,
		const WeaponTableEntry *entry) {
	return select_turret_window_bam(down_limit_bam, up_limit_bam,
			right_limit_bam, left_limit_bam, entry != nullptr,
			entry != nullptr
					? turret_window_limit_bam(entry->turret_yaw_range_deg)
					: 0,
			entry != nullptr
					? turret_window_limit_bam(entry->turret_pitch_max_deg)
					: 0,
			entry != nullptr
					? turret_window_limit_bam(entry->turret_pitch_min_deg)
					: 0);
}

} // namespace opennova::world
