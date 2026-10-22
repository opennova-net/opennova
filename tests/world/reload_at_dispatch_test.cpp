// The reload row's arm at its record's dispatch (D-CTRL-6, LI-s16): case 0xD3 requests the
// reload on the slot equipped then, under the binocular view's test, an equipped weapon, a
// magazine short of its capacity and a pool that is not 0, and the request takes the slot's
// next action only from IDLE, EMPTYIDLE or OVERHEATED: so a reload ahead of a switch or a
// fire in the same drain queues RELOAD first, and one behind them queues nothing.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0xD3 @0x4E1278..0x4E12F3 (the
//  binocular test @0x4E1278, the EquippedSlot @0x4E128B, the capacity test @0x4E12D7, the
//  pool test @0x4E12E0, WeaponSlot_RequestReload @0x4E12E9); WeaponSlot_RequestReload
//  @0x53F110 (the next action's test @0x53F11E..0x53F12D)]
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
		player = world.registry.spawn(0, entity);
		world.cached.local_player = player;
		weapon.active = true;
		weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, weapon.def);
		weapon.def.clip_capacity = 30;
		weapon.slot.clip = 12;
		weapon.slot.reserve = 60;
	}

	bool reload() { return local_weapon_request_reload(world, weapon, view, nullptr); }
};

void test_the_request_queues_reload_on_the_equipped_slot() {
	Fixture f;
	CHECK(f.reload());
	CHECK(f.weapon.slot.next == weapon_action::kReload);
	CHECK(!f.weapon.reload_pressed); // no pump latch: the request is the dispatch's
}

void test_the_case_gates() {
	{
		Fixture f;
		f.weapon.slot.clip = 30; // a full magazine
		CHECK(!f.reload() && f.weapon.slot.next == weapon_action::kIdle);
	}
	{
		Fixture f;
		f.weapon.slot.reserve = 0; // an empty pool
		CHECK(!f.reload() && f.weapon.slot.next == weapon_action::kIdle);
	}
	{
		Fixture f;
		f.view.binoculars_view_active = true;
		CHECK(!f.reload() && f.weapon.slot.next == weapon_action::kIdle);
	}
	{
		Fixture f;
		f.weapon.active = false; // no equipped weapon
		CHECK(!f.reload() && f.weapon.slot.next == weapon_action::kIdle);
	}
	{
		Fixture f;
		f.world.registry.get(f.player)->health = 0; // the pump's own input block
		CHECK(!f.reload() && f.weapon.slot.next == weapon_action::kIdle);
	}
}

void test_a_reload_behind_a_queued_action_queues_nothing() {
	Fixture f;
	f.weapon.slot.next = weapon_action::kSwitchFrom; // a switch the drain queued first
	(void)f.reload();
	CHECK(f.weapon.slot.next == weapon_action::kSwitchFrom);
	Fixture g;
	g.weapon.slot.next = weapon_action::kFire; // a fire request queued first
	(void)g.reload();
	CHECK(g.weapon.slot.next == weapon_action::kFire);
	Fixture h;
	h.weapon.slot.next = weapon_action::kOverheated;
	CHECK(h.reload());
	CHECK(h.weapon.slot.next == weapon_action::kReload);
}

} // namespace

int main() {
	test_the_request_queues_reload_on_the_equipped_slot();
	test_the_case_gates();
	test_a_reload_behind_a_queued_action_queues_nothing();
	if (failures != 0) {
		std::printf("reload_at_dispatch: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("reload_at_dispatch: ok\n");
	return 0;
}
