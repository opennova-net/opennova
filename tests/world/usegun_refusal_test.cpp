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
	local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
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

// D-WPN-52's unarmed attach: a local player holding no EquippedSlot (a kit with no
// selectable slot: the spawn's select found none) takes the attach's direct leg on a
// UseGun seat (in a session, which admits it) and on an EWeap carrier's ctrlx seat
// alike. +0x308 takes the null slot and the EquippedSlot the gun's slot at once: no
// mount, so no switch on either slot, and no AdmDef byte stamp; the first-person model
// follows the slot's def. The seat detach then mounts the current slot word's slot,
// which holds no def, so the mount returns at once and the player leaves holding the
// gun's slot on foot.
// [orig: Entity_AttachToUseGunSlot @0x546B80 (the null test @0x546C23..0x546C25, the
//  direct leg @0x546C42..0x546C4E, +0x308 @0x546C15 / @0x546C48);
//  Entity_AttachToVehicleSlot @0x4946D0 (@0x49481D..0x494825, @0x494867..0x494898);
//  Entity_DetachFromVehicle @0x4355F0 (the fallback @0x435650..0x435658, the mount
//  @0x43565F, no local restore @0x435669..0x43566F); Player_MountWeaponSlot @0x4DFA40
//  (the target def @0x4DFA53..0x4DFA58)]
void test_an_unarmed_attach_takes_the_direct_leg() {
	for (const bool use_gun : {true, false}) {
		Rig r;
		w::World &world = r.world;
		world.rules.mp_session = true;
		world.cached.local_player = r.requester_h;
		w::LocalPlayer local(world);
		world.local_player_state = &local;
		// The spawn's rebuild over an empty kit: the select finds no slot.
		local.loadout.spawn_kit().clear();
		w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
				local.inventory_valid, /*select_spawn_default=*/true);
		CHECK(local.inventory_valid && !local.equipped_slot_held());
		const uint8_t adm_before = r.requester().equipped_adm_index;
		const int seat = use_gun ? r.use_gun_index() : 0;
		CHECK(r.attach(r.requester_h, seat));
		CHECK(r.requester().mounted && r.requester().use_gun_slot_swapped);
		w::sync_local_usegun_weapon_transition(world, local.weapon);
		const w::LocalPlayerWeapon &weapon = local.weapon;
		CHECK(weapon.usegun_slot_active && !weapon.usegun_on_foot);
		CHECK(weapon.usegun_mount == r.gun_h && weapon.usegun_weapon_adm == kGunAdm);
		CHECK(weapon.usegun_switch == w::LocalUseGunSwitch::kNone &&
				weapon.usegun_switch_action == -1);
		CHECK(weapon.usegun_saved_adm == 0xFF);
		CHECK(local.inventory.selected_combo == -1 &&
				local.inventory.selected_mount == w::EntityHandle::kInvalid);
		CHECK(r.requester().equipped_adm_index == adm_before);
		CHECK(r.gun().primary_weapon_slot.current != w::weapon_action::kSwitchTo &&
				r.gun().primary_weapon_slot.next != w::weapon_action::kSwitchTo);
		CHECK(!weapon.events.empty() && weapon.events.back().switch_to_weapon == "WPN_MINIGUN" &&
				weapon.events.back().preserve_slot_state);
		CHECK(local.equipped_slot_held());

		// The shell installs the gun's def; the detach finds no slot to mount.
		w::WeaponInstallData data;
		data.name = "WPN_MINIGUN";
		data.clipsize = world.tables.weapons.entries[kGunAdm].clipsize;
		data.table_baked = true;
		w::local_weapon_install(world, local.weapon, data, true, false, &local.inventory,
				local.view);
		CHECK(local.weapon.active);
		CHECK(world.vehicles.detach(r.requester_h));
		CHECK(!r.requester().mounted);
		w::sync_local_usegun_weapon_transition(world, local.weapon);
		CHECK(weapon.usegun_slot_active && weapon.usegun_on_foot);
		CHECK(weapon.usegun_mount == r.gun_h);
		CHECK(weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
		CHECK(r.requester().equipped_adm_index == adm_before);
		world.local_player_state = nullptr;
	}
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
	local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
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

// D-WPN-48's borrowed-gun edge: the player holds gun A's slot on foot (a refused
// UseGun attach), then takes EWeap carrier B's ctrlx seat. The attach saves the
// EquippedSlot, A's slot, at +0x308 (it is not B's own slot) before the mount of B's
// [orig: Entity_AttachToVehicleSlot @0x4946D0 (the EWeap gate @0x49480B, the local
// test @0x494815, the EquippedSlot @0x49481D..0x494825, not the carrier's slot
// @0x494827..0x49482F, the save @0x494831, the mount @0x494838)]. Leaving B mounts
// +0x308 when its def's category is below 11, else the current personal slot
// [orig: Entity_DetachFromVehicle @0x4355F0 (the local gate @0x435616..0x43561C,
// parentSlot 3 / 2 @0x43562A..0x435638, +0x308 @0x43563A, the category test
// @0x43564B..0x43564E, the fallback @0x435650..0x435658, the mount @0x43565F)]: the
// player ends on foot holding A's slot, and the completion stamps A's def into the
// AdmDef byte the C2S 0x06 and 0x0C carry [orig: WeaponAction_SwitchFrom @0x5433B0
// (@0x543475, @0x543496)]. Every select's store replaces the saved gun [orig:
// Player_SelectWeaponSlot @0x4DD680 (@0x4DD6DA)].
void test_a_ctrlx_seat_saves_a_gun_held_on_foot() {
	constexpr uint8_t kCannonAdm = 2;
	for (const bool low_category : {true, false}) {
		Rig r;
		w::World &world = r.world;
		world.tables.weapons.entries.resize(3);
		w::WeaponTableEntry &cannon = world.tables.weapons.entries[kCannonAdm];
		cannon.name = "WPN_CANNON";
		cannon.valid = true;
		cannon.category = 10;
		cannon.clipsize = 10;
		cannon.startrounds = 10;
		cannon.ammo_class_count = 1;
		cannon.ammo_class_id = 0;
		cannon.ammo_index = 0;
		bake(cannon);
		if (!low_category) world.tables.weapons.entries[kGunAdm].category = 11;
		w::Entity b;
		b.kind = w::EntityKind::Item;
		b.has_item_def = true;
		b.has_graphic_model = true;
		b.item_type = 6;
		b.item_attrib = w::kItemAttribEweap | w::kItemAttribPlayerControl;
		b.health = b.health_max = 1000;
		b.alive = true;
		b.position = {106.0f, 200.0f, 10.0f};
		b.primary_weapon = "WPN_CANNON";
		w::Seat c;
		c.type = w::SeatType::Controller;
		c.bone_index = 1;
		c.source_name = "ctrlx00";
		c.seat_local = {0.5f, 1.5f, 1.0f};
		b.seats.push_back(c);
		const w::EntityHandle b_h = world.registry.spawn(1, b);
		CHECK(world.vehicles.prepare_weapon_slot(*world.registry.get(b_h)));

		CHECK(r.attach(r.claimant_h, 0)); // the claimant holds A's +368
		world.cached.local_player = r.requester_h;
		w::LocalPlayer local(world);
		world.local_player_state = &local;
		local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
		w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
				local.inventory_valid, /*select_spawn_default=*/true);
		const auto install = [&](const std::string &name, bool preserve) {
			w::WeaponInstallData data;
			data.name = name;
			data.clipsize = world.tables.weapons.entries[size_t(
					world.tables.weapons.index_of(name.c_str()))].clipsize;
			data.table_baked = true;
			w::local_weapon_install(world, local.weapon, data, preserve, false,
					&local.inventory, local.view);
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

		// Refused at A's UseGun seat: A's slot is held on foot.
		CHECK(!r.attach(r.requester_h, r.use_gun_index()));
		for (int i = 0; i < 4; ++i) pump();
		CHECK(local.weapon.usegun_on_foot && local.weapon.usegun_mount == r.gun_h);
		CHECK(local.inventory.selected_mount == w::EntityHandle::kInvalid);

		// B's ctrlx seat: +0x308 takes A's slot; the mount borrows B's.
		CHECK(world.vehicles.attach_to_seat(r.requester_h, {b_h, 0, w::SeatType::Controller}));
		CHECK(local.inventory.selected_mount == r.gun_h.packed);
		for (int i = 0; i < 32 && local.weapon.usegun_mount != b_h; ++i) pump();
		CHECK(local.weapon.usegun_slot_active && !local.weapon.usegun_on_foot);
		CHECK(local.weapon.usegun_mount == b_h);
		CHECK(r.requester().equipped_adm_index == kCannonAdm);

		// Leaving B mounts +0x308: A's slot (category 9), held on foot; or, a def of
		// category 11 there, the current personal slot.
		CHECK(world.vehicles.detach(r.requester_h));
		for (int i = 0; i < 32 && (local.weapon.usegun_switch != w::LocalUseGunSwitch::kNone ||
				local.weapon.usegun_mount == b_h); ++i)
			pump();
		CHECK(local.weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
		if (low_category) {
			CHECK(local.weapon.usegun_slot_active && local.weapon.usegun_on_foot);
			CHECK(local.weapon.usegun_mount == r.gun_h);
			CHECK(r.requester().equipped_adm_index == kGunAdm);
			// Held on foot: the next pumps leave it there.
			for (int i = 0; i < 8; ++i) pump();
			CHECK(local.weapon.usegun_on_foot && local.weapon.usegun_mount == r.gun_h);
		} else {
			CHECK(!local.weapon.usegun_slot_active && !local.weapon.usegun_on_foot);
			CHECK(r.requester().equipped_adm_index == kRifleAdm);
		}
		// A select's store replaces the saved gun.
		local.inventory.selected_mount = r.gun_h.packed;
		CHECK(w::weapon_select_slot(world.tables.weapons, local.inventory, kRifleCombo, false));
		CHECK(local.inventory.selected_mount == w::EntityHandle::kInvalid);
		world.local_player_state = nullptr;
	}
}

// The borrowed seat's detach mounts slot[g_CurrentWeaponSlot] when +0x308 holds no
// def of category < 11, whatever personal def the attach left in the AdmDef byte. The
// player holds gun A's slot on foot (category 11) and takes carrier B's ctrlx seat, so
// +0x308 names A's slot. With a kit whose spawn select missed slot 195 (a pistol alone),
// the current slot word stays 195 and holds no def: the mount returns at once, no local
// restore follows, and the player leaves holding B's slot on foot. With the word naming
// the pistol while the rifle was equipped, the switch back draws the pistol and its
// completion makes the pistol's slot the EquippedSlot.
// [orig: Entity_DetachFromVehicle @0x4355F0 (+0x308 @0x43563A, its def
//  @0x435644..0x435649, the category test @0x43564B..0x43564E, the fallback
//  @0x435650..0x435658, the mount @0x43565F, no local restore @0x435669..0x43566F);
//  Player_InitPlayer @0x4E15F0 (g_CurrentWeaponSlot = 195 @0x4E17FF);
//  Player_MountWeaponSlot @0x4DFA40 (the target def @0x4DFA53..0x4DFA58,
//  g_PendingWeaponSlot @0x4DFB16); WeaponAction_SwitchFrom @0x5433B0 (@0x543475)]
void test_a_borrowed_seat_detach_mounts_the_current_slot() {
	constexpr uint8_t kCannonAdm = 2;
	constexpr uint8_t kPistolAdm = 3;
	constexpr int32_t kPistolCombo = 2 * 65;
	for (const bool pistol_only : {true, false}) {
		Rig r;
		w::World &world = r.world;
		world.tables.weapons.entries.resize(4);
		w::WeaponTableEntry &cannon = world.tables.weapons.entries[kCannonAdm];
		cannon.name = "WPN_CANNON";
		cannon.valid = true;
		cannon.category = 10;
		cannon.clipsize = 10;
		cannon.startrounds = 10;
		cannon.ammo_class_count = 1;
		cannon.ammo_class_id = 0;
		cannon.ammo_index = 0;
		bake(cannon);
		w::WeaponTableEntry &pistol = world.tables.weapons.entries[kPistolAdm];
		pistol.name = "WPN_M9";
		pistol.valid = true;
		pistol.category = 2;
		pistol.clipsize = 15;
		pistol.startrounds = 150;
		pistol.maxclips = 10;
		pistol.ammo_class_count = 1;
		pistol.weapon_class_slot = 1;
		pistol.ammo_class_id = 1;
		bake(pistol);
		world.tables.weapons.entries[kGunAdm].category = 11;
		w::Entity b;
		b.kind = w::EntityKind::Item;
		b.has_item_def = true;
		b.has_graphic_model = true;
		b.item_type = 6;
		b.item_attrib = w::kItemAttribEweap | w::kItemAttribPlayerControl;
		b.health = b.health_max = 1000;
		b.alive = true;
		b.position = {106.0f, 200.0f, 10.0f};
		b.primary_weapon = "WPN_CANNON";
		w::Seat c;
		c.type = w::SeatType::Controller;
		c.bone_index = 1;
		c.source_name = "ctrlx00";
		c.seat_local = {0.5f, 1.5f, 1.0f};
		b.seats.push_back(c);
		const w::EntityHandle b_h = world.registry.spawn(1, b);
		CHECK(world.vehicles.prepare_weapon_slot(*world.registry.get(b_h)));

		CHECK(r.attach(r.claimant_h, 0)); // the claimant holds A's +368
		world.cached.local_player = r.requester_h;
		w::LocalPlayer local(world);
		world.local_player_state = &local;
		if (pistol_only)
			local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M9", -1, -1, -1}};
		else
			local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1},
					w::WeaponKitEntry{"WPN_M9", -1, -1, -1}};
		w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
				local.inventory_valid, /*select_spawn_default=*/true);
		CHECK(local.inventory.current_slot == 195);
		CHECK(local.inventory.equipped_combo == (pistol_only ? kPistolCombo : kRifleCombo));
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
		install(pistol_only ? "WPN_M9" : "WPN_M4AUTO", false);
		for (int i = 0; i < 8; ++i) pump();

		// Refused at A's UseGun seat: A's slot is held on foot. B's ctrlx seat then
		// saves A's slot at +0x308 and borrows B's.
		CHECK(!r.attach(r.requester_h, r.use_gun_index()));
		for (int i = 0; i < 4; ++i) pump();
		CHECK(local.weapon.usegun_on_foot && local.weapon.usegun_mount == r.gun_h);
		CHECK(world.vehicles.attach_to_seat(r.requester_h, {b_h, 0, w::SeatType::Controller}));
		CHECK(local.inventory.selected_mount == r.gun_h.packed);
		for (int i = 0; i < 32 && local.weapon.usegun_mount != b_h; ++i) pump();
		CHECK(local.weapon.usegun_slot_active && !local.weapon.usegun_on_foot);
		CHECK(local.weapon.usegun_mount == b_h);
		CHECK(r.requester().equipped_adm_index == kCannonAdm);
		// The word names the pistol's slot while the rifle's was the personal one.
		if (!pistol_only) local.inventory.current_slot = kPistolCombo;

		CHECK(world.vehicles.detach(r.requester_h));
		seen.clear();
		pump(); // the transition stages the switch back, if any
		for (int i = 0; i < 32 && local.weapon.usegun_switch != w::LocalUseGunSwitch::kNone; ++i)
			pump();
		for (int i = 0; i < 8; ++i) pump();
		CHECK(local.weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
		if (pistol_only) {
			// Slot 195 holds no def: B's slot stays the EquippedSlot, held on foot.
			CHECK(local.weapon.usegun_slot_active && local.weapon.usegun_on_foot);
			CHECK(local.weapon.usegun_mount == b_h);
			CHECK(r.requester().equipped_adm_index == kCannonAdm);
			CHECK(local.inventory.equipped_combo == kPistolCombo);
			for (const w::WeaponPresentationEvent &e : seen)
				CHECK(e.switch_to_weapon.empty() && !e.clear_weapon);
		} else {
			CHECK(!local.weapon.usegun_slot_active && !local.weapon.usegun_on_foot);
			CHECK(r.requester().equipped_adm_index == kPistolAdm);
			CHECK(local.inventory.equipped_combo == kPistolCombo);
			bool drew_pistol = false;
			for (const w::WeaponPresentationEvent &e : seen)
				if (e.switch_to_weapon == "WPN_M9") drew_pistol = true;
			CHECK(drew_pistol);
		}
		world.local_player_state = nullptr;
	}
}

// The detach's switch-back completion stores g_PendingWeaponSlot whenever it is set,
// whatever the slot holds by then: the slot the detach staged, one a later mount's
// store put over it (a weapon key during the switch back), or a slot a grant's rebuild
// emptied. The AdmDef byte takes the stored slot's def as it stands, none giving 0xFF.
// [orig: WeaponAction_SwitchFrom @0x5433B0 (the non-null test @0x54346C..0x543473,
//  the store @0x543475, the stamp @0x543481..0x543496); AdmDef_GetIndexFromPtr
//  @0x53FE60 (-1 for a null def); Player_MountWeaponSlot @0x4DFA40
//  (g_PendingWeaponSlot @0x4DFB16)]
void test_the_detach_completion_stores_the_pending_slot_as_it_stands() {
	constexpr uint8_t kPistolAdm = 2;
	constexpr uint8_t kShotgunAdm = 3;
	constexpr int32_t kPistolCombo = 2 * 65;
	constexpr int32_t kShotgunCombo = 4 * 65;
	enum class Case { kAsStaged, kRestaged, kEmptied };
	for (const Case c : {Case::kAsStaged, Case::kRestaged, Case::kEmptied}) {
		Rig r;
		w::World &world = r.world;
		world.tables.weapons.entries.resize(4);
		w::WeaponTableEntry &pistol = world.tables.weapons.entries[kPistolAdm];
		pistol.name = "WPN_M9";
		pistol.valid = true;
		pistol.category = 2;
		w::WeaponTableEntry &shotgun = world.tables.weapons.entries[kShotgunAdm];
		shotgun.name = "WPN_SHOTGUN";
		shotgun.valid = true;
		shotgun.category = 4;
		world.cached.local_player = r.requester_h;
		auto local = std::make_unique<w::LocalPlayer>(world);
		world.local_player_state = local.get();
		w::WeaponInventory &inventory = local->inventory;
		inventory.reset(world.tables.weapons);
		local->inventory_valid = true;
		inventory.slots[kRifleCombo].adm_index = kRifleAdm;
		inventory.slots[kPistolCombo].adm_index = kPistolAdm;
		inventory.slots[kShotgunCombo].adm_index = kShotgunAdm;
		inventory.equipped_combo = kRifleCombo; // the personal slot the attach left
		// The borrow on the gun, and the switch back the seat's detach staged: its
		// mount stored the pistol's slot.
		w::LocalPlayerWeapon &weapon = local->weapon;
		weapon.active = true;
		weapon.usegun_slot_active = true;
		weapon.usegun_mount = r.gun_h;
		weapon.usegun_weapon_adm = kGunAdm;
		weapon.usegun_switch = w::LocalUseGunSwitch::kDetach;
		weapon.usegun_switch_action = w::weapon_action::kSwitchRank;
		inventory.pending_combo = kPistolCombo;
		weapon.usegun_saved_adm = kPistolAdm;
		if (c == Case::kRestaged) inventory.pending_combo = kShotgunCombo;
		if (c == Case::kEmptied) inventory.slots[kPistolCombo].adm_index = -1;
		weapon.events.clear();

		w::commit_local_usegun_weapon_switch(world, weapon);
		CHECK(weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
		CHECK(!weapon.usegun_slot_active);
		switch (c) {
			case Case::kAsStaged:
				CHECK(inventory.equipped_combo == kPistolCombo);
				CHECK(r.requester().equipped_adm_index == kPistolAdm);
				CHECK(!weapon.events.empty() && weapon.events.back().switch_to_weapon == "WPN_M9");
				break;
			case Case::kRestaged:
				CHECK(inventory.equipped_combo == kShotgunCombo);
				CHECK(r.requester().equipped_adm_index == kShotgunAdm);
				CHECK(!weapon.events.empty() &&
						weapon.events.back().switch_to_weapon == "WPN_SHOTGUN");
				break;
			case Case::kEmptied:
				CHECK(inventory.equipped_combo == kPistolCombo);
				CHECK(r.requester().equipped_adm_index == 0xFF);
				CHECK(!weapon.events.empty() && weapon.events.back().clear_weapon);
				break;
		}
		world.local_player_state = nullptr;
	}
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

// D-NET-NEW-L7-4: the walk's request reaches the host's dispatcher only through the
// authority's own client connection, which a dedicated host never creates; there the
// queue sends nothing, no S2C 0x49 echo comes and an NPC gunner's gun is never
// refilled: its RELOAD cycles, a request each cycle, and the clip stays empty. A
// playing authority (single player, a listen host) refills the gun as its echo does.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 (is_authority and is_mp_session_peer
// @0x4C9B6A..0x4C9B76, the store @0x4C9BA6); CNapiNetwork_QueueReliableMessage
// @0x4C4FA0 (@0x4C4FA6..0x4C4FA8); NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0
// (the def test @0x42C103, the refill @0x42C116)]
void test_a_dedicated_host_refills_no_gun() {
	for (const bool peer : {true, false}) {
		Rig r(false);
		r.world.rules.mp_session_peer = peer;
		r.world.tables.weapons.entries[kGunAdm].action_fsm.clip_capacity = 100;
		CHECK(r.attach(r.claimant_h, 0)); // the NPC gunner on the lone gun
		w::WeaponSlotState &slot = r.gun().primary_weapon_slot;
		slot.clip = 0;
		slot.reserve = 100;
		CHECK(pump_until_reload(r.world));
		if (peer) {
			CHECK(slot.clip == 100);
			CHECK(slot.reserve == 0);
			continue;
		}
		CHECK(slot.clip == 0);
		CHECK(slot.reserve == 100);
		r.world.out.weapon_reloads.clear();
		CHECK(pump_until_reload(r.world));
		CHECK(slot.clip == 0);
		CHECK(slot.reserve == 100);
	}
}

// D-NET-NEW-L7-4 (V-L7-s4's FOUND-1): the walk also visits an unoccupied EWeap gun whose
// heat window is open, and its RELOAD there addresses the gun itself: the authority queues
// [gun][FF FF], and only a playing authority's echo refills it; a dedicated host refills
// nothing. [orig: WeaponAction_ProcessAllEntities @0x542690 (the pool-1 walk
// @0x5426E9..0x542716); WeaponAction_Reload @0x5430B0 (@0x5430CA..0x543103);
// NetPacket_SendReloadRequest @0x432930 (@0x43296C..0x432974)]
void test_an_unoccupied_gun_relays_its_reload() {
	for (const bool peer : {true, false}) {
		Rig r(false);
		r.world.rules.mp_session_peer = peer;
		r.world.tables.weapons.entries[kGunAdm].action_fsm.clip_capacity = 100;
		w::WeaponSlotState &slot = r.gun().primary_weapon_slot;
		slot.clip = 0;
		slot.reserve = 100;
		slot.heat_window_end_tick = 1000000; // the window open: the walk visits the gun
		r.gun().item_type_index = 1;          // a resolved def row (the walk's live test)
		CHECK(!r.gun().primary_occupant.valid());
		CHECK(pump_until_reload(r.world));
		CHECK(r.world.out.weapon_reloads.size() == 1);
		if (!r.world.out.weapon_reloads.empty()) {
			CHECK(r.world.out.weapon_reloads[0].entity_handle == r.gun_h.packed);
			CHECK(r.world.out.weapon_reloads[0].reload_param == 0xFFFF);
		}
		CHECK(slot.clip == (peer ? 100 : 0));
		CHECK(slot.reserve == (peer ? 0 : 100));
	}
}

// A joiner whose personal SWITCHFROM is running when the host's forced UseGun attach lands
// with a gun of the outgoing weapon's category: the mount stores the gun's slot as
// g_PendingWeaponSlot and its SWITCHRANK queue writes nothing over the running SWITCHFROM,
// whose completion (instant: the pending def is Emplaced) stores the gun's slot and queues
// its SWITCHTO; the key's personal target never commits. Ours committed only when the
// completing action was the transition's own, so the SWITCHFROM committed nothing and a
// SWITCHRANK ran first. [orig: Player_MountWeaponSlot @0x4DFA40 (@0x4DFB16, @0x4DFB96);
// WeaponSlot_TryQueueSwitchRank @0x53F1C0; WeaponAction_SwitchFrom @0x5433B0 (the
// Emplaced sentinel @0x543435..0x54344A, the store @0x543475, the SWITCHTO @0x5434A3)]
void test_a_running_switchfrom_commits_the_forced_gun() {
	Rig r;
	w::World &world = r.world;
	constexpr uint8_t kPistolAdm = 2;
	w::WeaponTable &table = world.tables.weapons;
	table.entries[kGunAdm].category = 3; // the outgoing rifle's category
	table.entries[kGunAdm].rank = 5;
	table.entries.resize(3);
	w::WeaponTableEntry &pistol = table.entries[kPistolAdm];
	pistol.name = "WPN_PISTOL";
	pistol.valid = true;
	pistol.category = 2;
	pistol.clipsize = 15;
	pistol.startrounds = 60;
	pistol.maxclips = 4;
	pistol.ammo_class_count = 1;
	pistol.weapon_class_slot = 2;
	pistol.ammo_class_id = 1;
	bake(pistol);
	world.cached.local_player = r.requester_h;
	w::LocalPlayer local(world);
	world.local_player_state = &local;
	local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1},
			w::WeaponKitEntry{"WPN_PISTOL", -1, -1, -1}};
	w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory,
			local.inventory_valid, /*select_spawn_default=*/true);
	const int32_t rifle = local.inventory.equipped_combo;
	CHECK(rifle == kRifleCombo);
	const auto install = [&](const std::string &name, bool preserve) {
		w::WeaponInstallData data;
		data.name = name;
		data.clipsize = table.entries[size_t(table.index_of(name.c_str()))].clipsize;
		data.table_baked = true;
		w::local_weapon_install(world, local.weapon, data, preserve, false,
				&local.inventory, local.view);
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

	// The pistol's key: SWITCHFROM on the rifle, running.
	const w::WeaponSwitchOutcome key = w::weapon_switch_to_handle(table, local.inventory,
			2 * 65, w::local_weapon_switch_gates(world, local.weapon, &local.inventory));
	CHECK(key.kind == w::WeaponSwitchOutcome::kMount && !key.same_category);
	w::handle_weapon_switch_outcome(world, local.weapon, &local.inventory, key, local.view);
	for (int i = 0; i < 4; ++i) pump();
	CHECK(local.weapon.slot.current == w::weapon_action::kSwitchFrom);

	// The host's forced attach lands on the joiner (the UseGun row, bone 2).
	world.ai.is_authority = false;
	CHECK(world.vehicles.apply_confirmed_mount(r.requester_h, r.gun_h, 2));
	w::sync_local_usegun_weapon_transition(world, local.weapon);
	CHECK(local.weapon.usegun_switch == w::LocalUseGunSwitch::kAttach);
	CHECK(!local.weapon.switch_in_flight);
	pump();
	// The running SWITCHFROM's completion committed the gun and drew it.
	CHECK(local.weapon.usegun_slot_active);
	CHECK(local.weapon.usegun_switch == w::LocalUseGunSwitch::kNone);
	CHECK(r.gun().primary_weapon_slot.next == w::weapon_action::kSwitchTo ||
			r.gun().primary_weapon_slot.current == w::weapon_action::kSwitchTo);
	CHECK(r.requester().equipped_adm_index == kGunAdm);
	CHECK(local.inventory.equipped_combo == rifle);
	world.local_player_state = nullptr;
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
	local.loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
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
	test_an_unarmed_attach_takes_the_direct_leg();
	test_a_weapon_key_ends_a_slot_held_on_foot();
	test_a_ctrlx_seat_saves_a_gun_held_on_foot();
	test_a_borrowed_seat_detach_mounts_the_current_slot();
	test_the_detach_completion_stores_the_pending_slot_as_it_stands();
	test_the_walk_relays_an_authority_reload();
	test_a_dedicated_host_refills_no_gun();
	test_an_unoccupied_gun_relays_its_reload();
	test_the_walk_queues_reloads_in_slot_order();
	test_a_running_switchfrom_commits_the_forced_gun();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS usegun_refusal\n");
	return 0;
}
