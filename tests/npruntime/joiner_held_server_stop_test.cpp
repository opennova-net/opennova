// A joiner's session reset stops a server an earlier error-arm exit left unstopped (the process
// keeps it, ClientProcessState::unstopped_server): a destroying route's FullDestroy and the Post
// Menu's reason-2 arm run CNapiGameSession_ResetActiveSession with the joiner's socket still
// open, so after the joiner's own leave burst each held joiner takes its STOP burst over that
// socket and its /INOUT removal line, then HOST STOPPED over that socket's bind address and
// port; an error arm (reason 5) tears nothing down and keeps the server held
// (D-NET-NEW-L1-26's joiner leg).
// [orig: CNapiNPConnection_LogHostStopped @0x61E780 - the manager's socket +8 / +12 @0x61E7A6;
//  NapiUdpSocket_CreateAndBind @0x62D2A0 - bindAddress @0x62D4C1, boundPort @0x62D4C4]
// [orig: CNapiGameSession_FullDestroy @0x4C96A0 - the leave @0x4C96DB,
//  CNapiGameSession_ResetActiveSession called @0x4C96E2; CNapiGameSession_ResetActiveSession
//  @0x4C8A70 - CNapiNPConnection_Destroy called @0x4C8A93, NapiNPProtocol_StopServer called
//  @0x4C8AA1; PostMenu_RouteMissionExit @0x568460 - reason 2's reset @0x56846F, the error arms'
//  loc_5686DA (no teardown); NapiNPProtocol_StopServer @0x62A820]
#include <base/gameprofile/gameprofile.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/protocol_message.h> // frame_session_packet (the held joiner's keepalive)
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_console.h> // NapiLog
#include <runtime/inmatch/server_files.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/player_spawn.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);
const PeerAddr kHost{0x0200000Au, 32786};  // the joined host
const PeerAddr kHeld{0x0100007Fu, 31000};  // the held server's joiner
// The join's own socket: bound on every interface, at the client quad's first port.
const PeerAddr kOwnSocket{0u, 32768};
constexpr uint32_t kHeldCk = 0x5A5A1234u;

class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

struct CaptureSocket final : IDatagramSocket {
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

// A socket that also delivers queued datagrams, each from its source.
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

struct Outcome {
	int host_goodbyes = 0;    // the joiner's own leave burst, to the host
	int stops = 0;            // STOP datagrams to the held joiner
	bool stop_ahead = false;  // a STOP sent ahead of a datagram to the host
	bool held_after = false;
	std::string inout;
	int enumerator_drains = 0; // the embedder's process-enumerator drain runs
	bool drain_ahead_of_stop = false; // the drain ran before the held server's first STOP
};

Outcome close_with_a_held_server(int32_t reason) {
	Outcome out;
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;

	inmatch::ClientProcessState process;
	process.unstopped_server.running = true;
	process.unstopped_server.session_name = "Held";
	inmatch::NapiNPConnection held;
	held.type = inmatch::NapiNPConnection::kTypeServerSide;
	held.phase = inmatch::ConnectionPhase::Joined;
	held.peer = kHeld;
	held.client_ck = kHeldCk;
	process.unstopped_server.nodes.push_back(inmatch::UnstoppedNode{held, {}, {}});

	// The kernel lives on the heap: a World-carrying frame is megabytes.
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->world.registry.configure_pool(0, 16);
	CaptureSocket socket;
	inmatch::JoinerRole role;
	role.bind(*kernel);
	role.set_socket(&socket, kHost);
	role.set_title(&kJo);
	role.use_process_state(&process);
	role.set_server_logs(logs);
	role.set_local_address(kOwnSocket);
	// The session reset's connection destroy drains the process enumerator ahead of the held
	// server's stop (CNapiGameSession_ResetActiveSession: the destroy, then StopServer).
	role.set_enumerator_drain([&out, &socket] {
		++out.enumerator_drains;
		bool stopped = false;
		for (const auto &sent : socket.sent) stopped = stopped || sent.first == kHeld;
		out.drain_ahead_of_stop = !stopped;
	});
	role.kit_seams.apply_authoritative = [] {};
	role.kit_seams.reseed_on_side_change = [] { return false; };
	role.kit_seams.push = [] {};
	role.create_runtime("HeldStopJoiner", inmatch::JoinRole::Player, "", "");
	// Its joiner was heard just now: the join attempt's pump keeps it (no reap).
	process.unstopped_server.nodes[0].conn.receive_stamp_ms =
			static_cast<uint32_t>(role.runtime->join_clock_ms());
	inmatch::TickInput input;
	role.poll_preload();
	role.runtime->seed_session(0x0FE0E112u, 0x0000BEEFu, "CLIENT-HELD-SCRK", "SERVER-HELD-SCRK",
			1, 0, /*self_handle=*/0x0005, world::kPlayerInfantryTypeId);
	role.run_tick(input);
	role.runtime->set_mission_exit_reason(reason);
	socket.sent.clear();
	role.close();

	const std::vector<uint8_t> stop = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(kHeldCk,
					make_disconnect_event(1, 9, 0, 0, "", 0, "NP.C:SH:STOP")));
	for (const auto &[to, bytes] : socket.sent) {
		if (to == kHeld) {
			if (bytes == stop) ++out.stops;
		} else if (to == kHost) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			if (nw_decode_inbound(bytes.data(), bytes.size(), opcode, body) &&
					opcode == SESSION_OPCODE_CLIENT_GOODBYE)
				++out.host_goodbyes;
			if (out.stops != 0) out.stop_ahead = true;
		}
	}
	out.held_after = process.unstopped_server.running ||
			!process.unstopped_server.nodes.empty();
	out.inout = files.files[inmatch::kInoutLogFileName] + inout.pending();
	return out;
}

// FullDestroy(1) (ESC's route) and reason 2's reset: the joiner's leave, then the held server's
// STOP burst over the same socket, the removal line ahead of HOST STOPPED, nothing held after.
void test_a_session_reset_stops_the_held_server_after_the_leave() {
	for (const int32_t reason : {inmatch::kMissionExitQuit, inmatch::kMissionExitReset}) {
		const Outcome out = close_with_a_held_server(reason);
		CHECK(out.host_goodbyes == 4);
		CHECK(out.stops > 0);
		CHECK(!out.stop_ahead);
		CHECK(!out.held_after);
		// The connection destroy drained the process enumerator once, ahead of the stop.
		CHECK(out.enumerator_drains == 1 && out.drain_ahead_of_stop);
		const std::size_t removed = out.inout.find(
				"127.0.0.1:31000 : SERVER PLAYER REMOVED \"\". [1,9,0,0,\"\",0,\"NP.C:SH:STOP\"]");
		// HOST STOPPED formats the socket the stop goes out on: its bind address and port.
		const std::size_t stopped =
				out.inout.find("0.0.0.0:32768 : HOST STOPPED \"Held\".");
		CHECK(removed != std::string::npos && stopped != std::string::npos && removed < stopped);
	}
}

// An error arm (reason 5) tears nothing down: the server stays held for the next reset.
void test_an_error_arm_keeps_the_server_held() {
	const Outcome out = close_with_a_held_server(5);
	CHECK(out.host_goodbyes == 0);
	CHECK(out.stops == 0);
	CHECK(out.held_after);
	CHECK(out.inout.find("PLAYER REMOVED") == std::string::npos);
	// No reset, no destroy: the enumerator's records stay for the next drain.
	CHECK(out.enumerator_drains == 0);
}

// The join attempt's protocol pump is Pump(-1): it receives first, a held node's joiner's 0x43
// from its own source reaching that node (the receive stamp moves), then walks the held nodes
// ahead of the join's own connection, so a node its joiner left silent past cs_dir0's timeout
// (120000 ms) is reaped over the join's socket: its four 0x86 echoing the SERTMOUT record and
// its /INOUT removal line, the node off the held list (D-NET-NEW-L1-26, the join leg).
// [orig: UI_JoinSelectedSession - CNapiNetwork_PumpAndCheckState called @0x569E60;
//  CNapiNetwork_PumpAndCheckState @0x4C6E80 - NapiNPProtocol_Pump(-1) called @0x4C6EA2;
//  NapiNPProtocol_Pump @0x62A650 - PumpRecvQueues called @0x62A6AC, the walk's PumpFlags
//  called @0x62A6F8, the destroy @0x62A793; NapiNPProtocol_HandleSessionPacket @0x626A00 -
//  FindConnection(1, sender) called @0x626AC5]
void test_the_join_attempts_pump_steps_the_held_server() {
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;

	const PeerAddr kSilent{0x0100007Fu, 31001};
	inmatch::ClientProcessState process;
	process.unstopped_server.running = true;
	process.unstopped_server.session_name = "Held";
	for (const PeerAddr &peer : {kHeld, kSilent}) {
		inmatch::NapiNPConnection held;
		held.type = inmatch::NapiNPConnection::kTypeServerSide;
		held.phase = inmatch::ConnectionPhase::Joined;
		held.peer = peer;
		held.client_ck = peer == kHeld ? kHeldCk : 0x22222222u;
		held.server_scrk = "SERVERHELDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD";
		held.client_scrk = "CLIENTHELDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD";
		held.server_sk = 0x0BADF00Du;
		process.unstopped_server.nodes.push_back(inmatch::UnstoppedNode{held, {}, {}});
	}

	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->world.registry.configure_pool(0, 16);
	QueueSocket socket;
	inmatch::JoinerRole role;
	role.bind(*kernel);
	role.set_socket(&socket, kHost);
	role.set_title(&kJo);
	role.use_process_state(&process);
	role.set_server_logs(logs);
	role.kit_seams.apply_authoritative = [] {};
	role.kit_seams.reseed_on_side_change = [] { return false; };
	role.kit_seams.push = [] {};
	role.create_runtime("HeldPumpJoiner", inmatch::JoinRole::Player, "", "");
	const uint32_t now = static_cast<uint32_t>(role.runtime->join_clock_ms());
	// Both joiners last heard 120001 ms ago, both nodes' last sends then too.
	for (inmatch::UnstoppedNode &node : process.unstopped_server.nodes) {
		node.conn.receive_stamp_ms = now - 120001u;
		node.conn.last_session_send_ms = now - 120001u;
	}
	// kHeld's joiner sends a keepalive the join's socket reads ahead of the pump's walk.
	SessionSequencing joiner;
	std::vector<uint8_t> keepalive_body;
	CHECK(frame_session_packet(joiner,
			SessionCrypto{process.unstopped_server.nodes[0].conn.client_scrk, {},
					process.unstopped_server.nodes[0].conn.server_sk},
			{}, keepalive_body));
	socket.inbound.emplace_back(kHeld,
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(keepalive_body)));

	role.poll_preload(); // the hello, then the attempt's pump
	CHECK(role.runtime->phase() == inmatch::JoinerConnection::Phase::Hello);
	CHECK(process.unstopped_server.running);
	CHECK(process.unstopped_server.nodes.size() == 1);
	if (process.unstopped_server.nodes.size() == 1) {
		CHECK(process.unstopped_server.nodes[0].conn.peer == kHeld);
		CHECK(process.unstopped_server.nodes[0].conn.receive_stamp_ms - now < 120000u);
	}
	int goodbyes = 0;
	for (const auto &[to, bytes] : socket.sent) {
		if (!(to == kSilent)) continue;
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (nw_decode_inbound(bytes.data(), bytes.size(), opcode, body) &&
				opcode == SESSION_OPCODE_SERVER_GOODBYE)
			++goodbyes;
	}
	CHECK(goodbyes == 4);
	const std::string text = files.files[inmatch::kInoutLogFileName] + inout.pending();
	CHECK(text.find("127.0.0.1:31001 : SERVER PLAYER REMOVED") != std::string::npos);
	CHECK(text.find("NP.C:PT:SERTMOUT") != std::string::npos);
}

} // namespace

int main() {
	test_a_session_reset_stops_the_held_server_after_the_leave();
	test_an_error_arm_keeps_the_server_held();
	test_the_join_attempts_pump_steps_the_held_server();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_held_server_stop: all passed\n");
	return 0;
}
