// The PowerThrow windup's two starts (D-CTRL-6, LI-s16): the left mouse button's down at the
// update's message pump restarts the windup on a slot that can fire with ammo and clears it
// otherwise, whatever row the button binds; the fire row's press (the press latch) starts one
// only when none runs; a held tick starts none. A press that met a slot it could not fire
// therefore starts no windup while it stays held, and its release throws nothing.
// [orig: Input_ProcessBindings @0x4DDA50 - the event-2 leg @0x4DDA88..0x4DDAFE;
//  Input_HandleActionBinding_0 @0x4E0420 - case 0x95's press leg @0x4E08FD..0x4E095C, the
//  release leg @0x4E07E9]
#include <cstdio>

#include <runtime/world/player_weapon.h>
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

	// One update's pump at the next logic tick, the binding held or not, a press latched or
	// not, a left button's down at its message pump or not.
	void pump(bool held, bool pressed = false, bool left_down = false) {
		++world.logic_tick;
		if (left_down) local_weapon_left_button_down(world, weapon, view);
		weapon.fire_held = held;
		weapon.fire_pressed = weapon.fire_pressed || pressed;
		LocalWeaponPumpIO io;
		io.view = &view;
		local_weapon_pump_tick(world, weapon, io);
	}
};

void test_a_press_starts_the_windup_a_hold_does_not() {
	Fixture f;
	f.pump(true);
	CHECK(f.weapon.power_throw_start_tick == 0); // a held tick starts none
	f.pump(true, true);
	const uint32_t started = f.weapon.power_throw_start_tick;
	CHECK(started == f.world.logic_tick);
	f.pump(true, true); // a second press while one runs leaves it
	CHECK(f.weapon.power_throw_start_tick == started);
	f.pump(false); // the release throws
	CHECK(f.weapon.power_throw_start_tick == 0);
	CHECK(f.weapon.fired_serial == 1);
}

void test_a_press_the_slot_could_not_fire_starts_nothing_while_held() {
	Fixture f;
	f.weapon.slot.clip = 0; // no ammo: the press's windup test fails
	f.pump(true, true);
	CHECK(f.weapon.power_throw_start_tick == 0);
	f.weapon.slot.clip = 3; // the slot can fire again, the binding still held
	for (int tick = 0; tick < 5; ++tick) f.pump(true);
	CHECK(f.weapon.power_throw_start_tick == 0);
	const uint32_t fired = f.weapon.fired_serial;
	f.pump(false);
	CHECK(f.weapon.fired_serial == fired);
	CHECK(f.weapon.pending_throw_charge == 0); // the release throws nothing
}

void test_the_left_down_restarts_or_clears_the_windup() {
	Fixture f;
	f.pump(true, false, true);
	const uint32_t first = f.weapon.power_throw_start_tick;
	CHECK(first == f.world.logic_tick); // the left-down starts it, no press needed
	f.pump(true);
	f.pump(true, false, true); // a second left-down restarts it at its own tick
	CHECK(f.weapon.power_throw_start_tick == f.world.logic_tick);
	CHECK(f.weapon.power_throw_start_tick != first);
	f.weapon.slot.clip = 0;
	f.pump(true, false, true); // on a slot that cannot fire it clears the windup
	CHECK(f.weapon.power_throw_start_tick == 0);
}

void test_the_left_downs_gates() {
	Fixture f;
	f.view.binoculars_view_active = true;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(!f.weapon.power_throw_left_down); // the binocular view: no test
	f.view.binoculars_view_active = false;
	Entity *body = f.world.registry.get(f.player);
	body->mounted = true;
	body->mount_type = SeatType::Driver;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(!f.weapon.power_throw_left_down); // a Driver seat (parentSlot 5): no test
	body->mount_type = SeatType::Passenger;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(f.weapon.power_throw_left_down);
	f.weapon.power_throw_left_down = false;
	f.weapon.active = false;
	local_weapon_left_button_down(f.world, f.weapon, f.view);
	CHECK(!f.weapon.power_throw_left_down); // no equipped weapon: no test
}

void test_an_ordinary_def_keeps_no_windup() {
	Fixture f;
	f.weapon.def.flags = 0;
	f.weapon.power_throw_start_tick = 7;
	f.pump(false, false, true);
	CHECK(f.weapon.power_throw_start_tick == 0);
}

} // namespace

int main() {
	test_a_press_starts_the_windup_a_hold_does_not();
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
