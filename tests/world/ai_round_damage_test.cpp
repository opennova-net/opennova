// The damage an AI's rifle round deals a person: the 01TR coop aikill case's first hits on the
// staged joiner, as retail's hosts and ours draw them (L4-s31). The PKM gunners stand 3..5 units
// off and strike in the round's first tick: 114 at an x1.0 section, 142 at a torso section (x1.25).
// The AK74 rifleman on the roof stands ~20 units off, past one tick's travel (900 / 62), so its
// round strikes in its second tick after one flight drag: 75 at a torso section, where a muzzle
// hit would be 76. Retail's hosts drew all three as the first hit (x8 RR 114, x8 RR-aa 142, x7
// RR 75), and so did ours (x8 OO 142, x9 OO / OR 75).
// The ammo rows are retail's ammo.def AMMO_PKM_MM (ammo 38: velocity 825, weight_in_grains 122,
// drag 0.292) and AMMO_ak74_556MM (ammo 21: 900, 60, 0.255), min / max damage 0.
// [orig: RoundData_SpawnRound - the launch speed `shl ecx, 10h` @0x4EC595, the divide by 62
//  @0x4EC598..0x4EC5AB; Weapon_CalcImpactDamage @0x4EC920 - the person section switch
//  @0x4EC9BF (0..4 x1.25 flt_7C6F18, 5..8 x1.0, 9..12 / 15..18 x0.5 flt_7C3B94, 13 / 14 x3.0
//  flt_7C6F80), (62 * |v|) >> 16 @0x4ECB04 clamped at 1219 @0x4ECB06, x weight / 875
//  @0x4ECB2B, the section scale @0x4ECB2F..0x4ECB37, the floor / cap @0x4ECB77..0x4ECB91;
//  the flight drag Entity_ApplyDragAndBounceForce @0x4E5EC0 (the (proj, 1, 0) call @0x4EAA64);
//  AmmoDef_ParseProperty's drag through Math_ParseFixedPoint16 @0x6131F0]
#include <formats/def/def_scan.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/entity.h>
#include <runtime/world/impact_damage.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include "../common/title_fixture.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

AmmoTableEntry rifle_ammo(const char *name, int32_t velocity, int32_t grains, const char *drag) {
	AmmoTableEntry a;
	a.name = name;
	a.velocity = velocity;
	a.weight_in_grains = grains;
	a.drag_fp16 = opennova::defscan::parse_fixed16_digits_n(drag, std::strlen(drag));
	a.drag = float(a.drag_fp16) / 65536.0f;
	a.min_damage = 0;
	a.max_damage = 0;
	a.valid = true;
	return a;
}

// A level shot along +X at the launch speed: (velocity << 16) / 62 units per tick.
FixedVec3 launch(const AmmoTableEntry &a) {
	return FixedVec3{int32_t((int64_t(a.velocity) << 16) / 62), 0, 0};
}

// One flight tick's drag in the air (the Z word above a dry map's water word).
FixedVec3 after_drag(FixedVec3 v, const AmmoTableEntry &a) {
	projectile_apply_drag(v, a, /*position_z_q16=*/43 * 65536, /*water_z_q16=*/0);
	return v;
}

int32_t damage(FixedVec3 v, const AmmoTableEntry &a, int32_t section, const Entity &target,
               const World &world) {
	return calc_impact_damage(v, a, section, /*hit_bone=*/0, target, /*shooter=*/nullptr,
	                          /*ammo_index=*/-1, world);
}

} // namespace

int main() {
	auto world = std::make_unique<World>();
	test_titles::jo_world(*world);
	world->rules.mp_session = true;          // the host, the round's authority
	world->rules.projectile_authority = true;
	auto person = std::make_unique<Entity>();
	person->item_type = 3;    // ItemDef type 3, a person
	person->item_attrib = 0;  // not the 0x200 seat row
	person->carry_flags = 0;  // no body armor (the +44 bit 8)

	const AmmoTableEntry pkm = rifle_ammo("AMMO_PKM_MM", 825, 122, "0.292");
	const AmmoTableEntry ak74 = rifle_ammo("AMMO_ak74_556MM", 900, 60, "0.255");

	// The PKM's first-tick strike: (62 * 872051) >> 16 = 824; 824 * 122 / 875 = 114.
	CHECK(damage(launch(pkm), pkm, 5, *person, *world) == 114);  // x1.0 section
	CHECK(damage(launch(pkm), pkm, 0, *person, *world) == 142);  // torso, 114 x 1.25
	CHECK(damage(launch(pkm), pkm, 4, *person, *world) == 142);
	CHECK(damage(launch(pkm), pkm, 9, *person, *world) == 57);   // limb, x0.5

	// The AK74 at the muzzle: 899 * 60 / 875 = 61, x1.25 = 76; after one flight tick's drag
	// (about 881 units / s) 60, x1.25 = 75: the 75 the roof rifleman's first hit draws.
	CHECK(damage(launch(ak74), ak74, 0, *person, *world) == 76);
	const FixedVec3 second_tick = after_drag(launch(ak74), ak74);
	CHECK(second_tick.x < launch(ak74).x);
	CHECK(damage(second_tick, ak74, 0, *person, *world) == 75);
	CHECK(damage(second_tick, ak74, 5, *person, *world) == 60);

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
