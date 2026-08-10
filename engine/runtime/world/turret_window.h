#pragma once

#include <cstdint>

#include <world/angle.h>
#include <world/weapon_table.h>

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
// sub_540CC0 calls on the returned values.
struct TurretWindow {
	// Upper/lower clamp bounds per axis, ready for the emplaced clamp
	// (upper >= value >= lower in wrapped BAM). All four zero = no window.
	int32_t yaw_upper = 0;
	int32_t yaw_lower = 0;
	int32_t pitch_upper = 0;
	int32_t pitch_lower = 0;
	// True when the per-seat authored quartet was selected (the addeweap arc);
	// false = the weapon-def fallback (possibly empty).
	bool per_seat = false;
	bool empty() const {
		return yaw_upper == 0 && yaw_lower == 0 && pitch_upper == 0 &&
				pitch_lower == 0;
	}
};

// Degrees -> BAM clamp bound for the weapon-def leg. A half-arc of 180 or
// more is the full circle (the "360" gun family) — no effective window; 0
// tells callers to skip. [orig: the itemDef fallback consumers' witnessed
// no-window semantics, docs/net/novaworld-net-re.md §5.38e]
inline int32_t turret_window_limit_bam(int16_t degrees) {
	if (degrees <= 0 || degrees >= 180) return 0;
	return static_cast<int32_t>(
			bam_from_degrees_wrapped(static_cast<double>(degrees)));
}

// Select the clamp window: the per-seat authored quartet (down/up/right/left,
// signed BAM as parsed) wins when any value is nonzero; otherwise the
// fallback window (symmetric yaw range, +max/-min pitch), all in BAM.
// [orig: Entity_GetWeaponTurretLimits @0x540d70 — the all-zero test
// @0x540e27 selects between the two legs]
inline TurretWindow select_turret_window_bam(int32_t down_limit_bam,
		int32_t up_limit_bam, int32_t right_limit_bam, int32_t left_limit_bam,
		int32_t fallback_yaw_range_bam, int32_t fallback_pitch_max_bam,
		int32_t fallback_pitch_min_bam) {
	TurretWindow window;
	if ((down_limit_bam | up_limit_bam | right_limit_bam | left_limit_bam) !=
			0) {
		window.per_seat = true;
		window.yaw_upper = right_limit_bam;
		window.yaw_lower = left_limit_bam;
		window.pitch_upper = down_limit_bam;
		window.pitch_lower = up_limit_bam;
		return window;
	}
	window.yaw_upper = fallback_yaw_range_bam;
	window.yaw_lower = -fallback_yaw_range_bam;
	window.pitch_upper = fallback_pitch_max_bam;
	window.pitch_lower = -fallback_pitch_min_bam;
	return window;
}

// The weapon-table flavor of the fallback: degree fields through the
// >=180-is-no-window conversion (entry may be null).
inline TurretWindow select_turret_window(int32_t down_limit_bam,
		int32_t up_limit_bam, int32_t right_limit_bam, int32_t left_limit_bam,
		const WeaponTableEntry *entry) {
	return select_turret_window_bam(down_limit_bam, up_limit_bam,
			right_limit_bam, left_limit_bam,
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
