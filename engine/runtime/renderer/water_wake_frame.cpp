#include "water_wake_frame.h"
#include <base/io/fixed.h>
#include <algorithm>
#include <cmath>

namespace opennova::renderer {

// [orig: sub_56BD20 @ 0x56BD20; WaterRing_InitSlot @ 0x5DDD80 (the
//  surface-ring row init)]
void WaterWakePool::add(int32_t x, int32_t y, float opacity) {
	for (auto &row : rows_) {
		if (row.active)
			continue;
		row = { true, int32_t(uint32_t(x) & 0xfffe0000u), int32_t(uint32_t(y) & 0xfffe0000u), 0,
			opacity, 0 };
		return;
	}
}

// [orig: WaterRing_TickAll @ 0x5DDE10 (the per-tick fade); WaterRing_RemoveSlot @ 0x5DDDB0 (the
//  row removal)]
void WaterWakePool::tick() {
	for (int i = 0; i < 128; ++i) {
		auto &row = rows_[i];
		if (!row.active)
			continue;
		++row.age;
		if (row.age < 12)
			row.alpha = float(double(row.age) * double(0.083333336f) * row.peak);
		else if (row.age < 32)
			row.alpha = float(double(32 - row.age) * double(0.05f) * row.peak);
		else {
			row.active = false;
			// Clear the vacated tail: retaining retail's duplicated final
			// slot makes a completely full bank loop forever on expiry.
			// Intentional bounded-pool correction: vehicle-client-movers-re.md (D-VEH-2).
			for (int j = i; j < 127; ++j)
				rows_[j] = rows_[j + 1];
			rows_.back() = {};
			--i;
		}
	}
}

// [orig: WaterRing_BuildMesh @ 0x5DDEF0; WaterRing_Draw @ 0x5DE0F0]
void compile_water_wakes(const WaterWakePool &pool, int32_t water_height,
		uint32_t entity_update_counter, const int32_t camera[3], WaterWakeFrame &out) {
	out.clear();
	// The first UV scrolls by (counter & 0x1FF) / 512 and (counter & 0x3FF) *
	// -0.01171875, the counter being the entity-update counter (see the header)
	// [orig: WaterRing_Draw @ 0x5DE0F0, the scroll @ 0x5DE277..0x5DE2AD].
	const float scroll_u = float(entity_update_counter & 511u) * 0.001953125f;
	const float scroll_v = float(entity_update_counter & 1023u) * -0.01171875f;
	for (const auto &wake : pool.rows()) {
		if (!wake.active)
			continue;
		const double dx = double(wake.x) - camera[0], dy = double(wake.y) - camera[1],
					 dz = double(water_height) - camera[2];
		const int32_t distance =
				int32_t(std::min(2147483647.0, std::sqrt(dx * dx + dy * dy + dz * dz)));
		const int32_t height = int32_t(uint32_t(water_height) + uint32_t(distance >> 11) + 2048u);
		const float x = float(wake.x) * io::kInvFp16One, y = float(height) * io::kInvFp16One,
					z = -float(wake.y) * io::kInvFp16One;
		const int32_t base = int32_t(out.vertices.size());
		// The static ring mesh is built in the render (d3d) frame as
		// (sin * r, 0, cos * r) [orig: WaterRing_BuildMesh @ 0x5de01d..
		// 0x5de03e] and placed through Math_FixedPointToFloat3_YNegated
		// (@ 0x5de181); the output frame here is the render frame's x/z swap
		// (godot/src/util/axes.h), so the ring's render x offset lands on z and
		// its render z offset on x. The swap keeps the ring's winding and
		// angular texture sense (its front faces up, a mirrored copy would
		// face down).
		for (int row = 0; row < 9; ++row) {
			for (int col = 0; col < 19; ++col) {
				const double radius = (double(row) + double(0.1f)) * 2.5;
				const double angle = double(col) * double(0.34906587f);
				out.vertices.push_back({ x + float(std::cos(angle) * radius), y,
						z + float(std::sin(angle) * radius),
						float(double(col) * double(0.22222222f)) + scroll_u,
						float(row) * 0.25f + scroll_v, float(double(col) * double(0.055555556f)),
						float(row) * 0.125f, wake.alpha });
			}
		}
		for (int row = 0; row < 8; ++row) {
			for (int col = 0; col < 18; ++col) {
				const int32_t a = base + row * 19 + col;
				const int32_t triangle[6] = { a, a + 19, a + 20, a, a + 20, a + 1 };
				out.indices.insert(out.indices.end(), triangle, triangle + 6);
			}
		}
	}
}
} // namespace opennova::renderer
