#include <runtime/environment/weather_ctrl_registers.h>

namespace opennova::env {

void WeatherCtrlRegisters::publish_rings(const WeatherOscillator *oscillator) {
	if (oscillator == nullptr) {
		rings_valid_ = false;
		return;
	}
	rings_ = *oscillator;
	rings_valid_ = true;
}

// [orig: HUD_CacheEntityDisplayInfo @ 0x4A3D9E..0x4A3DD1 - the slot's amp_ring and osc_ring
//  words into 0x83FD00 / 0x83FD08]
bool WeatherCtrlRegisters::sample_at(
		int32_t x_q16, int32_t y_q16, int32_t z_q16, int32_t &flicker, int32_t &swing) const {
	if (!rings_valid_) {
		return false;
	}
	const uint8_t slot = rings_.ring_slot(x_q16, y_q16, z_q16);
	flicker = rings_.amp_ring[slot];
	swing = rings_.osc_ring[slot];
	return true;
}

WeatherCtrlRegisters &weather_ctrl_registers() {
	static WeatherCtrlRegisters registers;
	return registers;
}

} // namespace opennova::env
