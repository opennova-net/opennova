// D-AI-9 (org2) -- the player body's death edge raises the player-death
// transaction inside the entity update. Retail's org2 motor latches the dead bit
// on an unhidden body at Health <= 0 whose dead bit is clear and, on the
// authority, calls Entity_CheckAndProcessDeath, which for a player body is
// GameEvent_PlayerDeath; the update runs after Server_TickUpdate (its 0x0A fan
// and send pump), the pool-0 bodies in slot order after the projectiles and the
// explosion queue, so a Health write lands its death in that frame's update,
// a raise ahead of the walk cancels it, two bodies die in slot order, and the
// transaction's sends lead the next frame's queue. A damage-time record for the
// body only tallies.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 -- the hidden bit
//  @0x4B411E..0x4B4127, Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE, the latch
//  @0x4B4CB5..0x4B4CDB, `cmp is_authority` @0x4B4CE1, the call @0x4B4CEA;
//  Entity_CheckAndProcessDeath @0x51B550 (GameEvent_PlayerDeath @0x51B55F);
//  Entity_UpdateAllEntities -- Weapon_UpdateAllProjectiles @0x4C223A, the
//  explosion queue @0x4C223F, the pool-0 walk @0x4C2426..0x4C2474 (the body
//  update call @0x4C2472); Game_ProcessMainFrame -- Server_TickUpdate @0x5266B6,
//  Entity_UpdateAllEntities @0x52674B; GameEvent_PlayerDeath -- +368 = 120
//  @0x516F4F, +360 floored @0x516EC4..0x516EC6]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/infantry.h> // compute_death_anim_state
#include <runtime/world/entity.h>
#include <runtime/world/match.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/system.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A team host with three remote players: 0 (team 1), 1 (team 2) and the
// observer 2 (team 2), each body in the pool-0 slot `slots[i]`, no spawn zones.
struct Fixture {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<w::World> heap = std::make_unique<w::World>();
	w::World &world = *heap;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;

	explicit Fixture(std::vector<uint16_t> slots = {1, 2, 3}) {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.game_type = 0x10000u; // TDM
		ctx.config.respawn_timeout = 1;
		ctx.config.death_messages = 1; // the 0x1E kill feed
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.registry.configure_pool(0, 16);
		w::MatchRules rules;
		rules.game_type = 0x10000u;
		world.match.configure(rules);
		ctx.world = &world;
		transports.reserve(slots.size());
		for (size_t i = 0; i < slots.size(); ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			w::PlayerSpawn spawn;
			spawn.position = {10.0f * float(i), 0.0f, 0.0f};
			spawn.team = static_cast<uint8_t>(i == 0 ? 1 : 2);
			spawn.min_entity_slot = slots[i];
			players.push_back(w::spawn_remote_player(world, spawn));
			world.match.upsert_player({players.back(), static_cast<uint8_t>(i + 1), "P"});
		}
		for (size_t i = 0; i < slots.size(); ++i) {
			inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			c.reply.player_slot = static_cast<uint8_t>(i + 1);
			c.admission_stage = inmatch::GameAdmissionStage::Complete;
			c.s2c_send_boundary_open = true; // every frame's 0x0A
			ctx.np_protocol.connection_list.push_back(std::move(c));
		}
		inmatch::Server_TickUpdate(ctx); // the first frame's one-second service
		for (size_t i = 0; i < slots.size(); ++i) (void)drain(i);
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	w::Entity &entity(size_t i) { return *world.registry.get(players[i]); }
	// A lethal hit on body i by player k's body: Health 0, the shooter (+0x178)
	// and the hit's staged death selection (+0x2C0), which keeps the shooter
	// through the edge's consume. [orig: Projectile_ProcessDamageOnTarget
	// @0x4E81F3; OrganicClass_HandleEvent @0x407483; the edge's +0x178 clear
	// for an unstaged death @0x4B4C72..0x4B4C8D]
	void hit(size_t i, size_t k) {
		entity(i).health = 0;
		entity(i).last_attacker = players[k];
		entity(i).death_anim_state = w::compute_death_anim_state(0, 0, w::death_cause::kGeneric);
	}
	bool dead_bit(size_t i) {
		return ((entity(i).flags | entity(i).engine_flags) & w::kEntityFlagDead) != 0u;
	}
	std::vector<ns::Datagram> drain(size_t i) {
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (transports[i].pop_outbound(d)) out.push_back(d);
		return out;
	}
	// The 0x13 records' victims in a drained run, in order.
	std::vector<uint16_t> deaths(const std::vector<ns::Datagram> &run) {
		std::vector<uint16_t> out;
		for (const ns::Datagram &d : run)
			if (d.tag == s2c::ENTITY_DEATH && d.body.size() >= 2)
				out.push_back(static_cast<uint16_t>(d.body[0] | (d.body[1] << 8)));
		return out;
	}
	// The index of the first datagram with `tag` in a run, or -1.
	static int first(const std::vector<ns::Datagram> &run, uint8_t tag) {
		for (size_t k = 0; k < run.size(); ++k)
			if (run[k].tag == tag) return static_cast<int>(k);
		return -1;
	}
};

// A script system that writes one body's Health in the script pass, ahead of
// the entity update's pool-0 walk (a SETHP).
struct HealthWrite final : w::ISystem {
	w::EntityHandle body;
	int32_t health = 0;
	bool armed = false;
	const char *name() const override { return "test_health_write"; }
	void tick(w::World &world, const w::TickContext &) override {
		if (!armed) return;
		armed = false;
		if (w::Entity *e = world.registry.get(body)) e->health = health;
	}
};

// A Health write ahead of frame N: frame N's 0x0A goes out with the body still
// undead, its entity update latches the death and runs the transaction (the
// other-player kill's 120-second window, +360 at its floor), and the 0x13 and
// the 0x1E wait to lead frame N+1's queue, ahead of that frame's 0x0A.
void test_health_write_dies_in_its_frames_entity_update() {
	Fixture f;
	f.hit(0, 1);
	CHECK(!f.dead_bit(0)); // the write raises nothing itself
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.dead_bit(0));
	CHECK(!f.entity(0).alive);
	CHECK(f.conn(0).link.downed_revive_seconds == 120u);
	CHECK(f.conn(0).link.respawn_delay_seconds == 3u);
	const std::vector<ns::Datagram> frame_n = f.drain(2);
	CHECK(Fixture::first(frame_n, s2c::PER_FRAME_UPDATE) >= 0);
	CHECK(f.deaths(frame_n).empty());
	CHECK(Fixture::first(frame_n, s2c::GAME_EVENT) < 0);
	inmatch::Server_TickUpdate(f.ctx);
	const std::vector<ns::Datagram> frame_n1 = f.drain(2);
	CHECK(f.deaths(frame_n1) == std::vector<uint16_t>{f.players[0].packed});
	const int death_at = Fixture::first(frame_n1, s2c::ENTITY_DEATH);
	const int feed_at = Fixture::first(frame_n1, s2c::GAME_EVENT);
	const int frame_at = Fixture::first(frame_n1, s2c::PER_FRAME_UPDATE);
	CHECK(death_at >= 0 && feed_at > death_at && frame_at > feed_at);
	CHECK(f.world.round_sim.deaths.empty());
	// The killer is credited once, by the edge's transaction.
	const w::MatchPlayer *killer = f.world.match.player(f.players[1]);
	CHECK(killer != nullptr && killer->stats[w::MatchStats::kEnemyKills] == 1);
	// The victim's own 0x61 disarm and 0x52 camera ride the same release.
	const std::vector<ns::Datagram> victim = f.drain(0);
	CHECK(Fixture::first(victim, s2c::TICK_SEED) >= 0);
	CHECK(Fixture::first(victim, s2c::DEATH_CAMERA_TARGET) >= 0);
}

// A script raise in the same frame, ahead of the pool-0 walk, closes the edge
// before it runs: no death.
void test_raise_ahead_of_the_walk_cancels_it() {
	Fixture f;
	HealthWrite heal;
	heal.body = f.players[0];
	heal.health = 100;
	heal.armed = true;
	f.world.add_system(&heal);
	f.entity(0).health = 0;
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(!f.dead_bit(0));
	CHECK(f.entity(0).health == 100);
	CHECK(f.deaths(f.drain(2)).empty());
}

// Two bodies at Health 0 in one frame die in pool-0 slot order, whatever order
// their entities were spawned in.
void test_two_bodies_die_in_slot_order() {
	Fixture f({6, 2, 3}); // player 0's body in slot 6, player 1's in slot 2
	f.entity(0).health = 0;
	f.entity(1).health = 0;
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.dead_bit(0) && f.dead_bit(1));
	CHECK(f.deaths(f.drain(2)) ==
			(std::vector<uint16_t>{f.players[1].packed, f.players[0].packed}));
}

// A damage-time record for the body only tallies: the edge raises the one
// transaction, and the kill is credited once.
void test_damage_record_only_tallies() {
	Fixture f;
	f.hit(0, 1);
	w::RoundDeath d;
	d.victim = f.players[0];
	d.killer = f.players[1];
	d.victim_handle = f.players[0].packed;
	d.killer_handle = f.players[1].packed;
	d.kill_event = true;
	f.world.round_sim.deaths.push_back(d);
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths(f.drain(2)) == std::vector<uint16_t>{f.players[0].packed});
	const w::MatchPlayer *killer = f.world.match.player(f.players[1]);
	CHECK(killer != nullptr && killer->stats[w::MatchStats::kEnemyKills] == 1);
}

// A hidden body (Flags bit 0) at Health 0: the edge skips it.
void test_hidden_body_is_skipped() {
	Fixture f;
	f.entity(0).flags |= 1u;
	f.entity(0).health = 0;
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(!f.dead_bit(0));
	CHECK(f.deaths(f.drain(2)).empty());
}

// D-NET-558: the authority's entity update through the pre-round. Retail's gate
// tests the local client's copy of the pre-round byte (g_ClientPreRoundTimer,
// whose one writer is the client's 0x0A handler), not the authority's timer, so
// a dedicated host runs its entity update, the player body's edge included,
// every pre-round frame while a human is in it, where the script pass holds.
// [orig: Game_ProcessMainFrame -- `cmp g_ClientPreRoundTimer` @0x52672C, the
//  Entity_UpdateAllEntities call @0x52674B; the store NapiNPClientMsg_0x00A
//  @0x430064; Server_TickUpdate -- the script admission @0x51D8BD]
void test_dedicated_host_updates_through_the_preround() {
	Fixture f;
	HealthWrite script;
	script.body = f.players[2];
	script.health = 50;
	script.armed = true;
	f.world.add_system(&script);
	f.world.preround_delay_seconds = 5;
	f.hit(0, 1);
	const uint32_t updates = f.world.entity_update_counter;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.preround_delay_seconds != 0); // still the pre-round
	CHECK(f.world.entity_update_counter == updates + 1);
	CHECK(f.dead_bit(0));
	CHECK(script.armed); // the script pass held
	CHECK(f.entity(2).health == 100);
}

// A host that also plays holds its entity update while its own client's copy of
// the byte is up (the host role stamps it from its loopback 0x0A), and runs it
// once the copy clears.
void test_own_clients_copy_holds_the_update() {
	Fixture f;
	f.world.preround_delay_seconds = 5;
	f.world.cached.client_preround_timer = 5;
	f.hit(0, 1);
	const uint32_t updates = f.world.entity_update_counter;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.entity_update_counter == updates);
	CHECK(!f.dead_bit(0));
	f.world.cached.client_preround_timer = 0;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.entity_update_counter == updates + 1);
	CHECK(f.dead_bit(0));
}

} // namespace

int main() {
	test_health_write_dies_in_its_frames_entity_update();
	test_raise_ahead_of_the_walk_cancels_it();
	test_two_bodies_die_in_slot_order();
	test_damage_record_only_tallies();
	test_hidden_body_is_skipped();
	test_dedicated_host_updates_through_the_preround();
	test_own_clients_copy_holds_the_update();
	if (failures == 0) std::printf("player_death_edge: all passed\n");
	return failures == 0 ? 0 : 1;
}
