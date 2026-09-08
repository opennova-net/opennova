#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// The bounded surface-ring bank. Coordinates are mission-frame Q16.
// [orig: WaterWake_Alloc @ 0x5DDC60; WaterWake_Init @ 0x5DDD80]
class WaterWakePool {
public:
	struct Row {
		bool active = false;
		int32_t x = 0, y = 0, age = 0;
		float peak = 0, alpha = 0;
	};
	void add(int32_t x, int32_t y, float opacity);
	void tick();
	void clear() { rows_ = {}; }
	const std::array<Row, 128> &rows() const { return rows_; }

private:
	std::array<Row, 128> rows_{};
};

struct WaterWakeVertex {
	float x, y, z, u, v, u2, v2, alpha;
};
struct WaterWakeFrame {
	std::vector<WaterWakeVertex> vertices;
	std::vector<int32_t> indices;
	void clear() {
		vertices.clear();
		indices.clear();
	}
};

// Output is the render frame (mission x,z,-y), with the witnessed radial
// geometry, animated first UV and fixed gradient UV ready for device upload.
// [orig: WaterWake_CreateMesh @ 0x5DDEF0; WaterWake_Render @ 0x5DE0F0]
void compile_water_wakes(const WaterWakePool &pool, int32_t water_height, uint32_t tick,
		const int32_t camera[3], WaterWakeFrame &out);

} // namespace opennova::renderer
