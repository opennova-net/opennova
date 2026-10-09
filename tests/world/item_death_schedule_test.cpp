// The death schedule's def-side half: the death traits an items.def row carries itself
// (runtime/mission/item_traits.h item_death_traits_from_def, the fill resolve_item_traits makes per
// id), the destroy fade's defaults (runtime/world/destruction.h: 0 takes 50 and 25 ticks, the whole
// fade duration + 4 x stagger [orig: Entity_PublishSwapFadePhases @0x5C3F40]) and the class
// callbacks' witnessed numbers and names.
#include <runtime/mission/item_traits.h>
#include <runtime/world/destruction.h>

#include <formats/def/def.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common/test_expect.h"

using namespace opennova;

namespace {

template <size_t N> void put(char (&field)[N], const char *text) {
	std::strncpy(field, text, N - 1);
	field[N - 1] = 0;
}

int test_traits_from_def() {
	def::DefItemDef row;
	std::memset(&row, 0, sizeof(row));
	put(row.ai_function, "GNL2");
	row.destroy_timing_ticks[0] = 31;
	row.destroy_timing_ticks[1] = 62;
	row.destroy_timing_ticks[2] = 15;
	row.physics = 3;
	row.clipsize = 0x30000;
	put(row.ammo_marker3, "AT_SQUIB");
	put(row.particlefx.effect, "Effect_Smoke");
	row.unit_type = 11;
	row.kz = 7.5f;
	row.hp = 200;
	row.armor_impact = 40;
	row.armor_blast = 60;
	row.attrib = def::DEF_ITEM_ATTRIB_SD;
	row.attrib2 = def::DEF_ITEM_ATTRIB2_STATICDEATH;
	put(row.huskfinal, "wreck.3di");
	row.type = def::DEF_ITEM_TYPE_DECORATION;
	row.husk_sub_parts = 300;
	for (int s = 0; s < 16; ++s) row.husk_sub_part_types[s] = static_cast<unsigned char>(s + 1);
	row.debris_scale = 0.5f;
	put(row.sound_profile, "SP_TANK");
	put(row.sounddeath, "EXPLO_BIG");
	put(row.particlespawn, "Effect_Spawn");
	put(row.particledeath, "Effect_Dead");
	put(row.particleh2odeath, "Effect_Wet");
	put(row.particlefire, "Effect_Fire");
	put(row.particleother, "Effect_Other");
	put(row.particlefinale, "Effect_Finale");

	const world::ItemDeathTraits t = mission::item_death_traits_from_def(row);
	TEST_EXPECT(t.death_class == world::item_death_class_from_tag("gnl2"));
	TEST_EXPECT(t.destroy_timing_ticks[0] == 31 && t.destroy_timing_ticks[1] == 62 && t.destroy_timing_ticks[2] == 15);
	TEST_EXPECT(t.physics == 3 && t.squib_distance_q16 == 0x30000 && t.squib_ammo == "AT_SQUIB" &&
	            t.particlefx == "Effect_Smoke");
	TEST_EXPECT(t.unit_type == 11 && t.kz == 7.5f && t.armor_impact == 40 && t.armor_blast == 60);
	TEST_EXPECT(t.team_protect && !t.no_die && t.static_death && t.has_husk && t.is_decoration);
	TEST_EXPECT(t.husk_sub_part_count == 255);
	for (int s = 0; s < 16; ++s) TEST_EXPECT(t.husk_sub_part_types[s] == s + 1);
	TEST_EXPECT(t.husk_sub_part_types[16] == 0); // the clamp's slot, unauthored
	TEST_EXPECT(t.debris_scale == 0.5f && t.sound_profile == "SP_TANK" && t.sound_death == "EXPLO_BIG");
	TEST_EXPECT(t.particlespawn == "Effect_Spawn" && t.particledeath == "Effect_Dead" &&
	            t.particleh2odeath == "Effect_Wet" && t.particlefire == "Effect_Fire" &&
	            t.particleother == "Effect_Other" && t.particlefinale == "Effect_Finale");
	// Nothing the load adds: no regional sound, no model state.
	TEST_EXPECT(t.regional_sounds[0].name.empty() && t.regional_loops[0].empty() && !t.husk_model_loaded &&
	            t.kz_points.empty());

	// An hp-0 def reads its armor words as -1 [orig: Entity_InitFromModel @0x40DC95 / @0x40DC9F]; no
	// husk names, no husk; the no-die attrib; a negative sub-part count clamps to 0.
	row.hp = 0;
	row.huskfinal[0] = 0;
	row.attrib = def::DEF_ITEM_ATTRIB_NODIE;
	row.husk_sub_parts = -4;
	const world::ItemDeathTraits bare = mission::item_death_traits_from_def(row);
	TEST_EXPECT(bare.armor_impact == -1 && bare.armor_blast == -1 && !bare.has_husk && bare.no_die &&
	            !bare.team_protect && bare.husk_sub_part_count == 0);
	std::printf("traits: every def field the death reads, the hp-0 armor, the clamps\n");
	return 0;
}

int test_fade_defaults() {
	const int32_t authored[3] = {31, 62, 15};
	TEST_EXPECT(world::destroy_fade_duration_ticks(authored) == 62 && world::destroy_fade_stagger_ticks(authored) == 15 &&
	            world::destroy_fade_total_ticks(authored) == 62 + 4 * 15);
	const int32_t unset[3] = {0, 0, 0};
	TEST_EXPECT(world::destroy_fade_duration_ticks(unset) == 50 && world::destroy_fade_stagger_ticks(unset) == 25 &&
	            world::destroy_fade_total_ticks(unset) == 150);
	// The phases run over that span: half way, OBJECT_DESTROY is at a half.
	const world::DestroyFade half = world::destroy_fade_phases(world::destroy_fade_total_ticks(authored) / 2, authored);
	TEST_EXPECT(half.phases_q16[0] == 0x8000);
	const world::DestroyFade done = world::destroy_fade_phases(world::destroy_fade_total_ticks(unset), unset);
	for (const int32_t phase : done.phases_q16) TEST_EXPECT(phase == 0x10000);
	std::printf("fade: 50 and 25 for 0, the whole span duration + 4 x stagger\n");
	return 0;
}

// The fade's clock: with no delay the death tick; with one, nothing until it runs out, then the
// restamp on the first husked evaluation at or past it. The closed form a preview reads agrees with
// the game's step over every tick. [orig: Entity_PublishSwapFadePhases @0x5C3F40]
int test_fade_clock() {
	world::DestroyFadeClock clock = world::destroy_fade_clock(7, 0);
	TEST_EXPECT(clock.publish && !clock.restamp && clock.elapsed == 7);
	clock = world::destroy_fade_clock(30, 31);
	TEST_EXPECT(!clock.publish && !clock.restamp);
	clock = world::destroy_fade_clock(31, 31);
	TEST_EXPECT(clock.publish && clock.restamp && clock.elapsed == 0);
	TEST_EXPECT(world::destroy_fade_origin_tick(0, 4) == 0 && world::destroy_fade_origin_tick(31, 4) == 31 &&
	            world::destroy_fade_origin_tick(31, 40) == 40);
	for (const int32_t delay : {0, 3, 31}) {
		for (const int32_t husked_at : {0, 4, 32, 40}) {
			// Step the game's clock every tick from the husk on.
			int32_t death = 0, timer = delay;
			for (int32_t now = husked_at; now < 120; ++now) {
				const world::DestroyFadeClock step = world::destroy_fade_clock(now - death, timer);
				if (step.restamp) {
					timer = 0;
					death = now;
				}
				const int32_t origin = world::destroy_fade_origin_tick(delay, husked_at);
				TEST_EXPECT(step.publish == (now >= origin));
				if (step.publish) TEST_EXPECT(step.elapsed == now - origin);
			}
		}
	}
	std::printf("fade clock: the delay, the restamp, the closed form over every tick\n");
	return 0;
}

int test_schedule_names() {
	TEST_EXPECT(world::kGnrcDeathThinkTicks == 4 && world::kGnl2DeathThinkTicks == 32);
	TEST_EXPECT(!world::unit_type_is_boat(4) && world::unit_type_is_boat(5) && world::unit_type_is_boat(8) &&
	            !world::unit_type_is_boat(9) && world::kUnitTypeBridge == 11);
	TEST_EXPECT(std::string(world::kShipExplosionSound) == "EXPLO_SHIP_TINY" &&
	            std::string(world::kBridgeWaterShockEffect) == "Effect_ShockWaterBrdg" &&
	            std::string(world::kItemExplosionEffect) == "Effect_AirExp" &&
	            std::string(world::kItemExplosionAmmo) == "kz_M406HE" &&
	            std::string(world::kAmmoKzOrganicBlast) == "kz_OrganicBlast" && world::kKzPointBlastRadius == 5.0f);
	std::printf("names: the think ticks, the boat and bridge rows, the explosion and blast names\n");
	return 0;
}

} // namespace

int main() {
	if (test_traits_from_def() != 0) return 1;
	if (test_fade_defaults() != 0) return 1;
	if (test_fade_clock() != 0) return 1;
	if (test_schedule_names() != 0) return 1;
	return 0;
}
