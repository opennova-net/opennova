// The local player's mounted fire path across a seat switch: from a control seat that
// borrows no slot (a helicopter pilot's) to a gun that lends its own (a UseGun seat, or
// an EWeap carrier's ctrlx). Retail's process detaches first, and that detach mounts the
// personal slot (+0x308, else the current one) [orig: Entity_ProcessVehicleAttach
// @0x435AA0 (the call @0x435BCE) -> Entity_DetachFromVehicle @0x4355F0 (the mount
// @0x43562A..0x43565F)]; the attach then mounts the gun's slot through the same
// Player_MountWeaponSlot, whose store of g_PendingWeaponSlot replaces the personal one
// [orig: Entity_AttachToUseGunSlot @0x546B80 (the call @0x546C38);
// Entity_AttachToVehicleSlot @0x4946D0 (the call @0x494838); Player_MountWeaponSlot
// @0x4DFA40 (the store @0x4DFB16)], so the
// switch-out's completion takes the gun's slot and the gun fires. Ours kept the detach's
// personal mount in flight beside the attach's and committed it at the same completion,
// handing the player's equipped def back to the rifle and drawing its first-person model
// over the gun (push 8's retail GUT, the 03TR Black Hawk minigun).

#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

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

constexpr uint8_t kGunAdm = 0;
constexpr uint8_t kRifleAdm = 1;

void bake(w::WeaponTableEntry &def) {
	w::weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, def.action_fsm);
	def.action_fsm.actions[w::weapon_action::kSwitchFrom].delay_start = 2;
	def.action_fsm.actions[w::weapon_action::kSwitchFrom].delay_end = 2;
	def.action_fsm.actions[w::weapon_action::kSwitchTo].delay_start = 2;
	def.action_fsm.actions[w::weapon_action::kSwitchTo].delay_end = 2;
	def.action_fsm.actions[w::weapon_action::kFire].delay_end = 6;
}

w::Entity carrier(uint32_t attrib, w::SeatType seat_type, const char *seat_name) {
	w::Entity e;
	e.kind = w::EntityKind::Item;
	e.has_item_def = true;
	e.has_graphic_model = true;
	e.item_attrib = attrib;
	e.item_type_index = 7;
	e.health = e.health_max = 2000;
	e.alive = true;
	e.position = {100.0f, 200.0f, 10.0f};
	w::Seat seat;
	seat.type = seat_type;
	seat.bone_index = 1;
	seat.source_name = seat_name;
	seat.seat_local = {0.5f, 1.5f, 1.0f};
	e.seats.push_back(seat);
	return e;
}

void test_a_seat_switch_onto_a_gun_fires_the_gun(w::SeatType gun_seat) {
	auto wp = std::make_unique<w::World>();
	w::World &world = *wp;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	w::WeaponTable &table = world.tables.weapons;
	table.ammo_class_names = {"CLASS_762MM", "CLASS_556MM"};
	table.ammo_class_caps = {1000, 1000};
	table.entries.resize(2);
	w::WeaponTableEntry &gun = table.entries[kGunAdm];
	gun.name = "WPN_MINIGUN";
	gun.valid = true;
	gun.category = 9;
	gun.clipsize = 100;
	gun.startrounds = 100;
	gun.flags = w::weapon_flag::kEmplaced;
	gun.ammo_class_count = 1;
	gun.ammo_class_id = 0;
	bake(gun);
	w::WeaponTableEntry &rifle = table.entries[kRifleAdm];
	rifle.name = "WPN_M4AUTO";
	rifle.valid = true;
	rifle.category = 3;
	rifle.clipsize = 30;
	rifle.startrounds = 300;
	rifle.maxclips = 10;
	rifle.ammo_class_count = 1;
	rifle.weapon_class_slot = 1;
	rifle.ammo_class_id = 1;
	bake(rifle);

	// The helicopter: a PlayerControl hull whose ctrlx lends no slot.
	const w::EntityHandle heli_h = world.registry.spawn(1,
			carrier(w::kItemAttribPlayerControl, w::SeatType::Controller, "ctrlx00"));
	// The gun: an EWeap entity with its own slot; a ctrlx on it claims +368, which
	// needs PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947CC].
	const uint32_t gun_attrib = w::kItemAttribEweap |
			(gun_seat == w::SeatType::Controller ? w::kItemAttribPlayerControl : 0u);
	w::Entity gun_seed = carrier(gun_attrib, gun_seat,
			gun_seat == w::SeatType::Gunner ? "UseGun" : "ctrlx00");
	gun_seed.primary_weapon = "WPN_MINIGUN";
	const w::EntityHandle gun_h = world.registry.spawn(1, gun_seed);
	CHECK(world.vehicles.prepare_weapon_slot(*world.registry.get(gun_h)));

	w::Entity pl;
	pl.kind = w::EntityKind::Organic;
	pl.item_type_index = 3;
	pl.position = {102.0f, 200.0f, 10.0f};
	pl.health = pl.health_max = 150;
	pl.alive = true;
	pl.flags = pl.engine_flags = w::kEntityFlagPlayer;
	const w::EntityHandle player_h = world.registry.spawn(0, pl);
	world.cached.local_player = player_h;

	w::LocalPlayer local(world);
	world.local_player_state = &local;
	local.loadout.spawn_kit = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
	local.loadout.spawn_kit_set = true;
	w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
			local.inventory_valid, /*select_spawn_default=*/true);
	const int32_t personal = local.inventory.equipped_combo;
	CHECK(personal >= 0);

	// The shell: each switch event installs the def it names.
	std::vector<std::string> installs;
	const auto install = [&](const std::string &name, bool preserve) {
		w::WeaponInstallData data;
		data.name = name;
		data.clipsize = table.entries[size_t(table.index_of(name.c_str()))].clipsize;
		data.table_baked = true;
		w::local_weapon_install(world, local.weapon, data, preserve, false, &local.inventory,
				local.view);
		installs.push_back(name);
	};
	const auto pump = [&]() {
		++world.logic_tick;
		local.pump_local_weapon();
		const std::vector<w::WeaponPresentationEvent> events = local.weapon.events;
		local.weapon.events.clear();
		for (const w::WeaponPresentationEvent &e : events)
			if (!e.switch_to_weapon.empty()) install(e.switch_to_weapon, e.preserve_slot_state);
	};
	install("WPN_M4AUTO", false);
	for (int i = 0; i < 8; ++i) pump();

	// The pilot's seat, then the gun's, as the numbered seat key requests them
	// (LocalPlayer::select_numbered_seat).
	CHECK(world.vehicles.request_attach(player_h, {heli_h, 0, w::SeatType::Controller}));
	w::sync_local_usegun_weapon_transition(world, local.weapon);
	for (int i = 0; i < 8; ++i) pump();
	installs.clear();
	CHECK(world.vehicles.request_attach(player_h, {gun_h, 0, gun_seat}));
	w::sync_local_usegun_weapon_transition(world, local.weapon);
	// The attach's mount replaced the detach's pending personal slot.
	CHECK(!local.weapon.switch_in_flight);
	CHECK(local.weapon.usegun_switch == w::LocalUseGunSwitch::kAttach);
	for (int i = 0; i < 16 && !local.weapon.usegun_slot_active; ++i) pump();
	for (int i = 0; i < 16; ++i) pump();

	const w::Entity &player = *world.registry.get(player_h);
	CHECK(player.mounted && player.mount_target == gun_h);
	CHECK(local.weapon.usegun_slot_active);
	CHECK(player.equipped_adm_index == kGunAdm);
	CHECK(local.inventory.equipped_combo == personal);
	CHECK(installs.size() == 1 && installs[0] == "WPN_MINIGUN");

	// The trigger fires the gun's own slot. The binding request reads the equipped
	// def's category (ScriptWeaponInput::record_fire_request), so a mission that
	// blocks the rifle's category, as a training script does, still lets the gun fire.
	world.script.weapon_input.set_fire_blocked(rifle.category, true);
	const w::Entity &gun_e = *world.registry.get(gun_h);
	const int32_t clip_before = gun_e.primary_weapon_slot.clip;
	local.set_weapon_input(true, true, false);
	pump();
	for (int i = 0; i < 30; ++i) {
		local.set_weapon_input(true, false, false);
		pump();
	}
	CHECK(local.weapon.fired_serial > 0);
	CHECK(gun_e.primary_weapon_slot.clip < clip_before);
	CHECK(player.equipped_adm_index == kGunAdm);
	world.local_player_state = nullptr;
}

} // namespace

int main() {
	test_a_seat_switch_onto_a_gun_fires_the_gun(w::SeatType::Gunner);
	test_a_seat_switch_onto_a_gun_fires_the_gun(w::SeatType::Controller);
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS mounted_fire_path\n");
	return 0;
}
