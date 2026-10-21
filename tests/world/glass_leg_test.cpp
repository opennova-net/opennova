// The bullet GLASS leg (D-ITEM-6): a round stop on a building whose model is a row of the
// surface-material table walks the row's userpoints before any ring scar. Pinned on the
// shared stream's word counts and the effects CRT's reseeds the witnessed functions take,
// with ammo scar_type 1 on an atrm1a-shaped building (the table's four GLASS rows, ids 9 /
// 10 / 11 / 12):
//   a first hit on a window point:  12 words (the leg's two texture gates, the first-hit
//                                    rule's texture, the node's texture, four rolls of two)
//                                    and four reseeds; no ring scar;
//   a repeat hit on that point:      3 words (the two gates and the rule's texture), no reseed;
//   a miss on the model:             8 words (two per GLASS row), then the ring scar's 2;
//   scar_type 2 on a miss:           4 words (id 2 spans one texture), no ring scar.
// The rolls pass at or under the main column's ftol(p * 100): 100, 10, 20, 33.
// [orig: Impact_SpawnGlassEffectsOrScar @0x5CF1B0; Terrain_SpawnSurfaceEffectsAtUserPoints
//  @0x5CEA90; Scar_ProjectDecalOntoEntity @0x5CE4A0; Scar_HasProjectedDecalAt @0x5CC700;
//  Scar_TextureForId @0x5CC360 over g_ScarTable @0x8417A8; Effect_RollSurfaceEffectProbability
//  @0x5CC1F0 over g_SurfaceEffectProbTable @0x8418B8; the table @0x841980]

#include <runtime/world/impact_scar.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <base/crt/crt_rng.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

constexpr int32_t kWindowItem = 4242;
constexpr int32_t kPlainItem = 4243;

struct Rig {
	std::unique_ptr<World> world = std::make_unique<World>();
	EntityHandle building;
	EntityHandle plain;
	ProjectileHit hit;
	int32_t window[3] = {0, 0, 0}; // the GLASS1 point's world position
};

ModelUserPoint point(const char *name, int32_t x, int32_t y, int32_t z) {
	ModelUserPoint p;
	p.name = name;
	p.pos_q16[0] = x;
	p.pos_q16[1] = y;
	p.pos_q16[2] = z;
	p.dir_q16[1] = 0x10000;
	return p;
}

void build(Rig &rig) {
	World &world = *rig.world;
	world.env.water_z = 0;
	Entity b;
	b.kind = EntityKind::Building;
	b.item_type = 5;
	b.item_id = kWindowItem;
	b.has_graphic_model = true;
	b.position = {100.0f, 200.0f, 10.0f};
	rig.building = world.registry.spawn(2, b);
	Entity p = b;
	p.item_id = kPlainItem;
	p.position = {300.0f, 200.0f, 10.0f};
	rig.plain = world.registry.spawn(2, p);

	// The model's GHDR name compares case-insensitively; one point per GLASS row's name,
	// the GLASS1 one a unit off the origin at three units up.
	ItemDeathTraits window;
	window.graphic_name = "ATRM1A";
	window.graphic_userpoints = {point("GLASS1", 0x10000, 0, 0x30000),
			point("glass2", -0x40000, 0, 0x30000), point("GLASS3", 0, 0x50000, 0x30000),
			point("GLASS4", 0, -0x50000, 0x30000)};
	world.tables.item_death_traits.set(kWindowItem, window);
	ItemDeathTraits plain;
	plain.graphic_name = "bldg9";
	plain.graphic_userpoints = {point("GLASS1", 0x10000, 0, 0x30000)};
	world.tables.item_death_traits.set(kPlainItem, plain);

	const int32_t local[3] = {0x10000, 0, 0x30000};
	entity_placement_matrix(*world.registry.get(rig.building)).transform_point(local, rig.window);
	rig.hit.hit_class = ProjectileHitClass::StaticEntity;
	rig.hit.geometry_entity = rig.building;
	rig.hit.position_q16 = FixedVec3{rig.window[0], rig.window[1], rig.window[2]};
	rig.hit.normal_q16 = FixedVec3{0x10000, 0, 0};
	rig.hit.surface_type = 0;
	rig.hit.section_index = 0;
	rig.hit.face_index = 3;
}

// The words the shared stream gave since `from`, replayed; the state is left where it was.
std::vector<uint16_t> words_since(World &world, uint32_t from) {
	const uint32_t now = world.prng16_state;
	std::vector<uint16_t> out;
	world.prng16_state = from;
	while (world.prng16_state != now && out.size() < 64) out.push_back(world.next_prng16());
	world.prng16_state = now;
	return out;
}

void test_the_table_and_the_rolls() {
	CHECK(find_surface_material_row("GLASS1", "atrm1a") != nullptr, "atrm1a has a GLASS1 row");
	CHECK(find_surface_material_row("GLASS4", "ATRM1C")->scar_id == 12, "atrm1c's GLASS4 is id 12");
	CHECK(find_surface_material_row("GLASS1", "airtowr1")->radius_q16 == 0x20000,
			"airtowr1's GLASS1 box is two units");
	CHECK(find_surface_material_row("GLASS4", "atrm2a") == nullptr, "atrm2a has no GLASS4 row");
	// Every column's threshold, ftol(p * 100.0): the float 0.33 is 0.330000013, so 33.
	const int32_t main_limits[4] = {100, 10, 20, 33};
	const int32_t alt_limits[4] = {33, 5, 5, 10};
	for (int i = 0; i < 4; ++i) {
		CHECK(int32_t(double(kSurfaceEffectProbTable[i].main) * 100.0) == main_limits[i],
				"the main column's threshold");
		CHECK(int32_t(double(kSurfaceEffectProbTable[i].alt) * 100.0) == alt_limits[i],
				"the alt column's threshold");
	}
	// An unlisted name passes with no draw.
	auto world = std::make_unique<World>();
	const uint32_t s0 = world->prng16_state;
	CHECK(effect_roll_surface_probability(*world, "Effect_NotATableRow", false), "unlisted passes");
	CHECK(effect_roll_surface_probability(*world, "", false), "empty passes");
	CHECK(world->prng16_state == s0, "and neither draws");
	// id 1 spans four textures and draws; id 2 and id 13 span one and do not.
	(void)scar_texture_for_id(*world, 2);
	(void)scar_texture_for_id(*world, 13);
	CHECK(world->prng16_state == s0, "a one-texture id draws nothing");
	const int strip = scar_texture_for_id(*world, 9);
	CHECK(strip == 15 || strip == 16, "id 9 picks strip 15 or 16");
	CHECK(words_since(*world, s0).size() == 1, "with one word");
}

void test_a_window_point() {
	Rig rig;
	build(rig);
	World &world = *rig.world;
	const Entity &building = *world.registry.get(rig.building);

	// The first hit on the GLASS1 point: no ring scar, one decal, twelve words, four
	// reseeds, the last with the eleventh word.
	uint32_t s0 = world.prng16_state;
	CHECK(scar_add_entry(world, rig.hit, building, 1), "a matched window point ends the impact");
	std::vector<uint16_t> w = words_since(world, s0);
	CHECK(w.size() == 12, "a first hit on a window point takes twelve words");
	if (w.size() == 12) {
		CHECK(crt::crt_rand_state() == w[10], "the effects CRT is reseeded with the last roll's seed");
		const int32_t limits[4] = {100, 10, 20, 33};
		size_t expected = 0;
		for (int k = 0; k < 4; ++k)
			if (int32_t(w[5 + 2 * k]) % 100 <= limits[k]) ++expected;
		CHECK(world.out.vehicle_effects.size() == expected, "one spawn per passed roll");
		CHECK(!world.out.vehicle_effects.empty() &&
						world.out.vehicle_effects[0].effect == "Effect_BldGlassExp",
				"the glass effect always passes");
	}
	CHECK(world.out.scars.world_ring().cursor == 0, "no ring scar");
	const ProjectedDecalList *list =
			world.out.scars.find_decals(rig.building, building.registry_spawn_id);
	CHECK(list != nullptr && list->nodes.size() == 1, "one decal node");
	if (list != nullptr && !list->nodes.empty()) {
		CHECK(list->nodes[0].origin[0] == rig.window[0] && list->nodes[0].origin[1] == rig.window[1] &&
						list->nodes[0].origin[2] == rig.window[2],
				"at the userpoint");
		CHECK(list->nodes[0].radius_q16 == 0x23333, "with id 9's radius");
	}

	// A repeat hit on the same point: the rule finds the node, three words, no reseed.
	const uint32_t crt_before = crt::crt_rand_state();
	const size_t effects_before = world.out.vehicle_effects.size();
	s0 = world.prng16_state;
	CHECK(scar_add_entry(world, rig.hit, building, 1), "a repeat hit still matches");
	CHECK(words_since(world, s0).size() == 3, "a repeat hit takes three words");
	CHECK(crt::crt_rand_state() == crt_before, "and no reseed");
	CHECK(world.out.vehicle_effects.size() == effects_before, "and no effect");
	CHECK(world.out.scars.world_ring().cursor == 0, "still no ring scar");

	// A miss on the model: two words per GLASS row, then the ring scar's spin and texture.
	ProjectileHit miss = rig.hit;
	miss.position_q16.x = rig.window[0] + 10 * 0x10000;
	s0 = world.prng16_state;
	CHECK(scar_add_entry(world, miss, building, 1), "a miss takes the ring scar");
	CHECK(words_since(world, s0).size() == 10, "eight words for the four rows, two for the ring");
	CHECK(world.out.scars.world_ring().cursor == 1, "one ring slot");

	// scar_type 2 on a miss: the kind's texture gate draws nothing, no ring.
	s0 = world.prng16_state;
	CHECK(!scar_add_entry(world, miss, building, 2), "scar_type 2 leaves no ring");
	CHECK(words_since(world, s0).size() == 4, "one word per row");
	CHECK(world.out.scars.world_ring().cursor == 1, "the ring untouched");

	// The death clear frees the list, so the next hit on the point is a first hit again.
	world.out.scars.clear_entity(rig.building);
	CHECK(world.out.scars.find_decals(rig.building, building.registry_spawn_id) == nullptr,
			"the death clear frees the decals");
}

void test_a_model_without_a_row() {
	Rig rig;
	build(rig);
	World &world = *rig.world;
	const Entity &plain = *world.registry.get(rig.plain);
	ProjectileHit hit = rig.hit;
	hit.geometry_entity = rig.plain;
	const int32_t local[3] = {0x10000, 0, 0x30000};
	int32_t p[3];
	entity_placement_matrix(plain).transform_point(local, p);
	hit.position_q16 = FixedVec3{p[0], p[1], p[2]};
	const uint32_t s0 = world.prng16_state;
	CHECK(scar_add_entry(world, hit, plain, 1), "a model the table lacks takes the ring scar");
	CHECK(words_since(world, s0).size() == 2, "with only the ring's two words");
}

} // namespace

int main() {
	test_the_table_and_the_rolls();
	test_a_window_point();
	test_a_model_without_a_row();
	if (failures == 0) std::printf("glass_leg: all passed\n");
	return failures == 0 ? 0 : 1;
}
