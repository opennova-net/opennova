// D-NET-336: the host pumps a remote player's own weapon slot. Retail's weapon walk ticks
// every live pool-0 row's EquippedSlot with no locality gate [orig:
// WeaponAction_ProcessAllEntities @0x542690 (the pool-0 leg @0x5426AD..0x5426C1)], and a
// remote player's is the row of its slot table its accepted C2S 0x06 armed: the handler
// clears the counter, holds the phase, makes FIRE current and runs the fire action on it
// [orig: Server_ClientFiredRound @0x50BAA0 (the row @0x50C27F..0x50C28B, the arm
// @0x50C2B0..0x50C2F0, the fire action @0x50C30D) -> WeaponAction_Fire @0x542B10]. The
// walk then runs RECOIL and, the clip empty with rounds left, RELOAD, whose first tick on
// the authority queues a C2S 0x25 for the host's own dispatcher [orig: WeaponAction_Reload
// @0x5430B0 (@0x5430D9, @0x543103)]. Nothing refills the host's copy but the player's own
// C2S 0x25: the host's relay of its own request skips the refill [orig:
// NapiNPServerMsg_HandleReloadRequest @0x514DF0 (@0x514EFA)] and its S2C 0x49 echo only
// dips a person's arms [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0
// (@0x42C109..0x42C113)], so the copy cycles RELOAD -> IDLE -> RELOAD, a request each
// cycle, until the player's own request lands (the 01TR self_nade scenario's extra 0x49s).
// The weapon, its delays and the pools are a model of the test's own; the values follow
// the cited control flow.

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_powerup.h> // ServerRemoteWeaponTables
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;
using conn_fixture::make_seeded_conn;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

constexpr uint8_t kRifleAdm = 5;
constexpr uint16_t kCombo = 3 * 65 + 2;

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t(v >> 8));
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	for (int i = 0; i < 4; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

// The fixed 45-byte C2S 0x06 body (net-re §5.16), an adm-indexed primary round.
std::vector<uint8_t> fire_body(uint16_t shooter) {
	std::vector<uint8_t> b;
	put_u32(b, 0x01000000u); // a client tick past the seed
	put_u16(b, shooter);
	b.push_back(0x02);       // the adm-indexed arm
	b.push_back(kRifleAdm);
	for (int i = 0; i < 5; ++i) put_u32(b, 0); // the pose
	put_u16(b, 0xFFFF);      // no target
	put_u16(b, 1);           // hit part
	b.push_back(0x07);
	b.push_back(12);
	b.push_back(0);
	for (int i = 0; i < 5; ++i) put_u16(b, 0);
	return b;
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::vector<inmatch::NapiNPConnection> roster;
	w::EntityHandle shooter;

	Rig() {
		world.registry.configure_pool(0, 4);
		world.rules.mp_session = true;
		world.rules.projectile_authority = true;
		world.rules.auto_reload = true;
		world.ai.is_authority = true;
		w::WeaponTable &table = world.tables.weapons;
		table.ammo_class_names = {"CLASS_556MM"};
		table.ammo_class_caps = {1000};
		table.entries.resize(kRifleAdm + 1);
		w::WeaponTableEntry &rifle = table.entries[kRifleAdm];
		rifle.name = "WPN_RIFLE";
		rifle.valid = true;
		rifle.category = 3;
		rifle.rank = 2;
		rifle.clipsize = 1;
		rifle.ammo_class_id = 0;
		rifle.ammo_class_count = 1;
		rifle.ammo_index = 1;
		w::weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, rifle.action_fsm);
		rifle.action_fsm.clip_capacity = rifle.clipsize;
		rifle.action_fsm.ammo_cost = rifle.ammo_class_count;
		auto &acts = rifle.action_fsm.actions;
		acts[w::weapon_action::kFire].delay_end = 2;
		acts[w::weapon_action::kRecoil].delay_start = 2;
		acts[w::weapon_action::kRecoil].delay_end = 2;
		acts[w::weapon_action::kReload].delay_start = 4;
		acts[w::weapon_action::kReload].delay_end = 4;
		w::PlayerSpawn spawn;
		spawn.net_id = 0xFFF1;
		shooter = w::spawn_remote_player(world, spawn);
		roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
		conn().reply.ammo_pools[0] = 2;
		entity().weapon_slots[kCombo] = w::PersonalWeaponSlot{kRifleAdm, 1};
	}
	inmatch::NapiNPConnection &conn() { return roster[0]; }
	w::Entity &entity() { return *world.registry.get(shooter); }
	w::PersonalWeaponSlot &row() { return entity().weapon_slots[kCombo]; }
	void dispatch(uint8_t tag, const std::vector<uint8_t> &body) {
		(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn(),
				{make_protocol_message(tag, body)}, 100, roster, &world);
	}
	// One frame's walk, with the session's tables behind the seam (HostRole's).
	void walk() {
		inmatch::ServerRemoteWeaponTables tables(roster);
		world.remote_weapon_tables = &tables;
		++world.logic_tick;
		world.pump_weapon_actions();
		world.remote_weapon_tables = nullptr;
	}
};

void test_the_walk_cycles_a_remote_players_reload_until_its_own_request() {
	Rig r;
	r.dispatch(c2s::FIRED_ROUND, fire_body(r.shooter.packed));
	// The accepted fire spent the clip, pointed the EquippedSlot at the row and armed
	// it: FIRE current and done, RECOIL next.
	CHECK(r.row().clip == 0);
	CHECK(r.entity().equipped_slot.kind == w::WeaponSlotRef::kPersonal &&
			r.entity().equipped_slot.combo == kCombo);
	CHECK(r.row().current == w::weapon_action::kFire);
	CHECK(r.row().next == w::weapon_action::kRecoil);
	CHECK(r.row().phase == w::weapon_phase::kDone);

	// The walk runs RECOIL, then RELOAD, whose first tick queues the request: the
	// player itself addressed with its slot's combo, the clip still empty.
	std::vector<uint32_t> request_ticks;
	for (int i = 0; i < 64; ++i) {
		r.walk();
		for (const w::WeaponReloadRequest &q : r.world.out.weapon_reloads) {
			CHECK(q.entity_handle == r.shooter.packed);
			CHECK(q.reload_param == kCombo);
			request_ticks.push_back(r.world.logic_tick);
		}
		r.world.out.weapon_reloads.clear();
	}
	CHECK(request_ticks.size() >= 2);
	CHECK(r.row().clip == 0);
	CHECK(r.conn().reply.ammo_pools[0] == 2);

	// The player's own C2S 0x25 refills the host's copy from its pool; the cycle ends.
	opennova::WeaponReload own;
	own.entity_handle = r.shooter.packed;
	own.reload_param = kCombo;
	r.dispatch(c2s::WEAPON_RELOAD_REQUEST, opennova::encode_weapon_reload(own));
	CHECK(r.row().clip == 1);
	CHECK(r.conn().reply.ammo_pools[0] == 1);
	size_t late = 0;
	for (int i = 0; i < 64; ++i) {
		r.walk();
		late += r.world.out.weapon_reloads.size();
		r.world.out.weapon_reloads.clear();
	}
	CHECK(late == 0);
	CHECK(r.row().clip == 1);
}

// With no rounds left in the pool the recoil chains to EMPTYIDLE: nothing is requested.
// [orig: WeaponAction_Recoil @0x542DD0 (the arbiter)]
void test_an_empty_pool_requests_nothing() {
	Rig r;
	r.conn().reply.ammo_pools[0] = 0;
	r.dispatch(c2s::FIRED_ROUND, fire_body(r.shooter.packed));
	CHECK(r.row().clip == 0);
	CHECK(r.entity().equipped_slot.kind == w::WeaponSlotRef::kPersonal);
	size_t requests = 0;
	for (int i = 0; i < 32; ++i) {
		r.walk();
		requests += r.world.out.weapon_reloads.size();
		r.world.out.weapon_reloads.clear();
	}
	CHECK(requests == 0);
	CHECK(r.row().current == w::weapon_action::kEmptyIdle);
}

} // namespace

int main() {
	test_the_walk_cycles_a_remote_players_reload_until_its_own_request();
	test_an_empty_pool_requests_nothing();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_remote_slot_pump\n");
	return 0;
}
