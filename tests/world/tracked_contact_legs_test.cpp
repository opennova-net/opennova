// The tracked contact solve's extend-loop and respawn legs (D-NET-NEW-L6-16).
// - The extend loop resets every oscillator's remaining range to 0xFFFF, after the pad quad's
//   chassis-force clear. [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - the quad
//   @0x47DAE2 and Entity_ClearSuspensionForces @0x47DAF2; the reset @0x47DB70]
// - A pad pair down on the contact copy (0+2 or 1+3) raises +0x2F1 unless the hull is
//   wreck-settled. [orig: @0x47E15D..0x47E184]
// - The client crash window runs in the grounded arm only. [orig: @0x47E791..0x47E7F5]
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

	void solve(int32_t z) {
		Entity &v = e();
		v.veh.slide_z = -400; // the mover's dribble: no rest tick
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

} // namespace

int main() {
	test_the_extend_loop_resets_every_range();
	test_a_pad_pair_marks_the_hull_respawned();
	test_the_client_window_runs_only_grounded();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("tracked_contact_legs: all passed\n");
	return 0;
}
