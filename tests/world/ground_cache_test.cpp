// D-NET-NEW-L6-38 (D-NET-515): the gnrc / gnrl / gnl2 rows' class init caches the entity under
// them as their ground link (+0x28): the nearest pool-2, then pool-1 entity whose CFAC faces a
// 10-unit drop from the entity's position crosses. Retail's 03TR barrels sit on the truck that way,
// so the truck's contact solve skips them as its own riders.
// [orig: Entity_CreateBoundingProxy @0x407D60; Physics_RaycastNearestEntity @0x5394B0;
//  Physics_RaycastEntityPool @0x4143E0; Entity_InitAllFromModels @0x40E460; the rider skip in
//  Entity_CheckCollisionState @0x462A30 - @0x462E26..0x462E4F]
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

constexpr int32_t kOne = 65536;

// A 2 x 2 u quad (two triangles) at section-local z = `z` (1 by default), normal +z, projected
// on XY; `shifted` moves it half a unit along +x, so the local origin falls inside a triangle.
CollisionModel quad_model(
		uint8_t material = 5, uint32_t flags = 0, int z = 1, bool shifted = false) {
	CollisionModel m;
	const int16_t q8 = static_cast<int16_t>(z * 256);
	const int16_t x0 = shifted ? -128 : -256, x1 = shifted ? 384 : 256;
	m.face_vertices = { { x0, -256, q8 }, { x1, -256, q8 }, { x1, 256, q8 }, { x0, 256, q8 } };
	const auto face = [&](int a, int b, int c) {
		CollisionFace f;
		f.v[0] = static_cast<int16_t>(a);
		f.v[1] = static_cast<int16_t>(b);
		f.v[2] = static_cast<int16_t>(c);
		f.normal[2] = 16384;
		f.axis = 1;
		f.plane_dist = -z * kOne;
		f.min[0] = x0 * 256;
		f.max[0] = x1 * 256;
		f.min[1] = -kOne;
		f.max[1] = kOne;
		f.min[2] = f.max[2] = z * kOne;
		f.flags = flags;
		f.material = material;
		m.faces.push_back(f);
	};
	face(0, 1, 2);
	face(0, 2, 3);
	m.sections.assign(1, {});
	m.sections[0].face_count = 2;
	m.sections[0].face_vertex_count = 4;
	return m;
}

struct Rig {
	std::unique_ptr<World> owner = std::make_unique<World>();
	World &world = *owner;
	CollisionWorld collision;
	Rig() {
		world.registry.configure_pool(1, 4);
		world.registry.configure_pool(2, 4);
		world.collision = &collision;
	}
	// The hull sits at (10, 20); every rider a quarter / half unit off it, so each drop meets
	// the deck's upper-left triangle inside, off the shared diagonal.
	EntityHandle spawn(int pool, int32_t item_id, float z, const CollisionModel *model,
			bool rider = false) {
		Entity e;
		e.kind = EntityKind::Item;
		e.item_id = item_id;
		e.item_type_index = 3;
		e.has_item_def = true;
		e.alive = true;
		e.bound_radius = 3.0f;
		e.position = { rider ? 10.25f : 10.0f, rider ? 20.5f : 20.0f, z };
		const EntityHandle h = world.registry.spawn(pool, e);
		if (model != nullptr)
			collision.assign_entity(h, collision.add_model(*model));
		return h;
	}
	Entity &get(EntityHandle h) { return *world.registry.get(h); }
};

// A barrel 2 u over a truck's deck (its quad 1 u above the truck's origin) takes the truck; one
// 12 u over it (the deck 11 u down, past the 10-unit drop) takes none, as does one over a
// material-16 or a bit-2 face; a barrel already linked keeps its link.
void test_the_drop_finds_the_deck() {
	const CollisionModel deck = quad_model();
	{
		auto rig = std::make_unique<Rig>();
		const EntityHandle truck = rig->spawn(1, 1294, 0.0f, &deck);
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(rig->get(barrel).ground_target == truck);
	}
	{
		auto rig = std::make_unique<Rig>();
		rig->spawn(1, 1294, 0.0f, &deck);
		const EntityHandle barrel = rig->spawn(2, 4301, 12.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(!rig->get(barrel).ground_target.valid());
	}
	for (const auto &model : { quad_model(16, 0), quad_model(5, 2) }) {
		auto rig = std::make_unique<Rig>();
		rig->spawn(1, 1294, 0.0f, &model);
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(!rig->get(barrel).ground_target.valid());
	}
	{
		auto rig = std::make_unique<Rig>();
		rig->spawn(1, 1294, 0.0f, &deck);
		const EntityHandle other = rig->spawn(2, 4302, -20.0f, nullptr);
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		rig->get(barrel).ground_target = other;
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(rig->get(barrel).ground_target == other);
	}
}

// Pool 2 walks first: a pool-2 quad nearer than the truck's deck wins; a pool-2 quad farther
// down loses to the deck; the entity itself is never its own ground.
void test_the_pools_and_the_exclude() {
	const CollisionModel deck = quad_model();
	{
		auto rig = std::make_unique<Rig>();
		rig->spawn(1, 1294, 0.0f, &deck);
		const EntityHandle crate = rig->spawn(2, 4302, 1.0f, &deck); // its quad at z 2
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(rig->get(barrel).ground_target == crate);
	}
	{
		auto rig = std::make_unique<Rig>();
		const EntityHandle truck = rig->spawn(1, 1294, 0.0f, &deck);
		rig->spawn(2, 4302, -2.0f, &deck); // its quad at z -1, under the deck
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(rig->get(barrel).ground_target == truck);
	}
	{
		// Its own quad 1 u under it, nothing else: the drop skips the entity itself.
		auto rig = std::make_unique<Rig>();
		const CollisionModel under = quad_model(5, 0, -1, true);
		const EntityHandle self = rig->spawn(2, 4301, 3.0f, &under, true);
		cache_ground_entity_below(rig->world, rig->get(self));
		CHECK(!rig->get(self).ground_target.valid());
	}
}

// The load's pass runs the cache for the gnrc / gnrl / gnl2 rows alone.
void test_the_class_rows() {
	const CollisionModel deck = quad_model();
	auto rig = std::make_unique<Rig>();
	const EntityHandle truck = rig->spawn(1, 1294, 0.0f, &deck);
	ItemDeathTraits gnrc;
	gnrc.death_class = ItemDeathClass::kGnrc;
	ItemDeathTraits gnl2;
	gnl2.death_class = ItemDeathClass::kGnl2;
	ItemDeathTraits brrl;
	brrl.death_class = ItemDeathClass::kBarrel;
	rig->world.tables.item_death_traits.set(4301, gnrc);
	rig->world.tables.item_death_traits.set(4303, gnl2);
	rig->world.tables.item_death_traits.set(4304, brrl);
	const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
	const EntityHandle crate = rig->spawn(2, 4303, 3.0f, nullptr, true);
	const EntityHandle drum = rig->spawn(2, 4304, 3.0f, nullptr, true);
	run_ground_cache_class_inits(rig->world);
	CHECK(rig->get(barrel).ground_target == truck);
	CHECK(rig->get(crate).ground_target == truck);
	CHECK(!rig->get(drum).ground_target.valid());
}

} // namespace

int main() {
	test_the_drop_finds_the_deck();
	test_the_pools_and_the_exclude();
	test_the_class_rows();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ground_cache: all passed\n");
	return 0;
}
