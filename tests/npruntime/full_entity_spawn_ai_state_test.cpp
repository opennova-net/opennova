// D-NET-133: the S2C 0x0C / 0x18 aiState byte is entity+0x2B4's low byte, a per-class word:
// a person's 0, a dead vehicle's 0 (its death chain zeroes the block; the death effects'
// group slot stored there is nothing portable), a 6088 marker's effect name, a powerup's respawn
// countdown, a door building's section-touch mask, a vehicle's steering filter, another
// weapon carrier's MountSlot action counter.
// [orig: NetPacket_SerializeObjectToBuffer @0x504D10 (the read @0x50518C);
//  NetPacket_SerializeEntityStatesToBuffer @0x5030A0 (the read @0x5032A3); the writers
//  are cited at replication::entity_word_2b4]

#include <net/npwire/ingame_decode.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_system.h> // VehicleSystem::detach (the release)
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

namespace w = opennova::world;
namespace replication = opennova::replication;

int failures = 0;

void expect(bool cond, const char *msg) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
}

uint8_t ai_state_of(const w::Entity &e) {
	return replication::build_full_entity_spawn(e).ai_state;
}

w::Entity item(uint32_t attrib) {
	w::Entity e;
	e.kind = w::EntityKind::Item;
	e.has_item_def = true;
	e.item_attrib = attrib;
	return e;
}

} // namespace

int main() {
	// A person's word is 0 [orig: Entity_SpawnFromAnimSlotProperty @0x43C518;
	// Player_InitPlayer @0x4E187C].
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.item_attrib = w::kItemAttribEweap | w::kItemAttribPlayerControl;
	person.veh.steer_state = 0x1234;
	expect(ai_state_of(person) == 0, "a person's aiState is 0");

	// A vehicle's steering filter, its low byte [orig: Entity_UpdateVehiclePhysics @0x48C14E].
	w::Entity jeep = item(w::kItemAttribPlayerControl);
	jeep.item_type = 1;
	jeep.veh.steer_state = 0x12345;
	expect(ai_state_of(jeep) == 0x45, "a vehicle's aiState is its steering filter's low byte");
	jeep.veh.steer_state = -3;
	expect(ai_state_of(jeep) == 0xFD, "a negative filter keeps its low byte");
	// Dead, the death chain zeroed the block [orig: Entity_InitDeathSounds @0x493A09].
	jeep.flags |= w::kEntityFlagDead;
	expect(ai_state_of(jeep) == 0, "a dead vehicle's aiState is 0");

	// A weapon carrier's MountSlot counter (slot+0 at +0x2B4)
	// [orig: Entity_GetWeaponSlots @0x5460E0 (@0x5460F4..0x546108)].
	w::Entity loose_gun = item(w::kItemAttribEweap);
	loose_gun.primary_weapon_slot.counter = 3;
	expect(ai_state_of(loose_gun) == 3, "a gun's aiState is its MountSlot's counter");
	// Its gunner's release cuts a running counter to 7: the gunner leaves through the
	// detach, whose claimant leg takes the 0x20 arm for a carrier without PlayerControl
	// (V-L5-s5 0a) [orig: Entity_DetachFromVehicle @0x4355F0 - @0x435765..0x435772].
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(1, 4);
	w::Entity gun_seed = item(w::kItemAttribEweap);
	gun_seed.alive = true;
	gun_seed.health = 100;
	w::Seat usegun;
	usegun.type = w::SeatType::Gunner;
	usegun.bone_index = 6;
	gun_seed.seats.push_back(usegun);
	gun_seed.primary_weapon_slot.counter = 3;
	const w::EntityHandle gun_h = world->registry.spawn(1, gun_seed);
	w::Entity gunner_seed;
	gunner_seed.kind = w::EntityKind::Organic;
	gunner_seed.alive = true;
	gunner_seed.health = 100;
	const w::EntityHandle gunner_h = world->registry.spawn(0, gunner_seed);
	w::Entity &gun = *world->registry.get(gun_h);
	w::Entity &gunner = *world->registry.get(gunner_h);
	gun.seats[0].occupant = gunner_h;
	gun.primary_occupant = gunner_h;
	gunner.mounted = true;
	gunner.mount_target = gun_h;
	gunner.mount_bone = 6;
	gunner.mount_seat = 0;
	gunner.mount_type = w::SeatType::Gunner;
	expect(world->vehicles.detach(gunner_h), "the gunner detaches");
	expect(gun.primary_weapon_slot.counter == 7 && ai_state_of(gun) == 7,
			"the gunner's release cuts the gun's running counter to 7");
	// The gun's death touches no word: its counter rides on [orig:
	// Entity_UpdateChildAttachment @0x4409A0 - the death `or [esi+24h], 6` @0x440C28 /
	// @0x440B72].
	gun.alive = false;
	gun.flags |= w::kEntityFlagDead;
	expect(ai_state_of(gun) == 7, "a dead gun's aiState keeps its MountSlot's counter");
	// With PlayerControl (0x40) the MountSlot is +0x474, not the word
	// [orig: Entity_GetWeaponSlots @0x5460E0 (@0x5460F6..0x5460FA)].
	w::Entity controlled_gun = item(w::kItemAttribEweap | w::kItemAttribPlayerControl);
	controlled_gun.primary_weapon_slot.counter = 7;
	expect(ai_state_of(controlled_gun) == 0, "a PlayerControl carrier's word is not its MountSlot");

	// A powerup's respawn countdown: -1 idle, the ticks while counting
	// [orig: PowerupEntity_InitFromDef @0x442D65; Powerup_TickRespawn @0x442878].
	w::Entity powerup = item(w::kItemAttribPowerup);
	powerup.powerup_def_index = 0;
	powerup.powerup_respawn_timer = -1;
	expect(ai_state_of(powerup) == 0xFF, "an idle powerup's aiState is 0xFF");
	powerup.powerup_respawn_timer = 0x3E;
	expect(ai_state_of(powerup) == 0x3E, "a counting powerup's aiState is its countdown");

	// A door building's section-touch mask [orig: Entity_ComputeBoneCollisionForce
	// @0x4AEB22].
	w::Entity house;
	house.kind = w::EntityKind::Building;
	house.has_item_def = true;
	house.door_slot = 0;
	house.door_touch_mask = 0x4;
	expect(ai_state_of(house) == 0x4, "a door building's aiState is its touch mask");

	// A 6088 marker's effect name, its first character
	// [orig: Entity_SpawnFromBMSRecord @0x40F136..0x40F14F].
	w::Entity marker;
	marker.kind = w::EntityKind::Marker;
	marker.script_effect_name = "FIRE01";
	expect(ai_state_of(marker) == 'F', "a 6088 marker's aiState is its name's first byte");

	// An armed squib keeps its def's deathTime, dead or not
	// [orig: Entity_InitArcMovement @0x449975].
	w::Entity squib = item(0);
	squib.squib.interval = 0x2E;
	squib.flags |= w::kEntityFlagDead;
	expect(ai_state_of(squib) == 0x2E, "an armed squib's aiState is its deathTime");

	if (failures != 0) return 1;
	std::printf("full_entity_spawn_ai_state OK\n");
	return 0;
}
