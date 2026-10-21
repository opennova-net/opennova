// A hull with no graphic model (D-NET-433, the boxless stand-ins): each contact
// solve runs its sleep leg and its wake expiry and then returns, writing none of
// the solve's words, and the movers keep no stand-in for it: the hull moves by its
// own velocity words, gravity included, and its flags stay as they stand.
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 (@0x47C477..0x47C49F ->
//  @0x47C441); Entity_ProcessAircraftContactPhysics @0x47EF10 (@0x47F1B2..0x47F1CF ->
//  @0x47F176); Entity_UpdateWatercraftPhysics @0x48D480 (@0x48EBD9, @0x48ECC6)]
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

VehicleTraits boxed_traits(VehicleFamily family) {
	VehicleTraits t;
	t.physics = 1;
	t.family = family;
	t.box_x_lo = t.foot_x_lo = -2 * 65536;
	t.box_x_hi = t.foot_x_hi = 2 * 65536;
	t.box_y_lo = t.foot_y_lo = -65536;
	t.box_y_hi = t.foot_y_hi = 65536;
	t.box_z_lo = 0;
	t.box_z_hi = 2 * 65536;
	return t;
}

Entity hull(bool modeled) {
	Entity veh;
	veh.kind = EntityKind::Item;
	veh.has_item_def = true;
	veh.has_graphic_model = modeled;
	veh.item_id = 1291;
	veh.item_type_index = 7;
	veh.position.set_q16({100 << 16, 200 << 16, 10 << 16});
	veh.health = 3000;
	veh.health_max = 3000;
	veh.alive = true;
	return veh;
}

// The tracked solve leaves a model-less hull's Z, attitude and flags alone, past
// its wake expiry; its sleep leg still runs.
void test_the_tracked_solve_returns_for_no_model() {
	auto world = std::make_unique<World>();
	world->registry.configure_pool(1, 4);
	Entity veh = hull(false);
	veh.flags = kEntityFlagInAir;
	veh.veh.slide_z = -400;
	veh.veh.air_pitch_bam = 0x01000000;
	const EntityHandle h = world->registry.spawn(1, veh);
	Entity &e = *world->registry.get(h);
	const VehicleTraits t = boxed_traits(VehicleFamily::Ground);
	int32_t px = 101 << 16, py = 200 << 16, pz = 9 << 16;
	detail::ground_contact_solve(*world, e, t, e.veh, 100 << 16, 200 << 16, px, py, pz);
	CHECK(pz == (9 << 16));
	CHECK(px == (101 << 16) && py == (200 << 16));
	CHECK((e.flags & kEntityFlagInAir) != 0);
	CHECK(e.veh.air_pitch_bam == 0x01000000);
	CHECK(e.veh.slide_z == -400);

	// At rest (the sleep gate's words), the sleep leg undoes the dribble first.
	e.flags = 0;
	e.veh.slide_z = -200;
	px = 100 << 16;
	pz = 9 << 16;
	detail::ground_contact_solve(*world, e, t, e.veh, 100 << 16, 200 << 16, px, py, pz);
	CHECK(pz == (9 << 16) + 200);
	CHECK(e.veh.slide_z == -100);
}

// The aircraft solve clears the four drop accumulators and then returns for a
// model-less hull; a model with no box extents runs nothing at all.
void test_the_aircraft_solve_returns_for_no_model() {
	auto world = std::make_unique<World>();
	world->registry.configure_pool(1, 4);
	Entity veh = hull(false);
	veh.flags = kEntityFlagInAir | 0x8000u;
	veh.veh.slide_z = -400;
	veh.veh.plat_acc[0] = 300;
	veh.veh.plat_acc[3] = 700;
	const EntityHandle h = world->registry.spawn(1, veh);
	Entity &e = *world->registry.get(h);
	const VehicleTraits t = boxed_traits(VehicleFamily::Helicopter);
	int32_t px = 101 << 16, py = 200 << 16, pz = 3 << 16;
	detail::aircraft_contact_solve(*world, e, t, e.veh, 100 << 16, 200 << 16, px, py, pz);
	CHECK(pz == (3 << 16));
	CHECK((e.flags & (kEntityFlagInAir | 0x8000u)) == (kEntityFlagInAir | 0x8000u));
	for (int32_t v : e.veh.plat_acc) CHECK(v == 0);

	e.has_graphic_model = true;
	e.veh.plat_acc[1] = 500;
	const VehicleTraits boxless{};
	detail::aircraft_contact_solve(*world, e, boxless, e.veh, 100 << 16, 200 << 16, px, py, pz);
	CHECK(e.veh.plat_acc[1] == 500);
	CHECK(pz == (3 << 16));
}

// The tracked mover keeps no terrain or flat-plane stand-in for a model-less hull
// (the flat plane zeroed a negative slide word): its first gravity step (-324)
// sits in the solve's sleep window, so the sleep leg ahead of the model test
// undoes the dribble and halves the word, and the contact flags stay as they stand.
// [orig: @0x48D009; Entity_ProcessTrackedVehiclePhysics @0x47C1C0 (the sleep leg
//  @0x47C244..0x47C441, the null-model return @0x47C477..0x47C49F)]
void test_a_model_less_tracked_hull_takes_no_stand_in() {
	auto world = std::make_unique<World>();
	world->registry.configure_pool(1, 4);
	const EntityHandle h = world->registry.spawn(1, hull(false));
	Entity &e = *world->registry.get(h);
	const VehicleTraits t = boxed_traits(VehicleFamily::Ground);
	const int32_t z0 = e.position.z.q16;
	// The spawn's contact byte is 0 [orig: Mission_ResetBmsState @0x40DB80 (the
	//  Pool_Clear @0x442060 call @0x40DBC7); Entity_SpawnFromBMSRecord @0x40E9F0
	//  (the memset @0x40EA27)].
	CHECK(!e.veh.grounded);
	world->vehicles.tick_motor(e, t);
	CHECK(e.veh.slide_z == -162);
	CHECK(e.position.z.q16 == z0);
	CHECK((e.flags & (kEntityFlagInAir | 0x8000u)) == 0);
	// The sleep leg, ahead of the model test, re-derives the byte from the upright
	// pose [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 (@0x47C37B..0x47C395)].
	CHECK(e.veh.grounded);
}

// The boat mover reads its afloat latch as the last platform solve left it: a hull
// with no platform geometry keeps a clear latch, takes the -167 step and integrates
// Z, with no water-plane or terrain stand-in.
void test_a_model_less_boat_keeps_its_latch() {
	auto world = std::make_unique<World>();
	world->registry.configure_pool(1, 4);
	world->env.water_z = 20 << 16; // the hull sits under the water plane
	const EntityHandle h = world->registry.spawn(1, hull(false));
	Entity &e = *world->registry.get(h);
	VehicleTraits t;
	t.physics = 1;
	t.family = VehicleFamily::Watercraft;
	const int32_t z0 = e.position.z.q16;
	world->vehicles.tick_watercraft_motor(e, t);
	CHECK(!e.veh.plat_afloat);
	CHECK((e.flags & 0x8000u) == 0);
	CHECK(e.veh.slide_z == -167); // [orig: @0x48EBD9]
	CHECK(e.position.z.q16 == z0 - 167);
}

// The platform solve's null-model branch: retail writes the contact byte (+0x2F2) from an
// uninitialized stack word and, on the authority, Flags 0x10 from +0x2F0; ours leaves both
// as they stand (Q7-12 (a), PERMANENT: a deliberate fix of retail's unchecked read).
// [orig: Entity_ProcessPlatformPhysics @0x481870 (@0x481A85..0x481AC1 -> @0x481A06..0x481A22)]
void test_the_platform_solve_leaves_a_model_less_hull_alone() {
	auto world = std::make_unique<World>();
	world->registry.configure_pool(1, 4);
	world->ai.is_authority = true;
	for (const bool grounded : {true, false}) {
		const EntityHandle h = world->registry.spawn(1, hull(false));
		Entity &e = *world->registry.get(h);
		e.veh.grounded = grounded;
		e.veh.settle_2f0 = 1;
		e.flags = grounded ? 0x10u : 0u;
		VehicleTraits t;
		t.physics = 1;
		t.family = VehicleFamily::Watercraft;
		const int32_t z0 = e.position.z.q16;
		world->vehicles.watercraft_platform_solve(e, t);
		CHECK(e.veh.grounded == grounded);
		CHECK((e.flags & 0x10u) == (grounded ? 0x10u : 0u));
		CHECK(e.position.z.q16 == z0);
	}
}

} // namespace

int main() {
	test_the_tracked_solve_returns_for_no_model();
	test_the_aircraft_solve_returns_for_no_model();
	test_a_model_less_tracked_hull_takes_no_stand_in();
	test_a_model_less_boat_keeps_its_latch();
	test_the_platform_solve_leaves_a_model_less_hull_alone();
	if (failures == 0) std::printf("PASS model_less_hull\n");
	return failures == 0 ? 0 : 1;
}
