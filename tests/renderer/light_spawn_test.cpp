// renderer::LightSpawner and model_light_spawn: an entity's LGHT records
// walked once into the pool, its one cached word (shared by the muzzle glow
// and cleared by its destruction), the round glows' diff, and the
// interior-owner registry (renderer/light_spawn.h carries the witnesses). The
// lifecycle the GUT effect_light_world_test drove through Godot node exits.

#include <runtime/renderer/light_spawn.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace r = opennova::renderer;
namespace t = opennova::threedi;
namespace w = opennova::world;

int g_failures = 0;

void check(bool ok, const char *message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

constexpr int32_t kOne = 65536;

t::ThreediLight record(float x, float atten_end, uint8_t subobject = 0) {
	t::ThreediLight light;
	std::memset(&light, 0, sizeof(light));
	light.offset[0] = x;
	light.atten_end = atten_end;
	light.subobj_index = subobject;
	return light;
}

r::LightPlacement translation(float x, float y, float z) {
	r::LightPlacement placement{};
	placement[0] = placement[4] = placement[8] = 1.0f;
	placement[9] = x;
	placement[10] = y;
	placement[11] = z;
	return placement;
}

size_t live(const r::LightScene &scene) {
	return scene.inspect().live;
}

void a_record_spawns_through_the_entity_matrix_once() {
	// Model (x, y, z) reads as presentation (-x, y, z); presentation (x, y, z)
	// is mission (x, -z, y).
	const t::ThreediLight lamp = record(-1.0f, 8.0f);
	const r::LightSpawnParams params =
			r::model_light_spawn(lamp, translation(10.0f, 2.0f, -3.0f), 5, false, 0, 0);
	check(params.position_fixed[0] == 11 * kOne && params.position_fixed[1] == 3 * kOne &&
					params.position_fixed[2] == 2 * kOne,
			"the record's point through the entity matrix, in mission space");
	check(params.radius_fixed == 8 * kOne, "radius = atten_end * 65536");
	check(params.rgb[0] == 255 && params.rgb[1] == 255 && params.rgb[2] == 255, "a white colour");
	check(params.owner_entity == 0, "an unattached outdoor record lights the world");
	const r::LightSpawnParams attached =
			r::model_light_spawn(record(0.0f, 8.0f, 3), translation(0, 0, 0), 5, false, 0, 0);
	check(attached.owner_entity == 5 && attached.owner_section == 3,
			"an attached record is owned by its entity and subobject");
	const r::LightSpawnParams room =
			r::model_light_spawn(record(0.0f, 8.0f), translation(0, 0, 0), 5, false, 40, 2);
	check(room.owner_entity == 40 && room.owner_section == 2,
			"an unattached record inside a blink box is the building's");
}

void an_entity_walks_once_and_its_destruction_clears_the_final_lease() {
	r::LightScene scene;
	r::LightSpawner spawner(scene);
	const t::ThreediLight records[] = {record(0.0f, 4.0f), record(1.0f, 4.0f)};
	r::EntityLightSpawn spawn;
	spawn.owner_entity = r::LightOwnerIds::wire(9);
	spawn.records = records;
	spawn.record_count = 2;
	spawn.placement = translation(0, 0, 0);
	const r::LightHandle cached = spawner.spawn_entity(spawn);
	spawner.spawn_entity(spawn);
	check(live(scene) == 2, "one walk: two records, a repeat delivery spawns nothing");
	check(spawner.walked(spawn.owner_entity) && !cached.is_null(), "the final record's lease");
	spawner.destroy_entity(spawn.owner_entity);
	check(live(scene) == 1, "the destruction clears exactly the cached final lease");
	check(!spawner.walked(spawn.owner_entity), "a destroyed entity is forgotten");
	spawner.spawn_entity(spawn);
	check(live(scene) == 3, "a respawned entity walks its records again");
	spawner.reset();
	check(live(scene) == 0 && !spawner.walked(spawn.owner_entity), "the mission reset");
	r::EntityLightSpawn none = spawn;
	none.record_count = 0;
	spawner.spawn_entity(none);
	check(!spawner.walked(spawn.owner_entity), "an entity with no records is not walked");
}

void a_building_never_takes_its_own_blink_box() {
	r::LightScene scene;
	r::LightSpawner spawner(scene);
	const t::ThreediLight lamp = record(0.0f, 4.0f);
	r::EntityLightSpawn spawn;
	spawn.owner_entity = 40;
	spawn.records = &lamp;
	spawn.record_count = 1;
	spawn.building = true;
	spawn.blink_owner = 40;
	spawn.blink_section = 1;
	spawner.spawn_entity(spawn);
	r::LightDrawContext outdoor;
	outdoor.aabb_min_fixed = {-kOne, -kOne, -kOne};
	outdoor.aabb_max_fixed = {kOne, kOne, kOne};
	r::LightDrawSelection selection;
	scene.select_for_draws(&outdoor, 1, r::LightSelectionOptions{}, {1.0f, 1.0f, 1.0f},
			r::LightFlickerInputs{}, false, &selection);
	check(selection.count == 1, "a building's own unattached record stays a world light");
}

void the_muzzle_glow_rearms_the_model_lights_cached_word() {
	r::LightScene scene;
	r::LightSpawner spawner(scene);
	const t::ThreediLight lamp = record(0.0f, 4.0f);
	r::EntityLightSpawn spawn;
	spawn.owner_entity = r::LightOwnerIds::wire(3);
	spawn.records = &lamp;
	spawn.record_count = 1;
	const r::LightHandle cached = spawner.spawn_entity(spawn);
	spawner.muzzle_fire(spawn.owner_entity, {0, 0, 0});
	check(live(scene) == 1 && spawner.entity_handle(spawn.owner_entity) == cached,
			"a nonzero word re-arms the final model light, spawning nothing");
	const uint64_t bare = r::LightOwnerIds::wire(4);
	spawner.muzzle_fire(bare, {0, 0, 0});
	check(live(scene) == 2 && !spawner.entity_handle(bare).is_null(),
			"a zero word spawns the muzzle glow and keeps it");
	for (int tick = 0; tick < r::LightScene::kMuzzleGlowFadeTicks + 1; ++tick)
		scene.tick();
	check(live(scene) == 0, "five ticks later both re-armed lights die");
}

void round_glows_follow_their_rows() {
	r::LightScene scene;
	r::LightSpawner spawner(scene);
	std::vector<w::RoundGlowRow> rows(1);
	rows[0].id = 77;
	rows[0].pos = {1.0f, 2.0f, 3.0f};
	rows[0].radius = 2.0f;
	rows[0].color_rgb24 = 0x102030u;
	spawner.sync_round_glows(rows);
	check(live(scene) == 1 && spawner.live_round_glows() == 1, "a new round spawns its glow");
	rows[0].pos = {5.0f, 2.0f, 3.0f};
	spawner.sync_round_glows(rows);
	check(live(scene) == 1, "a known round follows, spawning nothing");
	rows.clear();
	spawner.sync_round_glows(rows);
	check(live(scene) == 0 && spawner.live_round_glows() == 0, "a released round's glow despawns");
}

void the_interior_registry_resolves_and_forgets() {
	r::LightScene scene;
	r::LightSpawner spawner(scene);
	spawner.register_building(12, r::LightOwnerIds::static_source(0));
	check(spawner.owner_for_bms(12) == r::LightOwnerIds::static_source(0), "a static's tagged id");
	spawner.register_building(13, r::LightOwnerIds::wire(6));
	check(spawner.owner_for_bms(13) == r::LightOwnerIds::wire(6), "a live entity's id");
	spawner.destroy_entity(r::LightOwnerIds::wire(6));
	check(spawner.owner_for_bms(13) == 0, "a destroyed building resolves no owner");
	check(spawner.owner_for_bms(0) == 0, "BMS id 0 is no building");
	check(r::LightOwnerIds::wire(-1) == 0 && r::LightOwnerIds::wire(0) == r::LightOwnerIds::kWireTag,
			"wire handle 0 stays a valid owner; a negative one none");
}

} // namespace

int main() {
	a_record_spawns_through_the_entity_matrix_once();
	an_entity_walks_once_and_its_destruction_clears_the_final_lease();
	a_building_never_takes_its_own_blink_box();
	the_muzzle_glow_rearms_the_model_lights_cached_word();
	round_glows_follow_their_rows();
	the_interior_registry_resolves_and_forgets();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::puts("light_spawn: ok");
	return 0;
}
