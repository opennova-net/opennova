// D-NET-420: the UseGun attach's +368 test, on a model of our own with a UseGun row
// beside a ctrlx row (no shipped model carries both). On the authority the attach
// succeeds only while +368 is empty or the requester's; a refusal still saves +0x308
// and points the EquippedSlot at the gun's slot, then writes nothing else, so the
// requester stays on foot holding it [orig: Entity_AttachToUseGunSlot @0x546BB6..0x546BC6,
// @0x546BCF..0x546C15, @0x546C42..0x546C4E, the success gate @0x546C56]. The weapon walk
// pumps that slot through it [orig: WeaponAction_ProcessAllEntities @0x5426AD..0x5426C1],
// the authority's next attach reads its action for the busy gate [orig:
// Entity_ProcessVehicleAttach @0x435B18..0x435B38], a client's forced attach stores +368
// over any claimant [orig: @0x546B93, @0x546C6D], and a gun whose own gunner sits in the
// UseGun seat refuses a second requester at the seat test first [orig: @0x435BA9].
// FOUND-4: any other entity leaving a seat 2 or 3 points its EquippedSlot at +0x308,
// borrowed or not [orig: Entity_DetachFromVehicle @0x435671..0x435687], and a non-player
// then drops it [orig: @0x43568D..0x435696].

#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
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
constexpr uint16_t kRifleCombo = 3 * 65;

w::WeaponSlotRef personal_ref() {
	w::WeaponSlotRef ref;
	ref.kind = w::WeaponSlotRef::kPersonal;
	ref.combo = kRifleCombo;
	ref.adm = kRifleAdm;
	return ref;
}

void bake(w::WeaponTableEntry &def) {
	w::weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, def.action_fsm);
	def.action_fsm.actions[w::weapon_action::kSwitchFrom].delay_start = 2;
	def.action_fsm.actions[w::weapon_action::kSwitchFrom].delay_end = 2;
	def.action_fsm.actions[w::weapon_action::kSwitchTo].delay_start = 2;
	def.action_fsm.actions[w::weapon_action::kSwitchTo].delay_end = 2;
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	w::EntityHandle gun_h, claimant_h, requester_h;

	// `ctrlx`: the gun carries a ctrlx row (index 0, bone 1) beside its UseGun row
	// (index 1, bone 2); without it the UseGun row is index 0, bone 2.
	explicit Rig(bool ctrlx = true) {
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(1, 8);
		w::WeaponTable &table = world.tables.weapons;
		table.ammo_class_names = {"CLASS_762MM", "CLASS_556MM"};
		table.ammo_class_caps = {1000, 1000};
		table.entries.resize(2);
		w::WeaponTableEntry &gun_def = table.entries[kGunAdm];
		gun_def.name = "WPN_MINIGUN";
		gun_def.valid = true;
		gun_def.category = 9;
		gun_def.clipsize = 100;
		gun_def.startrounds = 100;
		gun_def.flags = w::weapon_flag::kEmplaced;
		gun_def.ammo_class_count = 1;
		gun_def.ammo_class_id = 0;
		gun_def.ammo_index = 0;
		bake(gun_def);
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

		w::Entity g;
		g.kind = w::EntityKind::Item;
		g.has_item_def = true;
		g.has_graphic_model = true;
		g.item_type = 6;
		g.item_attrib = w::kItemAttribEweap | w::kItemAttribPlayerControl;
		g.health = g.health_max = 1000;
		g.alive = true;
		g.position = {100.0f, 200.0f, 10.0f};
		g.primary_weapon = "WPN_MINIGUN";
		if (ctrlx) {
			w::Seat c;
			c.type = w::SeatType::Controller;
			c.bone_index = 1;
			c.source_name = "ctrlx00";
			c.seat_local = {0.5f, 1.5f, 1.0f};
			g.seats.push_back(c);
		}
		w::Seat u;
		u.type = w::SeatType::Gunner;
		u.bone_index = 2;
		u.source_name = "UseGun";
		u.seat_local = {0.0f, -1.0f, 1.0f};
		g.seats.push_back(u);
		gun_h = world.registry.spawn(1, g);
		CHECK(world.vehicles.prepare_weapon_slot(gun()));

		claimant_h = body(false);
		requester_h = body(true);
		requester().equipped_slot = personal_ref();
	}
	w::EntityHandle body(bool player) {
		w::Entity e;
		e.kind = w::EntityKind::Organic;
		e.item_type_index = 3;
		e.position = {102.0f, 200.0f, 10.0f};
		e.health = e.health_max = 150;
		e.alive = true;
		if (player) e.flags = e.engine_flags = w::kEntityFlagPlayer;
		const w::EntityHandle h = world.registry.spawn(0, e);
		w::AiEntity *ae = world.ai.at(world.ai.attach(h));
		ae->health = 150;
		ae->inf.active = true;
		return h;
	}
	w::Entity &gun() { return *world.registry.get(gun_h); }
	w::Entity &claimant() { return *world.registry.get(claimant_h); }
	w::Entity &requester() { return *world.registry.get(requester_h); }
	int use_gun_index() const { return static_cast<int>(world.registry.get(gun_h)->seats.size()) - 1; }
	bool attach(w::EntityHandle who, int seat) {
		return world.vehicles.attach_to_seat(
				who, {gun_h, seat, gun().seats[static_cast<size_t>(seat)].type});
	}
};

void test_the_authority_refuses_a_held_gun_and_borrows_its_slot() {
	Rig r;
	// The claimant takes the ctrlx seat: +368 is its, and it borrows the gun's slot.
	CHECK(r.attach(r.claimant_h, 0));
	CHECK(r.gun().primary_occupant == r.claimant_h);
	CHECK(r.claimant().equipped_slot.names_mount(r.gun_h));
	// The requester asks for the free UseGun seat: refused, on foot, no relation.
	CHECK(!r.attach(r.requester_h, r.use_gun_index()));
	CHECK(!r.requester().mounted);
	CHECK(!r.gun().seats[static_cast<size_t>(r.use_gun_index())].occupant.valid());
	CHECK(r.gun().primary_occupant == r.claimant_h);
	CHECK(r.gun().primary_weapon_owner == r.claimant_h);
	CHECK(!r.requester().use_gun_slot_swapped);
	// ... but it saved +0x308 and holds the gun's slot.
	CHECK(r.requester().selected_slot == personal_ref());
	CHECK(r.requester().equipped_slot.names_mount(r.gun_h));

	// The weapon walk pumps that slot through it. The claimant leaves first (a
	// non-player drops its EquippedSlot), so the requester's is the slot's one pass:
	// an idle slot's kick clears.
	CHECK(r.world.vehicles.detach(r.claimant_h));
	CHECK(r.claimant().equipped_slot.kind == w::WeaponSlotRef::kNone);
	r.gun().primary_weapon_slot.kick = 4;
	r.world.pump_weapon_actions();
	CHECK(r.gun().primary_weapon_slot.kick == 0);

	// The busy gate reads the held slot's action on the requester's next attach.
	w::Entity other;
	other.kind = w::EntityKind::Item;
	other.has_item_def = true;
	other.has_graphic_model = true;
	other.health = 500;
	other.alive = true;
	other.position = {104.0f, 200.0f, 10.0f};
	w::Seat sit;
	sit.type = w::SeatType::Passenger;
	sit.bone_index = 1;
	sit.source_name = "sitex00";
	other.seats.push_back(sit);
	const w::EntityHandle other_h = r.world.registry.spawn(1, other);
	r.gun().primary_weapon_slot.current = w::weapon_action::kFire;
	CHECK(!r.world.vehicles.attach_to_seat(r.requester_h, {other_h, 0, w::SeatType::Passenger}));
	CHECK(!r.requester().mounted);
	r.gun().primary_weapon_slot.current = w::weapon_action::kOverheated;
	CHECK(r.world.vehicles.attach_to_seat(r.requester_h, {other_h, 0, w::SeatType::Passenger}));
	CHECK(r.requester().mounted);
	// A passenger seat leaves the held slot alone, and so does its detach.
	CHECK(r.world.vehicles.detach(r.requester_h));
	CHECK(r.requester().equipped_slot.names_mount(r.gun_h));
}

void test_a_client_forces_the_gun_claim() {
	Rig r;
	CHECK(r.attach(r.claimant_h, 0));
	// The decoded relation seats the requester and stores +368 over the claimant;
	// the next pass finds the seat unchanged.
	CHECK(r.world.vehicles.apply_confirmed_mount(r.requester_h, r.gun_h, 2));
	CHECK(r.requester().mounted && r.requester().mount_target == r.gun_h);
	CHECK(r.gun().primary_occupant == r.requester_h);
	CHECK(!r.world.vehicles.apply_confirmed_mount(r.requester_h, r.gun_h, 2));
}

void test_a_lone_guns_gunner_refuses_at_the_seat_first() {
	Rig r(false);
	CHECK(r.attach(r.claimant_h, 0));
	CHECK(r.gun().primary_occupant == r.claimant_h);
	CHECK(!r.attach(r.requester_h, 0));
	CHECK(r.requester().equipped_slot == personal_ref());
	CHECK(r.requester().selected_slot.kind == w::WeaponSlotRef::kNone);
}

void test_a_seat_detach_points_the_slot_at_the_saved_one() {
	{
		// An unarmed ctrlx borrows nothing; its detach still stores +0x308, null
		// when nothing saved one.
		Rig r;
		r.gun().item_attrib = w::kItemAttribPlayerControl;
		CHECK(r.attach(r.requester_h, 0));
		CHECK(r.requester().equipped_slot == personal_ref());
		CHECK(r.world.vehicles.detach(r.requester_h));
		CHECK(r.requester().equipped_slot.kind == w::WeaponSlotRef::kNone);
	}
	{
		// An armed ctrlx saves the personal slot and the detach restores it.
		Rig r;
		CHECK(r.attach(r.requester_h, 0));
		CHECK(r.requester().equipped_slot.names_mount(r.gun_h));
		CHECK(r.requester().selected_slot == personal_ref());
		CHECK(r.world.vehicles.detach(r.requester_h));
		CHECK(r.requester().equipped_slot == personal_ref());
	}
	{
		// A non-player drops its EquippedSlot after the store.
		Rig r;
		r.claimant().equipped_slot = personal_ref();
		CHECK(r.attach(r.claimant_h, 0));
		CHECK(r.world.vehicles.detach(r.claimant_h));
		CHECK(r.claimant().equipped_slot.kind == w::WeaponSlotRef::kNone);
	}
}

// The local player's refusal takes the same direct store: no switch, the borrowed
// slot held on foot until a mount's commit replaces it.
void test_the_local_player_holds_a_refused_gun_on_foot() {
	Rig r;
	w::World &world = r.world;
	CHECK(r.attach(r.claimant_h, 0));
	world.cached.local_player = r.requester_h;
	w::LocalPlayer local(world);
	world.local_player_state = &local;
	local.loadout.spawn_kit = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
	local.loadout.spawn_kit_set = true;
	w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
			local.inventory_valid, /*select_spawn_default=*/true);
	const int32_t personal = local.inventory.equipped_combo;
	CHECK(personal >= 0);
	local.inventory.selected_combo = -1;
	local.weapon.events.clear();

	CHECK(!r.attach(r.requester_h, r.use_gun_index()));
	CHECK(!r.requester().mounted);
	CHECK(local.inventory.selected_combo == personal);
	CHECK(local.weapon.usegun_slot_active && local.weapon.usegun_on_foot);
	CHECK(local.weapon.usegun_mount == r.gun_h);
	CHECK(local.weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
	CHECK(local.weapon.events.size() == 1 &&
			local.weapon.events[0].switch_to_weapon == "WPN_MINIGUN");
	// No seat to leave: the transition keeps it.
	w::sync_local_usegun_weapon_transition(world, local.weapon);
	CHECK(local.weapon.usegun_slot_active && local.weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
	// A personal mount's commit replaces it.
	local.inventory.pending_combo = personal;
	w::commit_pending_weapon_switch(world, local.weapon, &local.inventory);
	CHECK(!local.weapon.usegun_slot_active && !local.weapon.usegun_on_foot);
	world.local_player_state = nullptr;
}

// The real pump ends that hold: a category key mounts the personal slot over the gun's
// (the EquippedSlot's def, category 9, against the rifle's 3: SWITCHFROM queued on the
// gun's slot), and its completion stores g_PendingWeaponSlot as the EquippedSlot and
// draws the rifle [orig: Player_SwitchToWeaponByHandle @0x4E0170 (the compare
// @0x4E0271); Player_MountWeaponSlot @0x4DFA40 (@0x4DFB16, @0x4DFB9E);
// WeaponAction_SwitchFrom @0x5433B0 (the local completion @0x543464..0x5434A3)].
void test_a_weapon_key_ends_a_slot_held_on_foot() {
	Rig r;
	w::World &world = r.world;
	CHECK(r.attach(r.claimant_h, 0));
	world.cached.local_player = r.requester_h;
	w::LocalPlayer local(world);
	world.local_player_state = &local;
	local.loadout.spawn_kit = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
	local.loadout.spawn_kit_set = true;
	w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
			local.inventory_valid, /*select_spawn_default=*/true);
	const int32_t personal = local.inventory.equipped_combo;
	CHECK(personal == kRifleCombo);
	// The shell: each switch event installs the def it names.
	const auto install = [&](const std::string &name, bool preserve) {
		w::WeaponInstallData data;
		data.name = name;
		data.clipsize = world.tables.weapons.entries[size_t(
				world.tables.weapons.index_of(name.c_str()))].clipsize;
		data.table_baked = true;
		w::local_weapon_install(world, local.weapon, data, preserve, false,
				&local.inventory, local.view);
	};
	std::vector<w::WeaponPresentationEvent> seen;
	const auto pump = [&]() {
		++world.logic_tick;
		local.pump_local_weapon();
		const std::vector<w::WeaponPresentationEvent> events = local.weapon.events;
		local.weapon.events.clear();
		for (const w::WeaponPresentationEvent &e : events) {
			seen.push_back(e);
			if (!e.switch_to_weapon.empty()) install(e.switch_to_weapon, e.preserve_slot_state);
		}
	};
	install("WPN_M4AUTO", false);
	for (int i = 0; i < 8; ++i) pump();

	CHECK(!r.attach(r.requester_h, r.use_gun_index()));
	CHECK(local.weapon.usegun_slot_active && local.weapon.usegun_on_foot);
	for (int i = 0; i < 4; ++i) pump();
	CHECK(local.weapon.usegun_on_foot);
	seen.clear();

	// The rifle's key: the walk reads the gun's def as the EquippedSlot's.
	const w::WeaponSwitchOutcome out = w::weapon_switch_to_handle(world.tables.weapons,
			local.inventory, kRifleCombo,
			w::local_weapon_switch_gates(world, local.weapon, &local.inventory));
	CHECK(out.kind == w::WeaponSwitchOutcome::kMount && !out.same_category);
	w::handle_weapon_switch_outcome(world, local.weapon, &local.inventory, out, local.view);
	CHECK(local.weapon.switch_in_flight);
	CHECK(r.gun().primary_weapon_slot.next == w::weapon_action::kSwitchFrom);
	for (int i = 0; i < 32 && local.weapon.usegun_on_foot; ++i) pump();
	CHECK(!local.weapon.usegun_on_foot && !local.weapon.usegun_slot_active);
	CHECK(!local.weapon.switch_in_flight);
	CHECK(local.inventory.equipped_combo == personal);
	CHECK(r.requester().equipped_adm_index == kRifleAdm);
	// The completed SWITCHFROM draws the rifle (a fresh slot entering SWITCHTO).
	bool drew_rifle = false;
	for (const w::WeaponPresentationEvent &e : seen)
		if (e.switch_to_weapon == "WPN_M4AUTO" && !e.preserve_slot_state) drew_rifle = true;
	CHECK(drew_rifle);
	CHECK(local.weapon.slot.next == w::weapon_action::kSwitchTo ||
			local.weapon.slot.current == w::weapon_action::kSwitchTo);
	world.local_player_state = nullptr;
}

// D-NET-336 (the walk's leg): a reload the authority's walk begins on another row's
// slot queues its C2S 0x25 for the host's own dispatcher, which broadcasts S2C 0x49:
// a UseGun gunner's addresses the gun (an EWeap: 0xFFFF), a slot held on foot the
// holder with the slot's category * 65 + rank. [orig: WeaponAction_Reload
// @0x5430B6..0x543108; NetPacket_SendReloadRequest @0x43296C..0x432974]
bool pump_until_reload(w::World &world) {
	for (int i = 0; i < 200 && world.out.weapon_reloads.empty(); ++i) {
		++world.logic_tick;
		world.pump_weapon_actions();
	}
	return !world.out.weapon_reloads.empty();
}

void test_the_walk_relays_an_authority_reload() {
	{
		Rig r(false);
		CHECK(r.attach(r.claimant_h, 0)); // the NPC gunner on the lone gun
		r.gun().primary_weapon_slot.clip = 0;
		r.gun().primary_weapon_slot.reserve = 100;
		CHECK(pump_until_reload(r.world));
		CHECK(r.world.out.weapon_reloads.size() == 1);
		if (!r.world.out.weapon_reloads.empty()) {
			CHECK(r.world.out.weapon_reloads[0].entity_handle == r.gun_h.packed);
			CHECK(r.world.out.weapon_reloads[0].reload_param == 0xFFFF);
		}
	}
	{
		Rig r;
		CHECK(r.attach(r.claimant_h, 0));
		CHECK(!r.attach(r.requester_h, r.use_gun_index())); // held on foot
		CHECK(r.world.vehicles.detach(r.claimant_h));
		r.gun().primary_weapon_slot.clip = 0;
		r.gun().primary_weapon_slot.reserve = 100;
		CHECK(pump_until_reload(r.world));
		if (!r.world.out.weapon_reloads.empty()) {
			CHECK(r.world.out.weapon_reloads[0].entity_handle == r.requester_h.packed);
			CHECK(r.world.out.weapon_reloads[0].reload_param == 9 * 65);
		}
	}
	{
		// A client's walk relays nothing.
		Rig r(false);
		r.world.ai.is_authority = false;
		CHECK(r.world.vehicles.apply_confirmed_mount(r.claimant_h, r.gun_h, 2));
		r.gun().primary_weapon_slot.clip = 0;
		r.gun().primary_weapon_slot.reserve = 100;
		CHECK(!pump_until_reload(r.world));
	}
}

// The same-tick order: each reload queues at its row's visit in the pool-0 walk, the
// local player's at its own slot, so a gunner at a lower slot (a mission NPC loaded ahead
// of the host's player) relays first and the S2C 0x49 pair leaves in that order. Ours sent
// the local player's ahead of the walk's. [orig: WeaponAction_ProcessAllEntities
// @0x5426A6..0x5426C9; WeaponAction_Reload @0x5430B0 (@0x5430FF / @0x543103) ->
// NetPacket_SendReloadRequest @0x432930 (the queue @0x4329B7)]
void test_the_walk_queues_reloads_in_slot_order() {
	Rig r(false);
	w::World &world = r.world;
	CHECK(r.attach(r.claimant_h, 0)); // the NPC gunner, pool-0 slot 0
	CHECK(r.claimant_h.slot() < r.requester_h.slot());
	world.cached.local_player = r.requester_h;
	w::LocalPlayer local(world);
	world.local_player_state = &local;
	local.loadout.spawn_kit = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
	local.loadout.spawn_kit_set = true;
	w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
			local.inventory_valid, /*select_spawn_default=*/true);
	w::WeaponInstallData data;
	data.name = "WPN_M4AUTO";
	data.clipsize = world.tables.weapons.entries[kRifleAdm].clipsize;
	data.table_baked = true;
	w::local_weapon_install(world, local.weapon, data, false, false, &local.inventory,
			local.view);
	// Both slots enter RELOAD on the walk's next pass.
	const auto stage = [](w::WeaponSlotState &slot) {
		slot.current = w::weapon_action::kIdle;
		slot.next = w::weapon_action::kReload;
		slot.counter = 0;
		slot.phase = w::weapon_phase::kDone;
		slot.clip = 0;
		slot.reserve = 100;
	};
	stage(r.gun().primary_weapon_slot);
	stage(local.weapon.slot);
	++world.logic_tick;
	world.pump_weapon_actions();
	CHECK(world.out.weapon_reloads.size() == 2);
	if (world.out.weapon_reloads.size() == 2) {
		CHECK(world.out.weapon_reloads[0].entity_handle == r.gun_h.packed);
		CHECK(world.out.weapon_reloads[0].reload_param == 0xFFFF);
		CHECK(world.out.weapon_reloads[1].entity_handle == r.requester_h.packed);
		CHECK(world.out.weapon_reloads[1].reload_param == kRifleCombo);
	}
	CHECK(!local.last_reload.valid);
	world.local_player_state = nullptr;
}

} // namespace

int main() {
	test_the_authority_refuses_a_held_gun_and_borrows_its_slot();
	test_a_client_forces_the_gun_claim();
	test_a_lone_guns_gunner_refuses_at_the_seat_first();
	test_a_seat_detach_points_the_slot_at_the_saved_one();
	test_the_local_player_holds_a_refused_gun_on_foot();
	test_a_weapon_key_ends_a_slot_held_on_foot();
	test_the_walk_relays_an_authority_reload();
	test_the_walk_queues_reloads_in_slot_order();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS usegun_refusal\n");
	return 0;
}
