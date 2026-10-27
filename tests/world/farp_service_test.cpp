// The FARP service (D-HUD-14's rearm leg). Once a second the host walks the vehicles: one rested
// its farp_reuse_time since its last service and standing on a friendly FARP counts a step, its
// EWeap slot and its refNum group's guns gain their weapon's farpinfo rounds on every
// interval-th step, the vehicle is full once every slot holds its startrounds, and past 120
// steps its health drops to its criticalhp - 1. Off the FARP the service ends, stamped when it
// ran more than five steps. A rider in a control seat reads the rest left, else 0xFF once its
// vehicle was full, else 0, as the 0x0A's rearm byte.
// [orig: Server_UpdateEntityTargetLockAndWeaponOverlays @0x511820 (Server_TickUpdate
//  @0x51DF78); Entity_IsInFriendlyFarp @0x44A2B0; sub_546150 @0x546150;
//  Entity_AddMountSlotAmmo @0x546230; Entity_AddEWeapSlotAmmo @0x5462A0;
//  NetPacket_WritePlayerState @0x4FF8F0..0x4FF992]

#include <runtime/world/entity.h>
#include <runtime/world/vehicle_system.h>
#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/world.h>

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

constexpr uint8_t kCannonAdm = 1; // the vehicle's: clip 10, 50 rounds, farpinfo 20 1
constexpr uint8_t kGunAdm = 2;    // the attached gun's: clip 5, 9 rounds, farpinfo 3 2

void add_weapon(w::World &world, uint8_t adm, int16_t clip, int16_t start, int32_t rounds,
		int32_t interval) {
	if (world.tables.weapons.entries.size() <= adm) world.tables.weapons.entries.resize(adm + 1u);
	w::WeaponTableEntry &weapon = world.tables.weapons.entries[adm];
	weapon.valid = true;
	weapon.clipsize = clip;
	weapon.startrounds = start;
	weapon.farp_rounds = rounds;
	weapon.farp_interval = interval;
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	w::EntityHandle farp, vehicle;

	Rig() {
		world.registry.configure_pool(0, 4);
		world.registry.configure_pool(1, 8);
		world.registry.configure_pool(2, 4);
		add_weapon(world, kCannonAdm, 10, 50, 20, 1);
		add_weapon(world, kGunAdm, 5, 9, 3, 2);
		w::Entity pad;
		pad.kind = w::EntityKind::Item;
		pad.has_item_def = true;
		pad.item_attrib2 = 0x2000u; // FARP
		pad.item_type_index = 3;
		farp = world.registry.spawn(2, pad);
		w::Entity tank;
		tank.kind = w::EntityKind::Item;
		tank.has_item_def = true;
		tank.item_type = 1;
		tank.item_type_index = 2;
		tank.item_attrib = w::kItemAttribEweap | w::kItemAttribPlayerControl;
		tank.carry_flags = 0x40u;
		tank.health = tank.health_max = 400;
		tank.critical_hp = 100;
		tank.primary_weapon_slot_adm = kCannonAdm;
		tank.primary_weapon_slot.clip = 5;
		tank.primary_weapon_slot.reserve = 0;
		tank.ground_target = farp;
		vehicle = world.registry.spawn(1, tank);
		world.logic_tick = 62 * 90; // one rest past the start
	}
	w::Entity &at(w::EntityHandle h) { return *world.registry.get(h); }
	w::Entity &tank() { return at(vehicle); }
	void second() {
		world.vehicles.farp_service_second();
		++world.logic_tick;
	}
};

// On a friendly FARP each pass is a step: the cannon gains its 20 rounds per step, capped at its
// startrounds and split clip-first, and the vehicle reads full only at a step that finds the slot
// full already (the test runs ahead of the step's rounds).
// [orig: @0x5118BE (the step), sub_546150 @0x5118FB ahead of Entity_AddMountSlotAmmo
//  @0x511934, @0x5119C3]
void test_a_service_tops_up_and_reads_full() {
	Rig r;
	CHECK(w::entity_in_friendly_farp(r.world, r.tank()));
	r.second();
	CHECK(r.tank().farp_steps == 1 && !r.tank().farp_rearmed);
	CHECK(r.tank().primary_weapon_slot.clip == 10 && r.tank().primary_weapon_slot.reserve == 15);
	r.second(); // 25 -> 45
	r.second(); // 45 -> 65, capped at 50
	CHECK(r.tank().primary_weapon_slot.clip == 10 && r.tank().primary_weapon_slot.reserve == 40);
	CHECK(!r.tank().farp_rearmed);
	r.second(); // full ahead of the step
	CHECK(r.tank().farp_steps == 4 && r.tank().farp_rearmed);
}

// Off the FARP the service ends: a service of five steps or fewer leaves no stamp; one of more
// stamps the tick, and the vehicle then rests farp_reuse_time before its next step. A carried
// row, a non-vehicle and an entity off a FARP, or one whose carry bit 0x40 is down, take none.
// [orig: @0x51189E..0x5118B2; the gates @0x511867..0x511891; Entity_IsInFriendlyFarp
//  @0x44A2BE..0x44A2DA]
void test_leaving_ends_the_service_and_the_rest() {
	Rig r;
	for (int i = 0; i < 5; ++i) r.second();
	r.tank().ground_target = {};
	r.second();
	CHECK(r.tank().farp_steps == 0 && r.tank().farp_stamp == 0);
	r.tank().ground_target = r.farp;
	for (int i = 0; i < 6; ++i) r.second();
	r.tank().ground_target = {};
	const uint32_t ended = r.world.logic_tick;
	r.second();
	CHECK(r.tank().farp_steps == 0 && r.tank().farp_stamp == ended);
	r.tank().ground_target = r.farp;
	r.second();
	CHECK(r.tank().farp_steps == 0); // resting
	r.world.logic_tick = ended + 62 * 90;
	r.second();
	CHECK(r.tank().farp_steps == 1);
	// The walk's gates.
	Rig carried;
	carried.tank().flags |= w::kEntityFlagCarried;
	carried.second();
	CHECK(carried.tank().farp_steps == 0);
	Rig not_vehicle;
	not_vehicle.tank().item_type = 6;
	not_vehicle.second();
	CHECK(not_vehicle.tank().farp_steps == 0);
	Rig no_bit;
	no_bit.tank().carry_flags = 0;
	CHECK(!w::entity_in_friendly_farp(no_bit.world, no_bit.tank()));
}

// Past 120 steps the vehicle's health is set to its criticalhp - 1, every step, 0 at least.
// [orig: @0x5118C4..0x5118E3]
void test_a_long_stay_sets_the_critical_health() {
	Rig r;
	for (int i = 0; i < 120; ++i) r.second();
	CHECK(r.tank().health == 400);
	r.second();
	CHECK(r.tank().health == 99);
	Rig zero;
	zero.tank().critical_hp = 0;
	for (int i = 0; i < 121; ++i) zero.second();
	CHECK(zero.tank().health == 0);
}

// The vehicle's refNum group: each gun reads full or not into the vehicle's byte and gains its own
// weapon's farpinfo rounds on the vehicle's interval-th steps.
// [orig: @0x51193C..0x5119B5]
void test_the_group_guns_top_up_and_gate_full() {
	Rig r;
	r.tank().ref_num = 7;
	r.tank().ref_group_member = true;
	r.tank().primary_weapon_slot.clip = 10;
	r.tank().primary_weapon_slot.reserve = 40; // the cannon full
	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.has_item_def = true;
	gun.item_type = 6;
	gun.item_type_index = 4;
	gun.item_attrib = w::kItemAttribEweap;
	gun.ref_num = 7;
	gun.ref_group_member = true;
	gun.primary_weapon_slot_adm = kGunAdm;
	gun.primary_weapon_slot.clip = 2;
	gun.primary_weapon_slot.reserve = 0;
	const w::EntityHandle g = r.world.registry.spawn(1, gun);
	r.second(); // step 1: odd, no rounds for the gun
	CHECK(!r.tank().farp_rearmed && r.at(g).primary_weapon_slot.clip == 2);
	r.second(); // step 2: the gun 2 + 3 = 5
	CHECK(r.at(g).primary_weapon_slot.clip == 5 && r.at(g).primary_weapon_slot.reserve == 0);
	r.second();
	r.second(); // step 4: 8
	r.second();
	r.second(); // step 6: 11, capped at 9
	CHECK(r.at(g).primary_weapon_slot.clip == 5 && r.at(g).primary_weapon_slot.reserve == 4);
	CHECK(!r.tank().farp_rearmed);
	r.second();
	CHECK(r.tank().farp_rearmed);
}

// The rearm byte: a control-seat rider reads its vehicle's rest left (0xFE at most), then 0xFF
// once the vehicle was full with its carry bit up, else 0; a passenger reads 0.
// [orig: NetPacket_WritePlayerState @0x4FF8F0..0x4FF992]
void test_the_rearm_byte() {
	Rig r;
	w::Entity rider;
	rider.kind = w::EntityKind::Organic;
	rider.mounted = true;
	rider.mount_type = w::SeatType::Driver;
	rider.mount_target = r.vehicle;
	const w::EntityHandle h = r.world.registry.spawn(0, rider);
	r.tank().farp_stamp = r.world.logic_tick - 62 * 10;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 80);
	r.world.rules.farp_reuse_seconds = 400;
	r.tank().farp_stamp = r.world.logic_tick;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 0xFE);
	r.world.rules.farp_reuse_seconds = 90;
	r.tank().farp_stamp = r.world.logic_tick - 62 * 90;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 0);
	r.tank().farp_rearmed = true;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 0xFF);
	r.tank().carry_flags = 0;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 0);
	r.tank().carry_flags = 0x40u;
	r.at(h).mount_type = w::SeatType::Controller;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 0xFF);
	r.at(h).mount_type = w::SeatType::Passenger;
	CHECK(r.world.vehicles.farp_reload_byte(r.at(h)) == 0);
}

} // namespace

int main() {
	test_a_service_tops_up_and_reads_full();
	test_leaving_ends_the_service_and_the_rest();
	test_a_long_stay_sets_the_critical_health();
	test_the_group_guns_top_up_and_gate_full();
	test_the_rearm_byte();
	if (failures) {
		std::printf("farp_service: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("farp_service OK\n");
	return 0;
}
