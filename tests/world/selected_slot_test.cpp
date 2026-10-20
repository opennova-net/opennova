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

#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
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


// The local player's borrowed attach saves its EquippedSlot at +0x308 before the
// carrier's slot replaces it: a UseGun seat keeps the old +0x308 when the slot's def
// carries flags 0x80, a null EquippedSlot saves none (-1, retail's null pointer), and
// a ctrlx seat saves only under an EWeap carrier (an unarmed one borrows nothing).
// [orig: Entity_AttachToUseGunSlot @0x546BCF..0x546C31 (the 0x80 test
//  @0x546BE0..0x546BEE), @0x546C48; Entity_AttachToVehicleSlot - the EWeap gate
//  @0x49480B, @0x49481D..0x494831, a null slot's leg @0x494867..0x494898]
struct BorrowRig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	w::LocalPlayer local{*world};
	w::EntityHandle player_h, carrier_h;

	BorrowRig(uint32_t carrier_attrib, w::SeatType seat_type, int32_t equipped,
			int32_t selected) {
		world->registry.configure_pool(0, 8);
		world->registry.configure_pool(1, 8);
		w::WeaponTable &table = world->tables.weapons;
		table.entries.resize(3);
		table.entries[1].valid = true;
		table.entries[1].name = "RIFLE";
		table.entries[1].category = 3;
		table.entries[2].valid = true;
		table.entries[2].name = "KEEPER";
		table.entries[2].category = 2;
		table.entries[2].flags = 0x80;
		w::Entity carrier;
		carrier.kind = w::EntityKind::Item;
		carrier.has_item_def = true;
		carrier.has_graphic_model = true;
		carrier.item_attrib = carrier_attrib;
		carrier.health = carrier.health_max = 1000;
		carrier.alive = true;
		carrier.position = {100.0f, 200.0f, 10.0f};
		w::Seat seat;
		seat.type = seat_type;
		seat.bone_index = 1;
		seat.source_name = seat_type == w::SeatType::Gunner ? "UseGun" : "ctrlx00";
		carrier.seats.push_back(seat);
		carrier_h = world->registry.spawn(1, carrier);
		w::Entity pl;
		pl.kind = w::EntityKind::Organic;
		pl.position = {102.0f, 200.0f, 10.0f};
		pl.health = pl.health_max = 150;
		pl.alive = true;
		pl.flags = pl.engine_flags = w::kEntityFlagPlayer;
		player_h = world->registry.spawn(0, pl);
		world->cached.local_player = player_h;
		world->local_player_state = &local;
		local.inventory.reset(table);
		local.inventory.slots[kRifle].adm_index = 1;
		local.inventory.slots[kPistol].adm_index = 2;
		local.inventory.equipped_combo = equipped;
		local.inventory.selected_combo = selected;
		local.inventory_valid = true;
	}
	~BorrowRig() { world->local_player_state = nullptr; }
	bool attach(w::SeatType seat_type) {
		return world->vehicles.attach_to_seat(player_h, {carrier_h, 0, seat_type});
	}
};

void test_a_borrowed_attach_saves_the_selected_slot() {
	{
		// UseGun over a flags-0x80 slot: the old +0x308 stays.
		BorrowRig r(w::kItemAttribEweap, w::SeatType::Gunner, kPistol, kRifle);
		CHECK(r.attach(w::SeatType::Gunner));
		CHECK(r.local.inventory.selected_combo == kRifle);
	}
	{
		// UseGun over any other slot saves it.
		BorrowRig r(w::kItemAttribEweap, w::SeatType::Gunner, kRifle, kPistol);
		CHECK(r.attach(w::SeatType::Gunner));
		CHECK(r.local.inventory.selected_combo == kRifle);
	}
	{
		// An armed ctrlx (EWeap carrier) saves the slot, flags 0x80 or not.
		BorrowRig r(w::kItemAttribEweap | w::kItemAttribPlayerControl,
				w::SeatType::Controller, kPistol, kRifle);
		CHECK(r.attach(w::SeatType::Controller));
		CHECK(r.local.inventory.selected_combo == kPistol);
	}
	{
		// A null EquippedSlot saves the null pointer.
		BorrowRig r(w::kItemAttribEweap | w::kItemAttribPlayerControl,
				w::SeatType::Controller, -1, kRifle);
		CHECK(r.attach(w::SeatType::Controller));
		CHECK(r.local.inventory.selected_combo == -1);
	}
	{
		// An unarmed ctrlx borrows nothing and saves nothing.
		BorrowRig r(w::kItemAttribPlayerControl, w::SeatType::Controller, kPistol, kRifle);
		CHECK(r.attach(w::SeatType::Controller));
		CHECK(r.local.inventory.selected_combo == kRifle);
	}
}

} // namespace

int main() {
	test_an_unarmed_ride_mounts_the_stale_selected_slot();
	test_a_seated_select_stores_the_selected_slot_alone();
	test_a_borrowed_attach_saves_the_selected_slot();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS selected_slot\n");
	return 0;
}
