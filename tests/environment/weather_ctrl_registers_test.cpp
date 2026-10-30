// The weather's FLICKER / SWING CTRL registers (runtime/environment/weather_ctrl_registers): the
// frame's pair every model defaults to, and the wave rings a model hashes its own position onto,
// valid only while a weather published them.
// [orig: HUD_CacheEntityDisplayInfo @ 0x4A3D9E..0x4A3DD1 -> 0x83FD00 / 0x83FD08]

#include <runtime/environment/weather_ctrl_registers.h>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                       \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

void test_the_pair_and_the_rings() {
	opennova::env::WeatherCtrlRegisters registers;
	int32_t flicker = -1;
	int32_t swing = -1;
	registers.pair(flicker, swing);
	CHECK(flicker == 0 && swing == 0);
	registers.publish(11, 22);
	registers.pair(flicker, swing);
	CHECK(flicker == 11 && swing == 22);
	CHECK(!registers.sample_at(0, 0, 0, flicker, swing));

	opennova::env::WeatherOscillator oscillator;
	for (int i = 0; i < 256; ++i) {
		oscillator.amp_ring[i] = 1000 + i;
		oscillator.osc_ring[i] = 2000 + i;
	}
	oscillator.ring_index = 3;
	registers.publish_rings(&oscillator);
	CHECK(registers.rings_valid());
	const int32_t x = 5 << 14;
	const int32_t y = 2 << 14;
	const int32_t z = 1 << 15;
	const uint8_t slot = oscillator.ring_slot(x, y, z);
	CHECK(registers.sample_at(x, y, z, flicker, swing));
	CHECK(flicker == 1000 + slot && swing == 2000 + slot);
	// The copy is the publish's: a later tick of the source is not seen until the next publish.
	oscillator.amp_ring[slot] = 7;
	CHECK(registers.sample_at(x, y, z, flicker, swing) && flicker == 1000 + slot);
	registers.publish_rings(nullptr);
	CHECK(!registers.rings_valid());
	CHECK(!registers.sample_at(x, y, z, flicker, swing));
	// The process's one pair.
	CHECK(&opennova::env::weather_ctrl_registers() == &opennova::env::weather_ctrl_registers());
}

} // namespace

int main() {
	test_the_pair_and_the_rings();
	if (failures != 0) {
		std::printf("weather_ctrl_registers_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("weather_ctrl_registers_test passed\n");
	return 0;
}
