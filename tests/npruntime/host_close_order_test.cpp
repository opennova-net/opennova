// D-NET-NEW-L1-26: the client callback's process-lifetime disconnect latch (ctx+0xD94) and the
// post-mission router's error arm, driven through HostRole::close as the shell's session end runs
// it. The disconnected callback keeps the first record it is handed until a join's start clears
// it and maps the exit from the latch, so a listen host whose process still holds a kicked
// joiner session's record (DC 2 with the kick's code) maps its own leave (exit 1, the CIDEMIS
// record) to the kick's exit: 35 -> 7, 37 -> 10. Those exits take the router's error arm, which
// skips the session's destroy, so StopServer never runs and the joiners get no goodbye burst.
// With an empty latch the leave's own record is latched (it maps nothing), the exit stays 1 and
// the joiners get the STOP burst. The router decides the stop per exit: 1 and 3 destroy the
// session (the burst), the error arms skip it, a NovaWorld session's end (12) among them.
// (The latch's joiner leg, its write at a joiner's disconnect and
// its clear at a join's start, is the joiner's; here the kick's record is put in the latch as
// that leg leaves it.)
// [orig: CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 - the empty test @0x4C6535, the copy
//  @0x4C6545, valid @0x4C6559, the switch from @0x4C655F (35 -> 7 @0x4C65A2, 36 -> 9 @0x4C65C9);
//  CNapiNPConnection_QueueDisconnectEvent @0x4C7EF0 (a kick's DC 2 @0x4C7FAE, its code
//  @0x4C7FDD); CNapiNPConnection_TeardownActiveConnection @0x6253C0 (the client callback called
//  @0x6254E7); CNapiGameSession_CreateSession @0x4C97C0 (the own type-2's callback @0x4C9BEC);
//  Input_HandleActionBinding case 3 (exit 1 @0x49AF26, the leave @0x49AF42);
//  Game_TeardownMission @0x522350 (the pump called @0x522588); PostMenu_RouteMissionExit
//  @0x568460 (7 @0x568500..0x568526, 9..19 @0x56852B..0x568588, the destroy @0x568683);
//  CNapiGameSession_FullDestroy @0x4C96A0 (ResetActiveSession called @0x4C96E2);
//  NapiNPProtocol_StopServer @0x62A820 (the STOP record @0x62A8C3..0x62A8F6)]

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/mission/bms.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/client_runtime.h> // ClientProcessState
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/remote_admin.h> // input_action_exit
#include <runtime/inmatch/server_console.h> // inout_host_line
#include <runtime/inmatch/server_files.h>   // NapiLog (_inout.txt)
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
namespace ns = opennova::replication;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

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
const PeerAddr kPeer{0x0100007Fu, 31000};

struct CaptureSocket final : IDatagramSocket {
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

// A LAN listen host over the synthetic mission, its own client in the session, one remote
// joiner's node accepted (its 0x42) and the process's client words the test's.
struct ListenHost {
	std::map<std::string, std::string> files;
	std::unique_ptr<ms::MissionKernel> kernel = std::make_unique<ms::MissionKernel>();
	std::unique_ptr<inmatch::HostRole> role = std::make_unique<inmatch::HostRole>();
	std::unique_ptr<inmatch::ClientProcessState> process =
			std::make_unique<inmatch::ClientProcessState>();
	CaptureSocket socket;
	ns::UdpSessionTransport remote{ns::UdpSessionTransport::Role::Host};

	bool boot() {
		role->bind(*kernel);
		kernel->open_document(test_mission::two_entity_mission(), "close",
				test_boot::source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = game_type::for_mission_mode(bms::selected_game_mode(
				static_cast<bms::AttribFlags>(kernel->mission.header.attrib_flags)));
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.title = &kJo;
			cfg.config.server_name = "close";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true;
			cfg.client_process = process.get();
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role->bring_up(bringup);
		};
		std::string error;
		if (!kernel->boot(options, error)) {
			std::printf("boot failed: %s\n", error.c_str());
			return false;
		}
		role->set_socket(&socket);
		inmatch::NapiNPConnection joiner;
		joiner.type = inmatch::NapiNPConnection::kTypeServerSide;
		joiner.peer = kPeer;
		joiner.connection_id = inmatch::kFirstJoinerDcb;
		joiner.phase = inmatch::ConnectionPhase::Joined;
		joiner.link.transport = &remote;
		joiner.link.mode = ns::TransportMode::Client;
		ctx().np_protocol.connection_list.push_back(joiner);
		return true;
	}
	inmatch::NapiNPServerCtx &ctx() { return role->state.host_owner.ctx; }

	// The STOP goodbye datagrams the joiner was sent.
	int stop_burst() const {
		const std::vector<uint8_t> stop = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
				server_goodbye_to_bytes(0, make_disconnect_event(1, 9, 0, 0, "", 0,
						"NP.C:SH:STOP")));
		int count = 0;
		for (const auto &[to, bytes] : socket.sent)
			if (to == kPeer && bytes == stop) ++count;
		return count;
	}
};

// The listen host's own leave from the frame's input pass, then the session's end.
struct Outcome {
	int32_t exit_reason = 0;
	int stop_burst = 0;
	inmatch::ProcessDisconnectLatch latch;
};

Outcome leave_with_latch(const inmatch::ProcessDisconnectLatch &held) {
	auto host = std::make_unique<ListenHost>();
	Outcome out;
	if (!host->boot()) {
		++failures;
		return out;
	}
	host->process->disconnect_latch = held;
	CHECK(host->ctx().client_disconnect_latch == &host->process->disconnect_latch);
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx();
	inmatch::input_action_exit(binding, inmatch::ExitSite::Input);
	CHECK(host->ctx().mission_exit_reason == inmatch::kMissionExitQuit);
	host->role->close();
	out.exit_reason = host->ctx().mission_exit_reason;
	out.stop_burst = host->stop_burst();
	out.latch = host->process->disconnect_latch;
	return out;
}

inmatch::ProcessDisconnectLatch kick_record(uint32_t code) {
	inmatch::ProcessDisconnectLatch latch;
	// A received kick: the server's DS 1, DC 2, the kick's code as the DPC.
	latch.latch(make_disconnect_event(1, 2, 0, 0, "", code, ""));
	return latch;
}

void test_an_empty_latch_takes_the_leave_and_stops_the_server() {
	const Outcome out = leave_with_latch(inmatch::ProcessDisconnectLatch{});
	CHECK(out.exit_reason == inmatch::kMissionExitQuit);
	CHECK(out.stop_burst > 0);
	CHECK(out.latch.valid && out.latch.event.dc == 2 && out.latch.event.dpc == 0 &&
			out.latch.event.dstr == "I.C:CIDEMIS");
}

void test_a_stale_kick_maps_the_leave_and_skips_the_stop() {
	for (const auto &[code, exit] : {std::pair<uint32_t, int32_t>{35, inmatch::kMissionExitPirate},
				 std::pair<uint32_t, int32_t>{37, 10}}) {
		const Outcome out = leave_with_latch(kick_record(code));
		CHECK(out.exit_reason == exit);
		CHECK(out.stop_burst == 0);
		CHECK(out.latch.valid && out.latch.event.dpc == code); // the first record wins
	}
}

// A stale record that maps the reason 1 (an unlisted code) takes the leave's own route.
void test_a_stale_unlisted_code_keeps_the_stop() {
	const Outcome out = leave_with_latch(kick_record(17));
	CHECK(out.exit_reason == inmatch::kMissionExitQuit);
	CHECK(out.stop_burst > 0);
}

// The router per exit, with no own leave: the destroy's reasons (1, the map cycle's 3) stop the
// server; the error arms (the NovaWorld session's end 12, among 9..19; 5, 6, 7 and 20) reach
// loc_5686DA with no FullDestroy, so no STOP burst.
// [orig: PostMenu_RouteMissionExit @0x568460 - 1 @0x5684A8..0x5684AB -> loc_568654 (the destroy
//  called @0x568683); 5 @0x5684B1..0x5684D6, 6 @0x5684DB..0x5684FB, 7 @0x568500..0x568526,
//  9..19 @0x56852B..0x568588 (12 @0x56854F / @0x568552) -> loc_5686CB, 20 @0x56858E..0x5685BD;
//  loc_5686DA: SetNetworkType(0) called @0x5686E1, the modes @0x5686ED / @0x5686F9, the main
//  menu @0x568703; Game_ProcessMainFrame's NovaWorld exit @0x52655D..0x52657C stores 12]
void test_the_router_arm_decides_the_stop() {
	for (const auto &[reason, stops] : {std::pair<int32_t, bool>{inmatch::kMissionExitQuit, true},
				 std::pair<int32_t, bool>{inmatch::kMissionExitMapCycle, true},
				 std::pair<int32_t, bool>{inmatch::kMissionExitNovaWorld, false},
				 std::pair<int32_t, bool>{inmatch::kMissionExitDisconnectFirst, false},
				 std::pair<int32_t, bool>{inmatch::kMissionExitCdTrouble, false},
				 std::pair<int32_t, bool>{inmatch::kMissionExitSystem, false},
				 std::pair<int32_t, bool>{inmatch::kMissionExitPirate, false},
				 std::pair<int32_t, bool>{inmatch::kMissionExitBadMission, false}}) {
		auto host = std::make_unique<ListenHost>();
		if (!host->boot()) {
			++failures;
			continue;
		}
		host->ctx().mission_exit_reason = reason;
		host->role->close();
		CHECK(host->ctx().mission_exit_reason == reason);
		if (stops) {
			CHECK(host->stop_burst() > 0);
		} else {
			CHECK(host->stop_burst() == 0);
		}
	}
}

// The close's order (Game_TeardownMission, then the router's destroy): the teardown's pump takes
// the own client's held leave (its twin's /INOUT SERVER line) ahead of StopServer's destroys (the
// joiner's line and its STOP burst), and the host-stopped line comes last, after host_running
// clears. [orig: Game_TeardownMission @0x522350 - the pump called @0x522588;
//  NapiNPProtocol_StopServer @0x62A820 - the destroys @0x62A924, host_running cleared @0x62A95C,
//  CNapiNPConnection_LogHostStopped @0x62A97B]
void test_close_runs_the_pump_before_the_stop() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	host->ctx().logs.inout = &inout;
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx();
	inmatch::input_action_exit(binding, inmatch::ExitSite::Input);
	host->role->close();
	const std::string text = inout.pending();
	const std::size_t twin = text.find("0.0.0.0:0 : SERVER PLAYER REMOVED");
	const std::size_t joiner = text.find("127.0.0.1:31000 : SERVER PLAYER REMOVED");
	const std::string stopped = inmatch::inout_host_line(host->ctx(), /*started=*/false);
	const std::size_t last = text.rfind(stopped);
	CHECK(twin != std::string::npos && joiner != std::string::npos && last != std::string::npos);
	CHECK(twin < joiner && joiner < last);
	CHECK(host->stop_burst() > 0);
	host->ctx().logs.inout = nullptr;
}

} // namespace

int main() {
	test_an_empty_latch_takes_the_leave_and_stops_the_server();
	test_a_stale_kick_maps_the_leave_and_skips_the_stop();
	test_a_stale_unlisted_code_keeps_the_stop();
	test_the_router_arm_decides_the_stop();
	test_close_runs_the_pump_before_the_stop();
	std::printf("host_close_order: %d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
