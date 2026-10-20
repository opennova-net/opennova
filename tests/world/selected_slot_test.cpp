// D-WPN-48: entity+0x308, the last SELECTED weapon slot, is its own word, apart from the
// slot a mount stages (g_PendingWeaponSlot). Player_SelectWeaponSlot writes it (seated or
// not) [orig: Player_SelectWeaponSlot @0x4DD680 - @0x4DD6DA / @0x4DD7A8 / @0x4DD81C], the
// category key's walk and the cycle mount without selecting [orig:
// Player_SwitchToWeaponByHandle @0x4E0170 -> Player_MountWeaponSlot @0x4DFA40, which
// stores g_PendingWeaponSlot @0x4DFB16; Player_CycleWeaponSlot @0x4DFE70], and the
// local player's detach from a seat-2 / seat-3 seat that borrowed no slot mounts +0x308
// (else the current slot without a def of category < 11 there) [orig:
// Entity_DetachFromVehicle @0x43562A..0x43565F]. So after a spawn, a key switch and an
// unarmed ctrlx ride the player leaves holding the stale selected weapon.

#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>
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

constexpr int32_t kRifle = 3 * 65; // category 3 (the spawn default, combo 195)
constexpr int32_t kPistol = 2 * 65;

void test_an_unarmed_ride_mounts_the_stale_selected_slot() {
	auto world = std::make_unique<w::World>();
	w::WeaponTable &table = world->tables.weapons;
	table.entries.resize(3);
	table.entries[1].valid = true;
	table.entries[1].name = "RIFLE";
	table.entries[1].category = 3;
	table.entries[1].weapon_class_slot = 1;
	table.entries[2].valid = true;
	table.entries[2].name = "PISTOL";
	table.entries[2].category = 2;
	table.entries[2].weapon_class_slot = 2;
	w::WeaponInventory inv;
	inv.reset(table);
	inv.slots[kRifle].adm_index = 1;
	inv.slots[kPistol].adm_index = 2;

	// The spawn's select [orig: Player_InitPlayer's Player_SelectWeaponSlot call].
	CHECK(w::weapon_select_slot(table, inv, kRifle, true));
	CHECK(inv.selected_combo == kRifle && inv.equipped_combo == kRifle);

	// The category key: the walk mounts the pistol without selecting it.
	w::WeaponSwitchGates gates;
	gates.equipped_valid = true;
	gates.equipped_action = 0;
	const w::WeaponSwitchOutcome out = w::weapon_switch_to_handle(table, inv, kPistol, gates);
	CHECK(out.kind == w::WeaponSwitchOutcome::kMount && out.combo == kPistol);
	CHECK(inv.pending_combo == kPistol);
	CHECK(inv.selected_combo == kRifle);
	// Its switch-out completes: the pistol is equipped.
	inv.equipped_combo = inv.pending_combo;

	// An unarmed ctrlx ride borrows no slot; its detach mounts +0x308, the rifle,
	// a cross-category mount (SWITCHFROM out of the pistol).
	w::LocalPlayerWeapon weapon;
	weapon.active = true;
	weapon.nvg_scope_restore = true;
	w::PlayerViewState view;
	w::local_weapon_detach_mount(*world, weapon, &inv, view);
	CHECK(inv.pending_combo == kRifle);
	CHECK(weapon.switch_in_flight);
	CHECK(!weapon.nvg_scope_restore);
	CHECK(inv.equipped_combo == kPistol); // until the switch-out completes
	CHECK(inv.selected_combo == kRifle);  // the detach never clears it
}

void test_a_seated_select_stores_the_selected_slot_alone() {
	auto world = std::make_unique<w::World>();
	w::WeaponTable &table = world->tables.weapons;
	table.entries.resize(3);
	table.entries[1].valid = true;
	table.entries[1].category = 3;
	table.entries[2].valid = true;
	table.entries[2].category = 2;
	w::WeaponInventory inv;
	inv.reset(table);
	inv.slots[kRifle].adm_index = 1;
	inv.slots[kPistol].adm_index = 2;
	inv.equipped_combo = kPistol;
	// Seated (parentSlot 2 / 3) the select reaches +0x308 and not the EquippedSlot
	// [orig: the gate @0x4DD6FC, the stores @0x4DD7A8 / @0x4DD7D2].
	CHECK(w::weapon_select_slot(table, inv, kRifle, false));
	CHECK(inv.selected_combo == kRifle);
	CHECK(inv.equipped_combo == kPistol);
	CHECK(inv.pending_combo == -1);
}

} // namespace

int main() {
	test_an_unarmed_ride_mounts_the_stale_selected_slot();
	test_a_seated_select_stores_the_selected_slot_alone();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS selected_slot\n");
	return 0;
}
