// The impact-scar draw list: the witnessed six-vertex quad order and UVs, the
// per-texture batching, the fog-box and owner-visibility culls, and the
// terrain-first ring order.
// [orig: Scar_RenderCache @0x5CD830; Scar_RenderAllCaches @0x5CDF70]

#include <renderer/scar_draw_list.h>

#include <cmath>
#include <cstdio>

using namespace opennova::world;
using renderer::ScarDrawList;
using renderer::ScarViewContext;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.0005f; }

// A slot centred at (10, 20, 3) with axis A = +X, axis B = +Y and a 0.5 u
// radius.
ScarSlot make_slot(int32_t cx, int32_t cy, int32_t cz, uint8_t texture) {
	ScarSlot s;
	s.pos[0] = cx * 0x10000;
	s.pos[1] = cy * 0x10000;
	s.pos[2] = cz * 0x10000;
	s.axis_a[0] = 0x10000;
	s.axis_b[1] = 0x10000;
	s.radius_q16 = 0x8000;
	s.texture = texture;
	s.live = true;
	return s;
}

void test_quad_order_and_uvs() {
	ScarCache cache;
	cache.terrain_ring().slots[0] = make_slot(10, 20, 3, 2);
	cache.terrain_ring().in_use = true;
	ScarViewContext ctx;
	ctx.terrain_light_argb = 0x00808080u;
	ScarDrawList out;
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.batches.size() == 1, "one batch for one slot");
	CHECK(out.vertices.size() == 6, "six vertices per quad");
	CHECK(out.slots_live == 1 && out.slots_culled == 0, "the slot drew");
	if (out.vertices.size() != 6) return;
	const renderer::ScarVertex *v = out.vertices.data();
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
	CHECK(out.batches[0].owner_packed == 0xFFFF, "the terrain ring is the world owner");
	CHECK(out.batches[0].first_vertex == 0 && out.batches[0].vertex_count == 6, "batch span");
}

void test_batches_per_texture_and_ring_order() {
	ScarCache cache;
	// Terrain: two slots on strip 0 and one on strip 27.
	ScarRing &terrain = cache.terrain_ring();
	terrain.slots[0] = make_slot(0, 0, 0, 0);
	terrain.slots[1] = make_slot(1, 0, 0, 27);
	terrain.slots[2] = make_slot(2, 0, 0, 0);
	// An entity ring with one strip-3 slot.
	ScarRing *ring = cache.ring_for(EntityHandle::make(1, 7), 42);
	CHECK(ring != nullptr, "an entity ring leases");
	ring->slots[0] = make_slot(5, 5, 0, 3);
	ScarViewContext ctx;
	ScarDrawList out;
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.batches.size() == 3, "strip 0 + strip 27 for the terrain, strip 3 for the entity");
	if (out.batches.size() != 3) return;
	CHECK(out.batches[0].texture == 0 && out.batches[0].vertex_count == 12,
			"the terrain's strip-0 batch holds both quads");
	CHECK(out.batches[1].texture == 27 && out.batches[1].vertex_count == 6,
			"then the terrain's strip-27 batch");
	CHECK(out.batches[2].owner_packed == EntityHandle::make(1, 7).packed &&
					out.batches[2].texture == 3,
			"the entity ring follows the terrain ring");
	CHECK(out.vertices.size() == 24, "four quads in total");
}

void test_fog_box_cull() {
	ScarCache cache;
	cache.terrain_ring().slots[0] = make_slot(0, 0, 0, 0);    // near
	cache.terrain_ring().slots[1] = make_slot(100, 0, 0, 0);  // beyond the fog on x
	cache.terrain_ring().slots[2] = make_slot(0, -100, 0, 0); // beyond the fog on y
	ScarViewContext ctx;
	ctx.cam_x = 0.0f;
	ctx.cam_y = 0.0f;
	ctx.fog_distance = 50.0f;
	ScarDrawList out;
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 1 && out.slots_culled == 2, "the two far slots are culled");
	CHECK(out.vertices.size() == 6, "only the near quad is emitted");
	// The margin is fog + radius: a slot at fog + 0.4 with a 0.5 radius stays.
	cache.terrain_ring().slots[1] = make_slot(0, 0, 0, 0);
	cache.terrain_ring().slots[1].pos[0] = static_cast<int32_t>(50.4f * 65536.0f);
	cache.terrain_ring().slots[2].live = false;
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 2, "|dx| <= fog + r keeps the slot");
	// No fog distance means no cull.
	ctx.fog_distance = 0.0f;
	cache.terrain_ring().slots[2] = make_slot(0, -100, 0, 0);
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_culled == 0, "a zero fog distance disables the box cull");
}

bool hide_everything(std::uint16_t, void *) { return false; }
bool show_owner_7(std::uint16_t owner, void *) {
	return owner == EntityHandle::make(1, 7).packed;
}

void test_owner_visibility() {
	ScarCache cache;
	ScarRing *a = cache.ring_for(EntityHandle::make(1, 7), 1);
	ScarRing *b = cache.ring_for(EntityHandle::make(2, 9), 2);
	a->slots[0] = make_slot(0, 0, 0, 0);
	b->slots[0] = make_slot(1, 1, 1, 0);
	cache.terrain_ring().slots[0] = make_slot(3, 3, 3, 0);
	ScarViewContext ctx;
	ScarDrawList out;
	ctx.owner_visible = hide_everything;
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 1, "the terrain ring never takes the owner gate");
	CHECK(out.slots_culled == 2, "both entity rings were gated off");
	ctx.owner_visible = show_owner_7;
	renderer::compile_scar_draws(cache, ctx, out);
	CHECK(out.slots_live == 2 && out.slots_culled == 1, "only the visible owner draws");
	CHECK(out.batches.size() == 2, "terrain + the visible entity");
}

} // namespace

int main() {
	test_quad_order_and_uvs();
	test_batches_per_texture_and_ring_order();
	test_fog_box_cull();
	test_owner_visibility();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("scar_draw_list_test OK\n");
	return 0;
}
