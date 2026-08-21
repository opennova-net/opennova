// The impact-scar draw list — see scar_draw_list.h.
// [orig: Scar_RenderCache @0x5CD830; Scar_RenderAllCaches @0x5CDF70]

#include "renderer/scar_draw_list.h"

#include <cmath>

namespace renderer {

namespace {

using opennova::world::ScarRing;
using opennova::world::ScarSlot;

constexpr float kQ16 = 65536.0f;

// The half-axis products: `(r * axis + 0x8000) >> 16` per component
// [orig: the radius * axis products @0x5CD830 before Math_FixedPointToFloat3].
inline float half_axis(std::int32_t radius_q16, std::int32_t axis_q16) {
	const std::int64_t p = static_cast<std::int64_t>(radius_q16) * axis_q16 + 0x8000;
	return static_cast<float>(p >> 16) / kQ16;
}

void emit_quad(const ScarSlot &slot, const ScarViewContext &ctx, ScarDrawList &out) {
	const float cx = static_cast<float>(slot.pos[0]) / kQ16;
	const float cy = static_cast<float>(slot.pos[1]) / kQ16;
	const float cz = static_cast<float>(slot.pos[2]) / kQ16;
	const float ax = half_axis(slot.radius_q16, slot.axis_a[0]);
	const float ay = half_axis(slot.radius_q16, slot.axis_a[1]);
	const float az = half_axis(slot.radius_q16, slot.axis_a[2]);
	const float bx = half_axis(slot.radius_q16, slot.axis_b[0]);
	const float by = half_axis(slot.radius_q16, slot.axis_b[1]);
	const float bz = half_axis(slot.radius_q16, slot.axis_b[2]);
	// The six vertices in the witnessed order:
	//   C-A-B (0,0), C+A-B (1,0), C-A+B (0,1),
	//   C+A-B (1,0), C+A+B (1,1), C-A+B (0,1)
	const float corner[4][3] = {
		{cx - ax - bx, cy - ay - by, cz - az - bz}, // -A-B
		{cx + ax - bx, cy + ay - by, cz + az - bz}, // +A-B
		{cx - ax + bx, cy - ay + by, cz - az + bz}, // -A+B
		{cx + ax + bx, cy + ay + by, cz + az + bz}, // +A+B
	};
	const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
	const int order[6] = {0, 1, 2, 1, 3, 2};
	for (int k = 0; k < 6; ++k) {
		ScarVertex v;
		v.x = corner[order[k]][0];
		v.y = corner[order[k]][1];
		v.z = corner[order[k]][2];
		// [orig: Env_TerrainLightCombined | 0xFF000000 on every vertex]
		v.argb = ctx.terrain_light_argb | 0xFF000000u;
		v.u = uv[order[k]][0];
		v.v = uv[order[k]][1];
		out.vertices.push_back(v);
	}
}

bool world_slot_visible(const ScarSlot &slot, const ScarViewContext &ctx, ScarDrawList &out) {
	// The fog-box cull on the ground axes [orig: the two |delta| <= fog + r
	// tests before the view transform].
	if (ctx.fog_distance > 0.0f) {
		const float cx = static_cast<float>(slot.pos[0]) / kQ16;
		const float cy = static_cast<float>(slot.pos[1]) / kQ16;
		const float r = static_cast<float>(slot.radius_q16) / kQ16;
		if (std::fabs(cx - ctx.cam_x) > ctx.fog_distance + r ||
				std::fabs(cy - ctx.cam_y) > ctx.fog_distance + r) {
			++out.slots_culled;
			return false;
		}
	}
	// The owner gate per shared-ring slot [orig: the building byte / the
	// owner's containing-building handles @0x5CD830].
	if (ctx.owner_visible != nullptr && slot.owner.valid() &&
			!ctx.owner_visible(slot.owner.packed, ctx.user)) {
		++out.slots_culled;
		return false;
	}
	return true;
}

// The shared ring: world-space slots, one batch per texture strip.
void emit_world_ring(const ScarRing &ring, const ScarViewContext &ctx, ScarDrawList &out) {
	for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
		const std::uint32_t first = static_cast<std::uint32_t>(out.vertices.size());
		bool building = false;
		for (const ScarSlot &slot : ring.slots) {
			if (!slot.live || slot.texture != strip) continue;
			if (!world_slot_visible(slot, ctx, out)) continue;
			++out.slots_live;
			building = slot.building;
			emit_quad(slot, ctx, out);
		}
		const std::uint32_t count = static_cast<std::uint32_t>(out.vertices.size()) - first;
		if (count == 0) continue;
		ScarDrawBatch batch;
		batch.owner_packed = 0xFFFFu;
		batch.texture = static_cast<std::uint8_t>(strip);
		batch.building = building;
		batch.first_vertex = first;
		batch.vertex_count = count;
		out.batches.push_back(batch);
	}
}

// An entity ring: section-local slots, one batch per (section, texture strip)
// so the presenter can parent each run under the owner's section node.
void emit_entity_ring(const ScarRing &ring, const ScarViewContext &ctx, ScarDrawList &out) {
	bool sections_seen[256] = {};
	for (const ScarSlot &slot : ring.slots)
		if (slot.live) sections_seen[slot.bone] = true;
	for (int section = 0; section < 256; ++section) {
		if (!sections_seen[section]) continue;
		for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
			const std::uint32_t first = static_cast<std::uint32_t>(out.vertices.size());
			for (const ScarSlot &slot : ring.slots) {
				if (!slot.live || slot.texture != strip || slot.bone != section) continue;
				++out.slots_live;
				emit_quad(slot, ctx, out);
			}
			const std::uint32_t count =
					static_cast<std::uint32_t>(out.vertices.size()) - first;
			if (count == 0) continue;
			ScarDrawBatch batch;
			batch.owner_packed = ring.owner.packed;
			batch.texture = static_cast<std::uint8_t>(strip);
			batch.section = static_cast<std::uint8_t>(section);
			batch.entity_local = true;
			batch.first_vertex = first;
			batch.vertex_count = count;
			out.batches.push_back(batch);
		}
	}
}

} // namespace

void compile_scar_draws(const opennova::world::ScarCache &cache,
		const ScarViewContext &ctx, ScarDrawList &out) {
	out.vertices.clear();
	out.batches.clear();
	out.slots_live = 0;
	out.slots_culled = 0;
	// The shared ring first (alloc 0), then every leased entity ring
	// [orig: Scar_RenderAllCaches @0x5CDF70 zeroes the batches, renders the
	//  shared cache, then walks the entity caches].
	emit_world_ring(cache.world_ring(), ctx, out);
	for (const ScarRing &ring : cache.entity_rings()) {
		if (!ring.in_use) continue;
		// The owner visibility gate, once per ring — an owner draws while one
		// of its containing buildings is visible, or outright when it has none
		// [orig: @0x5CD830's owner+464..+476 tests].
		if (ctx.owner_visible != nullptr &&
				!ctx.owner_visible(ring.owner.packed, ctx.user)) {
			for (const ScarSlot &slot : ring.slots)
				if (slot.live) ++out.slots_culled;
			continue;
		}
		emit_entity_ring(ring, ctx, out);
	}
}

} // namespace renderer
