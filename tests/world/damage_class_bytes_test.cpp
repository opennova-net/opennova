// D-NET-691: the local player's damage-class bytes, the table Weapon_CalcImpactDamage
// reads, have four sites in retail: the two readers and two writers, the 0x2F accept and
// the armory screen's apply [orig: Weapon_CalcImpactDamage @0x4ECA24;
// Server_SendWeaponSlotListToPlayer @0x502871; NapiNPServerMsg_HandlePlayerLoadout
// @0x515E92; WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566182..0x56618C)].
// Player_InitPlayer holds none, so the spawn kit's rebuild writes nothing; the apply,
// out of a session, stores each entry's fourth value raw and clears nothing, and in a
// session sends the 0x2F instead (is_in_session @0x565D3A, the send @0x565D94). The
// weapons are a model of the test's own.

#include <runtime/inmatch/loadout_submit.h>
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

// L4-s6b FOUND 3: in a session the ACCEPT applies nothing locally: the selection
// becomes the process's loadout name list, the resident kit keeping its rows until the
// host's list reaches it (D-NET-143), and, off the authority, the slot table empties, the class
// and the damage bytes untouched, until the host's 0x5A grant; an authority keeps its
// slots. The one 0x2F it sends carries the body's team, the ARMORY's class (V-L2-s6
// FOUND 3: not the body's or the profile's), the selection through the name gate and the
// live slot word, which the reset leaves; DFX2's sender fixes the class.
// [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 - @0x565D27..0x565D4B, @0x565D4F,
//  WeaponSlotPool_ResetAllEntries @0x565D58..0x565D66, g_ArmorySelectedClass @0x565D86,
//  g_CurrentWeaponSlot @0x565D73, the team @0x565D8C, NetPacket_SendLoadoutSubmit
//  @0x565D94; NetPacket_SendLoadoutSubmit @0x42CDC0 - the name gate @0x42CF0B;
//  dfx2.exe NetPacket_SendLoadoutSubmit @0x42EB40 (@0x42EC15..0x42EC3E)]
void test_a_session_accept_resets_the_slots_and_applies_nothing() {
	Rig r;
	r.world.rules.mp_session = true;
	CHECK(r.accept(2)); // a kit in the slots first
	const int32_t live = r.local->inventory.equipped_combo;
	CHECK(r.local->inventory.slot(live) != nullptr &&
			r.local->inventory.slot(live)->adm_index == 1);
	r.body().player_class = 8;
	r.body().team = 2;
	r.body().ammo_damage_class = {7, 7, 7, 7, 7, 7};
	const std::vector<w::WeaponKitEntry> selection = {w::WeaponKitEntry{"WPN_M4AUTO", 3, -1, 5},
			w::WeaponKitEntry{"WPN_NONE_SUCH", 1, -1, -1}};
	const opennova::LoadoutSubmit sent = opennova::inmatch::armory_accept_in_session(r.world,
			*r.local, selection, 6, /*authority=*/false, opennova::bms::MissionType{});
	CHECK(r.local->loadout.spawn_kit().size() == 1 &&
			r.local->loadout.spawn_kit()[0].ammo_primary == 2);
	bool empty = true;
	for (const w::WeaponInventorySlot &slot : r.local->inventory.slots)
		empty = empty && slot.adm_index < 0;
	CHECK(empty);
	CHECK(r.body().player_class == 8 && r.body().ammo_damage_class[4] == 7);
	CHECK(sent.team == 2 && sent.player_class == 6);
	CHECK(sent.weapon_slot_index == static_cast<uint32_t>(live));
	CHECK(sent.entries.size() == 1);
	if (sent.entries.size() == 1)
		CHECK(sent.entries[0].adm_index == 1 && sent.entries[0].ammo_primary == 3 &&
				sent.entries[0].variant == 5);
	// The authority keeps its slots and sends the same 0x2F.
	Rig a;
	a.world.rules.mp_session = true;
	CHECK(a.accept(2));
	a.body().team = 1;
	const opennova::LoadoutSubmit own = opennova::inmatch::armory_accept_in_session(a.world,
			*a.local, selection, 6, /*authority=*/true, opennova::bms::MissionType{});
	CHECK(a.local->inventory.slot(a.local->inventory.equipped_combo) != nullptr &&
			a.local->inventory.slot(a.local->inventory.equipped_combo)->adm_index == 1);
	CHECK(own.team == 1 && own.player_class == 6 &&
			own.weapon_slot_index == static_cast<uint32_t>(a.local->inventory.equipped_combo));
	// DFX2's sender: 8, or 7 in a combat-vehicle mission, whatever the armory's class.
	Rig d;
	d.world.rules.mp_session = true;
	d.world.rules.title = opennova::gameprofile::GAME_DFX2;
	CHECK(opennova::inmatch::armory_accept_in_session(d.world, *d.local, selection, 6, false,
			opennova::bms::MissionType{}).player_class == 8);
	CHECK(opennova::inmatch::armory_accept_in_session(d.world, *d.local, selection, 6, false,
			opennova::bms::MissionType::CombatVehicleMission).player_class == 7);
}

void test_the_spawn_rebuild_writes_no_byte() {
	Rig r;
	r.body().ammo_damage_class = {7, 7, 7, 7, 7, 7};
	r.local->loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, 2}};
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
	test_a_session_accept_resets_the_slots_and_applies_nothing();
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
