// fragment_spawn_tail: an item piece's spawn ends in its own spawner's tail, as retail's does
// (V-ROLL-BH's FOUND on D-NET-1281, L5-s36).
//   - The piece factory (MissionKernel::spawn_item_piece, retail's clone) publishes no table:
//     a section piece or a palm fragment spawned mid-cadence leaves the pool-0 / pool-1
//     tables at their tick epoch and every slice in place, and the 17-tick slice phase runs
//     on from the last build, not from the spawn.
//   - A palm fragment builds its own list after its model init (it clears the Indoors bit,
//     with no blink volume near); a section piece builds none.
// [orig: Entity_CloneFromTemplateByType @0x4398A0 (callees Pool_AllocEntry,
//  Entity_AllocateAISlot, AnimMap_RegisterEntity); Entity_SpawnSectionEntity @0x4402D0 (no
//  proximity callee); Palm_SpawnFragment @0x53C1C0 - Entity_InitFromModel called @0x53C397,
//  Entity_BuildProximityList @0x53C39D; Entity_BuildProximityListsFromPools @0x4B8EB0 - the
//  counter zeroed @0x4B8ECA; the cadence gate @0x4C240F]

#include <formats/def/def.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/item_sections.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

namespace {

namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A piece kernel over an items.def text: a tower (pool 2) and item 900, the palm fragment.
bool boot_piece_kernel(opennova::mission::MissionKernel &kernel,
		opennova::def::DefItemsFile &items) {
	static const char kDefinitions[] =
			"begin Tower\r\n id 105050\r\n type building\r\n hp 100\r\n end\r\n"
			"begin Fragment\r\n id 100900\r\n type decoration\r\n hp 100\r\n end\r\n";
	if (opennova::def::def_parse_items_memory(
				reinterpret_cast<const unsigned char *>(kDefinitions),
				sizeof(kDefinitions) - 1, &items) != 0)
		return false;
	kernel.open_document(opennova::bms::File{}, "piece", {});
	kernel.set_items_table(&items);
	opennova::mission::KernelBootOptions options;
	options.playable = false;
	options.wac = false;
	options.collision = false;
	options.seat_specs = false;
	options.terrain = false;
	std::string error;
	return kernel.boot(options, error);
}

// The section spawn's seed (spawn_tower_section's) and the palm fragment's (palm_transition's).
w::Entity section_seed() {
	w::Entity seed;
	seed.kind = w::EntityKind::Building;
	seed.item_id = 5050;
	seed.item_type = 5;
	seed.health = 20;
	seed.engine_flags = 6;
	seed.alive = false;
	seed.item_section_piece = true;
	seed.section_clone = true;
	seed.death_motion = w::DeathMotionMode::SectionFalling;
	seed.position = {20.0f, 20.0f, 0.0f};
	return seed;
}

w::Entity palm_fragment_seed() {
	w::Entity seed;
	seed.kind = w::EntityKind::Building;
	seed.item_id = 900;
	seed.position = {-20.0f, 20.0f, 0.0f};
	seed.spawn_position = seed.position;
	seed.uniform_scale_q16 = 65536;
	seed.engine_flags = w::kEntityFlagDead;
	seed.alive = false;
	seed.health = 20;
	seed.item_section_piece = true;
	seed.palm_fragment = true;
	seed.palm_state = 16;
	seed.death_motion = w::DeathMotionMode::PalmPiece;
	return seed;
}

// The factory publishes nothing: three ticks after a build, a late dynamic beside a person and
// two pieces spawned leave the dynamic table without it and the person's slice empty; the
// tables take it at the next tick, and the slice at the 17th tick after the last build.
bool run_the_piece_factory_publishes_no_table() {
	auto kernel = std::make_unique<opennova::mission::MissionKernel>();
	test_titles::jo_world(kernel->world);
	opennova::def::DefItemsFile items{};
	if (!expect(boot_piece_kernel(*kernel, items), "factory: the piece kernel boots")) return false;
	w::World &world = kernel->world;
	w::CollisionWorld &collision = kernel->collision;
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.item_type_index = 4;
	person.bound_radius = 1;
	person.position = {0.0f, 0.0f, 0.0f};
	const w::EntityHandle person_h = world.registry.spawn(0, person);
	collision.build_initial_tables(world);
	for (int tick = 0; tick < 3; ++tick) collision.build_tick_tables(world);
	w::Entity late;
	late.kind = w::EntityKind::Item;
	late.has_item_def = true;
	late.item_type = 1;
	late.item_type_index = 4;
	late.bound_radius = 1;
	late.position = {1.0f, 0.0f, 0.0f};
	const w::EntityHandle late_h = world.registry.spawn(1, late);
	const int32_t statics = collision.static_count();
	const w::EntityHandle section = kernel->spawn_item_piece(section_seed());
	const w::EntityHandle fragment = kernel->spawn_item_piece(palm_fragment_seed());
	bool ok = expect(section.valid() && fragment.valid(), "factory: both pieces spawn");
	int32_t radius = 0;
	ok &= expect(!collision.dynamic_table_radius_q16(late_h, radius),
			"factory: no pool-0 / pool-1 table is published");
	ok &= expect(collision.candidate_count(person_h) == 0, "factory: no slice is rebuilt");
	ok &= expect(collision.static_count() == statics, "factory: the statics stand");
	for (int tick = 0; tick < 13; ++tick) collision.build_tick_tables(world);
	ok &= expect(collision.dynamic_table_radius_q16(late_h, radius),
			"factory: the next tick's Pool01 takes the late dynamic");
	ok &= expect(collision.candidate_count(person_h) == 0,
			"factory: the slices keep their phase through the 16th tick after the build");
	collision.build_tick_tables(world);
	ok &= expect(collision.candidate_count(person_h) == 1,
			"factory: the 17th tick after the build rebuilds the slices");
	opennova::def::def_free_items(&items);
	return ok;
}

// A piece spawner that marks each piece Indoors, so a list built over it shows (no blink volume
// is near: the list clears the bit).
struct MarkingSpawner : w::IItemPieceSpawner {
	w::World &world;
	std::vector<w::EntityHandle> pieces;
	explicit MarkingSpawner(w::World &target) : world(target) {}
	w::EntityHandle spawn_item_piece(const w::Entity &seed) override {
		w::Entity piece = seed;
		piece.flags |= w::kEntityFlagIndoors;
		const w::EntityHandle h = world.registry.spawn(2, piece);
		pieces.push_back(h);
		return h;
	}
};

struct EventRig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	bool world_jo = test_titles::jo_world(*world);
	std::unique_ptr<w::CollisionWorld> collision = std::make_unique<w::CollisionWorld>();
	MarkingSpawner spawner{*world};

	EventRig() {
		world->registry.configure_pool(2, 16);
		world->rules.mp_session = true;
		world->item_piece_spawner = &spawner;
		world->collision = collision.get();
	}
	w::Entity &spawn_item(int id, w::ItemDeathClass cls) {
		w::Entity e;
		e.item_id = id;
		e.has_item_def = true;
		e.item_type_index = 7;
		e.kind = w::EntityKind::Item;
		const w::EntityHandle h = world->registry.spawn(2, e);
		w::ItemDeathTraits traits;
		traits.death_class = cls;
		world->tables.item_death_traits.set(id, traits);
		return *world->registry.get(h);
	}
	bool indoors(w::EntityHandle h) const {
		const w::Entity *e = world->registry.get(h);
		return e != nullptr && (e->flags & w::kEntityFlagIndoors) != 0;
	}
};

// The palm's 0 -> 2 transition spawns its fragment and builds the fragment's own list.
bool run_a_palm_fragment_builds_its_own_list() {
	EventRig r;
	w::Entity &palm = r.spawn_item(21, w::ItemDeathClass::kPalm);
	palm.palm_sections = true;
	palm.position = {8.0f, 16.0f, 3.0f};
	palm.heading = w::placed_heading_bam(90);
	w::ItemDeathTraits *traits = r.world->tables.item_death_traits.get_mutable(21);
	traits->model_pivots_q16 = {{{0, 0, 2 * 65536}}, {{0, 0, 4 * 65536}}};
	r.world->tables.item_death_traits.set(900, *traits);
	w::destruction_notify_item_damage(*r.world, palm, 1, {2, 100});
	w::destruction_notify_item_damage(*r.world, palm, 1, {5, 1});
	if (!expect(palm.palm_state == 2 && r.spawner.pieces.size() == 1,
			"palm: the 0 -> 2 transition spawns one fragment")) return false;
	return expect(!r.indoors(r.spawner.pieces[0]), "palm: the fragment's own list runs");
}

// A tower section's spawn builds no list.
bool run_a_section_piece_builds_no_list() {
	EventRig r;
	w::Entity &tower = r.spawn_item(50, w::ItemDeathClass::kTower);
	tower.position = {1.0f, 2.0f, 3.0f};
	tower.heading = w::placed_heading_bam(90);
	w::ItemDeathTraits *traits = r.world->tables.item_death_traits.get_mutable(50);
	traits->model_loaded = true;
	traits->husk_sub_part_count = 4;
	traits->model_section_origins_q16 = {
			{{0, 0, 0}}, {{0, 0, 4 * 65536}}, {{0, 0, 8 * 65536}}, {{0, 0, 12 * 65536}}};
	traits->husk_section_origins_q16 = traits->model_section_origins_q16;
	traits->model_section_heights_q16.assign(4, 4 * 65536);
	w::destruction_notify_item_damage(*r.world, tower, 1, {2, 15});
	if (!expect(r.spawner.pieces.size() == 1, "section: the hit spawns one section piece"))
		return false;
	return expect(r.indoors(r.spawner.pieces[0]), "section: no list runs over the piece");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_the_piece_factory_publishes_no_table();
	ok &= run_a_palm_fragment_builds_its_own_list();
	ok &= run_a_section_piece_builds_no_list();
	if (!ok) return 1;
	std::printf("fragment_spawn_tail_test: OK\n");
	return 0;
}
