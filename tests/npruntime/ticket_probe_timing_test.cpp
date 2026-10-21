// NEW-L8-12: the join-ticket probe timing. Under the NovaWorld join-ticket gate the C2S 0x01 puts
// the joiner in NetPlayer state 3 and sends no S2C 0x02 probe; the service's enter result, on
// success, sets state 6 and sends it then. Without the gate the 0x01 sends it at once. The probe's
// body is the 512-byte padded timestamp: the host's GetTickCount at its send, the file-transfer
// session id, 0x100, then `rand() >> 3` bytes (D-NET-NEW-L8-20; a context with no world draws them
// from a fresh stream).
// [orig: NapiNPServerMsg_HandleJoinRequest @0x512F87..0x512FA6 (the gate, state 3, return),
//  @0x512FAF..0x512FBD (state 6, the probe); CNapiGameSession_HandlePlayEnterResponse @0x4D1C11
//  (state 6), @0x4D1C19 (CNetPlayer_SendFileTransferChunk(0) @0x4C9440)]

#include "npruntime/conn_fixture.h"

#include <base/io/crt_rand.h>
#include <base/io/le.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>

#include <cstdio>
#include <vector>

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

namespace {

int g_failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++g_failures;                                                            \
		}                                                                            \
	} while (0)

// [orig: NetPacket_BuildPaddedTimestampPayload @0x502A40 (+0 @0x502A5F, +4 @0x502A76, +8 @0x502A82,
//  `rand() >> 3` @0x502A9D); CNetPlayer_SendFileTransferChunk @0x4C9440 (0x100 @0x4C9459)]
std::vector<uint8_t> probe_body(uint32_t tick_ms, uint32_t session_id) {
	std::vector<uint8_t> body(512, 0);
	io::write_u32_le(body.data(), tick_ms);
	io::write_u32_le(body.data() + 4, session_id);
	io::write_u32_le(body.data() + 8, 0x100u);
	io::CrtRand fresh;
	for (size_t i = 12; i < body.size(); ++i) body[i] = static_cast<uint8_t>(fresh.next() >> 3);
	return body;
}

} // namespace

int main() {
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	ctx.np_protocol.host_start_tick = 5000u;
	inmatch::FileTransferSession words; // the process's words after one mission start
	words.id = 1u;
	ctx.file_transfer_session = &words;
	ns::LoopbackChannel wire;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			20, inmatch::NapiNPConnection::kTypeServerSide, &wire, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	conn.admission_stage = inmatch::GameAdmissionStage::AwaitFormPost;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const std::vector<ProtocolMessage> form_post{make_protocol_message(0x01, {0x00})};

	// Gated: the 0x01 holds the joiner in state 3 with no probe.
	ctx.novaworld_join_tickets_armed = true;
	std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(ctx.config, conn,
			form_post, 7, ctx.np_protocol.connection_list, nullptr, inputs);
	CHECK(replies.empty());
	CHECK(conn.join_ticket_validation);
	CHECK(conn.admission_stage == inmatch::GameAdmissionStage::AwaitPlayEnter);
	ns::Datagram d;
	CHECK(!wire.client_recv(d));

	// The enter result's success sends the probe, stamped with the host's tick count then.
	conn.player_enter_requested = true; // the watchdog's ClientPlayerEnterRequest
	ctx.np_protocol.host_run_duration_ms = 41u * 16u;
	CHECK(inmatch::Server_ApplyPlayerEnterResult(ctx, 20, true, 0, 41));
	CHECK(wire.client_recv(d) && d.tag == s2c::JOIN_PADDING_PROBE &&
			d.body == probe_body(5000u + 41u * 16u, 1u));
	CHECK(!wire.client_recv(d));
	CHECK(conn.admission_stage == inmatch::GameAdmissionStage::AwaitPaddingEcho);
	CHECK(conn.admission_padding_x == 5000u + 41u * 16u && conn.admission_padding_y == 1u);

	// Ungated: the 0x01 sends the probe at once.
	ctx.novaworld_join_tickets_armed = false;
	conn.admission_stage = inmatch::GameAdmissionStage::AwaitFormPost;
	conn.player_enter_requested = false;
	conn.player_enter_admitted = false;
	ctx.np_protocol.host_run_duration_ms = 7u * 16u;
	replies = inmatch::dispatch_session_replies(ctx.config, conn, form_post, 7,
			ctx.np_protocol.connection_list, nullptr, inputs);
	CHECK(replies.size() == 1 && replies[0].tag == s2c::JOIN_PADDING_PROBE &&
			replies[0].payload == probe_body(5000u + 7u * 16u, 1u));
	CHECK(!conn.join_ticket_validation);
	CHECK(conn.admission_stage == inmatch::GameAdmissionStage::AwaitPaddingEcho);

	std::printf("ticket_probe_timing: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
