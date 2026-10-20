// D-NET-691: the local player's damage-class bytes, the table Weapon_CalcImpactDamage
// reads, have four sites in retail: the two readers and two writers, the 0x2F accept and
// the armory screen's apply [orig: Weapon_CalcImpactDamage @0x4ECA24;
// Server_SendWeaponSlotListToPlayer @0x502871; NapiNPServerMsg_HandlePlayerLoadout
// @0x515E92; WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566182..0x56618C)].
// Player_InitPlayer holds none, so the spawn kit's rebuild writes nothing; the apply,
// out of a session, stores each entry's fourth value raw and clears nothing, and in a
// session sends the 0x2F instead (is_in_session @0x565D4D, the send @0x565D94). The
// weapons are a model of the test's own.

#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/weapon_fsm.h>
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

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<w::LocalPlayer> local;

	Rig() {
		world.registry.configure_pool(0, 4);
		w::WeaponTable &table = world.tables.weapons;
		table.ammo_class_names = {"CLASS_556MM"};
		table.ammo_class_caps = {1000};
		table.entries.resize(2);
		w::WeaponTableEntry &rifle = table.entries[1];
		rifle.name = "WPN_M4AUTO";
		rifle.valid = true;
		rifle.category = 3;
		rifle.clipsize = 30;
		rifle.startrounds = 90;
		rifle.maxclips = 3;
		rifle.ammo_class_id = 0;
		rifle.ammo_class_count = 1;
		rifle.weapon_class_slot = 1;
		rifle.ammo_index = 4;
		w::weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, rifle.action_fsm);
		world.tables.ammo.entries.resize(6);
		w::Entity body;
		body.kind = w::EntityKind::Organic;
		body.health = body.health_max = 100;
		body.alive = true;
		world.cached.local_player = world.registry.spawn(0, body);
		local = std::make_unique<w::LocalPlayer>(world);
		world.local_player_state = local.get();
	}
	~Rig() { world.local_player_state = nullptr; }
	w::Entity &body() { return *world.registry.get(world.cached.local_player); }
	bool accept(int32_t fourth) {
		return w::local_loadout_apply_accept(world, local->loadout, local->weapon,
				local->inventory, local->inventory_valid,
				{w::WeaponKitEntry{"WPN_M4AUTO", 2, -1, fourth}}, 8, false);
	}
};

void test_the_spawn_rebuild_writes_no_byte() {
	Rig r;
	r.body().ammo_damage_class = {7, 7, 7, 7, 7, 7};
	r.local->loadout.spawn_kit = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, 2}};
	r.local->loadout.spawn_kit_set = true;
	w::local_loadout_rebuild(r.world, r.local->loadout, r.local->weapon, r.local->inventory,
			r.local->inventory_valid, true);
	CHECK(r.body().ammo_damage_class == std::vector<uint8_t>(6, 7));
}

void test_the_offline_apply_stores_the_raw_byte() {
	Rig r;
	r.body().ammo_damage_class = {7, 7, 7, 7, 7, 7};
	CHECK(r.accept(2));
	CHECK(r.body().ammo_damage_class.size() == 6);
	CHECK(r.body().ammo_damage_class[4] == 2);
	CHECK(r.body().ammo_damage_class[0] == 7); // nothing cleared
	// The low byte, raw: 3 stays 3 and -1 stores 0xFF.
	CHECK(r.accept(3));
	CHECK(r.body().ammo_damage_class[4] == 3);
	CHECK(r.accept(-1));
	CHECK(r.body().ammo_damage_class[4] == 0xFF);
}

void test_an_in_session_apply_stores_nothing() {
	Rig r;
	r.world.rules.mp_session = true;
	CHECK(r.accept(2));
	CHECK(r.body().ammo_damage_class.empty());
}

} // namespace

int main() {
	test_the_spawn_rebuild_writes_no_byte();
	test_the_offline_apply_stores_the_raw_byte();
	test_an_in_session_apply_stores_nothing();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS damage_class_bytes\n");
	return 0;
}
