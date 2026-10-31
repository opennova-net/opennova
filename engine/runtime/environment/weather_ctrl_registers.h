// The weather's two global CTRL registers, FLICKER (3) and SWING (4): the frame's pair the
// local player's HUD and viewmodel legs write (every model's default), and the wave rings a
// model whose 3DI declares either register hashes its own position into, as the gnrc and Sway
// world bone callbacks rewrite the pair on the entity they draw. One pair per process, as
// retail's are two globals; the weather owner publishes it every display frame, the models'
// material and PANM evaluations read it.
// [orig: HUD_CacheEntityDisplayInfo @ 0x4A3D9E..0x4A3DD1 -> the globals 0x83FD00 / 0x83FD08,
//  called on the local player @ 0x5A8341 / @ 0x4DEE8B and on the rendered entity by
//  BoneCallback_gnrc_World @ 0x4E286C and BoneCallback_Sway_World @ 0x4E2B22]
#pragma once

#include <formats/env/env_weather.h>

#include <cstdint>

namespace opennova::env {

class WeatherCtrlRegisters {
public:
	// The frame's pair: what every model reads unless it hashes its own position.
	void publish(int32_t flicker, int32_t swing) {
		flicker_ = flicker;
		swing_ = swing;
	}
	void pair(int32_t &flicker, int32_t &swing) const {
		flicker = flicker_;
		swing = swing_;
	}
	// The ring copy a model hashes its position onto; null clears it (no weather stands).
	void publish_rings(const WeatherOscillator *oscillator);
	bool rings_valid() const { return rings_valid_; }
	// The pair at a mission point (16.16): the ring slot's amplitude and oscillation words;
	// false while no rings stand.
	// [orig: WeatherOscillator::ring_slot's readers; HUD_CacheEntityDisplayInfo
	//  @ 0x4A3D9E..0x4A3DD1]
	bool sample_at(
			int32_t x_q16, int32_t y_q16, int32_t z_q16, int32_t &flicker, int32_t &swing) const;

private:
	int32_t flicker_ = 0;
	int32_t swing_ = 0;
	WeatherOscillator rings_;
	bool rings_valid_ = false;
};

// The process's one register pair.
WeatherCtrlRegisters &weather_ctrl_registers();

} // namespace opennova::env
