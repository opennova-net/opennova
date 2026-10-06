// The impact-scar draw list — see scar_draw_list.h.
// [orig: Scar_RenderCache @0x5CD830; Scar_RenderAllCaches @0x5CDF70]

#include <runtime/renderer/scar_draw_list.h>

#include <base/io/fixed.h>
#include <runtime/world/collision.h>

#include <cmath>

namespace opennova::renderer {

// [orig: Scar_RenderCache @0x5CD830 -- the slot's isBuilding byte @0x5cd92c;
//  building mask @0x5cd93d; first blink-hit sentinel @0x5cd94a..0x5cd951;
//  four-hit scan @0x5cd955..0x5cd9ac]
// Entity::blink_hits carries ((section & 0x1F) | (pool_index << 8)) << 12.
// A building slot draws on any LOW-28 mask bit of its owner. Other slots
// bypass the mask entirely when their owner's FIRST containing-box slot (+464)
// is zero; otherwise any of the four hits (+464..+476) may admit them. The
// branch is the slot's byte, never the owner's pool: a decoration in the BMS
// Building list (pool 2, def type not 5) takes the containing-box leg.
// Missing occlusion instances retain the embedder's all-visible fallback.
bool scar_owner_visible(const world::Entity *owner, bool building,
		const ScarSectionMaskLookup &section_mask) {
	if (!owner) return false;
	if (building) {
		const auto mask = section_mask(owner->handle);
		return !mask || ((*mask & 0x0FFFFFFFu) != 0u);
	}
	if (owner->blink_hits[0] == 0u) return true;
	for (const uint32_t hit : owner->blink_hits) {
		if (hit == 0u) continue;
		const int section = static_cast<int>((hit >> 12) & 0x1Fu);
		const auto containing = world::EntityHandle::make(2, static_cast<int>(hit >> 20));
		const auto mask = section_mask(containing);
		if (!mask || ((*mask & (1u << section)) != 0u)) return true;
	}
	return false;
}


namespace {

using opennova::world::ScarRing;
using opennova::world::ScarSlot;

constexpr float kQ16 = io::kFp16One;

// The half-axis products: `(r * axis + 0x8000) >> 16` per component
// [orig: the radius * axis products @0x5CD830 before Math_FixedPointToFloat3].
inline float half_axis(std::int32_t radius_q16, std::int32_t axis_q16) {
	const std::int64_t p = static_cast<std::int64_t>(radius_q16) * axis_q16 + 0x8000;
	return static_cast<float>(p >> 16) / kQ16;
}

// One slot's six vertices around centre C with axes A and B (all Q16), into
// `dst` [orig: Scar_RenderCache @0x5CD830 — the quad build after the
// transforms].
void emit_quad_at(const std::int32_t pos[3], const std::int32_t axis_a[3],
		const std::int32_t axis_b[3], std::int32_t radius_q16, const ScarViewContext &ctx,
		std::vector<ScarVertex> &dst) {
	const float cx = static_cast<float>(pos[0]) / kQ16;
	const float cy = static_cast<float>(pos[1]) / kQ16;
	const float cz = static_cast<float>(pos[2]) / kQ16;
	const float ax = half_axis(radius_q16, axis_a[0]);
	const float ay = half_axis(radius_q16, axis_a[1]);
	const float az = half_axis(radius_q16, axis_a[2]);
	const float bx = half_axis(radius_q16, axis_b[0]);
	const float by = half_axis(radius_q16, axis_b[1]);
	const float bz = half_axis(radius_q16, axis_b[2]);
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
		// [orig: g_EnvTerrainLightCombined | 0xFF000000 on every vertex]
		v.argb = ctx.terrain_light_argb | 0xFF000000u;
		v.u = uv[order[k]][0];
		v.v = uv[order[k]][1];
		dst.push_back(v);
	}
}

// A slot as stored: world space for the shared ring, section-local for an
// entity ring [orig: the shared ring's `pos` taken as-is].
void emit_quad(const ScarSlot &slot, const ScarViewContext &ctx, ScarDrawList &out) {
	emit_quad_at(slot.pos, slot.axis_a, slot.axis_b, slot.radius_q16, ctx, out.vertices);
}

// An entity-ring slot taken through its owner's section matrix into world
// space, as Scar_RenderCache draws it: the centre with the translation, both
// axes rotation-only, each Q22 with the 0x200000 rounding, then the radius
// products [orig: @0x5CDA49 Math_FixedPointTransformPoint22 @0x615810 on the
// position; @0x5CDA64 / @0x5CDA7F Math_TransformPointFixedPoint22 @0x412E90 on
// the tangent and the bitangent; the matrix `bones + (slot byte +61 << 6)`].
void emit_world_quad(const ScarSlot &slot, const world::CollisionMatrix &section,
		const ScarViewContext &ctx, ScarDrawList &out) {
	std::int32_t pos[3];
	std::int32_t axis_a[3];
	std::int32_t axis_b[3];
	section.transform_point(slot.pos, pos);
	section.rotate_point(slot.axis_a, axis_a);
	section.rotate_point(slot.axis_b, axis_b);
	emit_quad_at(pos, axis_a, axis_b, slot.radius_q16, ctx, out.world_vertices);
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
	// The owner gate per shared-ring slot, on the slot's own building byte
	// [orig: the isBuilding byte @0x5CD92C / the owner's containing-building
	//  handles @0x5CD94A..0x5CD9AC].
	if (ctx.owner_visible != nullptr && slot.owner.valid() &&
			!ctx.owner_visible(slot.owner.packed, slot.building, ctx.user)) {
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
// so the presenter can parent each run under the owner's section node, each
// batch also in world space through the section matrix when it resolves.
// Every slot is gated on its own owner and building byte, as the shared
// ring's are [orig: Scar_RenderCache @0x5CD830 walks an entity cache's 256
// slots through the same per-slot gate @0x5CD92C..0x5CD9B2].
void emit_entity_ring(const ScarRing &ring, const ScarViewContext &ctx, ScarDrawList &out) {
	bool sections_seen[256] = {};
	bool slot_visible[opennova::world::kScarsPerEntity] = {};
	for (int i = 0; i < opennova::world::kScarsPerEntity; ++i) {
		const ScarSlot &slot = ring.slots[static_cast<size_t>(i)];
		if (!slot.live) continue;
		if (ctx.owner_visible != nullptr &&
				!ctx.owner_visible(slot.owner.packed, slot.building, ctx.user)) {
			++out.slots_culled;
			continue;
		}
		slot_visible[i] = true;
		sections_seen[slot.bone] = true;
	}
	for (int section = 0; section < 256; ++section) {
		if (!sections_seen[section]) continue;
		world::CollisionMatrix matrix;
		const bool world_resolved = ctx.section_matrix != nullptr &&
				ctx.section_matrix(ring.owner.packed, section, matrix, ctx.user);
		for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
			const std::uint32_t first = static_cast<std::uint32_t>(out.vertices.size());
			const std::uint32_t world_first =
					static_cast<std::uint32_t>(out.world_vertices.size());
			for (int i = 0; i < opennova::world::kScarsPerEntity; ++i) {
				const ScarSlot &slot = ring.slots[static_cast<size_t>(i)];
				if (!slot_visible[i] || slot.texture != strip || slot.bone != section) continue;
				++out.slots_live;
				emit_quad(slot, ctx, out);
				if (world_resolved) emit_world_quad(slot, matrix, ctx, out);
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
			batch.world_resolved = world_resolved;
			batch.world_first_vertex = world_resolved ? world_first : 0;
			out.batches.push_back(batch);
		}
	}
}

} // namespace

ScarStripState decode_scar_strip_mode(std::uint32_t mode_word) {
	ScarStripState s;
	// Bits 0-3: the framebuffer blend [orig: RenderState_DecodeBlendModeToD3DStates
	// @0x680F00 — case 1 writes SRCBLEND 5, DESTBLEND 6, ALPHABLENDENABLE 1].
	s.src_alpha_blend = (mode_word & 0xFu) == 1u;
	// Bits 4-7: the stage-0 alpha op [orig: RenderState_DecodeModeAlphaStage @0x680B00
	// — nibble 0x50 substate 0: ALPHAOP 4 (MODULATE), ARG1 TEXTURE, ARG2
	// DIFFUSE].
	s.alpha_modulate_texture_diffuse = ((mode_word >> 4) & 0xFu) == 5u;
	// Bits 8-13: the stage-0 colour op [orig: RenderState_DecodeModeColorStage @0x681080
	// — family 0x600 sub-pass 0: COLOROP 4 + GfxDevice_Modulate2XEnabled,
	// ARG1 TEXTURE, ARG2 DIFFUSE].
	s.color_modulate2x_texture_diffuse = (mode_word & 0x3F00u) == 0x600u;
	// Bits 16+: the pass flags [orig: CGfxShader_ApplyPass @0x683232..0x6832BE].
	s.alpha_test = (mode_word & 0x40000u) != 0u;
	s.fog = (mode_word & 0x20000u) != 0u;
	s.depth_write = (mode_word & 0x100000u) == 0u;
	s.cull_none = (mode_word & 0x400000u) != 0u;
	return s;
}

void compile_scar_draws(const opennova::world::ScarCache &cache,
		const ScarViewContext &ctx, ScarDrawList &out) {
	out.vertices.clear();
	out.world_vertices.clear();
	out.batches.clear();
	out.slots_live = 0;
	out.slots_culled = 0;
	// The shared ring first (alloc 0), then every leased entity ring
	// [orig: Scar_RenderAllCaches @0x5CDF70 zeroes the batches, renders the
	//  shared cache, then walks the entity caches].
	emit_world_ring(cache.world_ring(), ctx, out);
	for (const ScarRing &ring : cache.entity_rings()) {
		if (!ring.in_use) continue;
		emit_entity_ring(ring, ctx, out);
	}
}

}  // namespace opennova::renderer
