// The emplaced turret-window source selection: the per-seat addeweap arc
// (any nonzero value selects the whole quartet verbatim) versus the
// weapon-def fallback in the parser's integer BAM conversion.
// [orig: Entity_GetWeaponTurretLimits @0x540d70 — per-seat leg
//  @0x540db5..0x540e27, all-zero test @0x540e27, weapon-def leg
//  @0x540e35..0x540e58; arrays filled by ItemDef_ParseProperty @0x49eb00
//  from `addeweap[C|G] <bone> <itemid> [down up right left]`]

#include <runtime/world/turret_window.h>

#include <cstdio>

namespace {

namespace w = opennova::world;

constexpr int32_t kBamPerDegree = 11930464;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool per_seat_quartet_wins_over_weapon_window() {
	w::WeaponTableEntry entry;
	entry.turret_yaw_range_deg = 90;
	entry.turret_pitch_max_deg = 45;
	entry.turret_pitch_min_deg = 45;
	// The JOX "addeweapG ewep01 100184 70 10 100 100" shape: down/up/right/
	// left degrees, mins stored negated by the parser.
	const w::TurretWindow window = w::select_turret_window(
			70 * kBamPerDegree, -10 * kBamPerDegree, 100 * kBamPerDegree,
			-100 * kBamPerDegree, &entry);
	return expect(window.per_seat &&
			window.yaw_upper == 100 * kBamPerDegree &&
			window.yaw_lower == -100 * kBamPerDegree &&
			window.pitch_upper == 70 * kBamPerDegree &&
			window.pitch_lower == -10 * kBamPerDegree,
			"any nonzero per-seat value selects the authored quartet over the weapon window");
}

bool per_seat_zero_pair_pins_that_axis() {
	// An authored quartet with a zero azimuth pair still selects per-seat:
	// retail's caller runs both Math_ClampAngleToBounds calls on the returned values, so
	// the zero pair pins the axis at zero rather than falling to the weapon
	// window.
	w::WeaponTableEntry entry;
	entry.turret_yaw_range_deg = 90;
	const w::TurretWindow window = w::select_turret_window(
			37 * kBamPerDegree, -37 * kBamPerDegree, 0, 0, &entry);
	return expect(window.per_seat && window.yaw_upper == 0 &&
			window.yaw_lower == 0 &&
			window.pitch_upper == 37 * kBamPerDegree,
			"a zero pair inside a live quartet pins its axis instead of widening");
}

bool all_zero_quartet_falls_to_weapon_window() {
	// Bare `addeweap <bone> <itemid>` authors no arc: all four stay zero and
	// the weapon-def leg supplies the window.
	w::WeaponTableEntry entry;
	entry.turret_yaw_range_deg = 90;
	entry.turret_pitch_max_deg = 45;
	entry.turret_pitch_min_deg = 10;
	const w::TurretWindow window =
			w::select_turret_window(0, 0, 0, 0, &entry);
	return expect(!window.per_seat &&
			window.yaw_upper == w::turret_window_limit_bam(90) &&
			window.yaw_lower == -w::turret_window_limit_bam(90) &&
			window.pitch_upper == w::turret_window_limit_bam(45) &&
			window.pitch_lower == -w::turret_window_limit_bam(10),
			"an unauthored arc falls to the weapon-def window");
}

bool weapon_window_clamps_unconditionally() {
	// The fallback reads the parser's integer bounds: 180 stops 128 BAM
	// short of the half circle and an unauthored 0 is a real bound, so a
	// zero yawrange locks the traverse. Only a missing weapon def leaves no
	// window. [orig: WeaponDefs_ParseLineCallback imul 0xB60B60 @0x5443EC,
	//  imul 0xFF49F4A0 @0x544424; Entity_GetWeaponTurretLimits fallback
	//  @0x540E2C..0x540E58; Math_ClampAngleToBounds calls @0x44123C /
	//  @0x44128C, unconditional]
	w::WeaponTableEntry entry;
	entry.turret_yaw_range_deg = 180;
	entry.turret_pitch_max_deg = 0;
	entry.turret_pitch_min_deg = 45;
	const w::TurretWindow window =
			w::select_turret_window(0, 0, 0, 0, &entry);
	const w::TurretWindow none = w::select_turret_window(0, 0, 0, 0, nullptr);
	return expect(!window.per_seat && window.active &&
			window.yaw_upper == 180 * kBamPerDegree &&
			window.yaw_upper == 0x7FFFFF80 &&
			window.yaw_lower == -180 * kBamPerDegree &&
			window.pitch_upper == 0 &&
			window.pitch_lower == -45 * kBamPerDegree &&
			w::turret_window_limit_bam(-45) == -45 * kBamPerDegree &&
			!none.active && !none.per_seat,
			"the fallback keeps the parser's integer bounds and clamps with any def");
}

} // namespace

int main() {
	if (!per_seat_quartet_wins_over_weapon_window()) return 1;
	if (!per_seat_zero_pair_pins_that_axis()) return 1;
	if (!all_zero_quartet_falls_to_weapon_window()) return 1;
	if (!weapon_window_clamps_unconditionally()) return 1;
	std::puts("turret_window: OK");
	return 0;
}
