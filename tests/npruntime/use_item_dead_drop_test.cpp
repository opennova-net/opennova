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

#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

namespace {

namespace inmatch = opennova::inmatch;
namespace ms = opennova::mission;
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

// The role's frame reads the same operand (session.cpp: the dispatcher's head and the
// ToSpecial block, LocalPlayer::action_dispatch_drops): on a dedicated host in a session a
// live local body's Respawn key, held movement rows, ToSpecial dispatch and mouse look all
// drop; the same body on a listen host (a session peer) passes the head. The body is a Serve Only host's row body's
// shape: live, bound as the local player, its def bound (D-NET-352).
// [orig: Input_HandleActionBinding @0x49AD40 - the operand @0x49AD52..0x49AD7E, bit 0
//  @0x49AD95..0x49ADA0; case 55 -> Player_ResetLocalInput @0x49B20B; Input_FlushDeferredEvents
//  @0x497AC0 (the deferred ToSpecial passes the head again);
//  Input_TryTriggerMouseAxisBinding @0x499130 (the look queues through the head)]
struct RoleRig {
	std::unique_ptr<ms::MissionKernel> kernel_storage = std::make_unique<ms::MissionKernel>();
	ms::MissionKernel &kernel = *kernel_storage;
	std::unique_ptr<inmatch::HostRole> role_storage = std::make_unique<inmatch::HostRole>();
	inmatch::HostRole &role = *role_storage;
	RoleRig(bool session_peer) {
		role.bind(kernel);
		w::World &world = kernel.world;
		world.registry.configure_pool(0, 4);
		world.local_player_state = &kernel.local;
		world.tables.player.has_item_def = true;
		w::PlayerSpawn spawn;
		spawn.net_id = 0xFFF0;
		(void)w::spawn_player(world, spawn);
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.rules.mp_session_peer = session_peer;
	}
	void frame(uint32_t pressed, float look_dx = 0.0f) {
		inmatch::TickInput input;
		input.consume_one_shots = true;
		input.player.pressed_action_bits = pressed;
		input.player.look_delta_x = look_dx;
		input.player.movement.forward = true;
		role.apply_input(input);
	}
};

void run_the_roles_frame_reads_the_dedicated_operand() {
	for (const bool peer : {false, true}) {
		RoleRig r(peer);
		w::LocalPlayer &lp = r.kernel.local;
		CHECK(lp.has_local_player() && !lp.local_player_dead());
		CHECK(lp.action_dispatch_drops() == !peer);
		// The Respawn key: past the head, Player_ResetLocalInput resets the devices and
		// memsets the frame's queue (the frame's movement keys never land).
		r.frame(inmatch::PRESSED_RESPAWN);
		CHECK(r.role.take_input_reset() == peer);
		CHECK(!lp.input.forward); // the reset's memset, or the head dropping the held row
		// A held row (the forward key's, flags 0x0C000C05) passes the same head.
		r.frame(0);
		CHECK(lp.input.forward == peer);
		// The ToSpecial release: past the head its deferred dispatch takes the stash
		// (cleared at the release, @0x4E11D1); dropped, the deferred queue is gone and the
		// stash stays.
		lp.inventory_valid = true;
		lp.inventory.quick_switch_stash = 0;
		lp.to_special_queued = 1;
		r.frame(inmatch::PRESSED_TO_SPECIAL);
		CHECK(lp.to_special_queued == 0);
		CHECK(lp.inventory.quick_switch_stash == (peer ? -1 : 0));
		// The mouse look's axis events pass the same head.
		const int32_t heading = lp.input.look_heading;
		r.frame(0, 40.0f);
		CHECK((lp.input.look_heading != heading) == peer);
	}
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
	run_the_roles_frame_reads_the_dedicated_operand();
	test_the_toggle_carries_no_dead_test();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_use_item_dead_drop\n");
	return 0;
}
