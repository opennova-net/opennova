// D-NET-161 (b): the mission start's deck pass over the player spawn markers. Retail walks
// pool 3 for the markers whose items.def row is one of the types 6001..6004 / 6094..6097
// (ItemList_FindIndexByTypeId answering row 0 for a type no row carries), stores the entity a
// ray from 1 u over the marker to 7 u under it finds as the marker's ground link (null on a
// miss), and moves a marker that found one into that entity's frame in place: its position
// and yaw carrier-local, the words the S2C 0x20 streams.
// [orig: Game_StartMission @0x524360 (@0x525E6D..0x525F58); Entity_FindGroundObjectByRay @0x414380;
//  Entity_FindNearestByRay @0x413AF0; Entity_TransformWorldToLocal @0x43BB50]

#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

// A deck: a box 4 u by 50 u, 5 u tall, its floor at the entity's Z.
w::CollisionModel deck_box() {
	w::CollisionModel box;
	auto plane = [&](int nx, int ny, int nz, double d) {
		w::CollisionPlane p;
		p.nx = static_cast<int16_t>(nx);
		p.ny = static_cast<int16_t>(ny);
		p.nz = static_cast<int16_t>(nz);
		p.dist = static_cast<int32_t>(d * 65536.0);
		box.planes.push_back(p);
	};
	plane(16384, 0, 0, -2.0);
	plane(-16384, 0, 0, -2.0);
	plane(0, 16384, 0, -25.0);
	plane(0, -16384, 0, -25.0);
	plane(0, 0, 16384, -5.0);
	plane(0, 0, -16384, 0.0);
	w::CollisionVolume volume;
	volume.type = 1;
	volume.min_x = -(2 << 16);
	volume.max_x = 2 << 16;
	volume.min_y = -(25 << 16);
	volume.max_y = 25 << 16;
	volume.min_z = 0;
	volume.max_z = 5 << 16;
	volume.plane_start = 0;
	volume.plane_count = 6;
	box.volumes.push_back(volume);
	w::CollisionSection section;
	section.volume_start = 0;
	section.volume_count = 1;
	section.min_x = volume.min_x;
	section.max_x = volume.max_x;
	section.min_y = volume.min_y;
	section.max_y = volume.max_y;
	section.min_z = volume.min_z;
	section.max_z = volume.max_z;
	section.center[2] = static_cast<int32_t>(2.5 * 65536.0);
	section.radius = 26 << 16;
	box.sections.push_back(section);
	return box;
}

w::EntityHandle spawn_marker(w::World &world, int32_t row, int32_t x, int32_t y, int32_t z,
                             int32_t heading) {
	w::Entity marker;
	marker.kind = w::EntityKind::Item;
	marker.item_type_index = row;
	marker.has_item_def = true;
	marker.position.set_q16({x, y, z});
	marker.heading = heading;
	return world.registry.spawn(3, marker);
}

// A marker standing on a deck yawed a quarter turn takes the deck as its ground link and its
// words in the deck's frame: (dx, dy, dz) turned by -90 degrees is (dy, -dx, dz), the yaw
// less the deck's. A marker of another type, and one the ray misses, keep their world words;
// a marker on row 0 is a deck marker too while a deck type has no row (the find's 0).
void test_a_deck_marker_moves_into_its_carrier_frame() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(1, 4);
	world->registry.configure_pool(3, 8);
	// The items.def rows: row 1 is type 6003, row 2 type 6005 (a flag, not a deck type);
	// 6094..6097 and the rest have no row, so they answer row 0.
	world->tables.item_rows.resize(3);
	world->tables.item_rows[0].type_id = 1;
	world->tables.item_rows[1].type_id = 6003;
	world->tables.item_rows[2].type_id = 6005;

	w::CollisionWorld collision;
	w::Entity deck;
	deck.kind = w::EntityKind::Item;
	deck.item_type_index = 7;
	deck.item_id = 500;
	deck.has_item_def = true;
	deck.has_graphic_model = true;
	deck.item_type = 1;
	deck.position.set_q16({100 << 16, 200 << 16, 5 << 16});
	deck.bound_radius = 30.0f;
	deck.alive = true;
	deck.heading = 0x40000000;
	deck.veh.yaw_seeded = true;
	deck.veh.yaw_bam = 0x40000000;
	const w::EntityHandle deck_h = world->registry.spawn(1, deck);
	collision.assign_entity(deck_h, collision.add_model(deck_box()));
	world->ai.collision = &collision;

	// One unit off the deck's centre along X, half a unit over its top (10 u).
	const int32_t mx = 101 << 16, my = 200 << 16, mz = (10 << 16) + 0x8000;
	const w::EntityHandle on_deck = spawn_marker(*world, 1, mx, my, mz, 0x50000000);
	const w::EntityHandle other_type = spawn_marker(*world, 2, mx, my, mz, 0x50000000);
	const w::EntityHandle row_zero = spawn_marker(*world, 0, mx, my, mz, 0x50000000);
	const w::EntityHandle off_deck = spawn_marker(*world, 1, 160 << 16, 200 << 16, mz, 0);
	for (int i = 0; i < 17; ++i) collision.build_tick_tables(*world);

	world->vehicles.localize_deck_spawn_markers();

	const w::Entity &a = *world->registry.get(on_deck);
	CHECK(a.ground_target == deck_h);
	// The turn reads the deck's yaw through the x87 radian constant, which leaves
	// cos(90 deg) at -201 / 2^22, so the one unit along X lands at -4, not 0.
	CHECK(a.position.x.q16 == -4);
	CHECK(a.position.y.q16 == -(1 << 16));
	CHECK(a.position.z.q16 == (5 << 16) + 0x8000);
	CHECK(a.heading == 0x10000000);
	const w::Entity &b = *world->registry.get(other_type);
	CHECK(!b.ground_target.valid());
	CHECK(b.position.x.q16 == mx && b.position.y.q16 == my && b.position.z.q16 == mz);
	CHECK(b.heading == 0x50000000);
	const w::Entity &c = *world->registry.get(row_zero);
	CHECK(c.ground_target == deck_h);
	CHECK(c.position.x.q16 == -4 && c.position.y.q16 == -(1 << 16));
	const w::Entity &d = *world->registry.get(off_deck);
	CHECK(!d.ground_target.valid());
	CHECK(d.position.x.q16 == (160 << 16) && d.heading == 0);
}

} // namespace

int main() {
	test_a_deck_marker_moves_into_its_carrier_frame();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("deck_spawn_markers: ok\n");
	return 0;
}
