#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// The bounded surface-ring bank. Coordinates are mission-frame Q16.
// [orig: sub_5DDC60 @ 0x5DDC60 (the bank allocation); IDB: CWeatherSlot_Init
//  @ 0x5DDD80 (a misnomer: it initialises one surface-ring row); sub_5DDC90
//  @ 0x5DDC90 only loads wake5.tga / wakegrad.tga and sets their sampler
//  addressing; the draw-side 128-slot scanner over the bank @ 0x5DE340 feeds
//  render_water_surface_decal @ 0x5DE0F0]
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
// `entity_update_counter` drives the first UV's scroll: every draw reads the
// entity-update counter (World::entity_update_counter), which advances once
// per completed entity update, so a draw between two updates repeats the
// scroll; this compile runs once per fixed tick.
// [orig: create_water_surface_mesh @ 0x5DDEF0; render_water_surface_decal
//  @ 0x5DE0F0, the dword_24C1948 read @ 0x5DE25A, masked @ 0x5DE26D /
//  @ 0x5DE27F and stored @ 0x5DE277 / @ 0x5DE284]
void compile_water_wakes(const WaterWakePool &pool, int32_t water_height,
		uint32_t entity_update_counter, const int32_t camera[3], WaterWakeFrame &out);

} // namespace opennova::renderer
