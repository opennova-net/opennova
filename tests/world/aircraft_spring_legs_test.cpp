// The aircraft contact solve's per-pad spring legs (D-NET-NEW-L6-11): each pad probe rides its
// pad's compression word, the extend loop resets every oscillator's range and grows the sink of a
// pad off the ground (skipped for a clean, unsettled, inverted hull), and the +0x2F2 stability
// byte follows the pad pairs. The hull is item 1300's box (the Chinook) on a flat 42.0 field.
// [orig: Entity_ProcessAircraftContactPhysics @0x47EF10: the compression reads @0x47F514 /
//  @0x47F56A / @0x47F5E3 / @0x47F63F; the stability byte @0x480372..0x480424; the extend loop
//  @0x4806DC..0x480757]
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
// The settle's lift over the ground: pad 0's bottom (box_z_lo + r - r) sits at the ground.
constexpr int32_t kRestZ = kGround + 471295;

VehicleTraits chinook() {
	VehicleTraits tr;
	tr.family = VehicleFamily::Helicopter;
	tr.player_control = true;
	tr.torque = 3;
	tr.mass = 110;
	tr.max_slope = 417566240;
	tr.slip_slope = 357913920;
	tr.box_x_lo = -574629;
	tr.box_x_hi = 783204;
	tr.box_y_lo = -242676;
	tr.box_y_hi = 246045;
	tr.box_z_lo = -471295;
	tr.box_z_hi = 80556;
	tr.foot_x_lo = -537129;
	tr.foot_x_hi = 783204;
	tr.foot_y_lo = -215059;
	tr.foot_y_hi = 220315;
	tr.spring_comp = 20;
	tr.shock = 4;
	tr.flip = 45;
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
		world.registry.configure_pool(1, 4);
		Entity h;
		h.kind = EntityKind::Item;
		h.item_id = 1300;
		h.has_item_def = true;
		h.has_graphic_model = true;
		h.position.set_q16({100 << 16, 200 << 16, kRestZ});
		h.alive = true;
		h.health = h.health_max = 1000;
		hull = world.registry.spawn(1, h);
	}

	Entity &e() { return *world.registry.get(hull); }

	// One solve at Z, the mover's dribble making it no rest tick.
	void solve(const VehicleTraits &tr, int32_t z) {
		Entity &v = e();
		v.veh.slide_z = -400;
		int32_t px = 100 << 16, py = 200 << 16, pz = z;
		detail::aircraft_contact_solve(world, v, tr, v.veh, px, py, px, py, pz);
	}
};

// A pad's compression word lifts its probe: pads 4096 into the ground are in contact and the
// hull is grounded; the same pads compressed by 8192 sit clear of it and the hull is airborne.
void test_a_compressed_pad_lifts_its_probe() {
	const VehicleTraits tr = chinook();
	{
		auto rig = std::make_unique<Rig>();
		rig->solve(tr, kRestZ - 4096);
		CHECK((rig->e().flags & kEntityFlagInAir) == 0);
		CHECK(rig->e().veh.grounded);
	}
	{
		auto rig = std::make_unique<Rig>();
		for (int k = 0; k < 4; ++k) rig->e().veh.wheel_comp[k] = 8192;
		rig->solve(tr, kRestZ - 4096);
		CHECK((rig->e().flags & kEntityFlagInAir) != 0);
		CHECK(!rig->e().veh.grounded);
	}
}

// The extend loop: every oscillator's remaining range resets to 0xFFFF, and an upright hull
// grows each free pad's sink by ftol(0.75 * 250) = 187; a clean, unsettled, inverted hull skips
// the growth.
void test_the_extend_loop_grows_free_pads() {
	const VehicleTraits tr = chinook();
	{
		auto rig = std::make_unique<Rig>();
		for (int k = 0; k < 4; ++k) rig->e().veh.wheel_osc[k].extension = 5;
		rig->solve(tr, kRestZ + (8 << 16));
		const auto &m = rig->e().veh;
		for (int k = 0; k < 4; ++k) {
			CHECK(m.wheel_osc[k].extension == 0xFFFF);
			CHECK(m.plat_acc[k] == 187);
		}
		CHECK((rig->e().flags & kEntityFlagInAir) != 0);
	}
	{
		auto rig = std::make_unique<Rig>();
		rig->e().veh.air_roll_bam = INT32_MIN; // upside down
		rig->solve(tr, kRestZ + (8 << 16));
		const auto &m = rig->e().veh;
		for (int k = 0; k < 4; ++k) {
			CHECK(m.wheel_osc[k].extension == 0xFFFF);
			CHECK(m.plat_acc[k] == 0);
		}
	}
}

// The grounded spring loop resolves the force records' depths, never the contact copy, so a free
// pad whose oscillator steps stays out of contact: its sink keeps this tick's growth (187), where
// fitting the resolved depth (-delta, nonzero) as a contact would clear it. Pads 0 and 1 sit 4096
// into the ground (no diagonal pair, so the pair reset leaves the sinks), pads 2 and 3 are lifted
// clear by their 8192 compression, and pad 2's oscillator has a live amplitude.
// [orig: Entity_ProcessAircraftContactPhysics @0x47EF10 - `sub [esp+edi+..+var_180], eax`
//  @0x4813E5 writes the records; the fit's contact array `lea edx, [esp+..+wheelContactFlags]`
//  @0x481454 and the sink clear from @0x4814C7 read the copy]
void test_a_free_pads_oscillation_stays_out_of_contact() {
	VehicleTraits tr = chinook();
	tr.spring = 6;
	auto rig = std::make_unique<Rig>();
	auto &m = rig->e().veh;
	m.wheel_comp[2] = m.wheel_comp[3] = 8192;
	m.wheel_osc[2].amplitude = 3000;
	rig->solve(tr, kRestZ - 4096);
	CHECK((rig->e().flags & kEntityFlagInAir) == 0);
	CHECK(m.wheel_comp[2] != 8192); // the oscillator stepped: its delta is nonzero
	CHECK(m.plat_acc[0] == 0 && m.plat_acc[1] == 0);
	CHECK(m.plat_acc[2] == 187);
	CHECK(m.plat_acc[3] == 187);
}

} // namespace

int main() {
	test_a_compressed_pad_lifts_its_probe();
	test_the_extend_loop_grows_free_pads();
	test_a_free_pads_oscillation_stays_out_of_contact();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("aircraft_spring_legs: all passed\n");
	return 0;
}
