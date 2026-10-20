// D-NET-463 -- the Respawn key's request, host side (C2S 0x41, no body). On the
// authority, for a sender whose slot has an entity, retail zeroes the Health
// word and the last attacker (+0x178), stores the respawn timeout itself,
// unfloored, into the respawn delay (+360) and the spawn-target hold (+364),
// and closes the revive window (+368). Health 0 on a live, unhidden body is
// then the player body's death edge in the frame's entity update, which
// raises the player-death transaction over those holds (the death's floor of
// 3, +364 cleared with no spawn zones).
// [orig: NapiNPServerMsg_DeathTimeoutReset @0x510540; the edge
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
#include <net/npwire/ingame_decode.h>
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
	// Player i's 0x0A of the next frame, built with the phase-0 (weapon) block.
	FrameUpdate next_frame(size_t i) {
		conn(i).s2c_send_boundary_open = true;
		conn(i).link.s2c_phase = 0xFF; // the writer's ++ lands on phase 0
		inmatch::Server_TickUpdate(ctx);
		FrameUpdate frame;
		ns::Datagram d;
		while (transports[i].pop_outbound(d)) {
			if (d.tag != s2c::PER_FRAME_UPDATE) continue;
			frame = FrameUpdate{};
			(void)decode_frame_update(d.body.data(), d.body.size(),
					[](uint16_t) { return EntityClass::Player; }, frame);
		}
		return frame;
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
// in the frame's entity update re-arms +360 at its floor over them and clears
// +364 (no spawn zones); its 0x13 leads the next frame's queue.
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
	// The handler raises and queues nothing: the edge is the entity update's.
	CHECK(!f.dead_bit(0));
	CHECK(f.world.round_sim.deaths.empty());
	(void)f.sent_death(1);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.dead_bit(0));
	// The death's floor of 3 over the request's 1, stored after the frame's
	// periodic decrement; +364 cleared with no spawn zones.
	CHECK(f.conn(0).link.respawn_delay_seconds == 3);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 0);
	CHECK(!f.sent_death(1));
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.sent_death(1));
	CHECK(f.world.round_sim.deaths.empty());
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
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 0);
	// A body the damage already dropped, its edge not yet run: the request
	// queues nothing, and the edge raises the one death.
	Fixture g;
	g.entity(0).health = -5;
	g.request(0);
	CHECK(g.entity(0).health == 0);
	CHECK(g.world.round_sim.deaths.empty());
	inmatch::Server_TickUpdate(g.ctx);
	inmatch::Server_TickUpdate(g.ctx);
	CHECK(g.deaths_of(0) == 1);
}

// A hidden body (Flags bit 0, a held slot's, which every 0x0A re-sets) takes
// the stores; the edge skips it, so no death. [orig: NetPacket_WritePlayerState
// @0x4FF6D7 / @0x4FF7B8]
void test_hidden_body_raises_no_death() {
	Fixture f;
	f.ctx.config.respawn_timeout = 4;
	f.entity(0).flags |= 1u;
	f.conn(0).link.respawn_pending = true;
	f.request(0);
	CHECK(f.entity(0).health == 0);
	CHECK(f.conn(0).link.respawn_delay_seconds == 4);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 4);
	CHECK(f.world.round_sim.deaths.empty());
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 0);
	CHECK(!f.dead_bit(0));
}

// A 0x41 and a team change (0x4D) in one receive pump: the team change kills
// the body inline, latches its dead bit and lays its penalty over the holds, so
// the edge in the frame's entity update skips the dead body and runs no second
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
	inmatch::Server_TickUpdate(f.ctx);
	while (f.transports[1].pop_outbound(d)) deaths += d.tag == s2c::ENTITY_DEATH ? 1 : 0;
	CHECK(deaths == 1);
	CHECK(f.world.round_sim.deaths.empty());
	// At most one periodic second has run since the penalty's 60.
	CHECK(f.conn(0).link.respawn_delay_seconds >= 59);
}

// A 0x41 and a spectator convert (0x51) in one receive pump, in either order:
// the convert runs inline in the pump and hides the body (Flags |= 1 and
// Health 1; its deploy leg raises the Health and clears only the dead bit), so
// the edge after the pump skips it and no death is raised.
// [orig: Server_ConvertPlayerToSpectator -- Flags @0x519E92, Health @0x519EA4, the
//  deploy @0x519ECE; Server_DeployPlayer's dead-bit clear @0x51787A; the
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
		CHECK(f.world.round_sim.deaths.empty());
		inmatch::Server_TickUpdate(f.ctx);
		inmatch::Server_TickUpdate(f.ctx);
		CHECK(f.conn(0).link.spectator); // the convert took
		CHECK(!f.sent_death(1));
		CHECK(!f.dead_bit(0));
		CHECK(f.world.round_sim.deaths.empty());
	}
}

// D-NET-551: a 0x51 naming a target, then a 0x41, in one pump. The convert
// runs inline, and its deploy clears the +364 hold for a handle other than
// 0xFFFF; the 0x41 after it stores the timeout into +364 again and zeroes the
// Health the deploy raised. The hidden body raises no death, and its next 0x0A
// restamps Health 1.
// [orig: Server_ProcessClientRequestSpectatorRespawn -- the
//  Server_ConvertPlayerToSpectator call @0x51C8D5; Server_DeployPlayer -- `cmp
//  bp, 0FFFFh` @0x51786B, the +364 store @0x517872;
//  NapiNPServerMsg_DeathTimeoutReset -- Health @0x510568, +364 @0x510587;
//  NetPacket_WritePlayerState -- Health 1 @0x4FF728]
void test_a_named_convert_then_a_0x41_keeps_the_0x41s_hold() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	f.ctx.config.permanent_death = true;
	f.ctx.config.spectator_slots = 1;
	f.conn(0).link.spawn_target_hold_seconds = 4;
	(void)f.deaths_of(0);
	const uint16_t own = f.players[0].packed; // a named handle (resolves to nothing)
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0),
			{make_protocol_message(c2s::SPECTATOR_RESPAWN,
					 {static_cast<uint8_t>(own), static_cast<uint8_t>(own >> 8)}),
			 make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {})},
			100, f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK(f.conn(0).link.spectator);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 5u);
	CHECK(f.entity(0).health == 0);
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 0);
	CHECK(!f.dead_bit(0));
}

// D-NET-554: a 0x51, then a 0x4D, in one pump. The convert runs inline and sets
// the slot's spectator latch, which the team change's gate refuses: no team
// change, no death, no stamp.
// [orig: Server_ProcessClientRequestSpectatorRespawn @0x51C8D5;
//  Server_ConvertPlayerToSpectator -- the latch @0x519E61;
//  NapiNPServerMsg_0x04D_ChangeTeam -- `cmp [edi+188D7h], bl` @0x518FAE]
void test_a_convert_then_a_team_change_changes_nothing() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	f.ctx.config.permanent_death = true;
	f.ctx.config.spectator_slots = 1;
	f.ctx.config.change_team_penalty_seconds = 60;
	f.ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	(void)f.deaths_of(0);
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0),
			{make_protocol_message(c2s::SPECTATOR_RESPAWN, {0xFF, 0xFF}),
			 make_protocol_message(c2s::TEAM_CHANGE_REQUEST, {})},
			100, f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK(f.conn(0).link.spectator);
	CHECK(f.entity(0).team == 0);
	CHECK(f.conn(0).reply.team_change_ms == 0u);
	CHECK(f.conn(0).link.respawn_delay_seconds != 60u);
	CHECK(f.deaths_of(0) == 0);
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 0);
}

// D-NET-460: a 0x41, a Default Spawn 0x0E and a 0x41 in one pump at a respawn
// timeout of 0, in session: the 0x0E reads the dead bit, still clear after the
// 0x41's Health 0 (the edge runs after the pump), with no hold, and refuses, and
// the edge in the frame's entity update kills the body once.
// [orig: Server_ProcessClientRequestRespawn -- the dead bit @0x519CC1, the hold
//  @0x519CC7, is_in_session @0x519CD0 / @0x519CD7; Entity_UpdateInfantryPlayerBody
//  @0x4B4BFE]
void test_reset_deploy_reset_in_one_pump_dies_once() {
	Fixture f;
	f.ctx.config.respawn_timeout = 0;
	f.conn(0).seed_armed_clock = 0xDEADu; // a deploy's re-arm would restamp it
	(void)f.deaths_of(0);
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0),
			{make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {}),
			 make_protocol_message(c2s::RESPAWN_REQUEST, {0xFF, 0xFF}),
			 make_protocol_message(c2s::DEATH_TIMEOUT_RESET, {})},
			100, f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK(f.world.round_sim.deaths.empty());
	CHECK(f.conn(0).seed_armed_clock == 0xDEADu); // the 0x0E deployed nothing
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.deaths_of(0) == 1);
	CHECK(f.dead_bit(0));
	CHECK(f.world.round_sim.deaths.empty());
}

// D-NET-477: the 0x41's frame. The 0x41 runs in the tick's receive pump and its
// death is the player body's edge in the entity update after the tick, so the
// frame's 0x0A to the sender carries the body undead at Health 0: its +360 byte
// withheld (the writer's dead-bit test) and +364 at the raw timeout. The next
// frame's 0x0A carries the death's re-armed +360, its 0x13 ahead of it.
// [orig: Server_TickUpdate -- the receive pump @0x51D895, the per-slot 0x0A
//  @0x51E3E4; NetPacket_WritePlayerState -- the dead-bit test @0x4FF83C;
//  Game_ProcessMainFrame -- Entity_UpdateAllEntities @0x52674B after
//  Server_TickUpdate @0x5266B6; the edge Entity_UpdateInfantryPlayerBody
//  @0x4B4BF1..0x4B4CEA]
void test_the_0x41s_frame_fans_the_body_undead() {
	Fixture f;
	f.ctx.config.respawn_timeout = 5;
	// The slot's seed arm predates the death's 620-frame recent-spawn window.
	f.conn(0).seed_armed_clock = f.world.out.rounds.frame_clock - 620u;
	inmatch::Server_TickUpdate(f.ctx); // the first frame's periodic second
	(void)f.sent_death(1);
	(void)f.sent_death(0);
	f.request(0);
	const FrameUpdate own = f.next_frame(0);
	CHECK(own.local_tail_present && own.health == 0);
	CHECK(own.weapon.present);
	CHECK(own.weapon.slot_state360 == 0); // withheld: the dead bit is still clear
	CHECK(own.weapon.slot_state364 == 5); // the raw timeout
	CHECK(!f.sent_death(1));
	// The frame's entity update latched the death and re-armed +360 (5, over
	// the request's 5); no spawn zones clear +364.
	CHECK(f.dead_bit(0));
	CHECK(f.conn(0).link.respawn_delay_seconds == 5);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 0);
	const FrameUpdate next = f.next_frame(0);
	CHECK(next.weapon.present && next.weapon.slot_state360 == 5);
	CHECK(f.sent_death(1));
}

// D-NET-477: on a periodic-second frame the 0x41's death re-arms +360 after
// that second's decrement, so the re-armed value escapes it: the request's 1
// counts down to 0, and the death's floor of 3 stands.
// [orig: Server_TickUpdate -- the periodic decrement @0x51DFB0..0x51E00B ahead
//  of the per-slot 0x0A @0x51E3E4; GameEvent_PlayerDeath -- +360 floored
//  @0x516EC4..0x516EC6]
void test_a_periodic_frames_death_escapes_its_decrement() {
	Fixture f;
	f.ctx.config.respawn_timeout = 1;
	f.request(0); // the first frame is a periodic second
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.dead_bit(0));
	CHECK(f.conn(0).link.respawn_delay_seconds == 3);
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
	test_a_named_convert_then_a_0x41_keeps_the_0x41s_hold();
	test_a_convert_then_a_team_change_changes_nothing();
	test_reset_deploy_reset_in_one_pump_dies_once();
	test_the_0x41s_frame_fans_the_body_undead();
	test_a_periodic_frames_death_escapes_its_decrement();
	test_refusals();
	if (failures == 0) std::printf("death_timeout_reset: all passed\n");
	return failures == 0 ? 0 : 1;
}
