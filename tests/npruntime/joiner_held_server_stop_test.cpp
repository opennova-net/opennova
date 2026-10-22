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
#include <net/npwire/session_ping.h> // build_session_ping_body (the held joiner's ping)
#include <net/title/title_identity.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/punt_log.h> // ConnectLog (the manager's lines)
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
	// The connect log's file, read at each held joiner's send (null: not read): whether the
	// transport's teardown line was already written when a STOP went out.
	const MemoryFiles *files = nullptr;
	bool teardown_ahead_of_stop = false;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
		if (files == nullptr || !(to == kHeld)) return;
		const auto log = files->files.find(inmatch::kConnectLogFile);
		if (log != files->files.end() &&
				log->second.find("NET TEARING DOWN SOCKET") != std::string::npos)
			teardown_ahead_of_stop = true;
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
	bool teardown_line = false; // the connect log's NET TEARING DOWN SOCKET was written
	bool teardown_ahead_of_stop = false; // ... before a STOP went out
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
	// The connect log, on: the transport's teardown writes its line after the held server's
	// stop (CNapiGameSession_FullDestroy: ResetActiveSession's StopServer @0x4C8AA1 ahead of
	// CNapiNetwork_TearDownSocket @0x4C9763).
	inmatch::ConnectLog connect_log{files};
	connect_log.set_clock([] { return std::string("T"); });
	connect_log.start(inmatch::ConnectLogHeader{});
	socket.files = &files;
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
	role.runtime->set_connect_log(&connect_log);
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
	out.teardown_line = files.files[inmatch::kConnectLogFile].find("NET TEARING DOWN SOCKET") !=
			std::string::npos;
	out.teardown_ahead_of_stop = socket.teardown_ahead_of_stop;
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
		// FullDestroy's transport teardown writes its line after the reset's held-server stop;
		// reason 2's reset is no FullDestroy and writes none.
		CHECK(out.teardown_line == (reason == inmatch::kMissionExitQuit));
		CHECK(!out.teardown_ahead_of_stop);
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

// A join attempt over one held node (kHeld, keyed, heard 5000 ms before the attempt), with the
// process's /INOUT and _connectlog.txt logs on (D-NET-NEW-L1-26, the browse-socket legs).
struct HeldAttempt {
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inmatch::ConnectLog connect{files};
	inmatch::ClientProcessState process;
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	QueueSocket socket;
	inmatch::JoinerRole role;
	uint32_t now = 0;
	SessionSequencing joiner; // the held joiner's own sequencing

	HeldAttempt() {
		inout.set_stamp_text([] { return std::string(); });
		inout.set_enabled(true);
		connect.set_clock([] { return std::string(); });
		connect.start(inmatch::ConnectLogHeader{});
		inmatch::ServerLogs logs;
		logs.inout = &inout;
		logs.inout_switch = true;
		logs.connect = &connect;
		process.unstopped_server.running = true;
		process.unstopped_server.session_name = "Held";
		process.unstopped_server.protocol_name = "PN";
		inmatch::NapiNPConnection held;
		held.type = inmatch::NapiNPConnection::kTypeServerSide;
		held.phase = inmatch::ConnectionPhase::Joined;
		held.peer = kHeld;
		held.client_ck = kHeldCk;
		held.server_scrk = "SERVERHELDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD";
		held.client_scrk = "CLIENTHELDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD";
		held.server_sk = 0x0BADF00Du;
		process.unstopped_server.nodes.push_back(inmatch::UnstoppedNode{held, {}, {}});
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, kHost);
		role.set_title(&kJo);
		role.use_process_state(&process);
		role.set_server_logs(logs);
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.create_runtime("HeldLegsJoiner", inmatch::JoinRole::Player, "", "");
		now = static_cast<uint32_t>(role.runtime->join_clock_ms());
		inmatch::NapiNPConnection &conn = process.unstopped_server.nodes[0].conn;
		conn.receive_stamp_ms = now - 5000u;
		conn.last_session_send_ms = now;
	}
	const inmatch::NapiNPConnection &node() const {
		return process.unstopped_server.nodes[0].conn;
	}
	// The held joiner's next 0x43 (a header-only keepalive), keyed with `key`.
	std::vector<uint8_t> session_packet(uint32_t key) {
		std::vector<uint8_t> body;
		CHECK(frame_session_packet(joiner, SessionCrypto{node().client_scrk, {}, key}, {}, body));
		return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
	}
	std::string connect_text() { return files.files[inmatch::kConnectLogFile]; }
	std::string inout_text() { return files.files[inmatch::kInoutLogFileName] + inout.pending(); }
};

uint8_t sent_opcode(const std::vector<uint8_t> &bytes, std::vector<uint8_t> *body_out = nullptr) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(bytes.data(), bytes.size(), opcode, body)) return 0;
	if (body_out != nullptr) *body_out = std::move(body);
	return opcode;
}

// A join attempt's first frame runs Pump(-1) with the join's connection already at the list's
// tail: the held node's legs (its queued 0x25) go out first, then the join's own first send,
// the enumerate's 0x41.
// [orig: CNapiGameSession_InitTransportConnection @0x4C9E10 - CreateServerConnection called
//  @0x4C9E16, NapiNPProtocol_Pump(-1) @0x4CA14B; CNapiNPConnection_Create - the tail link
//  @0x62B17D..0x62B18A; NapiNPProtocol_Pump @0x62A650 - the walk's PumpFlags call @0x62A6F8]
void test_the_attempts_first_send_follows_the_held_walk() {
	HeldAttempt t;
	ProtocolMessage reset = make_protocol_message(0x25, {});
	reset.reliable = false;
	t.process.unstopped_server.nodes[0].queued.push_back(reset);
	t.role.poll_preload();
	std::size_t held_first = t.socket.sent.size();
	std::size_t hello = t.socket.sent.size();
	for (std::size_t i = 0; i < t.socket.sent.size(); ++i) {
		const auto &[to, bytes] = t.socket.sent[i];
		if (to == kHeld && held_first == t.socket.sent.size()) held_first = i;
		if (to == kHost && hello == t.socket.sent.size() &&
				sent_opcode(bytes) == SESSION_OPCODE_CLIENT_HELLO)
			hello = i;
	}
	CHECK(held_first < t.socket.sent.size());
	CHECK(hello < t.socket.sent.size());
	CHECK(held_first < hello);
	CHECK(t.process.unstopped_server.nodes[0].queued.empty());
}

// The socket legs at the attempt's receive, then the queue pass at its pump: a ping asking for
// an answer stamps the node and takes its 0x85 at once; a 0x43 keyed for another node's key is
// tossed; a goodbye is queued, and the pump's queue pass latches the client's record (its
// role 2), so the node is destroyed behind its legs: the removal line with that record and four
// 0x86 echoing it. The manager's lines go to the connect log in the read's order.
// [orig: Nwu_HandlePing @0x623A70 - the key @0x623BC2, the stamp @0x623C56, SendPing called
//  @0x623C6F; NapiNPProtocol_HandleSessionPacket @0x626A00 - the key @0x626B72;
//  NapiNPManager_HandlePacket @0x622F10 - cb_on_dispatch @0x622FCD, cb_on_error(4) @0x622FF9;
//  Nwu_HandleDisconnect @0x623CE0 - the key @0x623E74, the type-1 queue @0x624125, the latch
//  @0x6240A8..0x6240BA, RequestDisconnect called @0x6240C4; NapiNPProtocol_Pump @0x62A650 - the
//  pending test @0x62A6FD, the latch only where none is @0x62A770, the destroy @0x62A793]
void test_a_held_joiners_ping_and_goodbye_at_the_attempt() {
	HeldAttempt t;
	const uint32_t key = t.node().server_sk;
	t.socket.inbound.emplace_back(kHeld, nw_encode_outbound(SESSION_OPCODE_CLIENT_PING,
			build_session_ping_body(key, /*wants_reply=*/true, 1234u)));
	t.socket.inbound.emplace_back(kHeld, t.session_packet(0x12345678u));
	const DisconnectEvent leave = make_disconnect_event(2, 2, 0, 0, "I.C:CIDEMIS", 0, "");
	t.socket.inbound.emplace_back(kHeld,
			nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE, client_goodbye_to_bytes(key, leave)));
	t.role.poll_preload();

	const std::vector<uint8_t> pong = nw_encode_outbound(SESSION_OPCODE_SERVER_PING,
			build_session_ping_body(kHeldCk, /*wants_reply=*/false, 1234u));
	const std::vector<uint8_t> echoed =
			nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE, server_goodbye_to_bytes(kHeldCk, leave));
	int pongs = 0;
	int goodbyes = 0;
	bool pong_first = false;
	for (std::size_t i = 0; i < t.socket.sent.size(); ++i) {
		const auto &[to, bytes] = t.socket.sent[i];
		if (!(to == kHeld)) continue;
		if (bytes == pong) {
			++pongs;
			pong_first = goodbyes == 0;
		}
		if (bytes == echoed) ++goodbyes;
	}
	CHECK(pongs == 1 && pong_first);
	CHECK(goodbyes == 4);
	CHECK(t.process.unstopped_server.nodes.empty() && t.process.unstopped_server.queue.empty());
	CHECK(t.inout_text().find("127.0.0.1:31000 : SERVER PLAYER REMOVED \"\". "
			"[2,2,0,0,\"I.C:CIDEMIS\",0,\"\"]") != std::string::npos);

	const std::string text = t.connect_text();
	const std::size_t raw = text.find("NAPI GOT RAW ");
	const std::size_t ping_taken = text.find(" FROM 127.0.0.1:31000 [PN]");
	const std::size_t tossed = text.find(" FROM 127.0.0.1:31000 [4]");
	const std::size_t goodbye_taken =
			ping_taken == std::string::npos ? ping_taken : text.find(" [PN]", ping_taken + 1);
	CHECK(raw != std::string::npos && text.find("NAPI GOT COOKED ") != std::string::npos);
	CHECK(raw < ping_taken && ping_taken < tossed && tossed < goodbye_taken &&
			goodbye_taken != std::string::npos);
}

// A held joiner's packet past a gap waits on its node's ordered queue, and the queue pass's
// connection walk requests the missing one: one 0x84 over the attempt's socket naming
// sequence 1, keyed with the joiner's own key; the node's receive does not move.
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00 - the future leg @0x626C0C..0x626C3A;
//  NapiNPProtocol_PumpRecvQueues @0x6266A0 - the latch @0x6269BB,
//  CNapiNPConnection_SendMissingSeqList called @0x6269CE]
void test_a_held_gap_takes_its_missing_sequence_request() {
	HeldAttempt t;
	const uint32_t key = t.node().server_sk;
	(void)t.session_packet(key); // sequence 1, lost
	t.socket.inbound.emplace_back(kHeld, t.session_packet(key));
	const uint32_t stamp = t.node().receive_stamp_ms;
	t.role.poll_preload();
	int requests = 0;
	for (const auto &[to, bytes] : t.socket.sent) {
		std::vector<uint8_t> body;
		if (!(to == kHeld) || sent_opcode(bytes, &body) != SESSION_OPCODE_SERVER_RESEND_LIST)
			continue;
		++requests;
		std::vector<uint32_t> named;
		CHECK(decode_session_resend_list(body.data(), body.size(), kHeldCk, named));
		CHECK(named == std::vector<uint32_t>{1u});
	}
	CHECK(requests == 1);
	CHECK(t.process.unstopped_server.nodes.size() == 1 && t.node().receive_stamp_ms == stamp);
}

// The joined match's pumps run flags 0x1A and 0x2E2, neither with bit 0: a held joiner's 0x43
// its socket reads in the match is queued by the socket leg and stays queued, its node's
// receive unmoved.
// [orig: CNapiNetwork_PumpClientProtocolRecv @0x4C4FE0 - `push 1Ah` @0x4C4FEB;
//  CNapiNetwork_PumpClientProtocolSend @0x4C5000 - `push 2E2h` @0x4C500B;
//  NapiNPProtocol_PumpRecvQueues @0x6266A0 - the bit-0 test @0x6266C5;
//  NapiNPProtocol_HandleSessionPacket @0x626A00 - the queue store @0x626C8C]
void test_a_joined_match_leaves_a_held_session_packet_queued() {
	HeldAttempt t;
	t.role.poll_preload();
	t.role.runtime->seed_session(0x0FE0E112u, 0x0000BEEFu, "CLIENT-HELD-SCRK", "SERVER-HELD-SCRK",
			1, 0, /*self_handle=*/0x0005, world::kPlayerInfantryTypeId);
	inmatch::TickInput input;
	t.role.run_tick(input);
	CHECK(t.process.unstopped_server.nodes.size() == 1);
	const uint32_t stamp = t.node().receive_stamp_ms;
	t.socket.inbound.emplace_back(kHeld, t.session_packet(t.node().server_sk));
	t.role.run_tick(input);
	CHECK(t.process.unstopped_server.nodes.size() == 1);
	CHECK(t.node().receive_stamp_ms == stamp);
	CHECK(t.process.unstopped_server.queue.size() == 1);
	if (t.process.unstopped_server.queue.size() == 1) {
		CHECK(t.process.unstopped_server.queue[0].from == kHeld);
		CHECK(t.process.unstopped_server.queue[0].opcode == SESSION_OPCODE_PROTOCOL_MESSAGE);
	}
}

} // namespace

int main() {
	test_a_session_reset_stops_the_held_server_after_the_leave();
	test_an_error_arm_keeps_the_server_held();
	test_the_join_attempts_pump_steps_the_held_server();
	test_the_attempts_first_send_follows_the_held_walk();
	test_a_held_joiners_ping_and_goodbye_at_the_attempt();
	test_a_held_gap_takes_its_missing_sequence_request();
	test_a_joined_match_leaves_a_held_session_packet_queued();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_held_server_stop: all passed\n");
	return 0;
}
