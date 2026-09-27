#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// The bounded surface-ring bank. Coordinates are mission-frame Q16.
// [orig: sub_5DDC60 @ 0x5DDC60 (the bank allocation); WaterRing_InitSlot
//  @ 0x5DDD80 (it initialises one surface-ring row); WaterRing_LoadResources
//  @ 0x5DDC90 loads wake5.tga / wakegrad.tga and sets the ring shader's FFP
//  lighting sources (GfxShader_SetFfpLightingSources); the draw-side 128-slot scanner over the bank @ 0x5DE340 feeds
//  WaterRing_Draw @ 0x5DE0F0]
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

// Output is the presentation frame (mission x, z, -y; the render frame's x/z
// swap), with the witnessed radial geometry, animated first UV and fixed
// gradient UV ready for device upload.
// `entity_update_counter` drives the first UV's scroll: every draw reads the
// entity-update counter (World::entity_update_counter), which advances once
// per completed entity update, so a draw between two updates repeats the
// scroll; this compile runs once per fixed tick.
// [orig: WaterRing_BuildMesh @ 0x5DDEF0; WaterRing_Draw
//  @ 0x5DE0F0, the g_EntityUpdateCounter read @ 0x5DE25A, masked @ 0x5DE26D /
//  @ 0x5DE27F and stored @ 0x5DE277 / @ 0x5DE284]
void compile_water_wakes(const WaterWakePool &pool, int32_t water_height,
		uint32_t entity_update_counter, const int32_t camera[3], WaterWakeFrame &out);

} // namespace opennova::renderer
