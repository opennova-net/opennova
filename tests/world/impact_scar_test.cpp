// Impact-scar selection and ring policy.
// [orig: Impact_SpawnGlassEffectsOrScar @0x5CF1B0; Scar_AddEntry
//  @0x5CC830; Scar_TextureForId @0x5CC360; the scar table @0x8413A8/@0x8417A8]

#include <runtime/world/impact_scar.h>

#include <cstdio>

#include <runtime/renderer/scar_draw_list.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

void test_scar_selection() {
	// Ordinary faces take the scorch; surface type 15 takes the small
	// bullet-hole fallback.
	CHECK(scar_id_for_surface(0) == kScarIdNormal, "type 0 takes the scorch");
	CHECK(scar_id_for_surface(3) == kScarIdNormal, "type 3 takes the scorch");
	CHECK(scar_id_for_surface(15) == kScarIdGlassFallback,
			"type 15 takes the fallback");
	// 14 and 16 must NOT — the fallback is one exact type, not a range.
	CHECK(scar_id_for_surface(14) == kScarIdNormal, "14 is not the fallback");
	CHECK(scar_id_for_surface(16) == kScarIdNormal, "16 is not the fallback");

	// Radii come from the scar table, and the fallback is HALF the scorch.
	CHECK(scar_radius_q16(kScarIdNormal) == 0x2000, "scorch radius");
	CHECK(scar_radius_q16(kScarIdGlassFallback) == 0x1000, "fallback radius");
	CHECK(scar_radius_q16(kScarIdGlassFallback) * 2 ==
					scar_radius_q16(kScarIdNormal),
			"the fallback mark is half the scorch");
}

// THE PRNG DISCIPLINE. The scorch picks one of four textures with a draw; the
// fallback has a single texture and takes NONE. That matters because the stream
// is SHARED — drawing unconditionally would advance it on a path retail leaves
// alone and desynchronise every other consumer.
void test_texture_roll_discipline() {
	CHECK(scar_needs_texture_roll(kScarIdNormal),
			"the scorch rolls for its texture");
	CHECK(!scar_needs_texture_roll(kScarIdGlassFallback),
			"the fallback does NOT roll — it has one texture");

	// The fallback ignores whatever word it is handed.
	CHECK(scar_texture_index(kScarIdGlassFallback, 0) == 0, "fallback index 0");
	CHECK(scar_texture_index(kScarIdGlassFallback, 65535) == 0,
			"fallback ignores the word entirely");

	// The scorch spreads across all four, and each is reachable.
	bool seen[kScarNormalTextureCount] = {};
	for (uint32_t w = 0; w < 64; ++w)
		seen[scar_texture_index(kScarIdNormal, uint16_t(w))] = true;
	for (int i = 0; i < kScarNormalTextureCount; ++i)
		CHECK(seen[i], "every scorch texture is reachable");
	CHECK(scar_texture_index(kScarIdNormal, 4) == 0, "wraps at the set size");
	CHECK(scar_texture_index(kScarIdNormal, 7) == 3, "and indexes within it");
}

// THE RING WRAPS. It is not a growing list: the 257th impact on one entity
// overwrites the first, which is what bounds a long firefight against one wall.
void test_ring_wraps() {
	CHECK(scar_ring_slot(0) == 0, "the first impact takes slot 0");
	CHECK(scar_ring_slot(255) == 255, "up to the last slot");
	CHECK(scar_ring_slot(256) == 0, "the 257th overwrites the first");
	CHECK(scar_ring_slot(257) == 1, "and the ring continues");
	CHECK(scar_ring_slot(1000) == 1000 % 256, "for any count");
	// The cache geometry the cursor lives in.
	CHECK(kScarCacheEntityBytes == 16392, "256 x 64 B slots + owner + cursor");
	CHECK(kScarCacheEntities == 128, "128 entity rings in the cache");
}

// The slot writer's gates: no scar below the water plane, none on a husk.
void test_gates() {
	CHECK(scar_allowed(0x20000, 0x10000, false), "above water, live entity");
	CHECK(!scar_allowed(0x10000, 0x10000, false), "at the water plane is NOT above it");
	CHECK(!scar_allowed(0x8000, 0x10000, false), "below water takes no scar");
	CHECK(!scar_allowed(0x20000, 0x10000, true), "a husk takes no scar");
}

// Every scar is spun about its normal — a full BAM16 turn mapped to radians.
void test_spin() {
	CHECK(scar_spin_radians(0) == 0.0f, "word 0 is no rotation");
	const float half = scar_spin_radians(32768);
	CHECK(half > 3.14f && half < 3.15f, "half the word range is half a turn");
	const float most = scar_spin_radians(65535);
	CHECK(most > 6.28f && most < 6.284f, "the top of the range is nearly a turn");
}

bool perpendicular(int32_t nx, int32_t ny, int32_t nz, const int32_t t[3]) {
	const int64_t dot = int64_t(nx) * t[0] + int64_t(ny) * t[1] + int64_t(nz) * t[2];
	const bool nonzero = t[0] != 0 || t[1] != 0 || t[2] != 0;
	return dot == 0 && nonzero;
}

// THE TANGENT LADDER, case for case. The tie |ny| == |nx| zeroes X even when
// Z is the smallest component — a "least aligned axis" pick would not.
void test_tangent_ladder() {
	int32_t t[3];
	// |ny| < |nx|, |nx| > |nz|, |ny| > |nz| -> (ny, -nx, 0)
	scar_tangent(10, 5, 1, t);
	CHECK(t[0] == 5 && t[1] == -10 && t[2] == 0, "case A: (ny, -nx, 0)");
	CHECK(perpendicular(10, 5, 1, t), "case A is perpendicular");
	// |ny| < |nx|, |nx| > |nz|, |ny| <= |nz| -> (nz, 0, -nx)
	scar_tangent(10, 1, 5, t);
	CHECK(t[0] == 5 && t[1] == 0 && t[2] == -10, "case B: (nz, 0, -nx)");
	CHECK(perpendicular(10, 1, 5, t), "case B is perpendicular");
	// |ny| < |nx|, |nx| <= |nz| -> (-nz, 0, nx)
	scar_tangent(5, 1, 10, t);
	CHECK(t[0] == -10 && t[1] == 0 && t[2] == 5, "case C: (-nz, 0, nx)");
	CHECK(perpendicular(5, 1, 10, t), "case C is perpendicular");
	// |ny| == |nx| -> (0, -nz, ny), WHATEVER |nz| is.
	scar_tangent(5, 5, 1, t);
	CHECK(t[0] == 0 && t[1] == -1 && t[2] == 5,
			"the tie zeroes X even though Z is the smallest component");
	CHECK(perpendicular(5, 5, 1, t), "the tie case is perpendicular");
	scar_tangent(-5, 5, 100, t);
	CHECK(t[0] == 0 && t[1] == -100 && t[2] == 5, "the tie compares magnitudes");
	// |ny| > |nx|, |ny| <= |nz| -> (0, -nz, ny)
	scar_tangent(1, 5, 10, t);
	CHECK(t[0] == 0 && t[1] == -10 && t[2] == 5, "case E: (0, -nz, ny)");
	CHECK(perpendicular(1, 5, 10, t), "case E is perpendicular");
	// |ny| > |nx|, |ny| > |nz|, |nx| > |nz| -> (-ny, nx, 0)
	scar_tangent(5, 10, 1, t);
	CHECK(t[0] == -10 && t[1] == 5 && t[2] == 0, "case F: (-ny, nx, 0)");
	CHECK(perpendicular(5, 10, 1, t), "case F is perpendicular");
	// |ny| > |nx|, |ny| > |nz|, |nx| <= |nz| -> (0, nz, -ny)
	scar_tangent(1, 10, 5, t);
	CHECK(t[0] == 0 && t[1] == 5 && t[2] == -10, "case G: (0, nz, -ny)");
	CHECK(perpendicular(1, 10, 5, t), "case G is perpendicular");

	// Axis-aligned normals — the common wall/floor case — never degenerate.
	scar_tangent(0x10000, 0, 0, t);
	CHECK(perpendicular(0x10000, 0, 0, t), "+X face");
	scar_tangent(0, 0x10000, 0, t);
	CHECK(perpendicular(0, 0x10000, 0, t), "+Y face");
	scar_tangent(0, 0, 0x10000, t);
	CHECK(perpendicular(0, 0, 0x10000, t), "+Z face");
	scar_tangent(-0x10000, 0, 0, t);
	CHECK(perpendicular(-0x10000, 0, 0, t), "-X face");
}

// The basis: unit tangent/bitangent, perpendicular to the normal and to each
// other, right-handed about the normal, spun by the word.
void test_basis() {
	const int32_t n[3] = {0, 0, 0x10000}; // +Z face
	int32_t a[3], b[3];
	scar_basis(n, 0, a, b);
	const auto len2 = [](const int32_t v[3]) {
		return (int64_t(v[0]) * v[0] + int64_t(v[1]) * v[1] + int64_t(v[2]) * v[2]) >> 16;
	};
	CHECK(len2(a) > 0xFF00 && len2(a) < 0x10100, "the tangent is unit length");
	CHECK(len2(b) > 0xFF00 && len2(b) < 0x10100, "the bitangent is unit length");
	CHECK(int64_t(n[0]) * a[0] + int64_t(n[1]) * a[1] + int64_t(n[2]) * a[2] == 0,
			"the tangent lies in the face plane");
	CHECK(int64_t(a[0]) * b[0] + int64_t(a[1]) * b[1] + int64_t(a[2]) * b[2] == 0,
			"the pair is perpendicular");
	// A quarter-turn word (16384): `T' = T c - B s`, `B' = T s + B c` with
	// c = 0, s = 1 maps the tangent onto the NEGATED bitangent and the
	// bitangent onto the tangent [orig: @0x5ccb50..0x5ccc65].
	int32_t a2[3], b2[3];
	scar_basis(n, 16384, a2, b2);
	const auto close = [](int32_t x, int32_t y) { return x - y < 64 && y - x < 64; };
	CHECK(close(a2[0], -b[0]) && close(a2[1], -b[1]) && close(a2[2], -b[2]),
			"a quarter turn rotates the tangent onto the negated bitangent");
	CHECK(close(b2[0], a[0]) && close(b2[1], a[1]) && close(b2[2], a[2]),
			"and the bitangent onto the tangent");
	// THE HANDEDNESS the fix leaves: the bitangent is `n x t` taken BEFORE the
	// flip and is not recomputed, so the pair with the normal is LEFT-handed —
	// n . (a x b) < 0 — for every face and every spin [orig: @0x5CCADA the
	// cross, @0x5CCAE6..0x5CCB37 the tangent-only flip]. This is what makes the
	// quad's coordinate winding face the struck side once retail's Y-negated
	// upload (a reflection into D3D's left-handed frame) is applied, and what
	// the Godot packer's winding fold relies on (simulation_scars.cpp —
	// both of its folds are rotations and keep the order).
	const auto triple = [](const int32_t n_[3], const int32_t a_[3], const int32_t b_[3]) {
		const int64_t cx = (int64_t(a_[1]) * b_[2] - int64_t(a_[2]) * b_[1]) >> 16;
		const int64_t cy = (int64_t(a_[2]) * b_[0] - int64_t(a_[0]) * b_[2]) >> 16;
		const int64_t cz = (int64_t(a_[0]) * b_[1] - int64_t(a_[1]) * b_[0]) >> 16;
		return (int64_t(n_[0]) * cx + int64_t(n_[1]) * cy + int64_t(n_[2]) * cz) >> 16;
	};
	const int32_t faces[5][3] = {
		{0, 0, 0x10000}, {0x10000, 0, 0}, {0, -0x10000, 0},
		{0x9000, 0x7000, 0xA000}, {-0x3000, 0xE000, -0x5000},
	};
	for (const auto &f : faces) {
		int32_t fa[3], fb[3];
		scar_basis(f, 0, fa, fb);
		CHECK(triple(f, fa, fb) < 0, "the (tangent, bitangent, normal) triple is left-handed");
		scar_basis(f, 40000, fa, fb);
		CHECK(triple(f, fa, fb) < 0, "...and stays left-handed under the spin");
	}
}

// THE CACHE: rings lease per owner, wrap at 256, never evict, and clear on
// death; the terrain ring is always there.
void test_cache() {
	ScarCache cache;
	CHECK(cache.leased_count() == 0, "empty at start");
	const EntityHandle h = EntityHandle::make(1, 5);
	ScarRing *ring = cache.ring_for(h, 100);
	CHECK(ring != nullptr && ring->owner == h, "a ring leases to the owner");
	CHECK(cache.ring_for(h, 100) == ring, "the same owner finds its ring");
	CHECK(cache.leased_count() == 1, "one lease");
	// 257 writes wrap the cursor onto slot 0.
	for (int i = 0; i < 257; ++i) {
		ring->slots[ring->cursor].live = true;
		ring->slots[ring->cursor].texture = static_cast<uint8_t>(i % 4);
		ring->advance_cursor();
	}
	CHECK(ring->cursor == 1, "257 writes leave the cursor at 1");
	CHECK(ring->slots[0].texture == 0 && ring->slots[0].live,
			"the 257th impact overwrote slot 0 (texture 256 % 4 == 0)");
	// A reused handle with a new spawn id gets a FRESH ring, not the stale one.
	ScarRing *reused = cache.ring_for(h, 101);
	CHECK(reused == ring, "the same cache entry is re-leased");
	CHECK(reused->cursor == 0 && !reused->slots[0].live, "and it starts empty");
	CHECK(reused->lease == 101, "under the new lease");
	// Fill the cache: the 129th distinct owner gets nothing.
	for (int i = 0; i < kScarCacheEntities - 1; ++i)
		CHECK(cache.ring_for(EntityHandle::make(2, i), 1) != nullptr, "rings lease until full");
	CHECK(cache.leased_count() == kScarCacheEntities, "all 128 leased");
	CHECK(cache.ring_for(EntityHandle::make(3, 1), 1) == nullptr,
			"a miss with no free ring leaves no scar");
	CHECK(cache.ring_for(EntityHandle::make(2, 3), 1) != nullptr,
			"an existing owner still finds its ring when full");
	// The death clear releases the ring.
	cache.clear_entity(EntityHandle::make(2, 3));
	CHECK(cache.find(EntityHandle::make(2, 3)) == nullptr, "cleared owner has no ring");
	CHECK(cache.leased_count() == kScarCacheEntities - 1, "the ring is free again");
	CHECK(cache.ring_for(EntityHandle::make(3, 1), 1) != nullptr, "and a new owner can take it");
	// The shared world ring is the invalid-handle owner.
	CHECK(cache.ring_for(EntityHandle{}, 0) == &cache.world_ring(), "alloc 0 = the shared ring");
	// A shared-ring slot written by an owner is dropped by that owner's clear.
	cache.world_ring().slots[0].live = true;
	cache.world_ring().slots[0].owner = EntityHandle::make(2, 3);
	cache.world_ring().slots[1].live = true;
	cache.world_ring().slots[1].owner = EntityHandle::make(2, 4);
	cache.clear_entity(EntityHandle::make(2, 3));
	CHECK(!cache.world_ring().slots[0].live, "the cleared owner's shared slot is gone");
	CHECK(cache.world_ring().slots[1].live, "another owner's shared slot stays");
	cache.reset();
	CHECK(cache.leased_count() == 0, "reset releases everything");
	CHECK(!cache.world_ring().slots[1].live, "and empties the shared ring");
}

// THE SLOT WRITER through a World: the kind and def gates, the ring selection
// by pool, the PRNG discipline on the SHARED stream (the spin word first, the
// texture word second and only for the scorch), and the slot contents.
void test_add_entry() {
	World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	world.registry.configure_pool(2, 4);
	Entity e;
	e.kind = EntityKind::Item;
	e.item_type = 1; // a vehicle def
	e.position = {0.0f, 0.0f, 0.0f};
	const EntityHandle h = world.registry.spawn(1, e);
	const Entity *target = world.registry.get(h);
	CHECK(target != nullptr, "the target spawned");
	world.env.water_z = 0x10000; // water at 1.0

	ProjectileHit hit;
	hit.hit_class = ProjectileHitClass::DynamicEntity;
	hit.geometry_entity = h;
	hit.position_q16 = FixedVec3{3 * 0x10000, 4 * 0x10000, 5 * 0x10000};
	hit.normal_q16 = FixedVec3{0, 0, 0x10000};
	hit.surface_type = 3;
	hit.section_index = 2;
	hit.face_index = 7;
	hit.bone_index = 2;

	// The ammo kind gates everything: 0 leaves nothing, 2 skips the ring.
	const uint32_t untouched = world.prng16_state;
	CHECK(!scar_add_entry(world, hit, *target, kScarKindNone), "scar_type 0 leaves no mark");
	CHECK(!scar_add_entry(world, hit, *target, kScarKindGlassOnly),
			"scar_type 2 never reaches the ring fallback");
	CHECK(world.prng16_state == untouched, "and neither draws a word");

	// A hit the writer's gates reject leases NO ring — the lookup comes after
	// the face gate, so the 128 entries are not spent on misses.
	hit.face_index = -1;
	CHECK(!scar_add_entry(world, hit, *target, 1), "a faceless hit takes no scar");
	CHECK(world.out.scars.find(h) == nullptr, "and leases no ring");
	hit.face_index = 7;

	// A scorch on a pool-1 vehicle: the entity ring, two draws — the SPIN
	// word first, then the texture word that picks the strip.
	const uint32_t before = world.prng16_state;
	CHECK(scar_add_entry(world, hit, *target, 1), "an ordinary face takes a scar");
	uint16_t expect_texture_word = 0;
	uint32_t expect_state = before;
	{
		World probe;
		probe.prng16_state = before;
		(void)probe.next_prng16();             // the spin
		expect_texture_word = probe.next_prng16(); // the texture roll
		expect_state = probe.prng16_state;
	}
	CHECK(world.prng16_state == expect_state, "the scorch draws exactly two words");
	const ScarRing *ring = world.out.scars.find(h);
	CHECK(ring != nullptr && ring->cursor == 1, "one slot written in the entity ring");
	if (ring != nullptr) {
		const ScarSlot &s = ring->slots[0];
		CHECK(s.live, "the slot is live");
		CHECK(s.pos[0] == 3 * 0x10000 && s.pos[2] == 5 * 0x10000,
				"the hit point (no collision instance: the world frame stands in)");
		CHECK(s.radius_q16 == kScarRadiusNormalQ16, "the scorch radius");
		CHECK(s.texture == kScarNormalTextureFirst + (expect_texture_word % 4),
				"the strip comes from the SECOND draw");
		CHECK(s.bone == 2, "the struck section index");
		CHECK(s.owner == h, "the owner");
		CHECK(!s.building, "a vehicle is not a building");
		CHECK(s.normal[2] == 0x10000, "the normal is stored");
	}

	// The fallback (surface 15): ONE draw (the spin), the single bhole strip,
	// half radius.
	hit.surface_type = 15;
	const uint32_t before2 = world.prng16_state;
	CHECK(scar_add_entry(world, hit, *target, 1), "surface 15 takes the fallback");
	{
		World probe;
		probe.prng16_state = before2;
		probe.next_prng16();
		CHECK(world.prng16_state == probe.prng16_state,
				"the fallback draws exactly one word (the spin)");
	}
	if (ring != nullptr) {
		CHECK(ring->slots[1].texture == kScarGlassFallbackTextureStrip, "bhole1");
		CHECK(ring->slots[1].radius_q16 == kScarRadiusGlassFallbackQ16, "half radius");
	}

	// The gates draw NOTHING.
	const uint32_t before3 = world.prng16_state;
	hit.surface_type = 3;
	hit.material_flags = 0x400u;
	CHECK(!scar_add_entry(world, hit, *target, 1), "face flag 0x400 takes no scar");
	hit.material_flags = 0;
	hit.position_q16.z = 0x8000; // below the water plane
	CHECK(!scar_add_entry(world, hit, *target, 1), "below water takes no scar");
	hit.position_q16.z = 5 * 0x10000;
	hit.face_index = -1; // a sphere hit carries no face
	CHECK(!scar_add_entry(world, hit, *target, 1), "a hit without a face takes no scar");
	hit.face_index = 7;
	CHECK(world.prng16_state == before3, "a gated impact advances no stream word");
	// A husk takes none either.
	Entity *mut = world.registry.get(h);
	mut->engine_flags |= kEntityFlagHusk;
	CHECK(!scar_add_entry(world, hit, *mut, 1), "a husk takes no scar");
	mut->engine_flags &= ~kEntityFlagHusk;
	// The def gate: Flags bit 1 refuses; a non-vehicle def with NoScar refuses.
	mut->engine_flags |= kScarGateEntityFlag;
	CHECK(!scar_add_entry(world, hit, *mut, 1), "entity flag 1 takes no scar");
	mut->engine_flags &= ~kScarGateEntityFlag;
	mut->item_attrib |= kItemAttribNoScar;
	CHECK(scar_add_entry(world, hit, *mut, 1), "a VEHICLE takes a scar even with NoScar");
	mut->item_type = 2;
	CHECK(!scar_add_entry(world, hit, *mut, 1), "a non-vehicle def with NoScar takes none");
	mut->item_attrib &= ~kItemAttribNoScar;
	CHECK(scar_add_entry(world, hit, *mut, 1), "and without NoScar it does");
	mut->item_type = 1;

	// A BUILDING (pool 2) writes the SHARED ring with its owner and the
	// building byte; a PERSON (pool 0) the shared ring too.
	Entity b;
	b.kind = EntityKind::Building;
	b.item_type = 5;
	const EntityHandle bh = world.registry.spawn(2, b);
	hit.geometry_entity = bh;
	CHECK(scar_add_entry(world, hit, *world.registry.get(bh), 1), "a building scars");
	CHECK(world.out.scars.find(bh) == nullptr, "a building leases no entity ring");
	CHECK(world.out.scars.world_ring().cursor == 1, "it wrote the shared ring");
	CHECK(world.out.scars.world_ring().slots[0].owner == bh, "with its owner");
	CHECK(world.out.scars.world_ring().slots[0].building, "and the building byte");
	Entity p;
	p.kind = EntityKind::Organic;
	p.item_type = 3;
	const EntityHandle ph = world.registry.spawn(0, p);
	hit.geometry_entity = ph;
	CHECK(scar_add_entry(world, hit, *world.registry.get(ph), 1), "a person scars");
	CHECK(world.out.scars.world_ring().cursor == 2, "in the shared ring");
	CHECK(!world.out.scars.world_ring().slots[1].building, "not a building");
	// Attrib 0x80 promotes a non-pool-1 owner to an entity ring.
	Entity *person = world.registry.get(ph);
	person->item_attrib |= kItemAttribScarEntityLocal;
	CHECK(scar_add_entry(world, hit, *person, 1), "an attrib-0x80 def scars");
	CHECK(world.out.scars.find(ph) != nullptr, "into its own ring");

	// Death clears the entity's ring AND its shared-ring slots.
	world.out.scars.clear_entity(h);
	CHECK(world.out.scars.find(h) == nullptr, "the ring is gone after the clear");
	world.out.scars.clear_entity(bh);
	CHECK(!world.out.scars.world_ring().slots[0].live, "the building's shared slot is dropped");
	CHECK(world.out.scars.world_ring().slots[1].live, "the person's shared slot stays");
}

} // namespace

// An entity-ring slot is SECTION-LOCAL: a vehicle posed far from the origin
// and turned must store the hit at its model-local point, so the presenter's
// section-node mount puts the quad back on the struck face.
// [orig: Scar_AddEntry @0x5ccc99..0x5ccca5 — Matrix_Transpose3x3WithNegateCol3
//  then Math_TransformPointWithTranslation22 (translate, then rotate)]
void test_entity_ring_section_local() {
	World world;
	world.registry.configure_pool(1, 4);
	Entity e;
	e.kind = EntityKind::Item;
	e.item_type = 1;
	e.position = {245.0f, -361.0f, 27.0f};
	const EntityHandle h = world.registry.spawn(1, e);
	const Entity *target = world.registry.get(h);
	CHECK(target != nullptr, "the posed vehicle spawned");
	if (target == nullptr) return;
	world.env.water_z = 0;

	CollisionModel model;
	CollisionSection section;
	section.authored_bounds = true;
	section.min_x = section.min_y = section.min_z = -0x40000;
	section.max_x = section.max_y = section.max_z = 0x40000;
	section.radius = 0x70000;
	model.sections.push_back(section);
	CollisionWorld collision;
	collision.assign_entity(h, collision.add_model(std::move(model)));
	const int32_t origin[3] = {245 * 0x10000, -361 * 0x10000, 27 * 0x10000};
	const CollisionMatrix pose = collision_matrix_from_heading(0x40000000, origin); // 90 deg
	CHECK(collision.publish_entity_section_matrices(h, {pose}),
			"the section pose publishes");
	collision.build_tick_tables(world);
	world.collision = &collision;

	// A model-local point, posed into the world through the section matrix.
	const int32_t local[3] = {1 * 0x10000, 2 * 0x10000, 3 * 0x10000};
	int32_t world_point[3];
	pose.transform_point(local, world_point);
	ProjectileHit hit;
	hit.hit_class = ProjectileHitClass::DynamicEntity;
	hit.geometry_entity = h;
	hit.position_q16 = FixedVec3{world_point[0], world_point[1], world_point[2]};
	hit.normal_q16 = FixedVec3{0, 0, 0x10000};
	hit.surface_type = 15;
	hit.section_index = 0;
	hit.face_index = 1;
	CHECK(scar_add_entry(world, hit, *target, 1), "the posed vehicle takes a scar");
	const ScarRing *ring = world.out.scars.find(h);
	CHECK(ring != nullptr && ring->slots[0].live, "into its entity ring");
	if (ring == nullptr) return;
	const ScarSlot &slot = ring->slots[0];
	for (int axis = 0; axis < 3; ++axis) {
		const int32_t delta = slot.pos[axis] - local[axis];
		CHECK(delta >= -2 && delta <= 2,
				"the slot holds the section-local hit point, not a rotated world point");
	}
	CHECK(slot.texture == kScarGlassFallbackTextureStrip, "glass takes bhole1");

	// Drawn back through the same live section matrix (the world form a
	// node-less owner — a statically batched prop — draws), the quad is
	// centred on the world hit again [orig: Scar_RenderCache @0x5CDA49 —
	// `bones + bone << 6` applied to the stored section-local position].
	struct Lookup {
		const World *world;
		const CollisionWorld *collision;
	} lookup{&world, &collision};
	opennova::renderer::ScarViewContext ctx;
	ctx.user = &lookup;
	ctx.section_matrix = [](std::uint16_t owner, int section, CollisionMatrix &out,
								 void *user) {
		const auto *l = static_cast<const Lookup *>(user);
		EntityHandle handle;
		handle.packed = owner;
		return l->collision->entity_section_matrix(*l->world, handle, section, out);
	};
	opennova::renderer::ScarDrawList list;
	opennova::renderer::compile_scar_draws(world.out.scars, ctx, list);
	CHECK(list.batches.size() == 1 && list.batches[0].world_resolved,
			"the posed ring's batch resolves its world form");
	if (list.world_vertices.size() != 6) {
		CHECK(false, "one world-space quad");
		return;
	}
	float centre[3] = {0.0f, 0.0f, 0.0f};
	const int order_corners[4] = {0, 1, 4, 2}; // -A-B, +A-B, +A+B, -A+B
	for (int k : order_corners) {
		centre[0] += list.world_vertices[static_cast<size_t>(k)].x / 4.0f;
		centre[1] += list.world_vertices[static_cast<size_t>(k)].y / 4.0f;
		centre[2] += list.world_vertices[static_cast<size_t>(k)].z / 4.0f;
	}
	for (int axis = 0; axis < 3; ++axis) {
		const float expected = static_cast<float>(world_point[axis]) / 65536.0f;
		const float delta = centre[axis] - expected;
		CHECK(delta > -0.001f && delta < 0.001f,
				"the world form puts the quad back on the world hit point");
	}
}

int main() {
	test_scar_selection();
	test_texture_roll_discipline();
	test_ring_wraps();
	test_gates();
	test_spin();
	test_tangent_ladder();
	test_basis();
	test_cache();
	test_add_entry();
	test_entity_ring_section_local();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("impact_scar_test OK\n");
	return 0;
}
