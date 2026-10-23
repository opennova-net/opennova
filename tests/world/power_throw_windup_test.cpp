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

void test_an_ordinary_def_keeps_no_windup() {
	Fixture f;
	f.weapon.def.flags = 0;
	f.weapon.power_throw_start_tick = 7;
	local_weapon_left_button_down(f.world, f.weapon, f.view); // its def clears it
	CHECK(f.weapon.power_throw_start_tick == 0);
	CHECK(!local_weapon_release_fire(f.world, f.weapon)); // a release with no windup: nothing
	CHECK(!f.weapon.power_throw_released);
}

} // namespace

int main() {
	test_a_press_starts_the_windup_a_hold_does_not();
	test_the_hold_end_throws_nothing_without_a_release_record();
	test_the_charge_is_the_held_ticks();
	test_a_press_the_slot_could_not_fire_starts_nothing_while_held();
	test_the_left_down_restarts_or_clears_the_windup();
	test_the_left_downs_gates();
	test_an_ordinary_def_keeps_no_windup();
	if (failures != 0) {
		std::printf("power_throw_windup: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("power_throw_windup: ok\n");
	return 0;
}
