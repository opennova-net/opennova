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

#include <cstdint>
#include <cstdio>
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

struct Outcome {
	int host_goodbyes = 0;    // the joiner's own leave burst, to the host
	int stops = 0;            // STOP datagrams to the held joiner
	bool stop_ahead = false;  // a STOP sent ahead of a datagram to the host
	bool held_after = false;
	std::string inout;
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
	role.kit_seams.apply_authoritative = [] {};
	role.kit_seams.reseed_on_side_change = [] { return false; };
	role.kit_seams.push = [] {};
	role.create_runtime("HeldStopJoiner", inmatch::JoinRole::Player, "", "");
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
}

} // namespace

int main() {
	test_a_session_reset_stops_the_held_server_after_the_leave();
	test_an_error_arm_keeps_the_server_held();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_held_server_stop: all passed\n");
	return 0;
}
