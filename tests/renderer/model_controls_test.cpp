#include <runtime/renderer/model_controls.h>
#include "common/test_expect.h"

#include <cstdint>
#include <limits>
#include <vector>

using opennova::renderer::ModelControls;
using namespace opennova::threedi;

int main() {
	constexpr int door = THREEDI_CTRL_DOOR_00;
	constexpr int other = THREEDI_CTRL_DOOR_01;
	constexpr int first = THREEDI_CTRL_VEHICLE_SPECIAL1;
	constexpr int second = THREEDI_CTRL_VEHICLE_SPECIAL2;
	{
		ModelControls bus;
		TEST_EXPECT(!bus.store(-1, 5) && !bus.store(THREEDI_CTRL_REGISTER_COUNT, 5));
		TEST_EXPECT(bus.store(THREEDI_CTRL_LOD_FRAC, 0));
		TEST_EXPECT(bus.has(THREEDI_CTRL_LOD_FRAC));
		TEST_EXPECT(!bus.store(THREEDI_CTRL_LOD_FRAC, 0));
		TEST_EXPECT(bus.store(door, 0x10000) && bus.value(door) == 65536);
		TEST_EXPECT(bus.store(door, 0x1ffffffffLL) && bus.value(door) == -1);
		TEST_EXPECT(bus.store(door, 0x80000000LL) && bus.value(door) == std::numeric_limits<int32_t>::min());
		TEST_EXPECT(bus.store(door, 17, "writer-a"));
		TEST_EXPECT(!bus.store(door, 17, "writer-a"));
		TEST_EXPECT(bus.store(door, 17, "writer-b"));
		TEST_EXPECT(!bus.clear_owned(door, "writer-a") && bus.value(door) == 17);
		TEST_EXPECT(bus.clear_owned(door, "writer-b") && !bus.has(door));
		TEST_EXPECT(bus.runtime_values(0, 0)[door] == 0); // No prior value is restored.
		TEST_EXPECT(bus.store(door, 17, "writer-a"));
		TEST_EXPECT(bus.store(door, 17)); // Equal unowned write replaces ownership.
		TEST_EXPECT(!bus.clear_owned(door, "writer-a") && bus.value(door) == 17);
		TEST_EXPECT(bus.store(other, 3, "writer-a"));
		TEST_EXPECT(bus.store(door, 17, "writer-a"));
		TEST_EXPECT(bus.owned_registers("writer-a") == (std::vector<int>{other, door}));
		TEST_EXPECT(bus.present_registers() == (std::vector<int>{THREEDI_CTRL_LOD_FRAC, door, other}));
		TEST_EXPECT(bus.owned_registers("").empty() && !bus.clear_owned(door, ""));
		TEST_EXPECT(bus.clear(door) && !bus.clear(door));
		TEST_EXPECT(bus.owned_registers("writer-a") == (std::vector<int>{other}));
	}
	{
		ModelControls bus;
		TEST_EXPECT(bus.runtime_values(12, -7)[THREEDI_CTRL_FLICKER] == 12);
		TEST_EXPECT(bus.runtime_values(22, -9)[THREEDI_CTRL_SWING] == -9);
		TEST_EXPECT(bus.present_registers().empty());
		TEST_EXPECT(bus.store(THREEDI_CTRL_FLICKER, 0, "weather-probe"));
		TEST_EXPECT(bus.runtime_values(22, -9)[THREEDI_CTRL_FLICKER] == 0);
		bus.sample_weather(41, -43);
		TEST_EXPECT(bus.value(THREEDI_CTRL_FLICKER) == 41 && bus.value(THREEDI_CTRL_SWING) == -43);
		TEST_EXPECT(bus.clear_owned(THREEDI_CTRL_FLICKER, "weather-probe"));
		TEST_EXPECT(bus.runtime_values(22, -9)[THREEDI_CTRL_FLICKER] == 22);
		TEST_EXPECT(bus.runtime_values(22, -9)[THREEDI_CTRL_SWING] == -43);
	}
	{
		ModelControls bus;
		bus.play_part(0, 1, 1.0);
		bus.play_part(3, 1, 1.0);
		bus.play_part(1, 2, 1.0);
		TEST_EXPECT(!bus.has_active_parts() && !bus.advance_parts(1.0));
		bus.play_part(1, 1, 1.0);
		TEST_EXPECT(!bus.has(first) && !bus.advance_parts(0.0) && !bus.advance_parts(-1.0));
		TEST_EXPECT(bus.advance_parts(0.5) && bus.value(first) == 32488);
		TEST_EXPECT(bus.advance_parts(0.5) && bus.value(first) == 64976);
		TEST_EXPECT(bus.advance_parts(0.016) && bus.value(first) == 65536);
		TEST_EXPECT(!bus.has_active_parts());
		TEST_EXPECT(bus.restart_part(2, -1, 1.0) == ModelControls::Restart::Seeded);
		TEST_EXPECT(bus.value(second) == 65536);
		TEST_EXPECT(bus.advance_parts(0.5) && bus.value(second) == 33048);
		TEST_EXPECT(bus.restart_part(2, 0, 1.0) == ModelControls::Restart::Stopped);
		TEST_EXPECT(!bus.has_active_parts() && bus.value(second) == 33048);
		TEST_EXPECT(bus.restart_part(2, 2, 1.0) == ModelControls::Restart::Invalid);
		TEST_EXPECT(bus.value(second) == 33048);
	}
	{
		ModelControls bus;
		bus.restart_part(1, 1, 0.016);
		TEST_EXPECT(!bus.advance_parts(0.008));
		TEST_EXPECT(bus.advance_parts(0.008) && bus.value(first) == 65536);
		TEST_EXPECT(bus.has_active_parts()); // Exact endpoint is still live.
		TEST_EXPECT(!bus.advance_parts(0.016) && !bus.has_active_parts());
		bus.restart_part(1, 1, 0.0);
		TEST_EXPECT(bus.advance_parts(0.016) && bus.value(first) == std::numeric_limits<int32_t>::min());
		TEST_EXPECT(bus.advance_parts(0.016) && bus.value(first) == 0);
		TEST_EXPECT(bus.has_active_parts()); // The native x87 indefinite rate wraps forever.
		bus.clear_parts();
		TEST_EXPECT(!bus.has_active_parts() && bus.has(first));
	}
	{
		ModelControls bus;
		bus.play_part(2, 1, 1.0);
		bus.play_part(1, 1, 1.0);
		TEST_EXPECT(bus.active_part_registers() == (std::vector<int>{second, first}));
		bus.release_part(2);
		TEST_EXPECT(bus.active_part_registers() == (std::vector<int>{first}));
		TEST_EXPECT(!bus.advance_parts(0.008));
		bus.clear_parts();
		bus.play_part(1, 1, 1.0);
		TEST_EXPECT(!bus.advance_parts(0.008)); // A new empty-to-live epoch resets credit.
		TEST_EXPECT(bus.advance_parts(0.008) && bus.value(first) == 1048);
		bus.store(first, 9, "authority");
		bus.release_part(1);
		TEST_EXPECT(!bus.advance_parts(1.0) && bus.value(first) == 9);
		TEST_EXPECT(bus.clear_owned(first, "authority"));
	}
	return 0;
}
