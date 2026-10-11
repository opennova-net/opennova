// D-NET-463 -- the Respawn key's request, host side (C2S 0x41, no body). On the
// authority, for a sender whose slot has an entity, retail zeroes the Health
// word and the last attacker (+0x178), stores the respawn timeout itself,
// unfloored, into the respawn delay (+360) and the spawn-target hold (+364),
// and closes the revive window (+368). Health 0 on a live, unhidden body is
// then the player body's death edge, which raises the player-death
// transaction over those holds (the death's floor of 3, +364 cleared with no
// spawn zones).
// [orig: NapiNPServerMsg_ResetPlayerAmmo @0x510540; the edge
//  Entity_UpdateInfantryPlayerBody @0x4B4BF1..0x4B4CEA; GameEvent_PlayerDeath
//  @0x516EB6..0x516EEB]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <cstdio>
#include <memory>
#include <utility>
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

// A team host with two remote players (teams 1 and 2) and no spawn zones.
struct Fixture {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<w::World> heap = std::make_unique<w::World>();
	w::World &world = *heap;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;

	Fixture() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.game_type = 0x10000u; // TDM
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.registry.configure_pool(0, 32);
		ctx.world = &world;
		transports.reserve(2);
		for (size_t i = 0; i < 2; ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			w::PlayerSpawn spawn;
			spawn.position = {10.0f * float(i), 0.0f, 0.0f};
			spawn.team = static_cast<uint8_t>(i + 1);
			players.push_back(w::spawn_remote_player(world, spawn));
		}
		for (size_t i = 0; i < 2; ++i) {
			inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			c.reply.player_slot = static_cast<uint8_t>(i + 1);
			c.admission_stage = inmatch::GameAdmissionStage::Complete;
			ctx.np_protocol.connection_list.push_back(std::move(c));
		}
		entity(0).health = 100;
		entity(0).last_attacker = players[1];
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	w::Entity &entity(size_t i) { return *world.registry.get(players[i]); }
	void request(size_t i) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &ctx;
		(void)inmatch::dispatch_session_replies(ctx.config, conn(i),
				{make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {})}, 100,
				ctx.np_protocol.connection_list, &world, inputs);
	}
	bool sent_death(size_t i) {
		bool death = false;
		ns::Datagram d;
		while (transports[i].pop_outbound(d)) death = death || d.tag == s2c::ENTITY_DEATH;
		return death;
	}
	bool dead_bit(size_t i) {
		return ((entity(i).flags | entity(i).engine_flags) & w::kEntityFlagDead) != 0u;
	}
	// The 0x13s naming player i's body, and the 0x1E heal events (ev 45), staged
	// on player 1's transport since the last call.
	int deaths_of(size_t i, int *heals = nullptr) {
		int deaths = 0;
		ns::Datagram d;
		while (transports[1].pop_outbound(d)) {
			if (d.tag == s2c::ENTITY_DEATH && d.body.size() >= 2 &&
					(d.body[0] | (d.body[1] << 8)) == players[i].packed)
				++deaths;
			if (heals != nullptr && d.tag == s2c::GAME_EVENT && !d.body.empty() &&
					d.body[0] == 45)
				++*heals;
		}
		return deaths;
	}
};

// A live body: the stores, unfloored, then the edge's transaction (no killer)
// re-arms +360 at its floor over them and clears +364 (no spawn zones).
void test_live_body_stores_then_dies() {
	Fixture f;
	f.ctx.config.respawn_timeout = 1;
	f.conn(0).link.downed_revive_seconds = 7;
	f.request(0);
	CHECK(f.entity(0).health == 0);
	CHECK(!f.entity(0).last_attacker.valid());
	CHECK(f.conn(0).link.respawn_delay_seconds == 1);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 1);
	CHECK(f.conn(0).link.downed_revive_seconds == 0);
	// The handler raises nothing itself: the edge's record waits in the queue.
	CHECK(!f.dead_bit(0));
	CHECK(f.world.round_sim.deaths.size() == 1);
	if (!f.world.round_sim.deaths.empty()) {
		CHECK(f.world.round_sim.deaths[0].victim == f.players[0]);
		CHECK(!f.world.round_sim.deaths[0].killer.valid());
	}
	(void)f.sent_death(1);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.world.round_sim.deaths.empty());
	CHECK(f.sent_death(1));
	CHECK(f.dead_bit(0));
	// The death's floor of 3 over the request's 1 (at most one periodic second
	// has run since); +364 cleared with no spawn zones.
	CHECK(f.conn(0).link.respawn_delay_seconds >= 2);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 0);
}

// A dead body takes the stores and raises no second death: the request
// restarts both holds at the timeout and closes the revive window.
void test_dead_body_only_stores() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	f.entity(0).health = 0;
	f.entity(0).alive = false;
	f.entity(0).flags |= w::kEntityFlagDead;
	f.conn(0).link.respawn_delay_seconds = 1;
	f.conn(0).link.downed_revive_seconds = 120;
	f.request(0);
	CHECK(f.entity(0).health == 0);
	CHECK(!f.entity(0).last_attacker.valid());
	CHECK(f.conn(0).link.respawn_delay_seconds == 5);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 5);
	CHECK(f.conn(0).link.downed_revive_seconds == 0);
	CHECK(f.world.round_sim.deaths.empty());
	// A body the damage already dropped (its own transaction raised, the bit
	// not yet latched) raises no second one either.
	Fixture g;
	g.entity(0).health = -5;
	g.request(0);
	CHECK(g.entity(0).health == 0);
	CHECK(g.world.round_sim.deaths.empty());
}

// A hidden body (Flags bit 0, a spectator's) takes the stores; the edge skips
// it, so no death.
void test_hidden_body_raises_no_death() {
	Fixture f;
	f.ctx.config.respawn_timeout = 4;
	f.entity(0).flags |= 1u;
	f.request(0);
	CHECK(f.entity(0).health == 0);
	CHECK(f.conn(0).link.respawn_delay_seconds == 4);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 4);
	CHECK(f.world.round_sim.deaths.empty());
}

// A 0x41 and a team change (0x4D) in one receive pump: the team change kills
// the body inline and lays its penalty over the holds, so the 0x41's queued
// edge finds the dead bit set when the tick routes it and runs no second
// death; the penalty's +360 stands. [orig: Entity_UpdateInfantryPlayerBody --
// Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE; NapiNPServerMsg_0x04D_ChangeTeam
// -- GameEvent_PlayerDeath @0x519059 / @0x5190C7, +0x168 @0x51906B..0x519076]
void test_team_change_in_the_same_pump_dies_once() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	f.ctx.config.change_team_penalty_seconds = 60;
	f.ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	(void)f.sent_death(1);
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0),
			{make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {}),
			 make_protocol_message(c2s::TEAM_CHANGE_REQUEST, {})},
			100, f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK(f.entity(0).team == 2); // the team change took
	CHECK(f.dead_bit(0));
	int deaths = 0;
	ns::Datagram d;
	while (f.transports[1].pop_outbound(d)) deaths += d.tag == s2c::ENTITY_DEATH ? 1 : 0;
	inmatch::Server_TickUpdate(f.ctx);
	while (f.transports[1].pop_outbound(d)) deaths += d.tag == s2c::ENTITY_DEATH ? 1 : 0;
	CHECK(deaths == 1);
	CHECK(f.world.round_sim.deaths.empty());
	// At most one periodic second has run since the penalty's 60.
	CHECK(f.conn(0).link.respawn_delay_seconds >= 59);
}

// A 0x41 and a spectator convert (0x51) in one receive pump, in either order:
// retail's convert runs inline in the pump and hides the body (Flags |= 1 and
// Health 1; its deploy leg raises the Health and clears only the dead bit), so
// the edge after the pump skips it and no death is raised. Ours stages the
// convert for the tick, which routes the 0x41's record after it.
// [orig: Server_KillPlayerAndNotify -- Flags @0x519E92, Health @0x519EA4, the
//  deploy @0x519ECE; Server_ProcessPlayerDeath's dead-bit clear @0x51787A; the
//  edge's hidden bit Entity_UpdateInfantryPlayerBody @0x4B411E..0x4B4127]
void test_spectator_convert_in_the_same_pump_raises_no_death() {
	for (int order = 0; order < 2; ++order) {
		Fixture f;
		f.ctx.config.respawn_timeout = 5;
		f.ctx.config.permanent_death = true;
		f.ctx.config.spectator_slots = 1;
		(void)f.sent_death(1);
		std::vector<ProtocolMessage> pump{
				make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {}),
				make_protocol_message(c2s::SPECTATOR_RESPAWN, {0xFF, 0xFF})};
		if (order == 1) std::swap(pump[0], pump[1]);
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &f.ctx;
		(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0), pump, 100,
				f.ctx.np_protocol.connection_list, &f.world, inputs);
		CHECK(f.world.round_sim.deaths.size() == 1); // the 0x41's edge, queued
		inmatch::Server_TickUpdate(f.ctx);
		CHECK(f.conn(0).link.spectator); // the convert took
		CHECK(!f.sent_death(1));
		CHECK(!f.dead_bit(0));
		CHECK(f.world.round_sim.deaths.empty());
	}
}

// A medic heal the previous frame's entity update admitted, then a 0x41 in this
// frame's pump: retail stored the heal in that entity update (ahead of the
// pump), so the 0x41's Health 0 lies over it and the edge raises the death on
// a body at Health 0. Ours routes the heal at the tick's head, after the pump:
// the edge is tested ahead of it there, the 0x41's Health 0 goes back over the
// heal, and the death still runs. [orig: Entity_UpdateAllEntities --
// Projectile_ProcessExplosionQueue @0x4C223F -> GameEvent_HandleMedicInteraction,
// the GameEvent_HealPlayer call @0x4E6805, Health = max @0x50DE7F;
// NapiNPServerMsg_ResetPlayerAmmo @0x510568; the edge
// Entity_UpdateInfantryPlayerBody @0x4B4BF1..0x4B4CEA]
void test_previous_frame_heal_keeps_the_death() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	f.entity(0).has_item_def = true;
	f.entity(0).health_max = 100;
	f.entity(0).health = 40;
	f.entity(1).player_class = 5; // the Medic class
	f.world.round_sim.medic_interactions.push_back({f.players[0], f.players[1], false});
	(void)f.deaths_of(0);
	f.request(0);
	CHECK(f.world.round_sim.deaths.size() == 1);
	inmatch::Server_TickUpdate(f.ctx);
	int heals = 0;
	CHECK(f.deaths_of(0, &heals) == 1);
	CHECK(heals == 1); // the heal ran (its 0x1E ev 45)
	CHECK(f.dead_bit(0));
	CHECK(f.entity(0).health == 0); // the 0x41's store, not the heal's
	CHECK(f.world.round_sim.deaths.empty());
}

// Two edge records on one body route one death: the head routes neither and
// holds both, and the drain after the converts skips the second once the first
// latched the dead bit. [orig: Entity_UpdateInfantryPlayerBody -- the dead bit
// @0x4B4BFE]
void test_two_edge_records_on_one_body_die_once() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	(void)f.deaths_of(0);
	f.request(0);
	CHECK(f.world.round_sim.deaths.size() == 1);
	if (!f.world.round_sim.deaths.empty())
		f.world.round_sim.deaths.push_back(f.world.round_sim.deaths.front());
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 1);
	CHECK(f.dead_bit(0));
	CHECK(f.world.round_sim.deaths.empty());
}

// A 0x41, a Default Spawn 0x0E and a 0x41 in one pump at a respawn timeout of 0,
// in session: the 0x0E in ours reads the Health the 0x41 zeroed and deploys the body
// (retail's reads the dead bit, still clear, and refuses: D-NET-460), so the
// second 0x41 queues a second edge record on the redeployed body. The body
// dies once. [orig: Server_ProcessClientRequestRespawn @0x519CC1;
// Entity_UpdateInfantryPlayerBody @0x4B4BFE]
void test_reset_deploy_reset_in_one_pump_dies_once() {
	Fixture f;
	f.ctx.config.respawn_timeout = 0;
	(void)f.deaths_of(0);
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0),
			{make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {}),
			 make_protocol_message(c2s::RESPAWN_REQUEST, {0xFF, 0xFF}),
			 make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {})},
			100, f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK(f.world.round_sim.deaths.size() == 2); // one record per 0x41
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 1);
	CHECK(f.dead_bit(0));
	CHECK(f.world.round_sim.deaths.empty());
}

// Not the authority, no player slot, or a slot with no entity: nothing.
void test_refusals() {
	Fixture f;
	f.ctx.config.respawn_timeout = 4;
	f.ctx.is_authority = 0;
	f.request(0);
	CHECK(f.entity(0).health == 100 && f.entity(0).last_attacker.valid());
	CHECK(f.conn(0).link.respawn_delay_seconds == 0);
	f.ctx.is_authority = 1;
	f.conn(0).phase = inmatch::ConnectionPhase::New;
	f.request(0);
	CHECK(f.entity(0).health == 100 && f.conn(0).link.respawn_delay_seconds == 0);
	f.conn(0).phase = inmatch::ConnectionPhase::InMatch;
	f.conn(0).link.owned_entity = w::EntityHandle{};
	f.request(0);
	CHECK(f.entity(0).health == 100 && f.conn(0).link.respawn_delay_seconds == 0);
	CHECK(f.world.round_sim.deaths.empty());
}

} // namespace

int main() {
	test_live_body_stores_then_dies();
	test_dead_body_only_stores();
	test_hidden_body_raises_no_death();
	test_team_change_in_the_same_pump_dies_once();
	test_spectator_convert_in_the_same_pump_raises_no_death();
	test_previous_frame_heal_keeps_the_death();
	test_two_edge_records_on_one_body_die_once();
	test_reset_deploy_reset_in_one_pump_dies_once();
	test_refusals();
	if (failures == 0) std::printf("death_timeout_reset: all passed\n");
	return failures == 0 ? 0 : 1;
}
