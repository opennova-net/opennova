#include <hud/hud_vehicle_panel.h>

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

} // namespace opennova::hud
