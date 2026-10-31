// The fire row's press leg at its record's dispatch (D-CTRL-6, LI-s16; D-NET-1068 /
// LI-17): for a def without the PowerThrow flag, case 0x95 sets the mission script's latch
// for the EquippedSlot def's type and refuses a type the script blocks, refuses an
// OnlyFireScoped def outside the first-person optical view, turns an OnlyFireLocked def's
// accepted request into the dry click when no lock holds, and refuses a Driver seat, all at
// the dispatch; the weapon pump then requests the latched press with no WAC gate of its own,
// so a BlockFire the script sets after the dispatch refuses nothing, while a sourceless press
// keeps the pump's gate.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0x95's press leg: the WAC latch
//  @0x4E0961..0x4E0968, BlockFire @0x4E096F..0x4E0976, OnlyFireScoped @0x4E097C..0x4E09A0,
//  OnlyFireLocked @0x4E09AC..0x4E09C9, the seats @0x4E09CB..0x4E09FF,
//  Player_RequestPrimaryFire @0x4E0A06, nextAction = 5 @0x4E0A1E]
#include <cstdio>

#include <formats/def/def.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/title_rules.h>
#include <runtime/world/world.h>
#include "../common/title_fixture.h"

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

constexpr int kCategory = 3;

struct Fixture {
	World world;
	bool world_jo = test_titles::jo_world(world);
	LocalPlayerWeapon weapon;
	PlayerViewState view;
	EntityHandle player;

	Fixture() {
		world.registry.configure_pool(0, 8);
		Entity entity;
		entity.kind = EntityKind::Organic;
		entity.alive = true;
		entity.health = 100;
		entity.equipped_adm_index = 1;
		player = world.registry.spawn(0, entity);
		world.cached.local_player = player;
		world.cached.humans = 1;
		world.tables.weapons.entries.resize(2);
		auto &def = world.tables.weapons.entries[1];
		def.valid = true;
		def.category = static_cast<uint8_t>(kCategory);
		def.name = "WPN_PRESSTEST";
		weapon.def_name = def.name;
		weapon.active = true;
		weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, weapon.def);
		weapon.def.clip_capacity = 30;
		weapon.def.actions[weapon_action::kFire].delay_end = 6;
		weapon.slot.clip = 30;
	}

	bool press() { return local_weapon_press_fire(world, weapon, view); }

	void pump(bool held) {
		++world.logic_tick;
		weapon.fire_held = held;
		LocalWeaponPumpIO io;
		io.view = &view;
		local_weapon_pump_tick(world, weapon, io);
	}
};

void test_the_press_stamps_the_script_latch_at_the_dispatch() {
	Fixture f;
	CHECK(f.press());
	CHECK(f.weapon.fire_pressed && f.weapon.fire_press_dispatched && !f.weapon.fire_press_dry);
	CHECK(f.world.script.weapon_input.fire_requested(kCategory)); // before any pump
	f.pump(true);
	CHECK(f.weapon.fired_serial == 1);
}

void test_blockfire_refuses_at_the_dispatch_not_after_it() {
	{
		Fixture f;
		f.world.script.weapon_input.set_fire_blocked(kCategory, true);
		CHECK(!f.press());
		CHECK(!f.weapon.fire_pressed);
		CHECK(f.world.script.weapon_input.fire_requested(kCategory)); // still observable
	}
	{
		Fixture f;
		CHECK(f.press());
		// The script's pass blocks the type after the dispatch: the press stands.
		f.world.script.weapon_input.set_fire_blocked(kCategory, true);
		f.pump(true);
		CHECK(f.weapon.fired_serial == 1);
	}
	{
		Fixture f;
		f.world.script.weapon_input.set_fire_blocked(kCategory, true);
		f.weapon.fire_pressed = true; // a sourceless press: the pump's gate refuses it
		f.pump(true);
		CHECK(f.weapon.fired_serial == 0);
		CHECK(f.weapon.slot.clip == 30);
	}
}

void test_only_fire_scoped_refuses_outside_the_optical_view() {
	Fixture f;
	f.world.tables.weapons.entries[1].flags = weapon_flag::kOnlyFireScoped;
	CHECK(!f.press()); // no optical view up
	CHECK(!f.weapon.fire_pressed);
	CHECK(f.world.script.weapon_input.fire_requested(kCategory)); // the latch precedes the test
}

// The two flags are the EquippedSlot def's, not the mounted FSM def's (they differ on a
// borrowed carrier slot's route) [orig: EquippedSlot->Def+8 @0x4E097C / @0x4E09AC].
void test_the_flags_are_the_equipped_slot_defs() {
	Fixture f;
	f.weapon.def.flags = weapon_flag::kOnlyFireScoped | weapon_flag::kOnlyFireLocked;
	CHECK(f.press()); // the slot's def carries neither
	CHECK(!f.weapon.fire_press_dry);
}

void test_only_fire_locked_without_a_lock_dry_clicks() {
	Fixture f;
	f.world.tables.weapons.entries[1].flags = weapon_flag::kOnlyFireLocked;
	CHECK(f.press());
	CHECK(f.weapon.fire_press_dry); // the player's aim runtime holds no lock
	f.pump(true);
	CHECK(f.weapon.fired_serial == 0);
	CHECK(f.weapon.dry_serial == 1 || f.weapon.slot.current == weapon_action::kEmpty);
}

void test_a_driver_seat_refuses_after_the_latch() {
	Fixture f;
	Entity *body = f.world.registry.get(f.player);
	body->mounted = true;
	body->mount_type = SeatType::Driver;
	CHECK(!f.press());
	CHECK(!f.weapon.fire_pressed);
	CHECK(f.world.script.weapon_input.fire_requested(kCategory));
}

// A title whose lone gunner holds fire (DFX2-37, TitleRules::emplaced_solo_holds_fire): from a
// gunner's seat on an IsTurret gun whose +0x28 carrier counts one occupied seat at most, the press
// leg latches nothing, the release leg ends the windup and requests no throw, and the pump refuses
// a sourceless press (the press leg's mirror); a second occupant on the carrier fires, and so
// does JO's title with the lone gunner.
// [orig: the twin of Input_HandleActionBinding_0 @0x4DD4C0 (dfx2.exe) - @0x4DD8FE..0x4DD92C,
//  @0x4DDABB..0x4DDADB] [orig: Input_HandleActionBinding_0 @0x4E0420 - @0x4E085C..0x4E086F,
//  @0x4E09CB..0x4E0A06]
void test_a_lone_gunner_holds_fire_under_dfx2s_title() {
	Fixture f;
	f.world.registry.configure_pool(1, 4);
	Entity carrier_seed;
	carrier_seed.kind = EntityKind::Item;
	carrier_seed.has_item_def = true;
	carrier_seed.has_graphic_model = true;
	carrier_seed.graphic_model_userpoints = 4;
	Seat driver;
	driver.type = SeatType::Driver;
	Seat gunner;
	gunner.type = SeatType::Gunner;
	carrier_seed.seats = {driver, gunner};
	const EntityHandle carrier_h = f.world.registry.spawn(1, carrier_seed);
	Entity gun_seed;
	gun_seed.kind = EntityKind::Item;
	gun_seed.has_item_def = true;
	gun_seed.item_attrib2 = opennova::def::DEF_ITEM_ATTRIB2_ISTURRET;
	gun_seed.emplacement_parent = carrier_h;
	const EntityHandle gun_h = f.world.registry.spawn(1, gun_seed);
	Entity *body = f.world.registry.get(f.player);
	body->mounted = true;
	body->mount_target = gun_h;
	body->mount_type = SeatType::Gunner;
	Entity &carrier = *f.world.registry.get(carrier_h);
	carrier.seats[1].occupant = f.player;
	f.world.rules.title_rules = &title_rules("dfx2");
	CHECK(local_solo_turret_holds_fire(f.world));
	CHECK(!f.press());
	CHECK(!f.weapon.fire_pressed);
	f.weapon.power_throw_start_tick = 1;
	CHECK(!local_weapon_release_fire(f.world, f.weapon));
	CHECK(f.weapon.power_throw_start_tick == 0 && !f.weapon.power_throw_released);
	// A sourceless press meets the same gate in the pump (the press leg's mirror).
	f.weapon.fire_pressed = true;
	f.pump(true);
	CHECK(f.weapon.fired_serial == 0 && f.weapon.slot.clip == 30);
	// A driver aboard: two occupied seats, the gun fires.
	carrier.seats[0].occupant = EntityHandle::make(0, 7);
	CHECK(!local_solo_turret_holds_fire(f.world));
	f.weapon.fire_pressed = true;
	f.weapon.fire_press_dispatched = false;
	f.pump(true);
	CHECK(f.weapon.fired_serial == 1);
	CHECK(f.press());
	f.weapon.power_throw_start_tick = 1;
	CHECK(local_weapon_release_fire(f.world, f.weapon));
	CHECK(f.weapon.power_throw_released);
	// JO's title has no such test; the lone gunner stands whatever the rules (the HUD's panel reads
	// it under the title's own flag, DFX2-91).
	CHECK(!local_rides_solo_turret(f.world));
	carrier.seats[0].occupant = EntityHandle{};
	f.world.rules.title_rules = &title_rules("jo");
	CHECK(!local_solo_turret_holds_fire(f.world));
	CHECK(local_rides_solo_turret(f.world));
}

} // namespace

int main() {
	test_the_press_stamps_the_script_latch_at_the_dispatch();
	test_blockfire_refuses_at_the_dispatch_not_after_it();
	test_only_fire_scoped_refuses_outside_the_optical_view();
	test_the_flags_are_the_equipped_slot_defs();
	test_only_fire_locked_without_a_lock_dry_clicks();
	test_a_driver_seat_refuses_after_the_latch();
	test_a_lone_gunner_holds_fire_under_dfx2s_title();
	if (failures != 0) {
		std::printf("fire_press_leg: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("fire_press_leg: ok\n");
	return 0;
}
