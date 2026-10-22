// The host's GetTickCount readers on the process clock (D-NET-NEW-L8-29 and the clock sweep after
// D-NET-NEW-L8-28): retail reads GetTickCount, which runs on through a load or a held start, where
// ours' logic-tick seam (io::host_milliseconds_for_logic_tick) stood still. The World's
// tick_count_ms is the clock the host binds (HostConfig::clock_ms); each case below injects one,
// holds the logic tick and moves the clock, so only a reader on the process clock passes.
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/replication/connection.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

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
		(void)inmatch::dispatch_session_replies(ctx.config, conn(),
				{make_protocol_message(tag, std::move(body))}, world.logic_tick,
				ctx.np_protocol.connection_list, &world);
	}
};

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

} // namespace

int main() {
	test_time_sync_reads_the_process_clock();
	test_rtt_reads_the_process_clock();
	if (g_failures != 0) {
		std::fprintf(stderr, "npruntime_host_process_clock: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npruntime_host_process_clock: ok\n");
	return 0;
}
