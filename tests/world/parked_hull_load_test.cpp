// A parked hull and its addeweap children at the mission start, as a join streams them.
//
// D-NET-515 (the AI trailer): the 0x0D trailer streams the AI slot's +0x10 / +0x14, which
// the model init stamps from the entity's position before the vehicle settle
// [orig: Entity_InitFromModel @0x40E113..0x40E127; Game_StartMission - the model init
//  @0x52567F ahead of the settle @0x525F80..0x526071; NetPacket_SerializeEntityPoolToPacket_0
//  @0x503D59 / @0x503D74]. The settle's mover tick may move the hull; the trailer keeps the
// record's words. Oracle: the TDH_I3A Zodiac (slot 0x1004) of the four-topology parity
// suite parity-20261011, whose BMS record words 0xFEE7056C / 0xFFDD4B09 are what retail's
// trailer streams although the hull's join pose is (-281.1, -35.0); ours streamed the
// post-settle words.
//
// D-NET-514 (the contact wake): a hull's contact pass wakes a candidate (Flags 0x40) when
// the mass source is PlayerControl and the hull's def mass is more than twice the
// CANDIDATE's own def mass (+0x908, the allocator's 5 when the def authors none), or its
// bound radius more than twice the candidate's [orig: Entity_CheckCollisionState
// @0x462E91..0x462EA4, @0x462ED6..0x462EE3, the ORs @0x462EBE / @0x462ECD]. An addeweap
// gun (items.def: no mass) keeps the 5, so a Dune Buggy (mass 3) never wakes its
// neighbour's gun and a Mark V (mass 114) does: retail's TDH_I3A Mark V pair streams its
// children awake, its 01TR / ASH_I5A buggy pairs streams theirs asleep.

#include <runtime/mission/promote.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <formats/mission/bms.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

namespace w = opennova::world;
namespace ns = opennova::replication;
namespace bms = opennova::bms;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// The TDH_I3A Zodiac's record words (retail's trailer) and its join pose.
constexpr int32_t kZodiacX = static_cast<int32_t>(0xFEE7056Cu);
constexpr int32_t kZodiacY = static_cast<int32_t>(0xFFDD4B09u);
constexpr int32_t kZodiacZ = fx(13.490646);

void test_the_settle_keeps_the_trailer_words() {
	bms::File m{};
	bms::Entity rec{};
	rec.type = bms::ItemType::Item;
	rec.type_id = 1293; // Drivable Zodiac
	rec.x = kZodiacX;
	rec.y = kZodiacY;
	rec.z = kZodiacZ;
	rec.id = 21;
	m.items.push_back(rec);
	auto world = std::make_unique<w::World>();
	opennova::mission::promote_mission(m, *world, {});
	const w::EntityHandle h = world->registry.find_by_net_id(21);
	CHECK(h.valid());
	if (!h.valid()) return;
	w::Entity &e = *world->registry.get(h);
	e.item_type_index = 1;
	e.has_item_def = true;
	e.is_ai_capable = true;
	// The settle's mover tick moves the hull (its join pose), then the settle
	// saves the spawn pose.
	e.position.x.q16 = fx(-281.1);
	e.position.y.q16 = fx(-35.0);
	world->vehicles.capture_spawn_pose(e);
	CHECK(e.veh.spawn_pose_valid && e.veh.spawn_pose[0] == fx(-281.1) &&
	      e.veh.spawn_pose[1] == fx(-35.0));
	const opennova::PoolSpawnBatch batch = ns::build_pool1_spawn_batch(*world);
	CHECK(batch.records.size() == 1);
	if (batch.records.size() != 1) return;
	CHECK(batch.records[0].has_ai_trailer);
	CHECK(batch.records[0].ai_profile_1 == kZodiacX);
	CHECK(batch.records[0].ai_profile_2 == kZodiacY);
	// The record's position is the live one.
	CHECK(batch.records[0].pos_x == fx(-281.1) && batch.records[0].pos_y == fx(-35.0));
}

struct ContactRig {
	w::World world;
	w::CollisionWorld collision;

	ContactRig() {
		world.registry.configure_pool(1, 8);
		world.collision = &collision;
	}

	w::EntityHandle hull(float x, int32_t item_id, int32_t mass, float radius) {
		w::Entity seed;
		seed.kind = w::EntityKind::Item;
		seed.item_type_index = 7;
		seed.item_id = item_id;
		seed.has_item_def = true;
		seed.has_graphic_model = true;
		seed.item_type = 1;
		seed.item_attrib = 0x40u; // PlayerControl
		seed.def_mass = mass;
		seed.position = {x, 10.0f, 0.0f};
		seed.bound_radius = radius;
		seed.alive = true;
		const w::EntityHandle h = world.registry.spawn(1, seed);
		w::VehicleTraits traits;
		traits.physics = 1;
		traits.mass = mass;
		world.vehicles.traits.set(item_id, traits);
		return h;
	}

	// An addeweap gun riding `carrier`: EWeap, no mass authored (the allocator's 5).
	w::EntityHandle gun(float x, w::EntityHandle carrier, float radius) {
		w::Entity seed;
		seed.kind = w::EntityKind::Item;
		seed.item_type_index = 9;
		seed.item_id = 1419; // NON-Armored Emplaced 50cal for FAV
		seed.has_item_def = true;
		seed.has_graphic_model = true;
		seed.item_type = 0;
		seed.item_attrib = 0x20u; // EWeap
		seed.def_mass = 5;
		seed.position = {x, 10.0f, 1.0f};
		seed.bound_radius = radius;
		seed.ground_target = carrier;
		seed.alive = true;
		return world.registry.spawn(1, seed);
	}

	void contact(w::EntityHandle source) {
		for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);
		w::Entity &v = *world.registry.get(source);
		const int32_t pos[3] = {fx(v.position.x), fx(v.position.y), fx(v.position.z)};
		for (int i = 0; i < 3; ++i) v.saved_live_pos[i] = pos[i];
		v.saved_live_valid = true;
		const int32_t probes[1][3] = {{pos[0], pos[1], pos[2] + fx(1.5)}};
		const int32_t radii[1] = {fx(1.5)};
		w::VehicleProbeForce forces[1];
		w::EntityHandle hit;
		collision.resolve_vehicle_probes(world, v, pos, probes, radii, 1, 3000000, 4000000,
		                                 forces, hit);
	}
};

void test_a_light_hull_leaves_a_neighbours_gun_asleep() {
	ContactRig rig;
	// Two Dune Buggies (mass 3) 7 units apart, each with its gun.
	const w::EntityHandle a = rig.hull(10.0f, 1291, 3, 2.0f);
	const w::EntityHandle b = rig.hull(17.0f, 1291, 3, 2.0f);
	const w::EntityHandle gun_b = rig.gun(17.0f, b, 1.5f);
	CHECK(a.valid() && b.valid() && gun_b.valid());
	rig.contact(a);
	CHECK(rig.collision.candidate_count(a) >= 2);
	// 3 > 2 * 5 fails and 2.0 > 2 * 1.5 fails: the gun stays asleep.
	CHECK((rig.world.registry.get(gun_b)->flags & 0x40u) == 0);
}

void test_a_heavy_hull_wakes_a_neighbours_gun() {
	ContactRig rig;
	// Two Mark Vs (mass 114) side by side, each with its gun.
	const w::EntityHandle a = rig.hull(10.0f, 1320, 114, 2.0f);
	const w::EntityHandle b = rig.hull(17.0f, 1320, 114, 2.0f);
	const w::EntityHandle gun_b = rig.gun(17.0f, b, 1.5f);
	CHECK(a.valid() && b.valid() && gun_b.valid());
	rig.contact(a);
	// 114 > 2 * 5: the gun wakes; the neighbour hull (114 against 2 * 114, 2.0 against
	// 2 * 2.0) does not.
	CHECK((rig.world.registry.get(gun_b)->flags & 0x40u) != 0);
	CHECK((rig.world.registry.get(b)->flags & 0x40u) == 0);
}

} // namespace

int main() {
	test_the_settle_keeps_the_trailer_words();
	test_a_light_hull_leaves_a_neighbours_gun_asleep();
	test_a_heavy_hull_wakes_a_neighbours_gun();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS parked_hull_load\n");
	return 0;
}
