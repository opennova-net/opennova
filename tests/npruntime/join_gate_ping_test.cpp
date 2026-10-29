// The joiner's METPROTOCOL PING at its join's connect (D-NET-995): a NovaWorld join that
// reaches its 0x00 JOIN sends PING to the gate's metrics address over the join's socket ahead of
// the JOIN, the process's ping manager's answered rows in its ENTRY groups; a LAN join sends none;
// a second join inside METPING's minutes sends none (the process's stamp).
// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320 - state 4: UI_JoinSelectedSession's
//  success @0x56A4CE, the NovaWorld test @0x56A4D6, the throttle @0x56A4DE..0x56A506,
//  Server_SendPingMetricsToGate (the call @0x56A50B) ahead of the JOIN @0x56A524..0x56A53F;
//  Server_SendPingMetricsToGate @0x511BF0 - the send @0x5120CE]

#include <base/gameprofile/gameprofile.h>
#include <net/napi/envelope.h>
#include <net/novaworld/gate_metrics.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/mission/mission_kernel.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

const Protocol &kJo = *title_protocol("jo");
const PeerAddr kHost{0x0100007Fu, 32769}; // the joined host
const PeerAddr kGate{0x0A00A8C0u, 27000}; // METIPADDRESS 192.168.0.10, METIPPORT 27000
constexpr uint32_t kServerKey = 0x11223344u;
const std::string kServerScrk = "SERVER-PING-SCRK";

// A socket that delivers queued datagrams, each from its source, and records every send.
struct QueueSocket final : IDatagramSocket {
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbound;
	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty() || inbound.front().second.size() > cap) return 0;
		from = inbound.front().first;
		const std::vector<uint8_t> bytes = std::move(inbound.front().second);
		inbound.pop_front();
		std::copy(bytes.begin(), bytes.end(), buf);
		return static_cast<int>(bytes.size());
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

uint8_t opcode_of(const std::vector<uint8_t> &dg, std::vector<uint8_t> &body) {
	uint8_t opcode = 0;
	if (!nw_decode_inbound(dg.data(), dg.size(), opcode, body)) return 0;
	return opcode;
}

struct JoinRun {
	bool authed = false;            // the 0x42 went and the 0x82 answered it
	std::size_t settings_at = 0;    // the sends before the settings packet's frame
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;
	std::string client_scrk;
};

// One enumerated join driven through the 0x81, the 0x42 / 0x82 and the host's initial settings
// packet, whose receive queues the 0x00 JOIN.
JoinRun run_join(bool novaworld) {
	JoinRun run;
	// The kernel lives on the heap: a World-carrying frame is megabytes.
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->world.registry.configure_pool(0, 16);
	QueueSocket socket;
	inmatch::JoinerRole role;
	role.bind(*kernel);
	role.set_socket(&socket, kHost);
	role.set_protocol(&kJo);
	role.set_join_novaworld(novaworld);
	role.kit_seams.apply_authoritative = [] {};
	role.kit_seams.reseed_on_side_change = [] { return false; };
	role.kit_seams.push = [] {};
	role.create_runtime("PingJoiner", inmatch::JoinRole::Player, "", "");
	role.poll_preload(); // the enumerate's 0x41
	ClientHello hello;
	bool have_hello = false;
	for (const auto &[to, dg] : socket.sent) {
		std::vector<uint8_t> body;
		if (opcode_of(dg, body) == SESSION_OPCODE_CLIENT_HELLO)
			have_hello = parse_client_hello(body.data(), body.size(), hello);
	}
	CHECK(have_hello);
	if (!have_hello) return run;
	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	socket.inbound.emplace_back(kHost, nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			server_hello_to_bytes(server_hello)));
	// The 0x81's pass records the session; the 0x42 leaves at the first pump past the connect
	// state's start (a millisecond on the steady clock).
	ClientAuth auth;
	bool have_auth = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!have_auth && std::chrono::steady_clock::now() < deadline) {
		role.poll_preload();
		for (const auto &[to, dg] : socket.sent) {
			std::vector<uint8_t> body;
			if (opcode_of(dg, body) == SESSION_OPCODE_CLIENT_AUTH)
				have_auth = parse_client_auth(body.data(), body.size(), auth);
		}
	}
	CHECK(have_auth);
	if (!have_auth) return run;
	ServerAuth server_auth = build_server_auth(auth, 0x7F000001u, 32769, kServerKey, kServerScrk,
			"", "", "", false);
	server_auth.mi = 3;
	socket.inbound.emplace_back(kHost, nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH,
			server_auth_to_bytes(server_auth)));
	role.poll_preload();
	run.authed = true;
	run.client_scrk = auth.scrk;
	run.settings_at = socket.sent.size();
	// The host's initial settings packet: two settings-update records in S2C sequence 1.
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
	const std::vector<ProtocolMessage> settings = {
			make_protocol_message(0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
			make_protocol_message(0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
	};
	std::vector<uint8_t> settings_body;
	CHECK(frame_session_packet(server_seq, SessionCrypto{kServerScrk, {}, auth.ck}, settings,
			settings_body));
	socket.inbound.emplace_back(kHost, nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			std::move(settings_body)));
	role.poll_preload();
	run.sent = socket.sent;
	return run;
}

// The index of the host-bound datagram carrying the 0x00 JOIN at or after `from` (npos: none).
std::size_t join_index(const JoinRun &run, std::size_t from) {
	for (std::size_t i = from; i < run.sent.size(); ++i) {
		if (!(run.sent[i].first == kHost)) continue;
		std::vector<uint8_t> body;
		if (opcode_of(run.sent[i].second, body) != SESSION_OPCODE_PROTOCOL_MESSAGE) continue;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), run.client_scrk, header,
				messages))
			continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == 0x00) return i;
	}
	return std::string::npos;
}

std::size_t gate_sends(const JoinRun &run) {
	return static_cast<std::size_t>(std::count_if(run.sent.begin(), run.sent.end(),
			[](const auto &s) { return s.first == kGate; }));
}

void set_process_gate() {
	NwuLobbySession::GateMemory &memory = process_gate_memory();
	memory.response.met_label = "jo-metrics";
	memory.response.met_ping = 5;
	memory.response.met_port = kGate.port;
	memory.response.met_ext = 0;
	memory.gate_address = kGate.ip;
	GatePingManager &pings = process_gate_ping_manager();
	pings.start_sweep(1, {{11, "10.0.0.7"}, {12, "10.0.0.8"}});
	pings.apply_result(1, 11, 33);
	pings.report_tick = 0;
}

// The NovaWorld join's PING, at its connect's success and ahead of the JOIN.
void test_a_novaworld_join_reports_ping_ahead_of_its_join() {
	set_process_gate();
	const JoinRun run = run_join(true);
	CHECK(run.authed);
	for (std::size_t i = 0; i < run.settings_at && i < run.sent.size(); ++i)
		CHECK(!(run.sent[i].first == kGate)); // nothing before the connect's success
	CHECK(gate_sends(run) == 1);
	CHECK(run.settings_at < run.sent.size() && run.sent[run.settings_at].first == kGate);
	const std::size_t join = join_index(run, run.settings_at);
	CHECK(join != std::string::npos && join > run.settings_at);
	if (run.settings_at >= run.sent.size()) return;
	const std::vector<uint8_t> &dg = run.sent[run.settings_at].second;
	std::vector<uint8_t> cipher(dg.size());
	std::size_t n = 0;
	CHECK(napi_envelope_decode(dg.data(), dg.size(), cipher.data(), cipher.size(), &n) == 0);
	cipher.resize(n);
	GateMetricsReport report;
	CHECK(gate_metrics_decode(cipher.data(), cipher.size(), report));
	CHECK(report.block == "PING");
	CHECK(report.fields.size() == 3);
	if (report.fields.size() == 3) {
		CHECK(report.fields[0].name == "LABEL" && report.fields[0].value == "jo-metrics");
		CHECK(report.fields[1].name == "GCC" && report.fields[1].value.empty());
		CHECK(report.fields[2].name == "GV" &&
				report.fields[2].value == kJo.version_text);
	}
	CHECK(report.entries.size() == 1);
	if (report.entries.size() == 1 && report.entries[0].size() == 5) {
		CHECK(report.entries[0][0].value == "10" && report.entries[0][3].value == "7");
		CHECK(report.entries[0][4].name == "MS" && report.entries[0][4].value == "33");
	}
	CHECK(process_gate_ping_manager().report_tick != 0);
}

// A second NovaWorld join inside METPING's five minutes: the stamp holds, no PING.
void test_a_second_join_inside_the_minutes_reports_none() {
	const uint32_t stamp = process_gate_ping_manager().report_tick;
	const JoinRun run = run_join(true);
	CHECK(run.authed);
	CHECK(gate_sends(run) == 0);
	CHECK(join_index(run, run.settings_at) != std::string::npos);
	CHECK(process_gate_ping_manager().report_tick == stamp);
}

// A LAN join never reports, whatever the gate memory holds.
void test_a_lan_join_reports_none() {
	set_process_gate();
	const JoinRun run = run_join(false);
	CHECK(run.authed);
	CHECK(gate_sends(run) == 0);
	CHECK(join_index(run, run.settings_at) != std::string::npos);
	CHECK(process_gate_ping_manager().report_tick == 0);
}

} // namespace

int main() {
	test_a_novaworld_join_reports_ping_ahead_of_its_join();
	test_a_second_join_inside_the_minutes_reports_none();
	test_a_lan_join_reports_none();
	if (failures != 0) {
		std::printf("npruntime_join_gate_ping: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_join_gate_ping: ok\n");
	return 0;
}
