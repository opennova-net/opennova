// The contact solves' two depth views (D-NET-NEW-L6-11, -15, -21). Retail's second force
// pass writes the probe records in place, so the records hold pass two's depths whatever it found;
// a separate contact copy, taken from pass one, averages with pass two only when pass two
// classified a contact and takes the mass share. The tracked solve hands its second call the hit
// slot, which that call resets; the aircraft hands it none. The tracked spring loop lifts each
// corner by its resolved record; the amphibian water leg raises only the contact copy.
// [orig: Entity_ProcessAircraftContactPhysics @0x47EF10 - the copy @0x47F8AC..0x47F8D6, the second
//  call with a null hit slot @0x47FED0..0x47FF11, the average @0x47FF1F..0x48002B;
//  Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - the copy @0x47CB97..0x47CC05, the second call
//  into the records and the hit slot @0x47D1C7..0x47D209, the water leg's stores into the copy
//  @0x47D485..0x47D512, the lift @0x47EBF7..0x47EC05; Entity_CheckCollisionState @0x462A30 - the
//  slot reset @0x462AA7..0x462AB2]
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;
namespace t = opennova::terrain;

namespace {

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

// Pass two over a world with nothing to hit: the records come back zero, the contact copy keeps
// pass one's depths (no average, no share), the probes move by pass one's summed planar push,
// and the hit slot reads back empty only when pass two was handed it.
void test_the_second_pass_writes_the_records() {
	for (const bool slot : { true, false }) {
		auto owner = std::make_unique<World>();
		World &world = *owner;
		world.registry.configure_pool(1, 4);
		Entity h;
		h.kind = EntityKind::Item;
		h.alive = true;
		Entity &veh = *world.registry.get(world.registry.spawn(1, h));

		int32_t probes[4][3] = { { 0, 0, 0 }, { 65536, 0, 0 }, { 0, 65536, 0 }, { 65536, 65536, 0 } };
		const int32_t radii[4] = { 4096, 4096, 4096, 4096 };
		detail::PlatProbeForce forces[4];
		int32_t d[4];
		for (int i = 0; i < 4; ++i) {
			forces[i].fx = 100 * (i + 1);
			forces[i].fy = -50;
			forces[i].fz = 3000 + i;
			d[i] = forces[i].fz;
		}
		EntityHandle hit = EntityHandle::make(2, 7);
		int32_t px = 0, py = 0;
		detail::plat_second_pass(world, veh, detail::ProbeForcePass::CheckCollisionState, probes,
				radii, 0, 0, forces, d, px, py, 0, hit, slot);
		for (int i = 0; i < 4; ++i) {
			CHECK(forces[i].fz == 0 && forces[i].fx == 0 && forces[i].fy == 0);
			CHECK(d[i] == 3000 + i);
		}
		CHECK(probes[0][0] == 1000 && probes[0][1] == -200);
		CHECK(px == 1000 && py == -200);
		CHECK(hit.valid() == !slot);
	}
}

// The light solve's pass two (D-NET-NEW-L6-21): the push sums probes 0..4 only (the mid-hull
// probe's force moves nothing), the records come back zero, the copy keeps pass one's depths, the
// hit slot is reset, and a wall byte pass one set stays set.
// [orig: Entity_ProcessLightVehiclePhysics @0x479600 - @0x47A20F..0x47A2A8, the second
//  Entity_CheckCollisionState @0x47A2F6; Entity_CheckCollisionState @0x462A30 - @0x46326E]
void test_the_light_second_pass_leaves_the_mid_hull_probe_out() {
	auto owner = std::make_unique<World>();
	World &world = *owner;
	world.registry.configure_pool(1, 4);
	Entity h;
	h.kind = EntityKind::Item;
	h.alive = true;
	Entity &veh = *world.registry.get(world.registry.spawn(1, h));

	int32_t probes[6][3] = {};
	for (int i = 0; i < 6; ++i) probes[i][0] = 65536 * i;
	const int32_t radii[6] = { 4096, 4096, 4096, 4096, 4096, 4096 };
	detail::PlatProbeForce forces[6];
	int32_t d[6];
	for (int i = 0; i < 6; ++i) {
		forces[i].fx = 100 * (i + 1);
		forces[i].fy = -50;
		forces[i].fz = 3000 + i;
		d[i] = forces[i].fz;
	}
	forces[5].fx = 1 << 20; // the mid-hull probe: left out of the push
	forces[5].wall_contact = true;
	EntityHandle hit = EntityHandle::make(2, 7);
	int32_t px = 0, py = 0;
	detail::plat_second_pass(world, veh, detail::ProbeForcePass::CheckCollisionState, probes, radii,
			0, 0, forces, d, px, py, 0, hit, true, 5);
	for (int i = 0; i < 6; ++i) {
		CHECK(forces[i].fz == 0 && forces[i].fx == 0 && forces[i].fy == 0);
		CHECK(d[i] == 3000 + i);
	}
	CHECK(forces[5].wall_contact && !forces[0].wall_contact);
	CHECK(probes[0][0] == 1500 && probes[0][1] == -250);
	CHECK(px == 1500 && py == -250);
	CHECK(!hit.valid());
}

// Item 1294's box (the 01TR truck) as an amphibian, level over a flat 42.0 field, its pads half a
// unit clear of the ground and under a water word one unit above it. The water leg raises every
// pad's contact depth to the water (33416), so the hull takes the grounded arm, but the force
// records stay 0: the corners keep the bare level quad and the solver Z stays where it stood.
// Lifting the corners by the contact depths would raise it 0x2000 (the rise clamp).
VehicleTraits truck() {
	VehicleTraits tr;
	tr.family = VehicleFamily::Ground;
	tr.physics = 1;
	tr.torque = 7;
	tr.mass = 11;
	tr.max_slope = 596523200;
	tr.slip_slope = 417566240;
	tr.box_x_lo = tr.foot_x_lo = -547291;
	tr.box_x_hi = tr.foot_x_hi = 222429;
	tr.box_y_lo = tr.foot_y_lo = -108396;
	tr.box_y_hi = tr.foot_y_hi = 108068;
	tr.box_z_lo = 648;
	tr.box_z_hi = 304748;
	tr.spring = 6;
	tr.spring_comp = 20;
	tr.shock = 6;
	tr.flip = 10;
	tr.amphibian = true;
	return tr;
}

void test_the_tracked_water_leg_raises_only_the_contact_copy() {
	auto owner = std::make_unique<World>();
	World &world = *owner;
	std::vector<uint16_t> heights(1024 * 1024, uint16_t(42 * 256));
	std::vector<int> sectors(256, 1);
	t::TerrainHeightField terrain{};
	terrain.heightmap = heights.data();
	terrain.dim = 1024;
	terrain.layout.sector_grid = sectors.data();
	terrain.wrap_x = terrain.wrap_z = true;
	world.tables.terrain = &terrain;
	world.ai.terrain = &terrain;
	world.ai.is_authority = false;
	world.env.water_z = 43 << 16;
	world.registry.configure_pool(1, 4);
	Entity h;
	h.kind = EntityKind::Item;
	h.item_id = 1294;
	h.has_item_def = true;
	h.has_graphic_model = true;
	h.alive = true;
	h.health = h.health_max = 1000;
	Entity &v = *world.registry.get(world.registry.spawn(1, h));

	const VehicleTraits tr = truck();
	v.veh.slide_z = -400; // the mover's dribble: no rest tick
	const int32_t z = (42 << 16) - 648 + 32768;
	int32_t px = 100 << 16, py = 200 << 16, pz = z;
	detail::ground_contact_solve(world, v, tr, v.veh, px, py, px, py, pz);
	CHECK((v.flags & kEntityFlagInAir) == 0); // the copy's depths took the grounded arm
	CHECK((v.flags & 0x8000u) != 0);
	CHECK(pz == z);
	CHECK(v.veh.air_pitch_bam == 0);
	CHECK(v.veh.air_roll_bam == 0);
}

// D-NET-NEW-L6-22: the tank's in-water flag compares the pads' mean Z plus the hull-bottom word,
// which carries wheel 0's compression (as the tracked solve's does). A level tank high over the
// field with every wheel compressed 0x4000: the mean sits 0x4000 over the origin and the word
// takes it back, so the origin, 0x2000 under the water word, is in the water. Without the
// compression the mean would sit 0x2000 over the water: dry.
// [orig: Entity_ProcessWheeledVehiclePhysics @0x475DE0 - @0x4764AF..0x4764CC, @0x4774B0..0x4774D0]
void test_the_tank_hull_bottom_carries_wheel_0s_compression() {
	auto owner = std::make_unique<World>();
	World &world = *owner;
	std::vector<uint16_t> heights(1024 * 1024, uint16_t(42 * 256));
	std::vector<int> sectors(256, 1);
	t::TerrainHeightField terrain{};
	terrain.heightmap = heights.data();
	terrain.dim = 1024;
	terrain.layout.sector_grid = sectors.data();
	terrain.wrap_x = terrain.wrap_z = true;
	world.tables.terrain = &terrain;
	world.ai.terrain = &terrain;
	world.registry.configure_pool(1, 4);
	Entity h;
	h.kind = EntityKind::Item;
	h.item_id = 1294;
	h.has_item_def = true;
	h.has_graphic_model = true;
	h.alive = true;
	h.health = h.health_max = 1000;
	Entity &v = *world.registry.get(world.registry.spawn(1, h));

	VehicleTraits tr = truck();
	tr.family = VehicleFamily::Tank;
	tr.amphibian = false;
	for (int k = 0; k < 4; ++k) v.veh.wheel_comp[k] = 0x4000;
	v.veh.slide_z = -400; // the mover's dribble: no rest tick
	const int32_t z = (62 << 16);
	world.env.water_z = z + 0x2000;
	int32_t px = 100 << 16, py = 200 << 16, pz = z;
	detail::wheeled_contact_solve(world, v, tr, v.veh, px, py, px, py, pz);
	CHECK((v.flags & 0x8000u) != 0);
}

// D-NET-NEW-L6-23: the bike's only live wall leg. A wall byte from either pass halves a speed whose
// |velocity| passes 17580; nothing else of the head-on block runs (its wall bytes are never
// written in retail), so no velocity, however fast, touches anything but the speed.
// [orig: Entity_ProcessLightVehiclePhysics @0x479600 - @0x47AE05..0x47AE59, @0x47B19A..0x47B1A9]
void test_the_bike_wall_leg_only_halves_the_speed() {
	Entity::VehicleMotorState m;
	detail::PlatProbeForce forces[6];
	m.speed = 0x10000;
	m.vel_x = 30000; // head-on past every retail tier
	detail::light_wall_speed_halving(m, forces);
	CHECK(m.speed == 0x10000); // no wall byte
	forces[5].wall_contact = true;
	detail::light_wall_speed_halving(m, forces);
	CHECK(m.speed == 0x8000);
	m.vel_x = 17580;
	detail::light_wall_speed_halving(m, forces);
	CHECK(m.speed == 0x8000); // not past the threshold
	m.vel_x = 12000;
	m.vel_y = 12000;
	m.slide_z = 4000; // |v| = 17435.6: under
	detail::light_wall_speed_halving(m, forces);
	CHECK(m.speed == 0x8000);
	m.slide_z = 4500; // |v| = 17557.1: under
	detail::light_wall_speed_halving(m, forces);
	CHECK(m.speed == 0x8000);
	m.slide_z = 5000; // |v| = 17691.8: past
	detail::light_wall_speed_halving(m, forces);
	CHECK(m.speed == 0x4000);
}

} // namespace

int main() {
	test_the_second_pass_writes_the_records();
	test_the_light_second_pass_leaves_the_mid_hull_probe_out();
	test_the_tracked_water_leg_raises_only_the_contact_copy();
	test_the_tank_hull_bottom_carries_wheel_0s_compression();
	test_the_bike_wall_leg_only_halves_the_speed();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("contact_depth_views: all passed\n");
	return 0;
}
