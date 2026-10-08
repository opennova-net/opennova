// The impact-scar draw list: the witnessed six-vertex quad order and UVs, the
// per-texture batching, the shared-ring-first order, the entity rings'
// section-local batches and their world-space form through the section
// matrix, and the fog-box / owner-visibility culls (keyed on each slot's
// building byte).
// [orig: Scar_RenderCache @0x5CD830; Scar_RenderAllCaches @0x5CDF70]

#include <runtime/renderer/scar_draw_list.h>
#include <runtime/renderer/texture_filter.h>

#include <runtime/world/collision.h>

#include <cmath>
#include <cstdio>
#include <unordered_map>

using namespace opennova::world;
using opennova::renderer::ScarDrawList;
using opennova::renderer::ScarViewContext;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.0005f; }

// A slot centred at (cx, cy, cz) with axis A = +X, axis B = +Y and a 0.5 u
// radius, owned by `owner`.
ScarSlot make_slot(int32_t cx, int32_t cy, int32_t cz, uint8_t texture,
		EntityHandle owner = EntityHandle{}, uint8_t section = 0) {
	ScarSlot s;
	s.pos[0] = cx * 0x10000;
	s.pos[1] = cy * 0x10000;
	s.pos[2] = cz * 0x10000;
	s.axis_a[0] = 0x10000;
	s.axis_b[1] = 0x10000;
	s.normal[2] = 0x10000;
	s.radius_q16 = 0x8000;
	s.texture = texture;
	s.owner = owner;
	s.bone = section;
	s.live = true;
	return s;
}

void test_quad_order_and_uvs() {
	ScarCache cache;
	cache.world_ring().slots[0] = make_slot(10, 20, 3, 2);
	ScarViewContext ctx;
	ctx.terrain_light_argb = 0x00808080u;
	ScarDrawList out;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.batches.size() == 1, "one batch for one slot");
	CHECK(out.vertices.size() == 6, "six vertices per quad");
	CHECK(out.slots_live == 1 && out.slots_culled == 0, "the slot drew");
	if (out.vertices.size() != 6) return;
	const opennova::renderer::ScarVertex *v = out.vertices.data();
	// C-A-B (0,0), C+A-B (1,0), C-A+B (0,1), C+A-B (1,0), C+A+B (1,1), C-A+B (0,1)
	CHECK(near(v[0].x, 9.5f) && near(v[0].y, 19.5f) && near(v[0].u, 0) && near(v[0].v, 0),
			"vertex 0 = C-A-B (0,0)");
	CHECK(near(v[1].x, 10.5f) && near(v[1].y, 19.5f) && near(v[1].u, 1) && near(v[1].v, 0),
			"vertex 1 = C+A-B (1,0)");
	CHECK(near(v[2].x, 9.5f) && near(v[2].y, 20.5f) && near(v[2].u, 0) && near(v[2].v, 1),
			"vertex 2 = C-A+B (0,1)");
	CHECK(near(v[3].x, 10.5f) && near(v[3].y, 19.5f) && near(v[3].u, 1) && near(v[3].v, 0),
			"vertex 3 repeats C+A-B (1,0)");
	CHECK(near(v[4].x, 10.5f) && near(v[4].y, 20.5f) && near(v[4].u, 1) && near(v[4].v, 1),
			"vertex 4 = C+A+B (1,1)");
	CHECK(near(v[5].x, 9.5f) && near(v[5].y, 20.5f) && near(v[5].u, 0) && near(v[5].v, 1),
			"vertex 5 repeats C-A+B (0,1)");
	for (int i = 0; i < 6; ++i) {
		CHECK(near(v[i].z, 3.0f), "the quad lies in the slot's plane");
		CHECK(v[i].argb == 0xFF808080u, "the terrain light colour with alpha forced opaque");
	}
	CHECK(out.batches[0].texture == 2, "the batch carries the strip index");
	CHECK(out.batches[0].owner_packed == 0xFFFF, "the shared ring is the world owner");
	CHECK(!out.batches[0].entity_local, "shared-ring batches are world space");
	CHECK(out.batches[0].first_vertex == 0 && out.batches[0].vertex_count == 6, "batch span");
}

void test_batches_per_texture_and_ring_order() {
	ScarCache cache;
	// The shared ring: two slots on strip 0 and one on strip 27.
	ScarRing &shared = cache.world_ring();
	shared.slots[0] = make_slot(0, 0, 0, 0);
	shared.slots[1] = make_slot(1, 0, 0, 27);
	shared.slots[2] = make_slot(2, 0, 0, 0);
	// An entity ring with strip-3 slots on sections 1 and 4.
	const EntityHandle owner = EntityHandle::make(1, 7);
	ScarRing *ring = cache.ring_for(owner, 42);
	CHECK(ring != nullptr, "an entity ring leases");
	ring->slots[0] = make_slot(5, 5, 0, 3, owner, 4);
	ring->slots[1] = make_slot(6, 5, 0, 3, owner, 1);
	ring->slots[2] = make_slot(7, 5, 0, 3, owner, 4);
	ScarViewContext ctx;
	ScarDrawList out;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.batches.size() == 4,
			"strip 0 + strip 27 for the shared ring, then section 1 and section 4 for the entity");
	if (out.batches.size() != 4) return;
	CHECK(out.batches[0].texture == 0 && out.batches[0].vertex_count == 12,
			"the shared ring's strip-0 batch holds both quads");
	CHECK(out.batches[1].texture == 27 && out.batches[1].vertex_count == 6,
			"then the shared ring's strip-27 batch");
	CHECK(out.batches[2].owner_packed == owner.packed && out.batches[2].entity_local &&
					out.batches[2].section == 1 && out.batches[2].vertex_count == 6,
			"the entity ring follows, per section: section 1 first");
	CHECK(out.batches[3].section == 4 && out.batches[3].vertex_count == 12 &&
					out.batches[3].texture == 3,
			"section 4 holds its two quads");
	CHECK(out.vertices.size() == 36, "six quads in total");
}

void test_fog_box_cull_is_world_only() {
	ScarCache cache;
	cache.world_ring().slots[0] = make_slot(0, 0, 0, 0);    // near
	cache.world_ring().slots[1] = make_slot(100, 0, 0, 0);  // beyond the fog on x
	cache.world_ring().slots[2] = make_slot(0, -100, 0, 0); // beyond the fog on y
	ScarViewContext ctx;
	ctx.cam_x = 0.0f;
	ctx.cam_y = 0.0f;
	ctx.fog_distance = 50.0f;
	ScarDrawList out;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 1 && out.slots_culled == 2, "the two far shared slots are culled");
	CHECK(out.vertices.size() == 6, "only the near quad is emitted");
	// The margin is fog + radius: a slot at fog + 0.4 with a 0.5 radius stays.
	cache.world_ring().slots[1] = make_slot(0, 0, 0, 0);
	cache.world_ring().slots[1].pos[0] = static_cast<int32_t>(50.4f * 65536.0f);
	cache.world_ring().slots[2].live = false;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 2, "|dx| <= fog + r keeps the slot");
	// Entity-local slots are never box-culled here (their frame is the
	// owner's section; the presenter culls them with the node).
	const EntityHandle owner = EntityHandle::make(1, 2);
	ScarRing *ring = cache.ring_for(owner, 1);
	ring->slots[0] = make_slot(500, 500, 0, 0, owner, 0);
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 3, "an entity-local slot far from the camera still emits");
	// No fog distance means no cull.
	ctx.fog_distance = 0.0f;
	cache.world_ring().slots[2] = make_slot(0, -100, 0, 0);
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_culled == 0, "a zero fog distance disables the box cull");
}

bool hide_everything(std::uint16_t, bool, void *) { return false; }
bool show_owner_7(std::uint16_t owner, bool, void *) {
	return owner == EntityHandle::make(1, 7).packed;
}
// Records the building byte each gated slot hands the predicate.
bool record_building(std::uint16_t, bool building, void *user) {
	auto *seen = static_cast<std::vector<bool> *>(user);
	seen->push_back(building);
	return true;
}

void test_owner_visibility() {
	ScarCache cache;
	const EntityHandle a_owner = EntityHandle::make(1, 7);
	const EntityHandle b_owner = EntityHandle::make(1, 9);
	ScarRing *a = cache.ring_for(a_owner, 1);
	ScarRing *b = cache.ring_for(b_owner, 2);
	a->slots[0] = make_slot(0, 0, 0, 0, a_owner);
	b->slots[0] = make_slot(1, 1, 1, 0, b_owner);
	// Shared-ring slots are gated on their OWN owner; an ownerless slot is
	// never gated.
	cache.world_ring().slots[0] = make_slot(3, 3, 3, 0, EntityHandle::make(2, 1));
	cache.world_ring().slots[1] = make_slot(4, 4, 4, 0, a_owner);
	cache.world_ring().slots[2] = make_slot(5, 5, 5, 0);
	ScarViewContext ctx;
	ScarDrawList out;
	ctx.owner_visible = hide_everything;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 1, "only the ownerless shared slot survives a hide-all gate");
	CHECK(out.slots_culled == 4, "both entity rings and the two owned shared slots were gated off");
	ctx.owner_visible = show_owner_7;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 3 && out.slots_culled == 2,
			"the visible owner's shared slot and ring draw, the others do not");
	CHECK(out.batches.size() == 2, "one shared batch + the visible entity ring's batch");
}

// The gate reads each SLOT's building byte, in both rings, never the ring's
// kind [orig: Scar_RenderCache @0x5CD92C — `cmp byte [slot+60], 0`].
void test_owner_gate_takes_the_slot_building_byte() {
	ScarCache cache;
	const EntityHandle building = EntityHandle::make(2, 3);
	const EntityHandle crate = EntityHandle::make(2, 4);
	ScarSlot hut = make_slot(0, 0, 0, 0, building);
	hut.building = true;
	cache.world_ring().slots[0] = hut;
	cache.world_ring().slots[1] = make_slot(1, 0, 0, 0, crate); // a decoration: byte 0
	const EntityHandle vehicle = EntityHandle::make(1, 9);
	ScarRing *ring = cache.ring_for(vehicle, 1);
	ring->slots[0] = make_slot(2, 0, 0, 0, vehicle);
	std::vector<bool> seen;
	ScarViewContext ctx;
	ctx.owner_visible = record_building;
	ctx.user = &seen;
	ScarDrawList out;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(seen.size() == 3, "every live owned slot is gated, the entity ring's included");
	if (seen.size() != 3) return;
	CHECK(seen[0] && !seen[1], "the shared ring hands each slot's own byte");
	CHECK(!seen[2], "the entity ring's slot hands its byte too");
}

struct MatrixFixture {
	CollisionMatrix section;
	EntityHandle owner;
	int section_index = 0;
};

bool fixture_matrix(std::uint16_t owner, int section, CollisionMatrix &out, void *user) {
	const auto *f = static_cast<const MatrixFixture *>(user);
	if (owner != f->owner.packed || section != f->section_index) return false;
	out = f->section;
	return true;
}

// An entity ring's batch also comes out in WORLD space through the owner's
// section matrix, the way Scar_RenderCache draws it: the centre with the
// translation (Math_FixedPointTransformPoint22), both axes rotation-only
// (Math_TransformPointFixedPoint22), then the radius products
// [orig: @0x5CDA49, @0x5CDA64, @0x5CDA7F; Math_FixedPointTransformPoint22
//  @0x615810; Math_TransformPointFixedPoint22 @0x412E90].
void test_entity_ring_world_form() {
	ScarCache cache;
	MatrixFixture f;
	f.owner = EntityHandle::make(1, 5);
	f.section_index = 2;
	const int32_t origin[3] = {100 * 0x10000, -40 * 0x10000, 7 * 0x10000};
	f.section = collision_matrix_from_heading(0x40000000, origin); // 90 deg
	ScarRing *ring = cache.ring_for(f.owner, 1);
	ring->slots[0] = make_slot(1, 2, 3, 0, f.owner, 2);
	ring->slots[1] = make_slot(4, 0, 0, 0, f.owner, 6); // a section the matrix refuses
	ScarViewContext ctx;
	ctx.section_matrix = fixture_matrix;
	ctx.user = &f;
	ScarDrawList out;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.batches.size() == 2, "one batch per section");
	if (out.batches.size() != 2) return;
	const opennova::renderer::ScarDrawBatch &posed = out.batches[0];
	const opennova::renderer::ScarDrawBatch &refused = out.batches[1];
	CHECK(posed.section == 2 && posed.entity_local && posed.world_resolved,
			"the resolved section carries a world form");
	CHECK(!refused.world_resolved, "a section whose matrix does not resolve stays local only");
	CHECK(out.world_vertices.size() == 6, "only the resolved batch's quad is in world space");
	CHECK(out.vertices.size() == 12, "the section-local stream keeps both batches");
	if (out.world_vertices.size() != 6) return;
	// Expected, from the slot through the same fixed-point transforms.
	const ScarSlot &slot = ring->slots[0];
	int32_t c[3];
	int32_t a[3];
	int32_t b[3];
	f.section.transform_point(slot.pos, c);
	f.section.rotate_point(slot.axis_a, a);
	f.section.rotate_point(slot.axis_b, b);
	const auto half = [&](int32_t axis) {
		return static_cast<float>((static_cast<int64_t>(slot.radius_q16) * axis + 0x8000) >> 16) /
				65536.0f;
	};
	const float cx = static_cast<float>(c[0]) / 65536.0f;
	const float cy = static_cast<float>(c[1]) / 65536.0f;
	const float cz = static_cast<float>(c[2]) / 65536.0f;
	const opennova::renderer::ScarVertex &v0 = out.world_vertices[posed.world_first_vertex];
	CHECK(near(v0.x, cx - half(a[0]) - half(b[0])) && near(v0.y, cy - half(a[1]) - half(b[1])) &&
					near(v0.z, cz - half(a[2]) - half(b[2])),
			"world vertex 0 = M*C - M*A*r - M*B*r");
	const opennova::renderer::ScarVertex &v4 = out.world_vertices[posed.world_first_vertex + 4];
	CHECK(near(v4.x, cx + half(a[0]) + half(b[0])) && near(v4.y, cy + half(a[1]) + half(b[1])),
			"world vertex 4 = M*C + M*A*r + M*B*r");
	// The quarter turn keeps the local point's ground distance from the
	// section origin and lifts it by the translation.
	const float ground = std::sqrt((cx - 100.0f) * (cx - 100.0f) + (cy + 40.0f) * (cy + 40.0f));
	CHECK(std::fabs(ground - std::sqrt(5.0f)) < 0.001f && near(cz, 10.0f) &&
					!near(cx, 101.0f),
			"the centre is the local point posed (turned and moved) through the section matrix");
	const opennova::renderer::ScarVertex &l0 = out.vertices[posed.first_vertex];
	CHECK(near(l0.u, v0.u) && near(l0.v, v0.v) && l0.argb == v0.argb,
			"the two forms share the UVs and the colour");
	// Without a matrix callback the batches stay section-local only.
	ctx.section_matrix = nullptr;
	opennova::renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.world_vertices.empty() && !out.batches[0].world_resolved,
			"no callback, no world form");
}

} // namespace

// The strip table's drawer state: the two mode words the loader builds from
// the modeId selector and their decode [orig: Scar_LoadTextures @0x5CC315 /
// @0x5CC321; CGfxShader_ApplyPass @0x683190; RenderState_DecodeBlendModeToD3DStates
// @0x680F00; RenderState_DecodeModeAlphaStage @0x680B00; RenderState_DecodeModeColorStage
// @0x681080].
void test_strip_mode_words() {
	for (int strip = 0; strip < kScarTextureStripCount; ++strip) {
		const uint32_t expected = strip == 27 ? 0x460651u : 0x120651u;
		CHECK(scar_texture_strip_mode_word(strip) == expected,
				"every strip but bhole1 selects the scorch word");
		// The table's load word | 1 [orig: Scar_LoadTextures @0x5CC2F3]: bigscar
		// at most three levels, the glass holes one, the rest the pixel chain;
		// none carries 0x8.
		const uint32_t flags = scar_texture_strip_creation_flags(strip);
		const uint32_t word = strip == 4 ? 0x80001u : (strip >= 5 && strip <= 26) ? 0x40001u : 1u;
		CHECK(flags == word, "the strip's creation flags are its table word | 1");
		CHECK((flags & opennova::renderer::kTextureFlagDeviceFilter) == 0u,
				"no scar strip takes the device mode");
	}
	// The chains the shipped strips get (pixel_texture_last_level): the 64 x 64
	// scorches end at 4 x 4 (level 4), bhole1 128 x 128 at level 5, bigscar
	// 64 x 64 after three levels (2), a 64 x 64 glass hole on its one level.
	CHECK(opennova::renderer::pixel_texture_last_level(64, 64, scar_texture_strip_creation_flags(0)) == 4,
			"scorch1: five levels");
	CHECK(opennova::renderer::pixel_texture_last_level(128, 128, scar_texture_strip_creation_flags(27)) == 5,
			"bhole1: six levels");
	CHECK(opennova::renderer::pixel_texture_last_level(64, 64, scar_texture_strip_creation_flags(4)) == 2,
			"bigscar: three levels");
	CHECK(opennova::renderer::pixel_texture_last_level(64, 64, scar_texture_strip_creation_flags(5)) == 0,
			"a glass hole: one level");
	const opennova::renderer::ScarStripState scorch =
			opennova::renderer::decode_scar_strip_mode(kScarModeWordScorch);
	CHECK(scorch.src_alpha_blend, "scorch: SRCALPHA/INVSRCALPHA");
	CHECK(scorch.alpha_modulate_texture_diffuse, "scorch: MODULATE(TEXTURE, DIFFUSE) alpha");
	CHECK(scorch.color_modulate2x_texture_diffuse, "scorch: MODULATE2X(TEXTURE, DIFFUSE)");
	CHECK(!scorch.alpha_test, "scorch: NO alpha test — the 128 latch is inert");
	CHECK(scorch.fog, "scorch: fog on");
	CHECK(!scorch.depth_write, "scorch: z-write off");
	CHECK(!scorch.cull_none, "scorch: the CCW back-face cull");
	const opennova::renderer::ScarStripState hole = opennova::renderer::decode_scar_strip_mode(kScarModeWordHole);
	CHECK(hole.src_alpha_blend && hole.alpha_modulate_texture_diffuse &&
					hole.color_modulate2x_texture_diffuse && hole.fog,
			"bhole: the shared blend / stage / fog state");
	CHECK(hole.alpha_test, "bhole: ALPHATESTENABLE — GREATER 128");
	CHECK(hole.depth_write, "bhole: z-write on");
	CHECK(hole.cull_none, "bhole: cull none");
	CHECK(opennova::renderer::kScarAlphaTestRef == 128, "the drawer's latched ref");
	// A blend-off word (the terrain's 0x20200 family) decodes to no blend.
	const opennova::renderer::ScarStripState flat = opennova::renderer::decode_scar_strip_mode(0x20200u);
	CHECK(!flat.src_alpha_blend && !flat.alpha_modulate_texture_diffuse &&
					!flat.color_modulate2x_texture_diffuse && flat.fog && flat.depth_write,
			"a foreign word decodes field by field");
}

void test_native_owner_visibility() {
	std::unordered_map<uint16_t, uint32_t> masks;
	int reads = 0;
	const opennova::renderer::ScarSectionMaskLookup lookup = [&](EntityHandle handle) -> std::optional<uint32_t> {
		++reads;
		const auto found = masks.find(handle.packed);
		return found == masks.end() ? std::nullopt : std::optional<uint32_t>(found->second);
	};
	using opennova::renderer::scar_owner_visible;
	CHECK(!scar_owner_visible(nullptr, true, lookup) && reads == 0,
			"absent owner is rejected without a mask read");
	Entity owner;
	owner.handle = EntityHandle::make(2, 7);
	owner.kind = EntityKind::Building;
	CHECK(scar_owner_visible(&owner, true, lookup), "an unregistered occlusion instance keeps the all-visible host fallback");
	masks[owner.handle.packed] = 0;
	CHECK(!scar_owner_visible(&owner, true, lookup), "zero building mask is hidden");
	masks[owner.handle.packed] = 0xF0000000u;
	CHECK(!scar_owner_visible(&owner, true, lookup), "building ignores the high four bits");
	masks[owner.handle.packed] = 0x08000000u;
	CHECK(scar_owner_visible(&owner, true, lookup), "building includes bit 27");
	// A decoration in the BMS Building list: pool 2 like a building, but its
	// slots carry byte 0 (def type != 5), so its own mask is never read and
	// an empty first containing-box slot admits it outright
	// [orig: @0x5CD92C; Scar_AddEntry @0x5CCCC6].
	masks[owner.handle.packed] = 0;
	for (auto &hit : owner.blink_hits) hit = 0;
	reads = 0;
	CHECK(scar_owner_visible(&owner, false, lookup) && reads == 0,
			"a pool-2 decoration takes the containing-box leg, not its own mask");
	owner.blink_hits[0] = (9u << 20) | (2u << 12);
	masks[EntityHandle::make(2, 9).packed] = 0;
	CHECK(!scar_owner_visible(&owner, false, lookup),
			"a decoration inside a hidden building section is hidden");
	masks[EntityHandle::make(2, 9).packed] = 1u << 2;
	CHECK(scar_owner_visible(&owner, false, lookup),
			"and drawn once that section is visible");
	owner.kind = EntityKind::Organic;
	for (auto &hit : owner.blink_hits) hit = 0;
	owner.blink_hits[1] = (7u << 20) | (3u << 12);
	masks[EntityHandle::make(2, 7).packed] = 0;
	reads = 0;
	CHECK(scar_owner_visible(&owner, false, lookup) && reads == 0,
			"retail's first empty blink slot bypasses later nonzero slots");
	owner.blink_hits[0] = (8u << 20) | (31u << 12);
	masks[EntityHandle::make(2, 8).packed] = 0;
	CHECK(!scar_owner_visible(&owner, false, lookup), "all referenced sections hidden");
	masks[EntityHandle::make(2, 8).packed] = 0x80000000u;
	CHECK(scar_owner_visible(&owner, false, lookup), "non-building hit includes section 31");
	masks[EntityHandle::make(2, 8).packed] = 0;
	masks[EntityHandle::make(2, 7).packed] = 1u << 3;
	CHECK(scar_owner_visible(&owner, false, lookup), "a later containing box can admit the owner");
	masks.erase(EntityHandle::make(2, 7).packed);
	CHECK(scar_owner_visible(&owner, false, lookup), "a missing containing-building instance keeps the host fallback");
}

int main() {
	test_native_owner_visibility();
	test_quad_order_and_uvs();
	test_batches_per_texture_and_ring_order();
	test_fog_box_cull_is_world_only();
	test_owner_visibility();
	test_owner_gate_takes_the_slot_building_byte();
	test_entity_ring_world_form();
	test_strip_mode_words();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("scar_draw_list_test OK\n");
	return 0;
}
