// The class-C settle against a placed static (D-NET-NEW-L6-10, D-NET-515): a parked
// helicopter placed overlapping a small static is pushed clear of it by its contact solve
// over the mission start's settle and the first logic ticks. Retail's per-tick words are
// the oracle.
//
// The vectors are lifted from the 01TR co-op host (jox01). Hull 0x1010 is item 1300, a
// Chinook. Its traits words and placement words are below. The static is pool-2 slot 0x23cf,
// item 2281: one type-1 box volume with six axis planes. Its placement is pure yaw
// (heading 23855104, mission yaw 88), off the direction table's 2^22 BAM grid. The terrain
// under both is flat at 42.0. Retail placed every collision shell through the exact Euler
// builder [orig: BoneCallback_Simple @0x4E2600 -> Math_BuildFixedPointMatrixFromEulerAngles
// @0x613F40; Entity_InitFromModel @0x40DC30 (the build @0x40E0CC) builds it once and sets
// Flags 0x20000 @0x40E105], which is what pushes the hull to these words. With the old table path, ours
// pushed it 3.4% too far.
//
// The oracle is retail's pool-1 words for 0x1010: LP-s7's class-C read of the retail 01TR
// host, two launches identical. tN is the hull after the settle and N logic ticks.
// Rest from t14 on.
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace w = opennova::world;
namespace t = opennova::terrain;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

// Retail's words for 0x1010: the placement, then tN after the settle and N ticks.
struct Words {
	int tick;
	int32_t x, y, z;
};
constexpr Words kRetail[] = {
	{1, -52527368, 50639005, 3223807},
	{3, -52526176, 50641613, 3223807},
	{6, -52525200, 50643750, 3223807},
	{8, -52524853, 50644509, 3223807},
	{9, -52524734, 50644769, 3223807},
	{11, -52524569, 50645131, 3223807},
	{14, -52524504, 50645273, 3223807},
	{16, -52524504, 50645273, 3223807},
	{20, -52524504, 50645273, 3223807},
};
constexpr int32_t kHullPlacement[3] = {-52529320, 50634732, 3218688};
constexpr int32_t kHullHeading = -501022720;

// Item 2281's collision model as loaded: one section, one type-1 volume, six planes
// (Q14 normal, 16.16 distance).
w::CollisionModel static_2281() {
	w::CollisionModel m;
	const int16_t n[6][3] = {
		{16384, 0, 0}, {-16384, 0, 0}, {0, 16384, 0}, {0, -16384, 0}, {0, 0, 16384}, {0, 0, -16384}};
	const int32_t dist[6] = {-18756, -14968, -18317, -18631, -170832, -105303};
	for (int i = 0; i < 6; ++i) {
		w::CollisionPlane p;
		p.nx = n[i][0];
		p.ny = n[i][1];
		p.nz = n[i][2];
		p.dist = dist[i];
		m.planes.push_back(p);
	}
	w::CollisionVolume v;
	v.type = 1;
	v.min_x = -14968;
	v.max_x = 18756;
	v.min_y = -18631;
	v.max_y = 18317;
	v.min_z = -105303;
	v.max_z = 170832;
	v.plane_start = 0;
	v.plane_count = 6;
	m.volumes.push_back(v);
	w::CollisionSection s;
	s.volume_start = 0;
	s.volume_count = 1;
	s.min_x = -14968;
	s.max_x = 21508;
	s.min_y = -18631;
	s.max_y = 18317;
	s.min_z = -113305;
	s.max_z = 170832;
	s.authored_bounds = true;
	m.sections.push_back(s);
	return m;
}

// Item 1300's traits words.
w::VehicleTraits chinook() {
	w::VehicleTraits tr;
	tr.family = w::VehicleFamily::Helicopter;
	tr.player_control = true;
	tr.physics = 0;
	tr.torque = 3;
	tr.mass = 110;
	tr.max_slope = 417566240;
	tr.slip_slope = 357913920;
	tr.climb_speed = 7325;
	tr.unit_type = 3;
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
	tr.spring = 0;
	tr.spring_comp = 20;
	tr.shock = 4;
	tr.flip = 45;
	return tr;
}

struct Rig {
	std::unique_ptr<w::World> owner = std::make_unique<w::World>();
	std::unique_ptr<w::CollisionWorld> collision = std::make_unique<w::CollisionWorld>();
	w::World &world = *owner;
	// A flat 42.0 atlas: every texel's raw word 42 * 256, every sector mapped.
	std::vector<uint16_t> heights = std::vector<uint16_t>(1024 * 1024, uint16_t(42 * 256));
	std::vector<int> sectors = std::vector<int>(256, 1);
	t::TerrainHeightField terrain{};
	w::EntityHandle hull{}, block{};

	Rig() {
		terrain.heightmap = heights.data();
		terrain.dim = 1024;
		terrain.layout.sector_grid = sectors.data();
		terrain.wrap_x = terrain.wrap_z = true;
		// The kernel's terrain wiring: the tables, the AI field and the collision
		// world's raycasts (the settle's ground ray) share the one field.
		world.tables.terrain = &terrain;
		world.ai.terrain = &terrain;
		collision->terrain = &terrain;
		world.collision = collision.get();
		world.ai.collision = collision.get();
		world.ai.is_authority = true;
		world.registry.configure_pool(1, 4);
		world.registry.configure_pool(2, 4);

		w::Entity s;
		s.kind = w::EntityKind::Building;
		s.item_id = 2281;
		s.has_item_def = true;
		s.item_type_index = 1; // a loaded row's def ordinal (+0x1C), the static prox list's gate
		s.position.set_q16({-52391220, 50084402, 2857984});
		s.heading = 23855104;
		s.bound_radius.q16 = 171338;
		s.def_mass = 5;
		s.alive = true;
		block = world.registry.spawn(2, s);
		collision->assign_entity(block, collision->add_model(static_2281()));

		w::Entity h;
		h.kind = w::EntityKind::Item;
		h.item_id = 1300;
		h.item_type = 1;
		h.item_type_index = 7;
		h.has_item_def = true;
		h.has_graphic_model = true;
		h.item_attrib = 0x14100440u; // PlayerControl among them
		h.def_mass = 110;
		h.bound_radius.q16 = 1398635;
		h.position.set_q16({kHullPlacement[0], kHullPlacement[1], kHullPlacement[2]});
		h.heading = kHullHeading;
		h.alive = true;
		h.health = h.health_max = 1000;
		// The air probe's Z offset, the settle's lift over the ground (-box_z_lo).
		h.veh.air_probe_z_off = 471295;
		hull = world.registry.spawn(1, h);
		world.vehicles.traits.set(1300, chinook());
		world.ai.attach(hull);
	}

	const w::Entity &e() const { return *world.registry.get(hull); }

	void tick() {
		++world.logic_tick;
		collision->build_tick_tables(world);
		world.vehicles.update_motor(*world.registry.get(hull), /*is_authority=*/true);
	}
};

void test_the_settle_pushes_the_hull_off_the_static_as_retail_does() {
	auto rig = std::make_unique<Rig>();
	// The mission start's proximity build, then the settle's mover call.
	for (int i = 0; i < 17; ++i) rig->collision->build_tick_tables(rig->world);
	rig->world.vehicles.initialize_mission_vehicles();
	int done = 0;
	for (const Words &want : kRetail) {
		while (done < want.tick) {
			rig->tick();
			++done;
		}
		const w::Entity &e = rig->e();
		std::printf("t%-2d ours %d,%d,%d retail %d,%d,%d\n", want.tick, e.position.x.q16,
				e.position.y.q16, e.position.z.q16, want.x, want.y, want.z);
		CHECK(e.position.x.q16 == want.x);
		CHECK(e.position.y.q16 == want.y);
		CHECK(e.position.z.q16 == want.z);
		CHECK(e.veh.yaw_bam == kHullHeading);
	}
}

} // namespace

int main() {
	test_the_settle_pushes_the_hull_off_the_static_as_retail_does();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("settle_static_contact: all passed\n");
	return 0;
}
