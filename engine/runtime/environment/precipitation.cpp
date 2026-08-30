#include <runtime/environment/precipitation.h>

#include <algorithm>
#include <cstring>

namespace opennova::env {

void PrecipitationField::reset(uint16_t (*rand16)(void *), void *ctx) {
	// [orig: Precipitation_Reset @ 0x5df3a0 — memset the table, seed, kind 0]
	slots.fill(PrecipitationSlot{});
	fall_accum_z = 0;
	seed(rand16, ctx);
}

void PrecipitationField::seed(uint16_t (*rand16)(void *), void *ctx) {
	if (rand16 == nullptr) {
		return;
	}
	for (PrecipitationSlot &slot : slots) {
		// (r << 21 + 0x8000) >> 16 == r << 5; (r << 20 + 0x8000) >> 16 == r << 4
		// [orig: @ 0x5debed / @ 0x5dec0d / @ 0x5dec28]
		slot.x = static_cast<int32_t>(static_cast<uint32_t>(rand16(ctx)) << 5);
		slot.y = static_cast<int32_t>(static_cast<uint32_t>(rand16(ctx)) << 5);
		slot.z = static_cast<int32_t>(static_cast<uint32_t>(rand16(ctx)) << 4);
	}
}

void PrecipitationField::fall_tick(int32_t rain_pct_q16, uint32_t precipitation_kind) {
	if (rain_pct_q16 <= kRainGateQ16) {
		return;
	}
	const int32_t decay = precipitation_kind == 1u ? -kSnowFallPerTick : -kRainFallPerTick;
	for (PrecipitationSlot &slot : slots) {
		slot.z += decay;
	}
	fall_accum_z += decay;
}

int PrecipitationField::active_count(int32_t rain_pct_q16) {
	const int64_t scaled = (3072ll * rain_pct_q16 + 0x8000) >> 16;
	if (scaled <= 1) {
		return 0;
	}
	return static_cast<int>(std::min<int64_t>(scaled, kSlots));
}

void PrecipitationField::update(int32_t cam_x, int32_t cam_y, int32_t cam_z,
		int32_t rain_pct_q16, int32_t water_height_q16,
		const PrecipitationFloorSampler &sampler) {
	const int count = active_count(rain_pct_q16);
	if (count <= 0) {
		return;
	}
	const int32_t bound_x = cam_x - kBackXY;
	const int32_t bound_y = cam_y - kBackXY;
	const int32_t bound_z = cam_z - kBelowZ;
	for (int i = 0; i < count; ++i) {
		PrecipitationSlot &slot = slots[static_cast<size_t>(i)];
		bool wrapped = false;
		// The mask wrap keeps the low bits of the coordinate under the bound's
		// high bits, then lifts a result below the bound by one span
		// [orig: @ 0x5decf5..0x5ded03 (x), @ 0x5ded22..0x5ded30 (y),
		//  @ 0x5ded48..0x5ded57 (z)].
		if (static_cast<uint32_t>(slot.x - bound_x) >= static_cast<uint32_t>(kSpanXY)) {
			slot.x = bound_x ^ ((bound_x ^ slot.x) & (kSpanXY - 1));
			if (slot.x < bound_x) slot.x += kSpanXY;
			wrapped = true;
		}
		if (static_cast<uint32_t>(slot.y - bound_y) >= static_cast<uint32_t>(kSpanXY)) {
			slot.y = bound_y ^ ((bound_y ^ slot.y) & (kSpanXY - 1));
			if (slot.y < bound_y) slot.y += kSpanXY;
			wrapped = true;
		}
		if (static_cast<uint32_t>(slot.z - bound_z) >= static_cast<uint32_t>(kSpanZ)) {
			slot.z = bound_z ^ ((bound_z ^ slot.z) & (kSpanZ - 1));
			if (slot.z < bound_z) slot.z += kSpanZ;
			wrapped = true;
		}
		if (!wrapped) {
			continue;
		}
		// The floor: bilinear terrain, lifted to the water plane, then the
		// first entity surface on the ray from floor + 200 m down to it
		// [orig: @ 0x5ded75..0x5dedf4].
		int32_t floor_z = sampler.terrain_height != nullptr
				? sampler.terrain_height(sampler.ctx, slot.x, slot.y)
				: 0;
		if (water_height_q16 > floor_z) {
			floor_z = water_height_q16;
		}
		slot.floor_z = floor_z;
		if (sampler.entity_hit != nullptr) {
			int32_t hit_z = 0;
			if (sampler.entity_hit(sampler.ctx, slot.x, slot.y,
						floor_z + kRaycastHeightQ16, floor_z, hit_z) &&
					hit_z > slot.floor_z) {
				slot.floor_z = hit_z;
			}
		}
	}
}

} // namespace opennova::env
