// The tracked contact solve's +0x2F2 stability byte reads the latch bytes where retail reads
// them, before the crash state (D-NET-NEW-L6-13): a wreck at rest (+0x2F0) whose crash state
// rebuilds it upright this tick still leaves the byte clear. The hull is item 1294's box (the 01TR
// truck) resting on its four pads on a flat 42.0 field, on a client (no parked bit raised).
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0: the byte @0x47D7E4..0x47D8A8, its +0x2F0
//  tests @0x47D831 / @0x47D879; after it, the crash state's wreck rebuild on a client (the parked bit
//  unraised: @0x47DEA4..0x47DEB5, Entity_RebuildOrientationMatrixFromAxes the call @0x47DEC4)]
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

constexpr int32_t kGround = 42 << 16;

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

	Rig() {
		terrain.heightmap = heights.data();
		terrain.dim = 1024;
		terrain.layout.sector_grid = sectors.data();
		terrain.wrap_x = terrain.wrap_z = true;
		world.tables.terrain = &terrain;
		world.ai.terrain = &terrain;
		world.ai.is_authority = false;
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
};

// Pad 0's bottom (box_z_lo) sits 4096 into the field: all four pads are in contact and the
// hull is upright, so only the latch terms decide the byte.
void test_a_resting_wreck_keeps_the_byte_clear() {
	const VehicleTraits tr = truck();
	const int32_t z = kGround - 648 - 4096;
	{
		auto rig = std::make_unique<Rig>();
		Entity &v = rig->e();
		v.veh.slide_z = -400; // the mover's dribble: no rest tick
		int32_t px = 100 << 16, py = 200 << 16, pz = z;
		detail::ground_contact_solve(rig->world, v, tr, v.veh, px, py, px, py, pz);
		CHECK(v.veh.grounded);
	}
	{
		auto rig = std::make_unique<Rig>();
		Entity &v = rig->e();
		v.veh.slide_z = -400;
		v.veh.settle_2f0 = 1; // a wreck at rest; its crash state rebuilds it this tick
		int32_t px = 100 << 16, py = 200 << 16, pz = z;
		detail::ground_contact_solve(rig->world, v, tr, v.veh, px, py, px, py, pz);
		CHECK(v.veh.settle_2f0 == 0);
		CHECK(!v.veh.grounded);
	}
}

} // namespace

int main() {
	test_a_resting_wreck_keeps_the_byte_clear();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("contact_stability_byte: all passed\n");
	return 0;
}
