// The tracked contact solve's extend-loop and respawn legs (D-NET-NEW-L6-16), the aircraft and
// tracked crash-state latch (D-NET-NEW-L6-17) and the grounded arm's crashed block
// (D-NET-NEW-L6-19).
// - The extend loop resets every oscillator's remaining range to 0xFFFF, after the pad quad's
//   chassis-force clear. [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - the quad
//   @0x47DAE2 and Entity_ClearSuspensionForces @0x47DAF2; the reset @0x47DB70]
// - A pad pair down on the contact copy (0+2 or 1+3) raises +0x2F1 unless the hull is
//   wreck-settled. [orig: @0x47E15D..0x47E184]
// - The client crash window runs in the grounded arm only. [orig: @0x47E791..0x47E7F5]
// - The crash-state latch reads each solve's own middle spine probe (> 0), and the two families
//   differ in the pair clear, the miss arm, the park terms and the rebuild gate.
//   [orig: tracked @0x47DC56..0x47DD04, @0x47DD30..0x47DDA4, @0x47DF79..0x47DFA2; aircraft
//   @0x4807C6..0x48087C, @0x4808A8..0x480907, @0x480AE0..0x480AFC]
// The hull is item 1294's box (the 01TR truck) on a flat 42.0 field.
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/vehicle_suspension.h>
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

constexpr int32_t kGround = 42 << 16;
// Every pad 4096 into the field: pad 0's bottom (box_z_lo) sits 4096 under the ground.
constexpr int32_t kPadsDown = kGround - 648 - 4096;

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
	return tr;
}

struct Rig {
	std::unique_ptr<World> owner = std::make_unique<World>();
	World &world = *owner;
	std::vector<uint16_t> heights = std::vector<uint16_t>(1024 * 1024, uint16_t(42 * 256));
	std::vector<int> sectors = std::vector<int>(256, 1);
	t::TerrainHeightField terrain{};
	EntityHandle hull{};

	explicit Rig(bool authority) {
		terrain.heightmap = heights.data();
		terrain.dim = 1024;
		terrain.layout.sector_grid = sectors.data();
		terrain.wrap_x = terrain.wrap_z = true;
		world.tables.terrain = &terrain;
		world.ai.terrain = &terrain;
		world.ai.is_authority = authority;
		world.rules.logic_authority = authority;
		world.registry.configure_pool(1, 4);
		Entity h;
		h.kind = EntityKind::Item;
		h.item_id = 1294;
		h.has_item_def = true;
		h.has_graphic_model = true;
		h.alive = true;
		h.health = h.health_max = 1000;
		hull = world.registry.spawn(1, h);
	}

	Entity &e() { return *world.registry.get(hull); }

	void solve(int32_t z, int32_t vz = -400) {
		Entity &v = e();
		v.veh.slide_z = vz; // -400: the mover's dribble, no rest tick
		int32_t px = 100 << 16, py = 200 << 16, pz = z;
		detail::ground_contact_solve(world, v, truck(), v.veh, px, py, px, py, pz);
	}
};

void test_the_extend_loop_resets_every_range() {
	auto rig = std::make_unique<Rig>(true);
	for (int k = 0; k < 4; ++k) rig->e().veh.wheel_osc[k].extension = 5;
	rig->solve(kPadsDown);
	for (int k = 0; k < 4; ++k) CHECK(rig->e().veh.wheel_osc[k].extension == 0xFFFF);
}

// Pads 0..3 down: the pair 0+2 raises +0x2F1. Pads 0 and 1 only (the rear pair lifted clear by
// its 8192 compression): no 0+2 or 1+3 pair, so it stays 0.
void test_a_pad_pair_marks_the_hull_respawned() {
	{
		auto rig = std::make_unique<Rig>(true);
		rig->solve(kPadsDown);
		CHECK(rig->e().veh.fresh_2f1 == 1);
	}
	{
		auto rig = std::make_unique<Rig>(true);
		rig->e().veh.wheel_comp[2] = rig->e().veh.wheel_comp[3] = 8192;
		rig->solve(kPadsDown);
		CHECK((rig->e().flags & kEntityFlagInAir) == 0);
		CHECK(rig->e().veh.fresh_2f1 == 0);
	}
}

// A client hull eight units over the field, flagged airborne, takes the no-pad arm: no crash
// window runs, so no landing stamp is taken.
void test_the_client_window_runs_only_grounded() {
	auto rig = std::make_unique<Rig>(false);
	rig->world.logic_tick = 5;
	rig->e().flags |= kEntityFlagInAir;
	rig->solve(kPadsDown + (8 << 16));
	CHECK((rig->e().flags & kEntityFlagInAir) != 0);
	CHECK(rig->e().veh.airborne_stamp_2f8 == 0);
}

// The crash-state latch, called as each solve calls it (its contact copy, upright).
void test_the_crash_latch_per_family() {
	VehicleTraits tracked = truck();
	VehicleTraits air = truck();
	air.family = VehicleFamily::Helicopter;
	const int32_t up = 0x4000;
	const auto crashed_hull = [](Rig &rig) -> Entity & {
		Entity &v = rig.e();
		v.health = 0; // no wreck-rest burning
		v.veh.crashed = 1;
		v.veh.byte_2ef = 1;
		v.veh.speed = 100;
		v.veh.plat_acc[0] = 300;
		v.veh.plat_at_rest = true;
		return v;
	};
	{
		// The tracked middle spine is probe 5: it latches +0x2FC and keeps the crash pair.
		auto rig = std::make_unique<Rig>(false);
		Entity &v = crashed_hull(*rig);
		const int32_t depths[7] = { 0, 0, 0, 0, 0, 7, 0 };
		detail::vehicle_crash_state(rig->world, v, tracked, depths, up);
		CHECK(v.veh.wreck_2fc == 1);
		CHECK(v.veh.crashed == 1 && v.veh.byte_2ef == 1);
		CHECK(v.veh.plat_acc[0] == 0);
		CHECK(!v.veh.plat_at_rest);
	}
	{
		// Probe 6 is the tracked solve's rear spine: a miss, which clears +0x2FC.
		auto rig = std::make_unique<Rig>(false);
		Entity &v = crashed_hull(*rig);
		v.veh.wreck_2fc = 1;
		const int32_t depths[7] = { 0, 0, 0, 0, 0, 0, 7 };
		detail::vehicle_crash_state(rig->world, v, tracked, depths, up);
		CHECK(v.veh.wreck_2fc == 0);
		CHECK(v.veh.plat_at_rest);
	}
	{
		// The aircraft's middle spine is probe 6: it latches and drops the crash pair.
		auto rig = std::make_unique<Rig>(false);
		Entity &v = crashed_hull(*rig);
		const int32_t depths[7] = { 0, 0, 0, 0, 0, 0, 7 };
		detail::vehicle_crash_state(rig->world, v, air, depths, up);
		CHECK(v.veh.wreck_2fc == 1);
		CHECK(v.veh.crashed == 0 && v.veh.byte_2ef == 0);
	}
	{
		// A negative spine depth is no contact, and the aircraft has no miss arm.
		auto rig = std::make_unique<Rig>(false);
		Entity &v = crashed_hull(*rig);
		v.veh.wreck_2fc = 1;
		const int32_t depths[7] = { 0, 0, 0, 0, 0, 0, -7 };
		detail::vehicle_crash_state(rig->world, v, air, depths, up);
		CHECK(v.veh.wreck_2fc == 1);
		CHECK(v.veh.crashed == 1);
	}
	{
		// The tracked authority leaves the park bit alone on +0x2FC alone (moving: no +0x2F0).
		auto rig = std::make_unique<Rig>(true);
		Entity &v = rig->e();
		v.health = 0;
		v.veh.wreck_2fc = 1;
		v.veh.speed = 100;
		detail::vehicle_crash_state(rig->world, v, tracked, nullptr, up);
		CHECK((v.flags & kEntityFlagSuspensionCrashed) == 0);
	}
	{
		// The aircraft authority parks only while crashed and unparks only once neither crashed
		// nor wrecked: a wreck at rest (+0x2F0) that is not crashed keeps the bit as it stood.
		auto rig = std::make_unique<Rig>(true);
		Entity &v = rig->e();
		v.health = 0;
		v.veh.settle_2f0 = 1;
		detail::vehicle_crash_state(rig->world, v, air, nullptr, up);
		CHECK((v.flags & kEntityFlagSuspensionCrashed) == 0);
	}
}

// D-NET-NEW-L6-19: the grounded arm's crashed block, ahead of the airborne clear. A crashed hull
// still falling past 0x1000 rebounds at |vz >> 1| capped 0x5000 and strikes its pads (the strike
// forces fold into the chassis pose at once, none left queued); slower, it stops and re-enters the
// crash state (+0x2EF), as does a crashed hull already down. The latched hull then settles in the
// fit (crashed, frozen, slow and upright: +0x2F0).
// [orig: @0x47E7F5..0x47E8E0; Entity_ApplyLightVehicleSuspensionForces @0x463750 (the call
//  @0x47E861); Entity_ClearSuspensionForces @0x468980 (the call @0x47E870)]
void test_the_grounded_crashed_block() {
	const auto crashed_hull = [](Rig &rig, bool in_air) -> Entity & {
		Entity &v = rig.e();
		v.health = 0; // no wreck-rest burning
		v.veh.crashed = 1;
		if (in_air)
			v.flags |= kEntityFlagInAir;
		return v;
	};
	{
		auto rig = std::make_unique<Rig>(true);
		Entity &v = crashed_hull(*rig, true);
		rig->solve(kPadsDown, -0x14000);
		CHECK(v.veh.slide_z == 0x5000);
		CHECK(v.veh.crashed == 1 && v.veh.byte_2ef == 0);
		CHECK(v.veh.chassis_active);
		for (const auto &force : v.veh.chassis_forces) CHECK(force.rate == 0);
		CHECK((v.flags & kEntityFlagInAir) == 0);
	}
	{
		auto rig = std::make_unique<Rig>(true);
		Entity &v = crashed_hull(*rig, true);
		rig->solve(kPadsDown, -0x800);
		CHECK(v.veh.slide_z == 0);
		CHECK(v.veh.byte_2ef == 1);
		CHECK(v.veh.settle_2f0 == 1 && v.veh.crashed == 0);
	}
	{
		auto rig = std::make_unique<Rig>(true);
		Entity &v = crashed_hull(*rig, false);
		rig->solve(kPadsDown, -0x14000);
		CHECK(v.veh.byte_2ef == 1);
		CHECK(v.veh.settle_2f0 == 1 && v.veh.crashed == 0);
		CHECK(!v.veh.chassis_active);
	}
}

} // namespace

int main() {
	test_the_extend_loop_resets_every_range();
	test_a_pad_pair_marks_the_hull_respawned();
	test_the_client_window_runs_only_grounded();
	test_the_crash_latch_per_family();
	test_the_grounded_crashed_block();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("tracked_contact_legs: all passed\n");
	return 0;
}
