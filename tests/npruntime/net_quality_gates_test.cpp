// npruntime_net_quality_gates: the host CNetQuality send window's holds and its level arithmetic
// (F6, net parity lane L8).
//   - the window's level: the first sample after a clear lands in slot 1 of five; one fps-0
//     sample (bandwidth metric 255) in the window averages 51 (level 1) and two average 102
//     (level 2), the buckets 1..84 / 85..169;
//   - the round-over latch (g_RoundOver on the authority, set by the round end) holds the
//     window cleared, the level 0, and stops the 1 Hz S2C 0x46 quality resends.
// [orig: CNetQuality_UpdateMetrics @0x4C52C0 - the gates @0x4C52EA..0x4C5308 ->
//  CNetStats_ClearSendCounters @0x4C55E3, the cursor @0x4C530E..0x4C5318, the bandwidth term
//  @0x4C531B..0x4C5350, the level tail @0x4C5893; Server_TickUpdate's resend gate
//  @0x51DE4B..0x51DE5F; Server_ProcessRoundEnd's store @0x5168E4]

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_net_quality.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/ingame_message_id.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

int failures = 0;
void expect(bool cond, const char *what) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++failures;
}

// A HostOnly authority in session over a live world. Heap-held (MSVC's 1 MB test-main stack).
struct Host {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	Host() {
		inmatch::set_connection_mode(*ctx, inmatch::ConnectionMode::HostOnly);
		ctx->is_in_session = 1;
		world->rules.mp_session = true;
		world->registry.configure_pool(0, 8);
		ctx->world = world.get();
	}
	// One sample now, at frame rate `fps`.
	void sample(int32_t fps) {
		ctx->stats_avg_fps = fps;
		ctx->net_quality_sample_countdown = 0;
		inmatch::Server_SampleHostNetQuality(*ctx);
	}
};

void run_the_window_level_counts_fps_zero_samples() {
	Host one;
	one.sample(0);
	expect(one.ctx->host_quality_window.cursor == 1 &&
	               one.ctx->host_quality_window.bandwidth[1] == 255,
	       "the first sample after a clear lands in slot 1, fps 0 reading 255");
	one.sample(62);
	expect(one.ctx->host_network_quality == 51 && one.ctx->net_quality_level == 1,
	       "one fps-0 sample in the window: (255 + 1) / 5 = 51, level 1");

	Host two;
	two.sample(0);
	two.sample(0);
	two.sample(62);
	expect(two.ctx->host_network_quality == 102 && two.ctx->net_quality_level == 2,
	       "two fps-0 samples: (255 + 255 + 1) / 5 = 102, level 2");
	two.sample(62);
	two.sample(62);
	expect(two.ctx->host_network_quality == 102 && two.ctx->net_quality_level == 2,
	       "both 255s still in the window: level 2");
	two.sample(62);
	expect(two.ctx->host_network_quality == 51 && two.ctx->net_quality_level == 1,
	       "the first rotated out: 51, level 1");
	two.sample(62);
	expect(two.ctx->host_network_quality == 1 && two.ctx->net_quality_level == 1,
	       "both rotated out: 1, level 1");
}

void run_the_round_over_latch_holds_the_window() {
	Host h;
	ns::UdpSessionTransport wire{ns::UdpSessionTransport::Role::Host};
	inmatch::NapiNPConnection &conn = h.ctx->np_protocol.connection_list.emplace_back();
	conn.type = inmatch::NapiNPConnection::kTypeServerSide;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.transport = &wire;
	conn.link.mode = ns::TransportMode::Client;
	conn.reply.player_slot = 1;
	conn.reply.client_quality = 2;
	conn.reply.client_quality_dirty = true;

	h.sample(0);
	expect(h.ctx->host_network_quality == 51, "a live round samples");
	inmatch::Server_EmitQualityResends(*h.ctx, *h.world);
	ns::Datagram d;
	bool sent = false;
	while (wire.pop_outbound(d)) sent = sent || d.tag == opennova::s2c::PLAYER_SYNC;
	expect(sent, "a live round resends a dirty slot's quality");

	h.world->process_round_end(1);
	expect(h.world->match.outcome().ended, "the round is over");
	h.sample(62);
	expect(h.ctx->host_quality_window.cursor == 0 && h.ctx->host_network_quality == 0 &&
	               h.ctx->net_quality_level == 0,
	       "the round-over latch clears the window instead of sampling it");
	h.ctx->np_protocol.connection_list[0].reply.client_quality_dirty = true;
	inmatch::Server_EmitQualityResends(*h.ctx, *h.world);
	sent = false;
	while (wire.pop_outbound(d)) sent = sent || d.tag == opennova::s2c::PLAYER_SYNC;
	expect(!sent && h.ctx->np_protocol.connection_list[0].reply.client_quality_dirty,
	       "and holds the quality resend, the slot left dirty");
}

} // namespace

int main() {
	run_the_window_level_counts_fps_zero_samples();
	run_the_round_over_latch_holds_the_window();
	if (failures == 0) std::printf("npruntime_net_quality_gates: all passed\n");
	return failures == 0 ? 0 : 1;
}
