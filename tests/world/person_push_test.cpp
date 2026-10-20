// The movement resolver's person push (D-NET-NEW-L4-2): two overlapping bodies
// push apart by a quarter of the overlap per resolve, along the direction away
// from the peer. The threshold is 30% of the two entity+0 boundRadius words; the
// fpatan is scaled by the NEGATIVE BAM-per-radian double, so the table index
// (0x200000 - ang) >> 22 lands on the away direction on both axes. A joiner's
// remote persons are rows of the same pool-0 table, so its local body pushes
// away from them too; a dead peer pushes nothing.
// [orig: Entity_MovementCollisionResolver @0x4B2BD0 -- v100 @0x4B3B07, the
//  coarse reads @0x4B3B07..0x4B3B74, the dead skip @0x4B3B8D, the live
//  distance @0x4B3BCA, (thr - dist) >> 2 @0x4B3BD0..0x4B3BD6, the fmul by
//  g_NegBamPerRadian (-2^31/pi, loaded @0x4B3AD2) @0x4B3BF6, the index
//  @0x4B3C04..0x4B3C0C, X @0x4B3C31, Y @0x4B3C40;
//  Entity_BuildProximityLists_Pool01 -- entity+0 @0x4B93EB, the pool-0 walk
//  @0x4B93D0..0x4B9419; Math_BuildSinTable @0x613050]
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// A flat terrain at height 0.
struct Field {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	TerrainHeightField field;
	Field() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;
	}
};

struct Rig {
	std::unique_ptr<World> heap = std::make_unique<World>();
	World &world = *heap;
	CollisionWorld cw;
	Field field;
	Rig() {
		world.registry.configure_pool(0, 8);
		cw.terrain = &field.field;
	}
	EntityHandle person(double x, double y, float bound) {
		Entity e;
		e.kind = EntityKind::Organic;
		e.position = {static_cast<float>(x), static_cast<float>(y), 0.0f};
		e.bound_radius = bound; // the model init's entity+0 stamp
		e.item_type_index = 3;
		e.alive = true;
		return world.registry.spawn(0, e);
	}
	void rebuild() {
		for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);
	}
	// One full resolve of `body` at its words; returns the resolved x/y.
	void resolve(EntityHandle body, int32_t out[2]) {
		const Entity *e = world.registry.get(body);
		CollisionWorld::ResolveState state;
		int32_t pos[3] = {e->position.x.q16, e->position.y.q16, e->position.z.q16};
		int32_t vel[2] = {0, 0};
		int32_t vel_z = 0;
		int16_t health = 100;
		cw.resolve_entity(world, body, state, pos, vel, vel_z, 0, fx(1.8), 0, 0, true,
				true, 0, 43, 0u, health);
		out[0] = pos[0];
		out[1] = pos[1];
	}
};

// The push retail's arithmetic gives a body 0.125 u ahead of its peer on both
// axes, both bounds 1.0 u: the threshold 30% of 2.0 u, the planar distance
// truncated, a quarter of the overlap along 45 degrees, whose table entry is
// 128 (away on both axes).
struct Expected {
	int32_t dx = 0, dy = 0;
};
Expected expected_push() {
	const int32_t threshold = 30 * (65536 + 65536) / 100;
	const int32_t dist = static_cast<int32_t>(std::sqrt(2.0 * 8192.0 * 8192.0));
	const int32_t amount = (threshold - dist) >> 2;
	const DirTable &t = dir_table();
	Expected e;
	e.dx = static_cast<int32_t>((static_cast<int64_t>(amount) * t.sin22[128 + 256]) >> 22);
	e.dy = static_cast<int32_t>((static_cast<int64_t>(amount) * t.sin22[128]) >> 22);
	return e;
}

// Two registry bodies (the host's own and a joiner's): the body pushes away on
// both axes, by the expected words.
void test_two_bodies_push_apart_on_both_axes() {
	Rig rig;
	const EntityHandle a = rig.person(10.125, 10.125, 1.0f);
	(void)rig.person(10.0, 10.0, 1.0f);
	rig.rebuild();
	int32_t out[2];
	rig.resolve(a, out);
	const Expected e = expected_push();
	CHECK(e.dx > 0 && e.dy > 0);
	CHECK(out[0] == fx(10.125) + e.dx);
	CHECK(out[1] == fx(10.125) + e.dy);
	// Out of reach (0.6 u apart on one axis): nothing.
	Rig far;
	const EntityHandle b = far.person(10.6, 10.0, 1.0f);
	(void)far.person(10.0, 10.0, 1.0f);
	far.rebuild();
	far.resolve(b, out);
	CHECK(out[0] == fx(10.6) && out[1] == fx(10.0));
}

// A joiner: the remote person is a wire row, not a registry entity; the local
// body pushes away from it as from any pool-0 row, and passes over a dead one.
void test_a_joiners_body_pushes_away_from_a_remote_person() {
	for (const bool dead : {false, true}) {
		Rig rig;
		const EntityHandle local = rig.person(10.125, 10.125, 1.0f);
		rig.world.cached.local_player = local;
		WirePersonCollisionProxy remote;
		remote.wire_handle = 0x0058;
		remote.position_q16 = FixedVec3{fx(10.0), fx(10.0), 0};
		remote.bound_radius_q16 = 65536;
		remote.player = true;
		remote.dead = dead;
		rig.cw.replace_wire_collision_proxies({remote}, {}, /*local_player_wire_handle=*/0x0059);
		rig.rebuild();
		int32_t out[2];
		rig.resolve(local, out);
		const Expected e = expected_push();
		if (dead) {
			CHECK(out[0] == fx(10.125) && out[1] == fx(10.125));
		} else {
			CHECK(out[0] == fx(10.125) + e.dx);
			CHECK(out[1] == fx(10.125) + e.dy);
		}
	}
}

} // namespace

int main() {
	test_two_bodies_push_apart_on_both_axes();
	test_a_joiners_body_pushes_away_from_a_remote_person();
	if (failures == 0) std::printf("person_push: all passed\n");
	return failures == 0 ? 0 : 1;
}
