// A lossy UDP transport must not turn either pre-session or sequenced join legs into one-shots.
// The pre-session 0x41/0x42 legs retry byte-identically. Once a sequenced session exists, retail's
// active-send interval instead emits a NEW header-only sequence; the resulting gap makes the peer
// request reconstruction of the retained semantic packet through 0x44/0x84.

#include <npruntime/client_runtime.h>
#include <npruntime/napi_np_protocol.h>

#include "host_test_setup.h"

#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool is_opcode(const std::vector<uint8_t> &datagram, uint8_t expected) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected;
}

bool decode_session(const std::vector<uint8_t> &datagram, uint8_t expected_opcode,
		const std::string &scrk, ProtocolPacketHeader &header,
		std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected_opcode &&
			decode_protocol_packet_plaintext(
					body.data(), body.size(), scrk, header, messages);
}

bool is_resend_request(const std::vector<uint8_t> &datagram, uint8_t expected_opcode,
		uint32_t local_key, uint32_t expected_sequence) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	std::vector<uint32_t> requested;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected_opcode &&
			decode_session_resend_list(
					body.data(), body.size(), local_key, requested) &&
			requested == std::vector<uint32_t>{expected_sequence};
}

bool run_dropped_hello_and_auth_recover() {
	// Capture-pinned pre-session Hello/Auth retransmit cadence (the retail handshake timer xref
	// is still ungrilled) — deliberately NOT the 10000-ms session active-send interval.
	constexpr uint64_t kHandshakeRetryMs = 1000;
	const PeerAddr peer{0x0100007Fu, 32769};

	np::NapiNPServerCtx host;
	uint64_t now_ms = 0;
	np::ClientRuntime client("RetryJoiner", [&now_ms] { return now_ms; });
	const std::vector<uint8_t> first_hello = client.start();
	if (!expect(is_opcode(first_hello, SESSION_OPCODE_CLIENT_HELLO),
			"start emits retail ClientHello 0x41")) {
		return false;
	}
	const np::HandleResult cold_result = np::handle_server_datagram(
			host, peer, first_hello.data(), first_hello.size(), 0);
	if (!expect(cold_result.outbound.empty(),
			"cold host drops the initial ClientHello")) {
		return false;
	}
	if (!expect(client.Client_ProcessNetworkFrame(7).empty(),
			"handshake retry timer arms without an immediate duplicate")) {
		return false;
	}
	now_ms = kHandshakeRetryMs - 1;
	if (!expect(client.Client_ProcessNetworkFrame(7).empty(),
			"ClientHello is not retried before the active-send interval")) {
		return false;
	}

	np::test::bring_up_host(host, np::ConnectionMode::HostClient,
			np::SocketMode::Socketless, 0x0FE0E112u);
	now_ms = kHandshakeRetryMs;
	const std::vector<std::vector<uint8_t>> hello_retry =
			client.Client_ProcessNetworkFrame(7);
	if (!expect(hello_retry.size() == 1 && hello_retry[0] == first_hello,
			"dropped ClientHello retries byte-identically at the interval")) {
		return false;
	}

	const np::HandleResult hello_result = np::handle_server_datagram(
			host, peer, hello_retry[0].data(), hello_retry[0].size(), 1);
	if (!expect(hello_result.outbound.size() == 1 &&
				is_opcode(hello_result.outbound[0], SESSION_OPCODE_SERVER_HELLO),
			"real host accepts the retried ClientHello")) {
		return false;
	}
	client.receive(
			hello_result.outbound[0].data(), hello_result.outbound[0].size());
	const std::vector<std::vector<uint8_t>> auth_out =
			client.Client_ProcessNetworkFrame(9999);
	if (!expect(auth_out.size() == 1 &&
				is_opcode(auth_out[0], SESSION_OPCODE_CLIENT_AUTH),
			"ServerHello advances the joiner to ClientAuth 0x42")) {
		return false;
	}
	const std::vector<uint8_t> first_auth = auth_out[0];

	now_ms = 2 * kHandshakeRetryMs - 1;
	if (!expect(client.Client_ProcessNetworkFrame(1).empty(),
			"ClientAuth is not retried before the active-send interval")) {
		return false;
	}
	now_ms = 2 * kHandshakeRetryMs;
	const std::vector<std::vector<uint8_t>> auth_retry =
			client.Client_ProcessNetworkFrame(1);
	if (!expect(auth_retry.size() == 1 && auth_retry[0] == first_auth,
			"dropped ClientAuth retries byte-identically at the interval")) {
		return false;
	}

	const np::HandleResult auth_result = np::handle_server_datagram(
			host, peer, auth_retry[0].data(), auth_retry[0].size(), 2);
	if (!expect(!auth_result.outbound.empty(),
			"real host accepts the retried ClientAuth")) {
		return false;
	}
	for (const std::vector<uint8_t> &reply : auth_result.outbound)
		client.receive(reply.data(), reply.size());
	const std::vector<std::vector<uint8_t>> post_auth =
			client.Client_ProcessNetworkFrame(300);
	if (!expect(client.phase() == np::JoinerConnection::Phase::Driving,
			"retried pre-session handshake reaches the in-match drive")) {
		return false;
	}
	for (const std::vector<uint8_t> &datagram : post_auth) {
		if (!expect(!is_opcode(datagram, SESSION_OPCODE_CLIENT_AUTH),
				"ServerAuth cancels the ClientAuth retry")) {
			return false;
		}
	}
	return true;
}

bool run_client_active_probe_recovers_join() {
	// The JOINTOPERATIONS template's active_send_interval_ms [orig: CNapiNetwork_Init @0x4ca4a0].
	constexpr uint64_t kRetailActiveSendIntervalMs = 10000;
	constexpr uint32_t kServerKey = 0x11223344u;
	const std::string server_scrk = "SERVER-ACTIVE-PROBE-SCRK";
	uint64_t now_ms = 0;
	np::JoinerConnection joiner("ClientProbe", [&now_ms] { return now_ms; });

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	const std::vector<uint8_t> hello_datagram = joiner.start();
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_HELLO &&
				parse_client_hello(body.data(), body.size(), hello),
			"decode active-probe ClientHello")) {
		return false;
	}
	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const np::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	if (!expect(hello_result.outbound.size() == 1,
			"active-probe ServerHello emits ClientAuth")) {
		return false;
	}
	const std::vector<uint8_t> client_auth_datagram = hello_result.outbound[0];
	ClientAuth client_auth;
	if (!expect(nw_decode_inbound(
				client_auth_datagram.data(), client_auth_datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), client_auth),
			"decode active-probe ClientAuth")) {
		return false;
	}

	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, server_scrk,
			"", "", "", false);
	server_auth.mi = 3;
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	const np::JoinerConnection::PollResult auth_result = joiner.handle_datagram(
			server_auth_datagram.data(), server_auth_datagram.size());
	if (!expect(auth_result.outbound.empty(),
			"ServerAuth alone has no retained session record to probe")) {
		return false;
	}
	now_ms = kRetailActiveSendIntervalMs + 1;
	if (!expect(joiner.pump(0).empty(),
			"missing settings are recovered by the server-side active probe")) {
		return false;
	}

	SessionSequencing server_seq = np::make_jo_game_session_sequencing();
	std::vector<uint8_t> settings_body;
	if (!expect(frame_session_packet(
				server_seq, SessionCrypto{server_scrk, {}, client_auth.ck}, {
					make_protocol_message(
							0x00,
							{0x00, 0x00, 0x20, 0x00, 0x00,
							 0x14, 0x05, 0x00, 0x00},
							0xA0),
					make_protocol_message(
							0x00,
							{0x01, 0x00, 0x20, 0x00, 0x00,
							 0x14, 0x05, 0x00, 0x00},
							0xA0),
				}, settings_body),
			"frame active-probe settings")) {
		return false;
	}
	const std::vector<uint8_t> settings_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(settings_body));
	const np::JoinerConnection::PollResult settings_result =
			joiner.handle_datagram(settings_datagram.data(), settings_datagram.size());
	if (!expect(settings_result.outbound.size() == 2,
			"settings emit ACK plus retained JOIN")) {
		return false;
	}
	const std::vector<uint8_t> join_datagram = settings_result.outbound[1];
	ProtocolPacketHeader join_header;
	std::vector<ProtocolMessage> join_messages;
	const std::vector<uint8_t> base_game_join = {
			'V', 'E', 'R', 'S', 'I', 'O', 'N', 'C',
			'R', 'C', 'S', 'T', 'R', 'I', 'N', 'G',
			0x00, 0x02, 0x00, 0x30, 0x00,
	};
	if (!expect(decode_session(
				join_datagram, SESSION_OPCODE_PROTOCOL_MESSAGE,
				client_auth.scrk, join_header, join_messages) &&
				join_messages.size() == 1 &&
				join_messages[0].tag == 0x00 &&
				join_messages[0].payload == base_game_join,
			"base-game ServerHello emits the 21-byte JOIN without an EXP TLV")) {
		return false;
	}

	now_ms += kRetailActiveSendIntervalMs;
	if (!expect(joiner.pump(0).empty(),
			"retained JOIN does not probe at exactly the active interval")) {
		return false;
	}
	++now_ms;
	const std::vector<std::vector<uint8_t>> active_probe = joiner.pump(0);
	ProtocolPacketHeader probe_header;
	std::vector<ProtocolMessage> probe_messages;
	if (!expect(active_probe.size() == 1 &&
				active_probe[0].size() == 18 &&
				decode_session(
						active_probe[0], SESSION_OPCODE_PROTOCOL_MESSAGE,
						client_auth.scrk, probe_header, probe_messages) &&
				probe_messages.empty() &&
				probe_header.session_id == kServerKey &&
				probe_header.seq_num == 3 &&
				probe_header.ack_count == 1,
			"retained JOIN timeout emits a fresh header-only sequence 3")) {
		return false;
	}

	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(
				client_auth.ck, {2}, resend_body),
			"frame server JOIN resend request")) {
		return false;
	}
	const std::vector<uint8_t> resend_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_RESEND_LIST, std::move(resend_body));
	const np::JoinerConnection::PollResult recovered = joiner.handle_datagram(
			resend_datagram.data(), resend_datagram.size());
	return expect(recovered.outbound.size() == 1 &&
				recovered.outbound[0] == join_datagram,
			"server 0x84 reconstructs the retained JOIN at sequence 2");
}

bool run_server_active_probe_recovers_settings() {
	const PeerAddr peer{0x0100007Fu, 32769};
	np::NapiNPServerCtx host;
	np::test::bring_up_host(host, np::ConnectionMode::HostClient,
			np::SocketMode::Socketless, 0x0FE0E112u);
	np::ClientRuntime client("SettingsProbe");

	const std::vector<uint8_t> hello = client.start();
	const np::HandleResult hello_result = np::handle_server_datagram(
			host, peer, hello.data(), hello.size(), 0);
	if (!expect(hello_result.outbound.size() == 1,
			"settings recovery host emits ServerHello")) {
		return false;
	}
	client.receive(hello_result.outbound[0].data(), hello_result.outbound[0].size());
	const std::vector<std::vector<uint8_t>> auth =
			client.Client_ProcessNetworkFrame(0);
	if (!expect(auth.size() == 1, "settings recovery client emits ClientAuth"))
		return false;
	const np::HandleResult auth_result = np::handle_server_datagram(
			host, peer, auth[0].data(), auth[0].size(), 0);
	if (!expect(auth_result.outbound.size() == 2 &&
				is_opcode(auth_result.outbound[0], SESSION_OPCODE_SERVER_AUTH) &&
				is_opcode(
						auth_result.outbound[1],
						SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE),
			"host emits ServerAuth followed by retained settings")) {
		return false;
	}
	const std::vector<uint8_t> settings_datagram = auth_result.outbound[1];
	client.receive(auth_result.outbound[0].data(), auth_result.outbound[0].size());
	if (!expect(client.Client_ProcessNetworkFrame(0).empty(),
			"dropped settings leave the client waiting without ClientAuth retry")) {
		return false;
	}

	if (!expect(np::tick_connections(host, 10000, 0).empty(),
			"server does not active-probe at exactly 10000 ms")) {
		return false;
	}
	const std::vector<np::TickOut> ticks = np::tick_connections(host, 1, 0);
	if (!expect(ticks.size() == 1 && ticks[0].outbound.size() == 1,
			"retained settings make the server active-probe after 10000 ms")) {
		return false;
	}
	const np::NapiNPConnection &connection =
			host.np_protocol.connection_list.front();
	ProtocolPacketHeader probe_header;
	std::vector<ProtocolMessage> probe_messages;
	if (!expect(decode_session(
				ticks[0].outbound[0],
				SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
				connection.server_scrk, probe_header, probe_messages) &&
				probe_messages.empty() &&
				probe_header.seq_num == 2 &&
				probe_header.ack_count == 0,
			"server settings timeout emits fresh header-only sequence 2")) {
		return false;
	}

	client.receive(ticks[0].outbound[0].data(), ticks[0].outbound[0].size());
	const std::vector<std::vector<uint8_t>> missing =
			client.Client_ProcessNetworkFrame(0);
	if (!expect(missing.size() == 1 &&
				is_resend_request(
						missing[0], SESSION_OPCODE_CLIENT_RESEND_LIST,
						connection.server_sk, 1),
			"client requests missing server settings sequence 1")) {
		return false;
	}
	const np::HandleResult recovered = np::handle_server_datagram(
			host, peer, missing[0].data(), missing[0].size(), 0);
	if (!expect(recovered.outbound.size() == 1 &&
				recovered.outbound[0] == settings_datagram,
			"client 0x44 reconstructs the retained settings at sequence 1")) {
		return false;
	}

	client.receive(recovered.outbound[0].data(), recovered.outbound[0].size());
	const std::vector<std::vector<uint8_t>> post_settings =
			client.Client_ProcessNetworkFrame(0);
	return expect(post_settings.size() == 2 &&
				client.phase() == np::JoinerConnection::Phase::Driving,
			"recovered settings resume the retail ACK plus JOIN sequence");
}

// The EMPTY send interval keeps a QUIET session alive: with nothing queued and nothing
// retained, the pump mints a header-only packet every 30 s so the peer's 120 s connection
// timeout never fires. Without this leg a joiner parked at the deploy pick (or dead, with
// the uplink gate shut) transmits nothing and a stock host reaps it after ~2 minutes.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628fd0 empty leg @0x629041..0x629067;
//  cs_dir0.idle_send_interval_ms = 30000 / timeout_ms = 120000 @0x4ca4a0]
bool run_idle_keepalive_survives_a_quiet_session() {
	constexpr uint64_t kIdleIntervalMs = 30000;
	constexpr uint32_t kServerKey = 0x5A5A1234u;
	const std::string client_scrk = "CLIENT-IDLE-KEEPALIVE-SCRK";
	const std::string server_scrk = "SERVER-IDLE-KEEPALIVE-SCRK";
	// A non-zero start so the seeded anchor is distinguishable from "never sent".
	uint64_t now_ms = 1000;
	np::JoinerConnection joiner("IdleKeepalive", [&now_ms] { return now_ms; });
	// Seed a live in-match session with NOTHING retained and nothing queued — the exact
	// state a joiner parked at the deploy pick (or dead, uplink gate shut) sits in, and
	// the state the ACTIVE interval deliberately ignores.
	joiner.seed_in_match(kServerKey, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0001, 0x14B9);
	if (!expect(joiner.pump(0).empty(), "idle: a fresh seed does not mint immediately")) {
		return false;
	}

	now_ms = 1000 + kIdleIntervalMs;
	if (!expect(joiner.pump(0).empty(),
			"idle: no keepalive at exactly the empty interval")) {
		return false;
	}
	now_ms = 1000 + kIdleIntervalMs + 1;
	const std::vector<std::vector<uint8_t>> keepalive = joiner.pump(0);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(keepalive.size() == 1 &&
				decode_session(keepalive[0], SESSION_OPCODE_PROTOCOL_MESSAGE,
						client_scrk, header, messages) &&
				messages.empty() && header.session_id == kServerKey,
			"idle: a quiet session mints a header-only keepalive past the interval")) {
		return false;
	}
	// The mint re-anchors the clock: no second keepalive until another full interval.
	if (!expect(joiner.pump(0).empty(), "idle: the keepalive re-anchors the clock")) {
		return false;
	}
	now_ms += kIdleIntervalMs + 1;
	if (!expect(joiner.pump(0).size() == 1,
			"idle: the keepalive repeats every interval while the session stays quiet")) {
		return false;
	}
	// The whole point: a stock peer reaps at the production timeout, and the cadence pinned above
	// (the mint lands at kIdleIntervalMs + 1) keeps the gap between transmissions well inside it.
	return expect(2 * kIdleIntervalMs < np::JO_GAME_SESSION_TIMEOUT_MS,
			"idle: two keepalive intervals still fit inside the retail connection timeout");
}

} // namespace

int main() {
	if (!run_idle_keepalive_survives_a_quiet_session()) return 1;
	if (!run_dropped_hello_and_auth_recover()) return 1;
	if (!run_client_active_probe_recovers_join()) return 1;
	if (!run_server_active_probe_recovers_settings()) return 1;
	std::printf("OK\n");
	return 0;
}
