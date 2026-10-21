// D-NET-433 (the dead player's USE route): the held USE row (code 0xB1) carries the action
// dispatcher's flag 0x1, which drops the press ahead of its handler while the local player's
// dead bit is set, so a dead player's press arms nothing and latches no hold, and no release
// toggles a mount; the mount toggle and the numbered seat pick themselves carry no dead
// test (a body at Health 0 whose dead bit is clear still toggles).
// [orig: Input_HandleActionBinding @0x49AD40 - Flags & 2 @0x49AD76..0x49AD7E, the row's flag
//  0x1 @0x49AD8A..0x49ADA0 (the code-0xB1 row's flags 0x0C000805 in the catalog @0x8159A8);
//  Input_HandleActionBinding_0 @0x4E0B5E..0x4E0B71 (the hold); Entity_ToggleVehicleMount
//  @0x436950, Entity_FindAvailableSeat @0x436790]
// The world is the test's own.

#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

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

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<w::LocalPlayer> local = std::make_unique<w::LocalPlayer>(world);
	w::EntityHandle player;
	Rig() {
		world.registry.configure_pool(0, 4);
		world.local_player_state = local.get();
		w::PlayerSpawn spawn;
		spawn.net_id = 0xFFF0;
		player = w::spawn_player(world, spawn);
	}
	w::Entity &body() { return *world.registry.get(player); }
};

void test_a_dead_players_press_is_dropped() {
	Rig r;
	CHECK(r.world.cached.local_player == r.player);
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kMountChain);
	r.body().flags |= w::kEntityFlagDead;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kDropped);
	r.body().flags &= ~w::kEntityFlagDead;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kMountChain);
}

// The operand is the constant 2 on a dedicated host in a session (an authority that is no
// session peer), whatever its body: a Serve Only shell's live hidden body (D-NET-352) drops
// the press too. A listen host (a session peer), a joiner (no authority) and the single
// player (out of a session) read the dead bit.
// [orig: Input_HandleActionBinding - is_in_session @0x49AD52..0x49AD5B, is_authority
//  @0x49AD5D..0x49AD64, is_mp_session_peer @0x49AD66..0x49AD6D, `mov eax, 2` @0x49AD6F]
void test_a_dedicated_host_in_session_drops_every_press() {
	Rig r;
	r.world.rules.mp_session = true;
	r.world.rules.logic_authority = true;
	r.world.rules.mp_session_peer = false;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kDropped);
	// A listen host is a session peer: the dead bit decides.
	r.world.rules.mp_session_peer = true;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kMountChain);
	// A joiner is no authority.
	r.world.rules.mp_session_peer = false;
	r.world.rules.logic_authority = false;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kMountChain);
	// Out of a session (the single player) the dead bit decides.
	r.world.rules.mp_session = false;
	r.world.rules.logic_authority = true;
	CHECK(r.local->take_use_item_press() == w::UseItemArm::kMountChain);
}

void test_the_toggle_carries_no_dead_test() {
	Rig r;
	r.body().health = 0;
	r.body().alive = false;
	CHECK(r.local->mount_toggle_allowed());
}

} // namespace

int main() {
	test_a_dead_players_press_is_dropped();
	test_a_dedicated_host_in_session_drops_every_press();
	test_the_toggle_carries_no_dead_test();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_use_item_dead_drop\n");
	return 0;
}
