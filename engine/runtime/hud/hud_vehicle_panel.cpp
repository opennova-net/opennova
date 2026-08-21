#include <hud/hud_vehicle_panel.h>

#include <hud/hud_math.h>

namespace opennova::hud {

SeatHealthBand seat_health_band(int32_t health, int32_t max_health) {
	// [orig: the `if (!MaxHealthWithDifficulty) MaxHealthWithDifficulty = 1`
	//  guard immediately before the divide]
	if (max_health == 0) max_health = 1;
	const int64_t ratio = (static_cast<int64_t>(health) << 16) / max_health;
	const int32_t r = static_cast<int32_t>(ratio);
	// UNSIGNED first, SIGNED second -- see the header. Order matters as much
	// as the constants do.
	if (static_cast<uint32_t>(r) > 0xC000u) return SeatHealthBand::Good;
	if (r > 0x6FFF) return SeatHealthBand::Middle;
	return SeatHealthBand::Bad;
}

SeatHealthBand emplacement_health_band(int32_t health, int32_t max_health) {
	// [orig: @0x5a54d3..0x5a54e3 — the same max->1 guard, the ratio clamped to
	//  0x10000, then HUD_ClassifyHealthBand @0x59c1f0 (signed compares)]
	if (max_health == 0) max_health = 1;
	int64_t ratio = (static_cast<int64_t>(health) << 16) / max_health;
	if (ratio > 0x10000) ratio = 0x10000;
	const int band = health_color_band_fp16(static_cast<int32_t>(ratio));
	if (band == 0) return SeatHealthBand::Good;
	if (band == 1) return SeatHealthBand::Middle;
	return SeatHealthBand::Bad;
}

} // namespace opennova::hud
