// D-NET-281 — the death screen's team change, host side (C2S 0x4D). Retail's
// handler gates on the authority, the sender's non-spectator slot, the
// mpchangeteam_interval since the last request, TeamChoose and autobalance;
// then swaps team 1 <-> 2 (S2C 0x50), prints the C2Blue / C2Red system line
// (S2C 0x14 [7][255]), kills the player through the player-death transaction
// inline, and only then overwrites the death's respawn holds with the
// mpchangeteam_penalty and closes the revive window; the request stamp is
// taken whenever the gates pass.
// [orig: NapiNPServerMsg_0x04D_ChangeTeam @0x518F70; Server_ShouldAutoBalance
//  @0x4FCB40; Server_CalcTeamImbalance @0x4FCAE0; Server_BroadcastSystemMessage
//  @0x508260; GameEvent_PlayerDeath @0x516DD0]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/server_team_change.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <cstdio>
#include <memory>
#include <string>
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

// Two-team host with `teams.size()` remote players, TeamChoose on, a five
// second respawn timeout and no spawn zones.
struct Fixture {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<w::World> heap = std::make_unique<w::World>();
	w::World &world = *heap;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;

	explicit Fixture(const std::vector<uint8_t> &teams) {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.game_type = 0x10000u; // TDM
		ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
		ctx.config.respawn_timeout = 5;
		ctx.server_text.change_to_blue_format = "%s to Blue";
		ctx.server_text.change_to_red_format = "%s to Red";
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.registry.configure_pool(0, 32);
		ctx.world = &world;
		transports.reserve(teams.size());
		for (size_t i = 0; i < teams.size(); ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			w::PlayerSpawn spawn;
			spawn.position = {10.0f * float(i), 0.0f, 0.0f};
			spawn.team = teams[i];
			players.push_back(w::spawn_remote_player(world, spawn));
		}
		for (size_t i = 0; i < teams.size(); ++i) {
			inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			c.reply.player_slot = static_cast<uint8_t>(i + 1);
			c.reply.player_name = "P" + std::to_string(i + 1);
			c.assigned_team = teams[i];
			c.assigned_team_valid = true;
			c.admission_stage = inmatch::GameAdmissionStage::Complete;
			ctx.np_protocol.connection_list.push_back(std::move(c));
		}
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	w::Entity &entity(size_t i) { return *world.registry.get(players[i]); }
	void request(size_t i, uint32_t now_tick) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &ctx;
		(void)inmatch::dispatch_session_replies(ctx.config, conn(i),
				{make_protocol_message(c2s::TEAM_CHANGE_REQUEST, {})}, now_tick,
				ctx.np_protocol.connection_list, &world, inputs);
	}
	std::vector<ns::Datagram> drain(size_t i) {
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (transports[i].pop_outbound(d)) out.push_back(d);
		return out;
	}
	void drain_all() {
		for (size_t i = 0; i < transports.size(); ++i) (void)drain(i);
	}
};

int index_of(const std::vector<ns::Datagram> &sent, uint8_t tag) {
	for (size_t i = 0; i < sent.size(); ++i)
		if (sent[i].tag == tag) return static_cast<int>(i);
	return -1;
}

std::vector<uint8_t> system_line(const std::string &text) {
	std::vector<uint8_t> body = {7, 0xFF};
	body.insert(body.end(), text.begin(), text.end());
	body.push_back(0);
	return body;
}

// Team 2 -> 1: 0x50, the system line to everyone, the death transaction, then
// the penalty over the death's holds; the stamp.
void test_switch_kills_and_penalizes() {
	Fixture f({2, 1});
	f.drain_all();
	f.request(0, 100);
	CHECK(f.entity(0).team == 1);
	CHECK(f.conn(0).assigned_team == 1);
	const std::vector<ns::Datagram> to_p1 = f.drain(0);
	const std::vector<ns::Datagram> to_p2 = f.drain(1);
	const int assign = index_of(to_p1, s2c::TEAM_ASSIGN);
	const int line = index_of(to_p1, s2c::CHAT_BROADCAST);
	const int death = index_of(to_p1, s2c::ENTITY_DEATH);
	CHECK(assign >= 0 && line > assign && death > line);
	if (line >= 0) CHECK(to_p1[static_cast<size_t>(line)].body == system_line("P1 to Blue"));
	const int line2 = index_of(to_p2, s2c::CHAT_BROADCAST);
	CHECK(line2 >= 0 && index_of(to_p2, s2c::ENTITY_DEATH) > line2);
	if (line2 >= 0) CHECK(to_p2[static_cast<size_t>(line2)].body == system_line("P1 to Blue"));
	CHECK(f.entity(0).health == 0);
	CHECK((f.entity(0).flags & 2u) != 0);
	// The death armed max(timeout, 3) = 5 and, with no spawn zones, +364 = 0;
	// the penalty lands after it.
	CHECK(f.conn(0).link.respawn_delay_seconds == 60);
	CHECK(f.conn(0).link.spawn_target_hold_seconds == 60);
	CHECK(f.conn(0).link.downed_revive_seconds == 0);
	CHECK(f.conn(0).reply.team_change_ms == inmatch::host_milliseconds_for_logic_tick(100));
	// The death ran once: the next host ticks (one periodic second) raise no
	// second transaction and keep the penalty.
	for (int tick = 0; tick < 4; ++tick) inmatch::Server_TickUpdate(f.ctx);
	CHECK(index_of(f.drain(1), s2c::ENTITY_DEATH) < 0);
	CHECK(f.conn(0).link.respawn_delay_seconds >= 59);
}

// The interval runs from the stamp, in host ms; the next switch goes back to
// team 2 with the C2Red line.
void test_interval_gate() {
	Fixture f({2, 1});
	f.request(0, 100);
	CHECK(f.entity(0).team == 1);
	f.drain_all();
	// 300 s from tick 100's stamp is tick 18700 on the 62-tick host clock.
	f.request(0, 18699);
	CHECK(f.entity(0).team == 1);
	CHECK(f.conn(0).reply.team_change_ms == inmatch::host_milliseconds_for_logic_tick(100));
	CHECK(f.drain(0).empty());
	f.request(0, 18700);
	CHECK(f.entity(0).team == 2);
	const std::vector<ns::Datagram> sent = f.drain(0);
	const int line = index_of(sent, s2c::CHAT_BROADCAST);
	CHECK(line >= 0);
	if (line >= 0) CHECK(sent[static_cast<size_t>(line)].body == system_line("P1 to Red"));
	CHECK(f.conn(0).reply.team_change_ms == inmatch::host_milliseconds_for_logic_tick(18700));
}

// TeamChoose off, a spectator slot, or a non-authority: nothing, no stamp.
void test_refusals() {
	Fixture f({2, 1});
	f.ctx.config.mp_attributes &= ~inmatch::GameConfig::kMpAttribTeamChoose;
	f.request(0, 100);
	CHECK(f.entity(0).team == 2 && f.conn(0).reply.team_change_ms == 0);
	f.ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	f.conn(0).link.spectator = true;
	f.request(0, 100);
	CHECK(f.entity(0).team == 2 && f.conn(0).reply.team_change_ms == 0);
	f.conn(0).link.spectator = false;
	f.ctx.is_authority = 0;
	f.request(0, 100);
	CHECK(f.entity(0).team == 2 && f.conn(0).reply.team_change_ms == 0);
}

// A slot on neither team 1 nor 2 only stamps.
void test_other_team_only_stamps() {
	Fixture f({3, 1});
	f.drain_all();
	f.request(0, 100);
	CHECK(f.entity(0).team == 3);
	CHECK(f.drain(0).empty());
	CHECK(f.conn(0).reply.team_change_ms == inmatch::host_milliseconds_for_logic_tick(100));
	CHECK(f.conn(0).link.respawn_delay_seconds == 0);
}

// Autobalance: the slot-table difference (+1 per team-1 slot, -1 per team-2
// slot) must exceed one and reach both thresholds to refuse the switch.
void test_autobalance() {
	Fixture f({2, 1, 1, 1});
	CHECK(inmatch::Server_CalcTeamImbalance(f.ctx) == 2);
	CHECK(!inmatch::Server_ShouldAutoBalance(f.ctx)); // off by default
	f.ctx.config.auto_balance_enabled = true;
	CHECK(inmatch::Server_ShouldAutoBalance(f.ctx));
	f.request(0, 100);
	CHECK(f.entity(0).team == 2 && f.conn(0).reply.team_change_ms == 0);
	f.ctx.config.auto_balance_trigger_difference = 3;
	CHECK(!inmatch::Server_ShouldAutoBalance(f.ctx));
	f.request(0, 100);
	CHECK(f.entity(0).team == 1);
	// A difference of one never balances, whatever the thresholds.
	Fixture g({2, 1});
	g.ctx.config.auto_balance_enabled = true;
	g.ctx.config.auto_balance_min_difference = 0;
	g.ctx.config.auto_balance_trigger_difference = 0;
	CHECK(inmatch::Server_CalcTeamImbalance(g.ctx) == 0);
	CHECK(!inmatch::Server_ShouldAutoBalance(g.ctx));
	// Co-op and non-team types never balance.
	f.ctx.config.auto_balance_trigger_difference = 1;
	f.ctx.config.game_type = 0x30000u;
	CHECK(!inmatch::Server_ShouldAutoBalance(f.ctx));
	f.ctx.config.game_type = 0x1u;
	CHECK(inmatch::Server_CalcTeamImbalance(f.ctx) == 0);
	CHECK(!inmatch::Server_ShouldAutoBalance(f.ctx));
}

// An empty format is retail's null lookup: no line, the rest still runs.
void test_no_text_sends_no_line() {
	Fixture f({2, 1});
	f.ctx.server_text.change_to_blue_format.clear();
	f.drain_all();
	f.request(0, 100);
	CHECK(f.entity(0).team == 1);
	const std::vector<ns::Datagram> sent = f.drain(0);
	CHECK(index_of(sent, s2c::CHAT_BROADCAST) < 0);
	CHECK(index_of(sent, s2c::ENTITY_DEATH) >= 0);
	CHECK(f.conn(0).link.respawn_delay_seconds == 60);
}

} // namespace

int main() {
	test_switch_kills_and_penalizes();
	test_interval_gate();
	test_refusals();
	test_other_team_only_stamps();
	test_autobalance();
	test_no_text_sends_no_line();
	if (failures == 0) std::printf("team_change_request: all passed\n");
	return failures == 0 ? 0 : 1;
}
