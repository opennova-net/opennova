// The host's GetTickCount readers on the process clock (D-NET-NEW-L8-29 and the clock sweep after
// D-NET-NEW-L8-28): retail reads GetTickCount, which runs on through a load or a held start, where
// ours' logic-tick seam (io::host_milliseconds_for_logic_tick) stood still, and so did the run
// clock (host_run_duration_ms) the protocol timers read. The World's tick_count_ms and the host
// context's process_clock_ms are the clock the host binds (HostConfig::clock_ms); each case below
// injects one, holds the logic tick and the run clock and moves the clock, so only a reader on the
// process clock passes.
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/replication/connection.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++g_failures;
}

// A Serve Only host in session with one in-match peer past the control requests' live gate,
// its World on an injected clock.
struct ClockHost {
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx_owner = std::make_unique<inmatch::NapiNPServerCtx>();
	std::unique_ptr<w::World> world_owner = std::make_unique<w::World>();
	std::unique_ptr<ns::UdpSessionTransport> transport =
			std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPServerCtx &ctx = *ctx_owner;
	w::World &world = *world_owner;
	uint32_t clock = 70000;

	ClockHost() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
		inmatch::GameConfig config;
		config.max_players = 4;
		inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
		ctx.process_clock_ms = [this] { return clock; };
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 8);
		world.tick_count_ms = [this] { return clock; };
		const w::EntityHandle player = w::spawn_remote_player(world, w::PlayerSpawn{});
		ctx.world = &world;
		ctx.np_protocol.connection_list.push_back(conn_fixture::make_seeded_conn(7,
				inmatch::NapiNPConnection::kTypeServerSide, transport.get(),
				ns::TransportMode::Client, player, true));
		ctx.np_protocol.connection_list.back().reply.player_slot = 1;
		world.match.upsert_player({player, 1, "A"});
		world.match.player(player)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	}
	inmatch::NapiNPConnection &conn() { return ctx.np_protocol.connection_list.front(); }
	void reply(uint8_t tag, std::vector<uint8_t> body) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &ctx;
		(void)inmatch::dispatch_session_replies(ctx.config, conn(),
				{make_protocol_message(tag, std::move(body))}, world.logic_tick,
				ctx.np_protocol.connection_list, &world, inputs);
	}
};

// Whether a DESCRIPTION record naming `ddstr` left on `wire` (draining it).
bool sent_description(ns::UdpSessionTransport &wire, const char *ddstr) {
	const std::string text(ddstr);
	bool found = false;
	ns::Datagram datagram;
	while (wire.pop_outbound(datagram))
		if (datagram.tag == hightag::DESCRIPTION_PACKET &&
				std::search(datagram.body.begin(), datagram.body.end(), text.begin(),
						text.end()) != datagram.body.end())
			found = true;
	return found;
}

std::vector<uint8_t> u32_pair(uint32_t a, uint32_t b) {
	std::vector<uint8_t> out;
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(a >> (8 * i)));
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(b >> (8 * i)));
	return out;
}

// D-NET-NEW-L8-29: the time-sync round's host stamps and its validator's now are GetTickCount.
// The control quartet stamps the baseline and the round on the clock; across a held tick the
// clock runs 5000 ms while the client reports 5000 ms, inside the 3 % slack, so the round closes
// (on the logic-tick seam the host's delta would be 0 and the round would stay open).
// [orig: Server_RecordCaptureAndSendScore @0x507CA0 - GetTickCount @0x507CC2 / @0x507CDA into
//  +0x15F58 / +0x15F5C; NapiNPServerMsg_ValidateTimeSync @0x502210 - GetTickCount @0x5022B4,
//  the 1.03 slack tests, the round's reset @0x50235F..0x502371]
void test_time_sync_reads_the_process_clock() {
	ClockHost host;
	host.conn().reply.control_request_countdown = 1;
	inmatch::Server_TickUpdate(host.ctx);
	const inmatch::SessionReplyState &st = host.conn().reply;
	expect(st.time_sync_host_baseline_ms == 70000u && st.time_sync_round_host_ms == 70000u,
	       "the quartet stamps the time-sync baseline and round on the process clock");
	const uint32_t sequence = st.time_sync_sequence;
	host.reply(c2s::TIME_SYNC_REPLY, u32_pair(sequence, 1000u));
	expect(st.time_sync_previous_client_ms == 1000u, "the first reply opens the client's side");
	host.clock += 5000u; // the logic tick holds
	host.reply(c2s::TIME_SYNC_REPLY, u32_pair(sequence, 6000u));
	expect(st.time_sync_round_host_ms == 0u && st.time_sync_previous_client_ms == 0u &&
	               st.time_sync_host_baseline_ms == 70000u,
	       "a held tick's 5000 ms of clock closes the round");
}

// D-NET-NEW-L8-30: the RTT request's body and the return leg's now are GetTickCount. The row's
// S2C 0x57 carries the clock; 300 ms of clock later, the logic tick held, the echoed stamp's
// C2S 0x2C (no echo flag) measures a 300 ms round trip (the seam would measure 0).
// [orig: Server_TickUpdate - GetTickCount @0x51E417 into the 0x57's body @0x51E42E;
//  NapiNPServerMsg_HandlePingResponse @0x515070 - GetTickCount @0x515116, `sub eax, edi`
//  @0x51511C, the store @0x515121]
void test_rtt_reads_the_process_clock() {
	ClockHost host;
	inmatch::Server_TickUpdate(host.ctx);
	uint32_t stamp = 0;
	bool sent = false;
	ns::Datagram datagram;
	while (host.transport->pop_outbound(datagram))
		if (datagram.tag == s2c::RTT_ECHO && datagram.body.size() == 5) {
			sent = true;
			stamp = static_cast<uint32_t>(datagram.body[0]) |
					(static_cast<uint32_t>(datagram.body[1]) << 8) |
					(static_cast<uint32_t>(datagram.body[2]) << 16) |
					(static_cast<uint32_t>(datagram.body[3]) << 24);
		}
	expect(sent && stamp == 70000u, "the RTT request carries the process clock");
	host.clock += 300u; // the logic tick holds
	std::vector<uint8_t> back = u32_pair(stamp, 0u);
	back.resize(4); // the bare stamp: no echo flag, the return leg
	host.reply(c2s::RTT_CONSUMED, back);
	expect(host.conn().reply.rtt_ms == 300u, "the return leg measures 300 ms on the clock");
}

// D-NET-NEW-L8-31: the server-info refresh advances the cookie-key ring with GetTickCount plus
// the CRT stream's next draw. Two hosts alike but for a clock 1000 ms apart (the logic tick the
// same) mint keys 1000 apart at the round's first refresh (on the seam they would be equal).
// [orig: Server_TickUpdate - CSessionIdRing_AdvanceAndGenerate called @0x51D93D;
//  CSessionIdRing_AdvanceAndGenerate @0x4DBB80 - GetTickCount @0x4DBB97, rand @0x4DBB9F, the
//  key @0x4DBBA4..0x4DBBB5]
void test_cookie_ring_reads_the_process_clock() {
	ClockHost early;
	ClockHost late;
	late.clock += 1000u;
	const uint32_t index = early.ctx.cookie_key_table.index;
	inmatch::Server_TickUpdate(early.ctx);
	inmatch::Server_TickUpdate(late.ctx);
	expect(early.ctx.cookie_key_table.index != index &&
	               early.ctx.cookie_key_table.index == late.ctx.cookie_key_table.index,
	       "the round's first server tick advances the ring");
	expect(((late.ctx.cookie_key_table.current() - early.ctx.cookie_key_table.current()) &
	                0xFFFFFFu) == 1000u,
	       "the new key carries the process clock");
}

// D-NET-NEW-L8-32: the slot's +100360 stamp (the chat and console lines' 1000 ms throttle, the
// capture flip's stamp and the refused-touch nag's 10 s test) is GetTickCount. A chat line, then
// another 1000 ms of clock later with the logic tick held, both pass the throttle and stamp the
// clock (on the seam the second would be throttled); the console line likewise.
// [orig: NapiNPServer_HandleChatMessage @0x513760 - GetTickCount @0x5137DE, the 1000 ms test
//  @0x513811..0x513819, the stamp @0x513820; NapiNPServerMsg_HandleConsoleLine @0x5199D0 -
//  GetTickCount @0x519A1E, the stamp @0x519A3C]
void test_chat_stamp_reads_the_process_clock() {
	ClockHost host;
	const auto line = [](const char *text, bool with_channel) {
		std::vector<uint8_t> body;
		if (with_channel) body.push_back(0);
		for (const char *c = text; *c != 0; ++c) body.push_back(static_cast<uint8_t>(*c));
		body.push_back(0);
		return body;
	};
	host.reply(c2s::CHAT_MESSAGE, line("hello", true));
	expect(host.conn().reply.chat_last_ms == 70000u, "a chat line stamps the process clock");
	host.clock += 1000u; // the logic tick holds
	host.reply(c2s::CHAT_MESSAGE, line("again", true));
	expect(host.conn().reply.chat_last_ms == 71000u, "1000 ms of clock later the throttle passes");
	host.clock += 1000u;
	host.reply(c2s::ADMIN_NETLOG_COMMAND, line("STATUS", false));
	expect(host.conn().reply.chat_last_ms == 72000u, "the console line throttles on the same clock");
}

// D-NET-NEW-L8-33: the team change's cooldown test and stamp are GetTickCount. With a 1 s
// interval, a request at 70000 stamps it, and one 1000 ms of clock later (the tick held) passes
// the cooldown and stamps 71000 (on the seam it would be refused and the stamp kept).
// [orig: NapiNPServerMsg_0x04D_ChangeTeam @0x518F70 - GetTickCount @0x518FC9, the interval test
//  @0x518FCB..0x518FDF, the stamp's GetTickCount @0x5190F1 into +0x178E4 @0x5190F3]
void test_team_change_reads_the_process_clock() {
	ClockHost host;
	host.ctx.config.change_team_interval_seconds = 1;
	host.ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	host.reply(c2s::TEAM_CHANGE_REQUEST, {});
	expect(host.conn().reply.team_change_ms == 70000u, "a team change stamps the process clock");
	host.clock += 1000u; // the logic tick holds
	host.reply(c2s::TEAM_CHANGE_REQUEST, {});
	expect(host.conn().reply.team_change_ms == 71000u, "1000 ms of clock later the cooldown passes");
}

// D-NET-NEW-L8-34: the ticket wait's 120 s deadlines are GetTickCount against the net player's
// +0xA4, which each game-state change stamps with GetTickCount: the C2S 0x01's state 3, then the
// watchdog's enter request's state 4. The logic tick and the run clock held throughout (on the run
// clock nothing would expire):
//   - a joiner whose 0x01 entered state 3 at 70000, the arm then gone, is reaped with
//     "N.C:NONWTOVALU" past 120 s of clock from its 0x01;
//   - an armed host's request 5000 ms of clock after a joiner's 0x01 enters state 4 and restamps,
//     so the joiner is reaped with "N.C:NWJTICKTMOUT" past 120 s from the request (from the 0x01's
//     stamp it would be 5000 ms early).
// [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0 - SetGameState(3) called @0x512FA6;
//  CNetPlayer_SetGameState @0x4C4060 - GetTickCount @0x4C408A, +0xA4 @0x4C4156;
//  CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 - SetGameState(4) called @0x4C8BCA, state 3's
//  GetTickCount @0x4C8B90 and compare @0x4C8B98, state 4's @0x4C8CB9 / @0x4C8CC1]
void test_ticket_wait_reads_the_process_clock() {
	ClockHost host;
	host.ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	ns::UdpSessionTransport state3_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport state4_wire(ns::UdpSessionTransport::Role::Host);
	auto joiner = [&host](uint32_t id) -> inmatch::NapiNPConnection & {
		for (inmatch::NapiNPConnection &c : host.ctx.np_protocol.connection_list)
			if (c.connection_id == id) return c;
		return host.ctx.np_protocol.connection_list.front();
	};
	// A joined node posts its gated C2S 0x01 (the arm on), entering state 3.
	auto form_post = [&](uint32_t id, ns::UdpSessionTransport &wire) {
		inmatch::NapiNPConnection node = conn_fixture::make_conn(id,
				inmatch::NapiNPConnection::kTypeServerSide, &wire, ns::TransportMode::Client,
				w::EntityHandle{}, false);
		node.phase = inmatch::ConnectionPhase::Joined;
		node.admission_stage = inmatch::GameAdmissionStage::AwaitFormPost;
		host.ctx.np_protocol.connection_list.push_back(std::move(node));
		host.ctx.novaworld_join_tickets_armed = true;
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &host.ctx;
		(void)inmatch::dispatch_session_replies(host.ctx.config, joiner(id),
				{make_protocol_message(0x01, {0x00})}, host.world.logic_tick,
				host.ctx.np_protocol.connection_list, nullptr, inputs);
	};

	form_post(20, state3_wire);
	expect(joiner(20).join_ticket_validation && joiner(20).join_validated_host_ms == 70000u,
	       "the gated 0x01 enters state 3 and stamps the process clock");
	host.ctx.novaworld_join_tickets_armed = false;
	host.clock = 70000u + 0x1D4C0u;
	inmatch::Server_CheckPlayerTimeouts(host.ctx);
	expect(!joiner(20).host_disconnect_sent, "exactly 120 s past the 0x01 does not reap state 3");
	host.clock += 1u;
	inmatch::Server_CheckPlayerTimeouts(host.ctx);
	expect(joiner(20).host_disconnect_sent && sent_description(state3_wire, "N.C:NONWTOVALU"),
	       "past 120 s from the 0x01 the state-3 joiner is reaped");

	const uint32_t posted = host.clock;
	form_post(21, state4_wire);
	host.clock = posted + 5000u;
	inmatch::Server_CheckPlayerTimeouts(host.ctx);
	expect(joiner(21).player_enter_pending() && joiner(21).join_validated_host_ms == posted + 5000u,
	       "the enter request enters state 4 and restamps the process clock");
	host.clock = posted + 5000u + 0x1D4C0u;
	inmatch::Server_CheckPlayerTimeouts(host.ctx);
	expect(!joiner(21).host_disconnect_sent, "exactly 120 s past the request does not reap state 4");
	host.clock += 1u;
	inmatch::Server_CheckPlayerTimeouts(host.ctx);
	expect(joiner(21).host_disconnect_sent && sent_description(state4_wire, "N.C:NWJTICKTMOUT"),
	       "past 120 s from the request the state-4 joiner is reaped");
}

// D-NET-NEW-L8-35: the deploy-idle punt (t35) measures GetTickCount against the state-6 entry's
// GetTickCount stamp. A joiner added and joined at 70000, held for deployment: with the logic tick
// and the run clock held, exactly 360000 ms of clock later the periodic walk keeps it, and the next
// millisecond punts it t35 (on the run clock it would never be punted).
// [orig: Server_OnPlayerJoin @0x51A680 - GetTickCount @0x51A76A, +0x188E4 @0x51A770;
//  Server_TickUpdate - the hold @0x51E0F2, GetTickCount @0x51E0FB, the stamp @0x51E0FD, 0x57E40
//  @0x51E109, the punt @0x51E13D]
void test_deploy_idle_reads_the_process_clock() {
	auto ctx_owner = std::make_unique<inmatch::NapiNPServerCtx>();
	auto world_owner = std::make_unique<w::World>();
	inmatch::NapiNPServerCtx &ctx = *ctx_owner;
	w::World &world = *world_owner;
	uint32_t clock = 70000;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.config.max_players = 4;
	ctx.network_quality_broadcast_countdown = 100;
	ctx.process_clock_ms = [&clock] { return clock; };
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(2, 8);
	world.registry.configure_pool(3, 8);
	world.tick_count_ms = [&clock] { return clock; };
	w::Entity zone;
	zone.kind = w::EntityKind::Building;
	zone.item_id = 0x0500;
	zone.is_spawn_point = true;
	zone.alive = true;
	zone.team = 1;
	(void)world.registry.spawn(2, zone);
	ctx.world = &world;
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection joiner;
	joiner.peer = {0x0100007Fu, 34035};
	joiner.type = inmatch::NapiNPConnection::kTypeServerSide;
	joiner.connection_id = inmatch::kFirstJoinerDcb;
	joiner.link.mode = ns::TransportMode::Client;
	joiner.link.transport = &transport;
	joiner.player_name = "IdleJoiner";
	ctx.np_protocol.connection_list.push_back(std::move(joiner));
	inmatch::NapiNPConnection &live = ctx.np_protocol.connection_list.front();
	(void)inmatch::Server_BuildPlayerInfoAndAdd(ctx, live, world, /*round_init=*/false);
	(void)inmatch::Server_OnPlayerJoin(ctx, live, 1, {0, 0});
	expect(live.link.respawn_pending && live.reply.state6_entry_host_ms_valid &&
	               live.reply.state6_entry_host_ms == 70000u,
	       "the join stamps the state-6 entry on the process clock");
	auto tick_second = [&] {
		do {
			inmatch::Server_TickUpdate(ctx);
		} while (!world.match.periodic_second());
	};
	clock = 70000u + 360000u; // the logic tick and the run clock hold
	tick_second();
	expect(!live.host_disconnect_sent, "exactly six minutes of clock keeps the held joiner");
	clock += 1u;
	tick_second();
	expect(live.host_disconnect_sent && live.host_disconnect_mismatch_type == 35,
	       "the next millisecond of clock punts it t35");
}

// D-NET-NEW-L8-36: the spawn pump's re-nag of a held joiner (its S2C 0x03 at most once per
// 1000 ms) tests and stamps the pump's one GetTickCount. A joiner the cap holds is nagged at 70000;
// with the logic tick and the run clock held, exactly 1000 ms of clock later it is not, and the
// next millisecond it is again, stamped 71001 (on the run clock it would never be re-nagged).
// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8DC0 - GetTickCount @0x4C8DF8, kept
//  @0x4C8DFE; the stamp's zero test @0x4C8F7A..0x4C8F7F, `sub` @0x4C8F85, 0x3E8 @0x4C8F87, the
//  stamp @0x4C8FCD / @0x4C8FD1]
void test_hold_nag_reads_the_process_clock() {
	ClockHost host;
	host.ctx.config.max_players = 1; // the in-match peer fills the cap
	ns::UdpSessionTransport wire(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection held = conn_fixture::make_conn(8,
			inmatch::NapiNPConnection::kTypeServerSide, &wire, ns::TransportMode::Client,
			w::EntityHandle{}, false);
	held.phase = inmatch::ConnectionPhase::Joined;
	held.self_id_seen = true; // its 0x02 listed it
	host.ctx.np_protocol.connection_list.push_back(std::move(held));
	inmatch::SessionReplyState &st = host.ctx.np_protocol.connection_list.back().reply;
	(void)inmatch::Server_ProcessPendingPlayerSpawns(host.ctx, host.world);
	expect(st.admission_hold_nag_pending && st.admission_hold_nag_host_ms == 70000u,
	       "the held joiner is nagged and stamped on the process clock");
	st.admission_hold_nag_pending = false; // its 0x03 went
	host.clock += 1000u;                   // the logic tick and the run clock hold
	(void)inmatch::Server_ProcessPendingPlayerSpawns(host.ctx, host.world);
	expect(!st.admission_hold_nag_pending, "exactly 1000 ms of clock is no re-nag");
	host.clock += 1u;
	(void)inmatch::Server_ProcessPendingPlayerSpawns(host.ctx, host.world);
	expect(st.admission_hold_nag_pending && st.admission_hold_nag_host_ms == 71001u,
	       "the next millisecond of clock re-nags it");
}

} // namespace

int main() {
	test_time_sync_reads_the_process_clock();
	test_rtt_reads_the_process_clock();
	test_cookie_ring_reads_the_process_clock();
	test_chat_stamp_reads_the_process_clock();
	test_team_change_reads_the_process_clock();
	test_ticket_wait_reads_the_process_clock();
	test_deploy_idle_reads_the_process_clock();
	test_hold_nag_reads_the_process_clock();
	if (g_failures != 0) {
		std::fprintf(stderr, "npruntime_host_process_clock: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npruntime_host_process_clock: ok\n");
	return 0;
}
