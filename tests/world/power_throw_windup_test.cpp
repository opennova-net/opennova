// The PowerThrow windup at its events (D-CTRL-6, LI-s16 / LI-s17): the left mouse button's down
// at the update's message pump restarts the windup on a slot that can fire with ammo and clears
// it otherwise, whatever row the button binds; the fire row's press leg (a key's) starts one only
// when none runs; a held update starts none; and the throw is the release leg's, at a record
// that reads the binding up, its charge byte from the ticks the windup was held. A hold's end
// with no such record throws nothing: the windup waits for one. Every event reads the game's
// tick as the update's input phase does, the one the update's weapon walk reads.
// [orig: Input_ProcessBindings @0x4DDA50 - the event-2 leg @0x4DDA88..0x4DDAFE;
//  Input_HandleActionBinding_0 @0x4E0420 - case 0x95's press leg @0x4E08FD..0x4E095C, the
//  release leg @0x4E07E9..0x4E0879; Game_ProcessMainFrame @0x5263F0 - ++g_CurrentTick
//  @0x5265B4 ahead of the input frame @0x52661D and the weapon walk @0x526786]
#include <cstdio>

#include <runtime/world/player_weapon.h>
#include <runtime/world/throwables.h>
#include <runtime/world/world.h>

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

struct Fixture {
	World world;
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
		def.category = 6;
		def.name = "WPN_THROWTEST";
		weapon.def_name = def.name;
		weapon.active = true;
		weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, weapon.def);
		weapon.def.flags = weapon_flag::kPowerThrow;
		weapon.def.clip_capacity = 3;
		weapon.slot.clip = 3;
	}

	// One update: its message pump's left button down, its input frame's fire records (a key's
	// press with the binding held, a release reading it up), the world tick, then the pump with
	// the binding held or not.
	void update(bool held, bool press = false, bool release = false, bool left_down = false) {
		if (left_down) local_weapon_left_button_down(world, weapon, view);
		if (press) (void)local_weapon_power_throw_press(world, weapon);
		if (release) (void)local_weapon_release_fire(world, weapon);
		++world.logic_tick;
		weapon.fire_held = held;
		LocalWeaponPumpIO io;
		io.view = &view;
		local_weapon_pump_tick(world, weapon, io);
	}
};

void test_a_press_starts_the_windup_a_hold_does_not() {
	Fixture f;
	f.update(true);
	CHECK(f.weapon.power_throw_start_tick == 0); // a held update starts none
	CHECK(local_weapon_input_tick(f.world) == f.world.logic_tick + 1);
	f.update(true, true);
	const uint32_t started = f.weapon.power_throw_start_tick;
	CHECK(started == f.world.logic_tick); // the input phase's tick, the walk's
	f.update(true, true); // a second press while one runs leaves it
	CHECK(f.weapon.power_throw_start_tick == started);
	f.update(false, false, true); // the release record throws
	CHECK(f.weapon.power_throw_start_tick == 0);
	CHECK(f.weapon.fired_serial == 1);
}

void test_the_hold_end_throws_nothing_without_a_release_record() {
	Fixture f;
	f.update(true, false, false, true); // the left button's down starts it
	CHECK(f.weapon.power_throw_start_tick != 0);
	for (int tick = 0; tick < 5; ++tick) f.update(false); // up, but no record reads it
	CHECK(f.weapon.power_throw_start_tick != 0);
	CHECK(f.weapon.fired_serial == 0);
	f.update(false, false, true); // the record that reads the binding up throws
	CHECK(f.weapon.fired_serial == 1);
}

void test_the_charge_is_the_held_ticks() {
	Fixture f;
	f.update(true, true);
	const uint32_t started = f.weapon.power_throw_start_tick;
	for (int tick = 0; tick < 49; ++tick) f.update(true);
	f.update(false, false, true);
	// The release reads the tick its update's walk reads, 50 past the start: (50 - 31) / 93
	// of 255 [orig: @0x4E07F7..0x4E085A; flt_7CD390 = 1/93, flt_7C69F4 = 0.1, flt_7CA29C = 255].
	CHECK(f.world.logic_tick - started == 50);
	CHECK(f.weapon.pending_throw_charge == power_throw_charge_from_hold(50));
	CHECK(f.weapon.pending_throw_charge == 52);
	CHECK(f.weapon.fired_serial == 1);
}

void test_a_press_the_slot_could_not_fire_starts_nothing_while_held() {
	Fixture f;
	f.weapon.slot.clip = 0; // no ammo: the press's windup test fails
	f.update(true, true);
	CHECK(f.weapon.power_throw_start_tick == 0);
	f.weapon.slot.clip = 3; // the slot can fire again, the binding still held
	for (int tick = 0; tick < 5; ++tick) f.update(true);
	CHECK(f.weapon.power_throw_start_tick == 0);
	const uint32_t fired = f.weapon.fired_serial;
	f.update(false, false, true);
	CHECK(f.weapon.fired_serial == fired);
	CHECK(f.weapon.pending_throw_charge == 0); // the release throws nothing
}

void test_the_left_down_restarts_or_clears_the_windup() {
	Fixture f;
	f.update(true, false, false, true);
	const uint32_t first = f.weapon.power_throw_start_tick;
	CHECK(first == f.world.logic_tick); // the left-down starts it, no press needed
	f.update(true);
	f.update(true, false, false, true); // a second left-down restarts it at its own tick
	CHECK(f.weapon.power_throw_start_tick == f.world.logic_tick);
	CHECK(f.weapon.power_throw_start_tick != first);
	f.weapon.slot.clip = 0;
	f.update(true, false, false, true); // on a slot that cannot fire it clears the windup
	CHECK(f.weapon.power_throw_start_tick == 0);
}

void test_the_left_downs_gates() {
	Fixture f;
	f.weapon.power_throw_start_tick = 7;
	f.view.binoculars_view_active = true;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(f.weapon.power_throw_start_tick == 7); // the binocular view: no test
	f.view.binoculars_view_active = false;
	Entity *body = f.world.registry.get(f.player);
	body->mounted = true;
	body->mount_type = SeatType::Driver;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(f.weapon.power_throw_start_tick == 7); // a Driver seat (parentSlot 5): no test
	body->mount_type = SeatType::Passenger;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(f.weapon.power_throw_start_tick == local_weapon_input_tick(f.world));
	f.weapon.power_throw_start_tick = 7;
	f.weapon.active = false;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(f.weapon.power_throw_start_tick == 7); // no equipped weapon: no test
}

// A Driver, or a Controller of a carrier with no EWeap, keeps its own EquippedSlot, which the
// walk pumps; case 0x95's PowerThrow branch sits ahead of the press leg's seat test and the
// release leg has none, and the fire action and the send test no seat: a key's windup throws
// from those seats. The left button's down there starts none (its own seat test).
// [orig: Input_HandleActionBinding_0 @0x4E0420 - the PowerThrow sign bit @0x4E08FD ahead of
//  the seat test @0x4E09CB..0x4E09DE, the release leg @0x4E07E9..0x4E0879;
//  Input_ProcessBindings @0x4DDA50 - parentSlot 2 / 5 @0x4DDAAE / @0x4DDAB3;
//  WeaponAction_ProcessAllEntities @0x5426AD..0x5426C1; WeaponAction_Fire @0x542B10]
void test_a_drivers_key_windup_throws() {
	for (const SeatType seat : {SeatType::Driver, SeatType::Controller}) {
		Fixture f;
		Entity *body = f.world.registry.get(f.player);
		body->mounted = true;
		body->mount_type = seat; // a Controller of no carrier: no EWeap to borrow
		CHECK(local_weapon_input_block(f.world, f.weapon) == LocalWeaponInputBlock::kSeat);
		f.update(true, false, false, true);
		CHECK(f.weapon.power_throw_start_tick == 0); // the left-down's seat test
		f.update(true, true); // the key's press leg starts it
		const uint32_t started = f.weapon.power_throw_start_tick;
		CHECK(started != 0);
		for (int tick = 0; tick < 3; ++tick) f.update(true);
		CHECK(f.weapon.power_throw_start_tick == started); // the seated pump keeps it
		f.update(false, false, true); // the release record throws
		CHECK(f.weapon.power_throw_start_tick == 0);
		CHECK(f.weapon.fired_serial == 1);
	}
}

void test_an_ordinary_def_keeps_no_windup() {
	Fixture f;
	f.weapon.def.flags = 0;
	f.weapon.power_throw_start_tick = 7;
	local_weapon_left_button_down(f.world, f.weapon, f.view); // its def clears it
	CHECK(f.weapon.power_throw_start_tick == 0);
	CHECK(!local_weapon_release_fire(f.world, f.weapon)); // a release with no windup: nothing
	CHECK(!f.weapon.power_throw_released);
}

// The windup's test is WeaponSlot_CanFireInCurrentState's and WeaponSlot_HasAmmoAvailable's: the
// slot's current action alone (IDLE; an automatic def's FIRE, RECOIL, SCOPEUP and SCOPEDOWN too),
// never the queued action or a switch, and rounds loaded or in the pool (a clipless def's pool
// alone). So a reload record ahead of a key's press in the same drain leaves the press its
// windup. [orig: Input_HandleActionBinding_0 @0x4E0420 - the press leg's tests @0x4E0922 /
//  @0x4E0935; WeaponSlot_CanFireInCurrentState @0x53F0B0; WeaponSlot_HasAmmoAvailable @0x541B30]
void test_the_windup_test_reads_the_current_action_and_any_ammo() {
	{
		Fixture f;
		f.weapon.slot.next = weapon_action::kReload; // a reload record ahead in the drain
		CHECK(local_weapon_power_throw_press(f.world, f.weapon));
		CHECK(f.weapon.power_throw_start_tick == local_weapon_input_tick(f.world));
	}
	{
		Fixture f;
		f.weapon.switch_in_flight = true; // a walk whose SWITCHFROM the pump has not entered
		f.weapon.switch_deferred_action = weapon_action::kSwitchFrom;
		CHECK(local_weapon_power_throw_press(f.world, f.weapon));
	}
	struct Row {
		bool auto_fire;
		int32_t current;
		bool starts;
	};
	const Row rows[] = {
		{false, weapon_action::kIdle, true},      {false, weapon_action::kFire, false},
		{false, weapon_action::kRecoil, false},   {false, weapon_action::kEmptyIdle, false},
		{false, weapon_action::kScopeUp, false},  {true, weapon_action::kIdle, true},
		{true, weapon_action::kFire, true},       {true, weapon_action::kRecoil, true},
		{true, weapon_action::kScopeUp, true},    {true, weapon_action::kScopeDown, true},
		{true, weapon_action::kEmptyIdle, false}, {true, weapon_action::kReload, false},
		{true, weapon_action::kSwitchTo, false},
	};
	for (const Row &row : rows) {
		Fixture f;
		f.weapon.def.auto_fire = row.auto_fire;
		f.weapon.slot.current = row.current;
		CHECK(local_weapon_power_throw_press(f.world, f.weapon) == row.starts);
	}
	{
		Fixture f;
		f.weapon.slot.clip = 0;
		f.weapon.slot.reserve = 4; // none loaded, the pool has rounds
		CHECK(local_weapon_power_throw_press(f.world, f.weapon));
	}
	{
		Fixture f;
		f.weapon.def.clip_capacity = -1; // clipless: the pool alone
		f.weapon.slot.clip = 3;
		f.weapon.slot.reserve = 0;
		CHECK(!local_weapon_power_throw_press(f.world, f.weapon));
		f.weapon.slot.reserve = 2;
		CHECK(local_weapon_power_throw_press(f.world, f.weapon));
	}
}

// The weapon walk never clears a running windup: not for the dead body, a UseGun switch in flight
// or a def with no PowerThrow flag. g_FireChargeStartTick's writers are the input legs, the walk
// Player_SwitchToWeaponByHandle (past its seat gate) and the round's init.
// [orig: g_FireChargeStartTick @0xB76800 - Input_ProcessBindings @0x4DDAF6 / @0x4DDAFE;
//  Input_HandleActionBinding_0 @0x4E0801 / @0x4E0947 / @0x4E0954; Player_SwitchToWeaponByHandle
//  @0x4E019B (the seat gate @0x4E017D..0x4E0192); Game_InitNewRound @0x42277E;
//  DialogSystem_Init @0x52776D]
void test_the_walk_keeps_a_running_windup() {
	{
		Fixture f;
		f.update(true, true);
		const uint32_t started = f.weapon.power_throw_start_tick;
		CHECK(started != 0);
		Entity *body = f.world.registry.get(f.player);
		body->alive = false;
		body->health = 0;
		for (int tick = 0; tick < 3; ++tick) f.update(true);
		CHECK(f.weapon.power_throw_start_tick == started); // the dead body keeps it
	}
	{
		Fixture f;
		f.update(true, true);
		const uint32_t started = f.weapon.power_throw_start_tick;
		f.weapon.usegun_switch = LocalUseGunSwitch::kDetach;
		CHECK(local_weapon_input_block(f.world, f.weapon) == LocalWeaponInputBlock::kUseGunSwitch);
		for (int tick = 0; tick < 3; ++tick) f.update(true);
		CHECK(f.weapon.power_throw_start_tick == started); // a UseGun switch in flight keeps it
	}
	{
		Fixture f;
		f.update(true, true);
		const uint32_t started = f.weapon.power_throw_start_tick;
		f.weapon.def.flags = 0;
		for (int tick = 0; tick < 3; ++tick) f.update(true);
		CHECK(f.weapon.power_throw_start_tick == started); // a def with no PowerThrow flag keeps it
	}
}

void test_the_walk_clears_the_windup_past_its_seat_gate() {
	Fixture f;
	Entity *body = f.world.registry.get(f.player);
	for (const SeatType seat : {SeatType::Gunner, SeatType::Controller, SeatType::Driver}) {
		body->mounted = true;
		body->mount_type = seat;
		f.weapon.power_throw_start_tick = 7;
		local_weapon_walk_clears_windup(f.world, f.weapon);
		CHECK(f.weapon.power_throw_start_tick == 7); // the seat walks nothing
	}
	body->mount_type = SeatType::Passenger;
	local_weapon_walk_clears_windup(f.world, f.weapon);
	CHECK(f.weapon.power_throw_start_tick == 0);
	body->mounted = false;
	f.weapon.power_throw_start_tick = 7;
	local_weapon_walk_clears_windup(f.world, f.weapon);
	CHECK(f.weapon.power_throw_start_tick == 0);
}

} // namespace

int main() {
	test_a_press_starts_the_windup_a_hold_does_not();
	test_the_hold_end_throws_nothing_without_a_release_record();
	test_the_charge_is_the_held_ticks();
	test_a_press_the_slot_could_not_fire_starts_nothing_while_held();
	test_the_left_down_restarts_or_clears_the_windup();
	test_the_left_downs_gates();
	test_a_drivers_key_windup_throws();
	test_an_ordinary_def_keeps_no_windup();
	test_the_windup_test_reads_the_current_action_and_any_ammo();
	test_the_walk_keeps_a_running_windup();
	test_the_walk_clears_the_windup_past_its_seat_gate();
	if (failures != 0) {
		std::printf("power_throw_windup: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("power_throw_windup: ok\n");
	return 0;
}
