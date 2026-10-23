// D-NET-NEW-L6-38 (D-NET-515): the gnrc / gnrl / gnl2 rows' class init caches the entity under
// them as their ground link (+0x28): the nearest pool-2, then pool-1 entity whose CFAC faces a
// 10-unit drop from the entity's position crosses. Retail's 03TR barrels sit on the truck that way,
// so the truck's contact solve skips them as its own riders.
// [orig: Entity_CacheGroundEntityBelow @0x407D60; Physics_RaycastNearestEntity @0x5394B0;
//  Physics_RaycastEntityPool @0x4143E0; Entity_InitAllFromModels @0x40E460; the rider skip in
//  Entity_CheckCollisionState @0x462A30 - @0x462E26..0x462E4F]
#include <formats/def/def.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_world_materializer.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/powerup.h>
#include <runtime/world/world.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Pool 2's hit stands against a pool-1 hit at the same distance (pool 1 must be strictly
// nearer); a husked hull is met at its intact model, the walk reading entity+0x30 whatever Flags 4
// holds (its husk quad here lies past the drop).
// [orig: Physics_RaycastNearestEntity @0x5394B0 - the `jge` @0x53951E; Physics_RaycastEntityPool
//  @0x4143E0 - `mov esi, [ecx+30h]` @0x4145CF]
void test_the_tie_and_the_husk() {
	const CollisionModel deck = quad_model();
	{
		auto rig = std::make_unique<Rig>();
		rig->spawn(1, 1294, 0.0f, &deck);
		const EntityHandle crate = rig->spawn(2, 4302, 0.0f, &deck); // the same quad height
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(rig->get(barrel).ground_target == crate);
	}
	{
		auto rig = std::make_unique<Rig>();
		const EntityHandle truck = rig->spawn(1, 1294, 0.0f, &deck);
		rig->collision.assign_entity_husk(truck, rig->collision.add_model(quad_model(5, 0, -20)));
		rig->get(truck).engine_flags |= kEntityFlagHusk;
		const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
		cache_ground_entity_below(rig->world, rig->get(barrel));
		CHECK(rig->get(barrel).ground_target == truck);
	}
}

// One row's def+0x148: a gnrl row caches, a row of another class and a row with no def do not.
void test_one_rows_class_init() {
	const CollisionModel deck = quad_model();
	auto rig = std::make_unique<Rig>();
	const EntityHandle truck = rig->spawn(1, 1294, 0.0f, &deck);
	ItemDeathTraits gnrl;
	gnrl.death_class = ItemDeathClass::kGnrl;
	ItemDeathTraits brrl;
	brrl.death_class = ItemDeathClass::kBarrel;
	rig->world.tables.item_death_traits.set(4301, gnrl);
	rig->world.tables.item_death_traits.set(4304, brrl);
	const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
	const EntityHandle bare = rig->spawn(2, 4301, 3.0f, nullptr, true);
	rig->get(bare).has_item_def = false;
	const EntityHandle drum = rig->spawn(2, 4304, 3.0f, nullptr, true);
	run_ground_cache_class_init(rig->world, rig->get(barrel));
	run_ground_cache_class_init(rig->world, rig->get(bare));
	run_ground_cache_class_init(rig->world, rig->get(drum));
	CHECK(rig->get(barrel).ground_target == truck);
	CHECK(!rig->get(bare).ground_target.valid());
	CHECK(!rig->get(drum).ground_target.valid());
}

// D-NET-NEW-L6-39: a deployable of a gnrc row runs the spawn's def+0x148 as its ground cache, so
// the row stands on the deck under it (the link its 0x18 broadcast and its 0x0D TARGET carry).
// [orig: Entity_SpawnDeployable @0x51C2B0 - the def+0x148 call @0x51C462]
void test_the_deployable_spawn() {
	std::array<opennova::def::DefItemDef, 2> rows{};
	rows[0].id = opennova::mission::kItemIdOffset + 1; // index 0 is the clone's refusal
	rows[0].type = 1;
	rows[1].id = opennova::mission::kItemIdOffset + 4301;
	rows[1].type = opennova::def::DEF_ITEM_TYPE_DECORATION;
	std::strcpy(rows[1].ai_function, "gnrc");
	opennova::def::DefItemsFile items{ rows.data(), rows.size() };
	auto kernel = std::make_unique<opennova::mission::MissionKernel>();
	kernel->set_items_table(&items);
	for (int pool = 0; pool < 3; ++pool)
		kernel->world.registry.configure_pool(pool, 8);
	kernel->resolve_item_traits([](int32_t) { return uint8_t{ 0 }; });
	kernel->world.collision = &kernel->collision;
	Entity hull;
	hull.kind = EntityKind::Item;
	hull.item_id = 1294;
	hull.item_type_index = 3;
	hull.alive = true;
	hull.bound_radius = 3.0f;
	hull.position = { 10.0f, 20.0f, 0.0f };
	const EntityHandle truck = kernel->world.registry.spawn(1, hull);
	kernel->collision.assign_entity(truck, kernel->collision.add_model(quad_model()));
	const int32_t block[6] = { 10 * kOne + kOne / 4, 20 * kOne + kOne / 2, 3 * kOne, 0, 0, 0 };
	const EntityHandle crate = kernel->spawn_deployable(4301, 1, block);
	CHECK(crate.valid());
	if (!crate.valid())
		return;
	CHECK(kernel->world.registry.get(crate)->ground_target == truck);
}

// D-NET-NEW-L6-39: the mission start's powerup walk runs each bound row's def+0x148, a gnrc
// row's being the cache. [orig: Powerup_InitAllEntities @0x4432A0 - @0x4432E9 / @0x443335]
void test_the_powerup_walk() {
	const CollisionModel deck = quad_model();
	auto rig = std::make_unique<Rig>();
	const EntityHandle truck = rig->spawn(1, 1294, 0.0f, &deck);
	ItemDeathTraits gnrc;
	gnrc.death_class = ItemDeathClass::kGnrc;
	rig->world.tables.item_death_traits.set(4301, gnrc);
	PowerupDef row;
	row.name = "AmmoCrate";
	rig->world.tables.powerups.rows.push_back(row);
	rig->world.tables.powerups.loaded = true;
	std::array<opennova::def::DefItemDef, 4> entries{};
	std::strcpy(entries[3].powerup_def, "AmmoCrate");
	opennova::def::DefItemsFile items{ entries.data(), entries.size() };
	const EntityHandle barrel = rig->spawn(2, 4301, 3.0f, nullptr, true);
	rig->get(barrel).item_attrib |= kItemAttribPowerup;
	powerup_bind_entities(rig->world, items);
	CHECK(rig->get(barrel).powerup_def_index == 0);
	CHECK(rig->get(barrel).ground_target == truck);
}

// D-NET-NEW-L6-40: on a joiner the record stores +0x28 once. A 0x10 row (no +0x28 at all) and a
// 0x0D row whose TARGET named none keep the link their class init cached across a later fold; a
// named TARGET still resolves to its row.
// [orig: NapiNPClientMsg_0x010 @0x433400 - the memset @0x43348C; NapiNPClientMsg_0x00D
//  @0x432C40 - the store @0x4332D7]
void test_the_replica_keeps_a_cached_link() {
	namespace rp = opennova::replication;
	rp::ClientReplicaPipeline pipeline;
	opennova::StaticEntityRecord drum;
	drum.item_type_id = 4301;
	opennova::StaticEntityBatch statics;
	statics.start_index = 0;
	statics.records.push_back(drum);
	pipeline.apply(0x10, opennova::encode_static_entity_batch(statics));
	opennova::PoolSpawnRecord truck;
	truck.slot_id = 0x1000;
	truck.item_type_id = 1294;
	opennova::PoolSpawnRecord crate = truck;
	crate.slot_id = 0x1001;
	crate.item_type_id = 4302;
	opennova::PoolSpawnRecord rider = truck;
	rider.slot_id = 0x1002;
	rider.item_type_id = 4303;
	rider.target_handle = truck.slot_id;
	opennova::PoolSpawnBatch batch;
	batch.records = { truck, crate, rider };
	pipeline.apply(0x0D, opennova::encode_pool_spawn_batch(batch));
	auto world = std::make_unique<World>();
	world->registry.configure_pool(1, 8);
	world->registry.configure_pool(2, 8);
	rp::ClientWorldMaterializer materializer;
	materializer.sync(pipeline.state(), *world);
	Entity *on_drum = materializer.owned(*world, EntityHandle{ 0x2000 });
	Entity *on_crate = materializer.owned(*world, EntityHandle{ 0x1001 });
	Entity *on_rider = materializer.owned(*world, EntityHandle{ 0x1002 });
	CHECK(on_drum != nullptr && on_crate != nullptr && on_rider != nullptr);
	if (on_drum == nullptr || on_crate == nullptr || on_rider == nullptr)
		return;
	CHECK(on_rider->ground_target == EntityHandle{ 0x1000 });
	on_drum->ground_target = EntityHandle{ 0x1000 };
	on_crate->ground_target = EntityHandle{ 0x1000 };
	opennova::PoolSpawnRecord jeep = truck;
	jeep.slot_id = 0x1003;
	jeep.item_type_id = 1295;
	batch.records = { jeep };
	pipeline.apply(0x0D, opennova::encode_pool_spawn_batch(batch));
	materializer.sync(pipeline.state(), *world);
	CHECK(materializer.owned(*world, EntityHandle{ 0x1003 }) != nullptr);
	for (const int packed : { 0x2000, 0x1001, 0x1002 }) {
		const Entity *row = materializer.owned(*world, EntityHandle{ static_cast<uint16_t>(packed) });
		CHECK(row != nullptr && row->ground_target == EntityHandle{ 0x1000 });
	}
}

} // namespace

int main() {
	test_the_drop_finds_the_deck();
	test_the_pools_and_the_exclude();
	test_the_class_rows();
	test_the_tie_and_the_husk();
	test_one_rows_class_init();
	test_the_deployable_spawn();
	test_the_powerup_walk();
	test_the_replica_keeps_a_cached_link();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ground_cache: all passed\n");
	return 0;
}
