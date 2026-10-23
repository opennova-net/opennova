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
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/client_runtime.h> // ClientProcessState
#include <runtime/inmatch/disconnect_reason.h> // disconnect_reason_key (the router's text)
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h> // stop_unstopped_server (the shutdown's reset)
#include <runtime/inmatch/punt_log.h>         // ConnectLog (the held hello's lines)
#include <runtime/inmatch/remote_admin.h> // input_action_exit
#include <runtime/inmatch/server_console.h> // inout_host_line
#include <runtime/inmatch/server_files.h>   // NapiLog (_inout.txt)
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/player_spawn.h> // spawn_remote_player (the flag carrier)

#include <algorithm>
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
	// A host the same process runs later: its process words, its logs, its socket's address and
	// the socket attached ahead of the bring-up, as the shell's host screen binds it.
	inmatch::ClientProcessState *shared_process = nullptr;
	inmatch::ServerLogs logs;
	const PeerAddr *local_address = nullptr;
	bool socket_at_boot = false;
	// The menu's network type the host runs under (a NovaWorld host's setup keeps type 1).
	inmatch::NetworkType network_type = inmatch::NetworkType::Lan;

	bool boot(const opennova::bms::File &mission = test_mission::two_entity_mission()) {
		role->bind(*kernel);
		if (socket_at_boot) role->set_socket(&socket);
		kernel->open_document(mission, "close", test_boot::source_over(&files));
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
			cfg.network_type = network_type;
			cfg.serve_and_play = true;
			cfg.client_process = shared_process != nullptr ? shared_process : process.get();
			cfg.logs = logs;
			if (local_address != nullptr) {
				cfg.local_address = *local_address;
				cfg.local_address_known = true;
			}
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

// The listen host's own leave from the frame's input pass, then the session's end. The own
// client's callback runs inside the leave's disconnect, so the latch's mapping stands from the
// exit on (L1-s32 row 1) [orig: CNapiNPConnection_RequestDisconnect @0x61E0F0 - SetState(6)
// @0x61E0FA; CNapiNPConnection_SetState @0x626250 - TeardownActiveConnection @0x62628F].
struct Outcome {
	int32_t exit_reason_at_exit = 0;
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
	out.exit_reason_at_exit = host->ctx().mission_exit_reason;
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
	CHECK(out.exit_reason_at_exit == inmatch::kMissionExitQuit);
	CHECK(out.exit_reason == inmatch::kMissionExitQuit);
	CHECK(out.stop_burst > 0);
	CHECK(out.latch.valid && out.latch.event.dc == 2 && out.latch.event.dpc == 0 &&
			out.latch.event.dstr == "I.C:CIDEMIS");
}

void test_a_stale_kick_maps_the_leave_and_skips_the_stop() {
	for (const auto &[code, exit] : {std::pair<uint32_t, int32_t>{35, inmatch::kMissionExitPirate},
				 std::pair<uint32_t, int32_t>{37, 10}}) {
		const Outcome out = leave_with_latch(kick_record(code));
		CHECK(out.exit_reason_at_exit == exit);
		CHECK(out.exit_reason == exit);
		CHECK(out.stop_burst == 0);
		CHECK(out.latch.valid && out.latch.event.dpc == code); // the first record wins
	}
}

// A stale record that maps the reason 1 (an unlisted code) takes the leave's own route.
void test_a_stale_unlisted_code_keeps_the_stop() {
	const Outcome out = leave_with_latch(kick_record(17));
	CHECK(out.exit_reason_at_exit == inmatch::kMissionExitQuit);
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

// The S2C 0x25s the socket carried to `to`, decoded under that connection's key.
int wire_resets(const CaptureSocket &socket, const PeerAddr &to, const std::string &key) {
	int resets = 0;
	for (const auto &[peer, bytes] : socket.sent) {
		if (!(peer == to)) continue;
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(bytes.data(), bytes.size(), opcode, body) ||
				opcode != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
			continue;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), key, header, messages))
			continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == s2c::GAME_RESET && !m.flags.settings_update) ++resets;
	}
	return resets;
}

// The S2C 0x25s still queued on a connection's transport, unframed.
int queued_resets(ns::UdpSessionTransport &transport) {
	int resets = 0;
	ns::Datagram staged;
	while (transport.pop_outbound(staged))
		if (staged.tag == s2c::GAME_RESET) ++resets;
	return resets;
}

// The rest of the close's order: after the teardown's pump the authority's one-shot PostMission
// sweep runs (an authored PostMission entry's action, once, at the close), then the per-slot
// reset queues an S2C 0x25 to a remote slot in states 2..7 and none to a slot in state 1. No
// pump runs after the walk, so neither arm puts it on the wire: the destroy arm's STOP burst
// is all the joiner gets (the destroy drains the queue after the burst), an error arm sends
// nothing and leaves the connection, its 0x25 still queued.
// [orig: Game_TeardownMission @0x522350 - the pump called @0x522588, EventTrigger_UpdateAllWithFlag4
//  called @0x52266C, Server_DisconnectAndResetAllPlayerSlots called @0x52269B;
//  Server_DisconnectAndResetAllPlayerSlots @0x516160 - the active byte @0x516190, the state switch
//  over 2..7 @0x51619F..0x5161AB, the 0x25 @0x5161BB queued @0x5161D4 (NapiNPServer_SendToConn
//  @0x4C4F20 -> CNapiNPConnection_QueueMessage @0x628640); PostMenu_RouteMissionExit @0x568460 -
//  Server_EndMissionSession called @0x568672, the destroy @0x568683, the error arms'
//  SetTransportMode(0) @0x5686F9; NapiNPProtocol_StopServer @0x62A820 (the STOP record
//  @0x62A8C3..0x62A8F6); CNapiNPConnection_Destroy @0x62A4B0 - the teardown called @0x62A4F9,
//  CNapiNPConnection_DrainMessageQueues called @0x62A50A]
void test_close_sweeps_post_mission_then_queues_each_slots_reset(int32_t exit_reason) {
	opennova::bms::File mission = test_mission::two_entity_mission();
	opennova::bms::Event post{};
	post.flags = opennova::bms::EventFlags::PostMission;
	post.action_index = 0;
	post.action_count = 2;
	opennova::bms::Action increment{};
	increment.action_type = opennova::bms::ActionType::MisvarChange;
	increment.action_sub_type =
			static_cast<int32_t>(opennova::bms::MissionVariableActionSubType::Increment);
	increment.param1 = 9;
	// A sub-goal lost mid-round: its chat line relays to the joiners at its call.
	opennova::bms::Action lost{};
	lost.action_type = opennova::bms::ActionType::SubGoalLost;
	lost.param1 = 1;
	mission.events = {post};
	mission.actions = {increment, lost};
	auto host = std::make_unique<ListenHost>();
	if (!host->boot(mission)) {
		++failures;
		return;
	}
	const std::string server_scrk = "SERVER-CLOSE-SCRK";
	constexpr uint32_t kSyncedCk = 0x11223344u;
	inmatch::NapiNPConnection &synced = host->ctx().np_protocol.connection_list.back();
	synced.client_ck = kSyncedCk;
	synced.server_sk = 0x55667788u;
	synced.client_scrk = "CLIENT-CLOSE-SCRK";
	synced.server_scrk = server_scrk;
	synced.slot_state = inmatch::PlayerSlotState::Joined;
	synced.burst.spawned = true; // in the match: the relay's 0x90 fan takes it
	ns::UdpSessionTransport added_wire{ns::UdpSessionTransport::Role::Host};
	const PeerAddr added_peer{0x0100007Fu, 31001};
	inmatch::NapiNPConnection added;
	added.type = inmatch::NapiNPConnection::kTypeServerSide;
	added.peer = added_peer;
	added.connection_id = inmatch::kFirstJoinerDcb + 1;
	added.phase = inmatch::ConnectionPhase::Joined;
	added.client_ck = 0x22334455u;
	added.server_sk = 0x66778899u;
	added.client_scrk = "CLIENT-ADDED-SCRK";
	added.server_scrk = "SERVER-ADDED-SCRK";
	added.slot_state = inmatch::PlayerSlotState::Added;
	added.link.transport = &added_wire;
	added.link.mode = ns::TransportMode::Client;
	host->ctx().np_protocol.connection_list.push_back(added);
	CHECK(host->kernel->world.script.vars.get_mission(9) == 0);

	host->ctx().mission_exit_reason = exit_reason;
	host->role->close();
	CHECK(host->kernel->world.script.vars.get_mission(9) == 1); // the sweep ran once

	// The STOP goodbye leads with the joiner's own client CK [orig:
	// CNapiNPConnection_SendDisconnectPacket @0x61F2A0 - the peer key written @0x61F3AF].
	const std::vector<uint8_t> stop = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(kSyncedCk, make_disconnect_event(1, 9, 0, 0, "", 0,
					"NP.C:SH:STOP")));
	int stops = 0;
	for (const auto &[to, bytes] : host->socket.sent)
		if (to == kPeer && bytes == stop) ++stops;
	CHECK(wire_resets(host->socket, kPeer, server_scrk) == 0);
	CHECK(wire_resets(host->socket, added_peer, "SERVER-ADDED-SCRK") == 0);
	// The sweep's relay queued at its action's call, ahead of the walk's 0x25 [orig:
	//  EventAction_Dispatch - GameMsg_AddChatLineAndRelay called @0x454632;
	//  Server_BroadcastEntityActionPacket @0x5080D0 - SendFiltered(0x3F) called @0x508199].
	// On the destroy arm the queue stays on the joiner's transport; an error arm hands it to
	// the held node the process keeps (D-NET-NEW-L1-26).
	std::vector<uint8_t> queued;
	ns::Datagram staged;
	while (host->remote.pop_outbound(staged)) queued.push_back(staged.tag);
	for (const inmatch::UnstoppedNode &node : host->process->unstopped_server.nodes)
		if (node.conn.peer == kPeer)
			for (const ProtocolMessage &m : node.queued) queued.push_back(m.tag);
	const auto at = [&queued](uint8_t tag) {
		return std::find(queued.begin(), queued.end(), tag) - queued.begin();
	};
	CHECK(std::count(queued.begin(), queued.end(), s2c::GAME_RESET) == 1);
	CHECK(std::count(queued.begin(), queued.end(), s2c::OBJECTIVE_NOTIFICATION) == 1);
	CHECK(at(s2c::OBJECTIVE_NOTIFICATION) < at(s2c::GAME_RESET));
	CHECK(queued_resets(added_wire) == 0);
	bool kept = false;
	for (const inmatch::NapiNPConnection &conn : host->ctx().np_protocol.connection_list)
		if (conn.peer == kPeer) kept = true;
	if (exit_reason == inmatch::kMissionExitQuit) {
		CHECK(stops > 0 && !kept);
	} else {
		CHECK(stops == 0 && kept); // the error arm leaves the server unstopped
	}
}

// D-NET-NEW-L1-24 (L1-s16): the close's per-row 0x25 walk takes an orphaned row as any active row:
// a row in states 2..7 queues its 0x25 through its net player (send_mask 0x20 sends through the
// row's +0x1C), so the joiner whose re-add orphaned a state-6 row has two queued, and none on the
// wire (the walk runs after the teardown's last pump).
// [orig: Server_DisconnectAndResetAllPlayerSlots @0x516160 - the active byte @0x516190, the 0x25 to
//  the row @0x5161C2..0x5161D4; NapiNPServer_SendFiltered @0x4C87E0 - the 0x20 arm, the row's
//  +0x1C @0x4C8A1F]
void test_close_resets_an_orphaned_row_through_its_net_player() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	const std::string server_scrk = "SERVER-ORPHAN-SCRK";
	inmatch::NapiNPConnection &joiner = host->ctx().np_protocol.connection_list.back();
	joiner.client_ck = 0x13572468u;
	joiner.server_sk = 0x24681357u;
	joiner.client_scrk = "CLIENT-ORPHAN-SCRK";
	joiner.server_scrk = server_scrk;
	joiner.slot_state = inmatch::PlayerSlotState::Joined;
	joiner.net_player_serial = ++host->ctx().last_net_player_serial; // the re-add's draw
	inmatch::OrphanedPlayerRow orphan;
	orphan.row = joiner; // the joiner's row as its re-add left it, on another slot
	orphan.row.reply.player_slot = 5;
	host->ctx().orphaned_player_rows.push_back(orphan);
	host->ctx().mission_exit_reason = inmatch::kMissionExitQuit;
	host->role->close();
	CHECK(wire_resets(host->socket, kPeer, server_scrk) == 0);
	CHECK(queued_resets(host->remote) == 2);
}

// D-NET-NEW-L1-26 (L8-s11 FOUND 1): an error arm leaves the server unstopped in the process
// (ctx+0x68 set, the joiner's node kept with host_running set); the next host's pre-game setup
// resets the session ahead of its create: StopServer latches STOP on the old joiner, writes its
// /INOUT removal line and sends its STOP burst over the socket the host screen reopened, then
// HOST STOPPED over that socket's address and the old session's name, all ahead of the new
// session's HOST STARTED. With no socket open (the process's network shutdown) the lines alone.
// [orig: PostMenu_RouteMissionExit @0x568460 - the error arms' loc_5686DA, no FullDestroy;
//  UI_HandleHostSessionStart - SetTransportMode(3) @0x556EA6; MultiPlayer_JoinSessionStateMachine
//  @0x56A320 - PreMenu_HostSetup called @0x56A43F (ResetActiveSession @0x5691F5), then
//  CNapiGameSession_BuildAndCreateSession @0x5694D0 (called @0x56A46A);
//  CNapiGameSession_ResetActiveSession @0x4C8A70 - StopServer called @0x4C8AA1;
//  NapiNPProtocol_StopServer @0x62A820;
//  CNapiNPConnection_LogHostStopped @0x61E780 - the socket read @0x61E7A6, the placeholder
//  @0x61E7DD; NapiLib_Shutdown - ResetActiveSession called @0x4C958B]
struct StoppedLater {
	std::string text;
	int stops = 0;
	bool others = false;
	bool held_after = true;
};

StoppedLater error_arm_then(bool next_host) {
	StoppedLater out;
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ClientProcessState process;
	auto first = std::make_unique<ListenHost>();
	first->shared_process = &process;
	if (!first->boot()) {
		++failures;
		return out;
	}
	first->ctx().logs.inout = &inout;
	first->ctx().logs.inout_switch = true;
	inmatch::NapiNPConnection &joiner = first->ctx().np_protocol.connection_list.back();
	joiner.client_ck = 0x5A5A1234u;
	const std::string session_name = first->ctx().np_protocol.session_name;
	const std::vector<uint8_t> stop = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(0x5A5A1234u, make_disconnect_event(1, 9, 0, 0, "", 0,
					"NP.C:SH:STOP")));
	first->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
	first->role->close();
	int early_stops = 0;
	for (const auto &[to, bytes] : first->socket.sent)
		if (to == kPeer && bytes == stop) ++early_stops;
	CHECK(early_stops == 0);
	CHECK(inout.pending().find("PLAYER REMOVED") == std::string::npos);
	// The own pair is held with the joiner (the exit is not the own leave's): the twin, at
	// address 0, first; the own client beside the list.
	CHECK(process.unstopped_server.running && process.unstopped_server.nodes.size() == 2);
	CHECK(process.unstopped_server.own_client.has_value());
	if (process.unstopped_server.nodes.size() == 2) {
		CHECK(process.unstopped_server.nodes[0].conn.peer.ip == 0 &&
				process.unstopped_server.nodes[0].conn.type ==
						inmatch::NapiNPConnection::kTypeServerSide &&
				!process.unstopped_server.nodes[0].conn.disconnect_event_valid);
		CHECK(process.unstopped_server.nodes[1].conn.peer == kPeer);
	}
	first->ctx().logs.inout = nullptr;
	first.reset(); // the session goes; the process keeps the server

	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;
	const PeerAddr reopened{0x0100007Fu, 32768};
	std::string expected_stopped;
	if (next_host) {
		auto second = std::make_unique<ListenHost>();
		second->shared_process = &process;
		second->logs = logs;
		second->local_address = &reopened;
		second->socket_at_boot = true;
		if (!second->boot()) {
			++failures;
			return out;
		}
		for (const auto &[to, bytes] : second->socket.sent) {
			if (to == kPeer && bytes == stop)
				++out.stops;
			else if (out.stops == 0)
				out.others = true; // something ahead of the burst
		}
		expected_stopped = "127.0.0.1:32768 : HOST STOPPED \"" + session_name + "\".";
	} else {
		// The process's network shutdown, as the shell's exit runs it: no socket open.
		inmatch::stop_unstopped_server(process.unstopped_server, logs, nullptr, nullptr);
		expected_stopped = "???.???.???.???:????? : HOST STOPPED \"" + session_name + "\".";
	}
	out.held_after = process.unstopped_server.running ||
			!process.unstopped_server.nodes.empty();
	// What a pump's interval flush already wrote, then what the log still holds.
	out.text = files.files[inmatch::kInoutLogFileName] + inout.pending();
	const std::size_t removed = out.text.find(
			"127.0.0.1:31000 : SERVER PLAYER REMOVED \"\". [1,9,0,0,\"\",0,\"NP.C:SH:STOP\"]");
	const std::size_t stopped = out.text.find(expected_stopped);
	CHECK(removed != std::string::npos && stopped != std::string::npos && removed < stopped);
	// The reset destroys the own client first, then the stop's walk takes its twin ahead of the
	// joiner; neither sends on the socket (the manager's loopback).
	const std::size_t own_client = out.text.find("0.0.0.0:0 : CLIENT PLAYER REMOVED");
	const std::size_t own_twin = out.text.find(
			"0.0.0.0:0 : SERVER PLAYER REMOVED", own_client == std::string::npos ? 0 : own_client);
	CHECK(own_client != std::string::npos && own_twin != std::string::npos &&
			own_client < own_twin && own_twin < removed);
	CHECK(out.text.find("[1,9,0,0,\"\",0,\"NP.C:SH:STOP\"]", own_twin) != std::string::npos);
	if (next_host) {
		const std::size_t started = out.text.find(" : HOST STARTED ");
		CHECK(started == std::string::npos || stopped < started);
	}
	return out;
}

void test_an_error_arm_leaves_the_server_to_the_next_hosts_reset() {
	const StoppedLater out = error_arm_then(/*next_host=*/true);
	CHECK(out.stops > 0 && !out.others);
	CHECK(!out.held_after);
}

void test_the_process_shutdown_stops_a_held_server_without_a_socket() {
	const StoppedLater out = error_arm_then(/*next_host=*/false);
	CHECK(out.stops == 0);
	CHECK(!out.held_after);
}

// D-NET-NEW-L1-26 (L1-s18 row 2): the post-mission router's reason text for 9..19 reads ctx+0xE60,
// the own client connection, which its leave left in state 6 with its record (nothing destroys it
// before the session reset). A listen host whose stale kick maps its own leave to 10 shows the
// leave's own record: DC 2, DPC 0, "I.C:CIDEMIS", so GDC000 with that text, never the kick's. A
// NovaWorld session's end (12) with no leave reads a record with no code: no text. A host with
// no own client has no connection there: ERR1.
// [orig: PostMenu_RouteMissionExit @0x568460 - 9..19 -> CNapiNetwork_GetDisconnectReasonString
//  called @0x5686D0; CNapiNetwork_GetDisconnectReasonString @0x4C7000 - +0xE60 read @0x4C7001,
//  null ERR1 @0x4C701D, DC @0x4C708B (none: no text @0x4C7093), DSTR @0x4C709C, DPC
//  @0x4C70D4; CNapiNPConnection_SetState @0x626250 - the teardown called @0x62628F, state 6
//  @0x626297; CNapiNPConnection_PumpStateMachine @0x6292E0 (no state-6 arm)]
void test_the_remapped_exit_reads_the_own_client_record() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	host->process->disconnect_latch = kick_record(37);
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx();
	inmatch::input_action_exit(binding, inmatch::ExitSite::Input);
	host->role->close();
	CHECK(host->ctx().mission_exit_reason == 10);
	const std::optional<inmatch::ConnectionErrorRecord> record =
			host->role->own_client_error_record();
	CHECK(record && record->disconnect_code == 2 && record->disconnect_param == 0 &&
			record->disconnect_text == "I.C:CIDEMIS");
	if (record) {
		const inmatch::DisconnectReasonKey key = inmatch::disconnect_reason_key(&*record);
		CHECK(key.section != nullptr && std::string(key.section) == "MPGameDisconnectCodes" &&
				key.key == "GDC000" && key.substitutes && key.substitution == "I.C:CIDEMIS");
	}

	auto ended = std::make_unique<ListenHost>();
	if (!ended->boot()) {
		++failures;
		return;
	}
	ended->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
	ended->role->close();
	const std::optional<inmatch::ConnectionErrorRecord> live =
			ended->role->own_client_error_record();
	CHECK(live && live->connect_error == 0 && live->disconnect_code == 0);
	if (live) CHECK(inmatch::disconnect_reason_key(&*live).section == nullptr);

	inmatch::HostRole dedicated; // no own client
	CHECK(!dedicated.own_client_error_record());
	CHECK(inmatch::disconnect_reason_key(nullptr).key == "ERR1");
}

// The last frame's entity pass queued its records at their calls in retail, ahead of the
// teardown, whose pump sends them; ours raises them for the next tick's head, so the close
// routes them first: a relay the pass raised reaches the in-match joiner at the close, ahead of
// the teardown's own records: a flag carrier's destroy queues its flag's 0x2F at the destroy
// (D-NET-NEW-L1-25), behind the relay on the joiner's queue.
// [orig: Game_ProcessMainFrame - Entity_UpdateAllEntities called @0x52674B ahead of the exit
//  tests @0x526806; Game_TeardownMission - the Entity_Destroy call @0x52236F, then
//  Server_PumpNetworkTransport called @0x522588; Server_BroadcastEntityActionPacket @0x5080D0 -
//  SendFiltered(0x3F) called @0x508199]
void test_close_routes_the_last_entity_pass_first() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	const std::string server_scrk = "SERVER-PASS-SCRK";
	inmatch::NapiNPConnection &joiner = host->ctx().np_protocol.connection_list.back();
	joiner.client_ck = 0x31415926u;
	joiner.server_sk = 0x27182818u;
	joiner.client_scrk = "CLIENT-PASS-SCRK";
	joiner.server_scrk = server_scrk;
	joiner.slot_state = inmatch::PlayerSlotState::Joined;
	joiner.burst.spawned = true;
	opennova::world::HudRelay relay;
	relay.kind = 1;
	relay.team = 1;
	relay.key = "STRLOSEMSG001";
	host->kernel->world.out.hud_relays.push_back(relay);
	// A person carrying a flag (a pool-1 item its mounted child names): the teardown's destroy
	// drops it and queues its 0x2F.
	opennova::world::World &world = host->kernel->world;
	opennova::world::PlayerSpawn carrier_spawn;
	carrier_spawn.position = {22.0f, 20.0f, 0.0f};
	carrier_spawn.net_id = 0xFFF1;
	carrier_spawn.team = 1;
	const opennova::world::EntityHandle carrier =
			opennova::world::spawn_remote_player(world, carrier_spawn);
	opennova::world::Entity flag_row;
	flag_row.kind = opennova::world::EntityKind::Item;
	flag_row.item_id = 4091;
	flag_row.has_item_def = true;
	flag_row.alive = true;
	const opennova::world::EntityHandle flag = world.registry.spawn(1, flag_row);
	CHECK(carrier.valid() && flag.valid());
	if (carrier.valid() && flag.valid()) {
		world.registry.get(carrier)->mounted_child = flag;
		world.registry.get(flag)->primary_occupant = carrier;
	}
	host->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld; // the node stays
	host->role->close();
	CHECK(host->kernel->world.out.hud_relays.empty());
	// Wherever the teardown's pump left it: on the wire, or in the queue the held node keeps
	// past the error arm (its pending messages and its transport's, D-NET-NEW-L1-26).
	int relays = 0;
	for (const inmatch::UnstoppedNode &node : host->process->unstopped_server.nodes)
		for (const ProtocolMessage &m : node.queued)
			relays += m.tag == s2c::OBJECTIVE_NOTIFICATION ? 1 : 0;
	for (const auto &[to, bytes] : host->socket.sent) {
		if (!(to == kPeer)) continue;
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(bytes.data(), bytes.size(), opcode, body) ||
				opcode != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
			continue;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), server_scrk, header, messages))
			continue;
		for (const ProtocolMessage &m : messages)
			relays += m.tag == s2c::OBJECTIVE_NOTIFICATION ? 1 : 0;
	}
	for (const ProtocolMessage &m : host->role->state.host_owner.pending_session_messages[kPeer])
		relays += m.tag == s2c::OBJECTIVE_NOTIFICATION ? 1 : 0;
	ns::Datagram staged;
	while (host->remote.pop_outbound(staged))
		relays += staged.tag == s2c::OBJECTIVE_NOTIFICATION ? 1 : 0;
	CHECK(relays >= 1);

	// The joiner's records in queue order: what the teardown's pump sent, then the packets the
	// held node keeps framed, then its unbuilt queue (pending messages ahead of its transport's).
	std::vector<uint8_t> order;
	const auto take_packet = [&order, &server_scrk](const std::vector<uint8_t> &bytes) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(bytes.data(), bytes.size(), opcode, body) ||
				opcode != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
			return;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), server_scrk, header,
				messages))
			return;
		for (const ProtocolMessage &m : messages) order.push_back(m.tag);
	};
	for (const auto &[to, bytes] : host->socket.sent)
		if (to == kPeer) take_packet(bytes);
	for (const inmatch::UnstoppedNode &node : host->process->unstopped_server.nodes) {
		if (!(node.conn.peer == kPeer)) continue;
		for (const std::vector<uint8_t> &bytes : node.staged) take_packet(bytes);
		for (const ProtocolMessage &m : node.queued) order.push_back(m.tag);
	}
	const auto first = [&order](uint8_t tag) {
		return std::find(order.begin(), order.end(), tag) - order.begin();
	};
	const auto end = static_cast<std::ptrdiff_t>(order.size());
	CHECK(first(s2c::OBJECTIVE_NOTIFICATION) < end && first(s2c::OBJECTIVE_ENTITY_STATE) < end);
	CHECK(first(s2c::OBJECTIVE_NOTIFICATION) < first(s2c::OBJECTIVE_ENTITY_STATE));
}

// The destroy arm stops the server itself: nothing is left for a later reset.
void test_the_destroy_arm_leaves_no_server() {
	inmatch::ClientProcessState process;
	auto host = std::make_unique<ListenHost>();
	host->shared_process = &process;
	if (!host->boot()) {
		++failures;
		return;
	}
	host->ctx().mission_exit_reason = inmatch::kMissionExitQuit;
	host->role->close();
	CHECK(!process.unstopped_server.running && process.unstopped_server.nodes.empty());
}

// D-NET-NEW-L9-21 (L1-s18's hand-over): at the next host's session reset each held node's destroy
// runs its removal callback after its burst, and on the authority a node whose player was added
// reaches its slot's teardown, whose first leg is the process's NovaWorld session's
// (ClientHostPlayerRemoved in state 6): the leg gets the slot once per such node, after that
// node's STOP burst, and nothing for a node whose player was never added. The process's network
// shutdown passes no leg (the router cleared the connection mode, so no authority remains).
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the burst @0x6253EF..0x625424,
//  the protocol's +0x2D8 callback called @0x625438; NapiNPServer_DestroyPlayerCtx @0x4C8400
//  (installed @0x4C99D0) - Server_ForwardEventToSession called @0x4C8426;
//  Server_HandlePlayerDisconnect @0x51B5C0 - is_authority @0x51B5D0, the active byte @0x51B5DC,
//  CNapiGameSession_SendPlayerRemovedIfHosting called @0x51B5F3; PostMenu_RouteMissionExit
//  @0x568460 - CGameSession_SetConnectionMode(0) @0x5686ED; NapiLib_Shutdown @0x4C9580 - the reset
//  @0x4C958B]
// D-NET-NEW-L9-22: the same teardown's later leg stops the slot's clock and sends PLAYER through
// the process's sink, after the removal, on a NovaWorld host alone (the menu's network type 1
// stands through its setup); a LAN host's reset and the process exit send none. GCC is the
// install's CC.BIN, which the new host's tick init read again (none here: "").
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - CServerTick_SetState(0) @0x51B690,
//  Server_SendPlayerMetricsToGate @0x51B696 (the network type @0x4FAA67);
//  CNapiNetwork_SetNetworkType @0x4C4A50 - @0x4C4A82 / @0x4C4A85]
void test_the_next_hosts_reset_removes_each_held_player_from_the_novaworld_session() {
	enum class Reset { LanHost, NovaWorldHost, Exit };
	for (const Reset reset : { Reset::LanHost, Reset::NovaWorldHost, Reset::Exit }) {
		const bool next_host = reset != Reset::Exit;
		inmatch::ClientProcessState process;
		auto first = std::make_unique<ListenHost>();
		first->shared_process = &process;
		if (!first->boot()) {
			++failures;
			return;
		}
		first->ctx().np_protocol.connection_list.back().client_ck = 0x5A5A1234u;
		first->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
		first->role->close();
		// The own pair's twin first (address 0), then the joiner.
		CHECK(process.unstopped_server.nodes.size() == 2);
		if (process.unstopped_server.nodes.size() != 2)
			return;
		const inmatch::NapiNPConnection twin = process.unstopped_server.nodes.front().conn;
		const bool twin_added =
				twin.link.owned_entity.valid() || twin.phase >= inmatch::ConnectionPhase::PlayerAdded;
		// The held joiner's player was added on slot 3; a second held node never got its player.
		inmatch::NapiNPConnection &added = process.unstopped_server.nodes.back().conn;
		added.phase = inmatch::ConnectionPhase::PlayerAdded;
		added.reply.player_slot = 3;
		inmatch::NapiNPConnection pending = added;
		pending.peer = PeerAddr{ 0x0100007Fu, 31001 };
		pending.phase = inmatch::ConnectionPhase::Joined;
		pending.link.owned_entity = {};
		pending.reply.player_slot = 4;
		process.unstopped_server.nodes.push_back(inmatch::UnstoppedNode{pending, {}, {}});

		const std::vector<uint8_t> stop = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
				server_goodbye_to_bytes(
						0x5A5A1234u, make_disconnect_event(1, 9, 0, 0, "", 0, "NP.C:SH:STOP")));
		std::vector<std::pair<int, int>> removed; // (the slot, the STOP datagrams sent by then)
		// The PLAYER reports through the process's sink, each with the removals made by then.
		std::vector<std::pair<std::string, std::size_t>> reports;
		process.nwu_gate_metrics.host = [] {
			GateMetricsHost host;
			host.label = "LBL";
			host.version = "V1.7.5.7";
			return host;
		};
		process.nwu_gate_metrics.game_text = [](const char *, const char *, const char *fallback) {
			return std::string(fallback);
		};
		process.nwu_gate_metrics.send = [&reports, &removed](const std::string &text) {
			reports.emplace_back(text, removed.size());
		};
		if (next_host) {
			first.reset();
			auto second = std::make_unique<ListenHost>();
			second->shared_process = &process;
			second->socket_at_boot = true;
			second->network_type = reset == Reset::NovaWorldHost ? inmatch::NetworkType::NovaWorld
			                                                     : inmatch::NetworkType::Lan;
			const CaptureSocket &socket = second->socket;
			process.nwu_player_removed = [&removed, &socket, &stop](int player_number) {
				int stops = 0;
				for (const auto &[to, bytes] : socket.sent)
					if (to == kPeer && bytes == stop)
						++stops;
				removed.emplace_back(player_number, stops);
			};
			if (!second->boot()) {
				++failures;
				return;
			}
			// The twin's leg (the host slot), before any STOP datagram (its burst is the
			// loopback's), then the joiner's after its burst.
			CHECK(removed.size() == (twin_added ? 2u : 1u));
			if (removed.size() == (twin_added ? 2u : 1u)) {
				if (twin_added) {
					CHECK(removed.front().first == twin.reply.player_slot);
					CHECK(removed.front().second == 0);
				}
				CHECK(removed.back().first == 3);
				CHECK(removed.back().second > 0);
			}
			if (reset == Reset::NovaWorldHost) {
				CHECK(reports.size() == 1);
				if (reports.size() == 1) {
					CHECK(reports[0].first.find("BLOCK \"PLAYER\"\r\n") != std::string::npos);
					CHECK(reports[0].first.find("\tGCC \"\"\r\n") != std::string::npos);
					// After the joiner's removal, the twin's ahead of it: the twin is the host's
					// own slot, whose teardown returns at its local test before the PLAYER leg
					// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - the NovaWorld leg
					//  @0x51B5F3, the local byte @0x51B606, the return @0x51B621].
					CHECK(reports[0].second == (twin_added ? 2u : 1u));
				}
			} else {
				CHECK(reports.empty());
			}
		} else {
			process.nwu_player_removed = [&removed](int player_number) {
				removed.emplace_back(player_number, 0);
			};
			first->role->stop_unstopped_server_at_exit(inmatch::ServerLogs{});
			CHECK(removed.empty());
			CHECK(reports.empty());
		}
		CHECK(!process.unstopped_server.running && process.unstopped_server.nodes.empty());
	}
}

// A held node as an error-arm exit leaves it: its keys, its 0x42 accepted, its last receive and
// last send at 1000 ms on the host clock.
inmatch::UnstoppedNode held_node(const PeerAddr &peer, uint32_t client_ck) {
	inmatch::UnstoppedNode node;
	node.conn.type = inmatch::NapiNPConnection::kTypeServerSide;
	node.conn.phase = inmatch::ConnectionPhase::Joined;
	node.conn.peer = peer;
	node.conn.client_ck = client_ck;
	// 62 characters, a drawn key's longest: the 0x82 handler copies SCRK into 64 bytes
	// [orig: NapiNP_HandleServerJoinResponse @0x629840 - SCRK @0x629BF0].
	node.conn.server_scrk = "SERVERHELDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	node.conn.client_scrk = "CLIENTHELDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	node.conn.server_sk = 0x0BADF00Du;
	node.conn.receive_stamp_ms = 1000;
	node.conn.last_session_send_ms = 1000;
	return node;
}

// D-NET-NEW-L1-26 (L1-s19, V-ROLL-G on L1-s18 (a)): the held server between the error-arm exit
// and the next reset. The LAN browse's and a join's protocol pump, NapiNPProtocol_Pump(-1),
// steps each held node through PumpFlags' legs over the pump's socket:
//   - the first pump with a zero holdoff builds the node's queue: its 0x25 leaves in one S2C
//     0x83 over the node's own keys, and nothing else goes;
//   - a joiner's 0x43 that socket reads is queued by its node's socket leg, and the pump's
//     queue pass stamps its node's receive (a header-only keepalive counts); a datagram from
//     no held node's address is not taken;
//   - past cs_dir0's timeout (120000 ms) since its last receive the node is reaped: the send
//     interval's keepalive first, then its removal line with {1, 3, elapsed, timeout, "", 0,
//     "NP.C:PT:SERTMOUT"} and its four 0x86 echoing that record, and it leaves the held list;
//   - so the next reset's StopServer bursts STOP to the node still held alone.
// [orig: NapiNPProtocol_Pump(-1) from CNapiNetwork_PumpTransportAndProtocol @0x4C6E3D;
//  CNapiNPConnection_PumpFlags @0x629780 (the type test @0x629788..0x62978F);
//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 (BuildOutgoingPackets called @0x6292B4);
//  CNapiNPConnection_PumpStateMachine @0x6292E0 (PumpSendIntervals called @0x62933F, the
//  elapsed test @0x62935C..0x629364, the SERTMOUT record @0x6293B7..0x6293E6);
//  NapiNPProtocol_Pump @0x62A650 (the pending walk @0x62A6FD..0x62A793);
//  CNapiNPConnection_TeardownActiveConnection @0x6253C0 (the line @0x6253D2, the burst
//  @0x6253EF..0x625424); NapiNPProtocol_HandleSessionPacket @0x626A00 (the +0x1F5 test
//  @0x626A76, FindConnection(1, sender) @0x626AC5, ParseMessages' stamp @0x625D54);
//  NapiNPProtocol_StopServer @0x62A820]
void test_the_browse_pump_steps_the_held_server() {
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;

	const PeerAddr kStale{0x0100007Fu, 31002};
	inmatch::UnstoppedServer server;
	server.running = true;
	server.session_name = "Held";
	server.nodes.push_back(held_node(kPeer, 0x11111111u));
	server.nodes.push_back(held_node(kStale, 0x22222222u));
	ProtocolMessage reset = make_protocol_message(s2c::GAME_RESET, {});
	reset.reliable = false;
	server.nodes[0].queued.push_back(reset);

	// The first pump: the queued 0x25 leaves, alone.
	CaptureSocket socket;
	inmatch::pump_unstopped_server(server, logs, socket, 1016);
	CHECK(socket.sent.size() == 1);
	if (socket.sent.size() == 1) {
		CHECK(socket.sent[0].first == kPeer);
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> records;
		CHECK(nw_decode_inbound(socket.sent[0].second.data(), socket.sent[0].second.size(),
				opcode, body) && opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE);
		CHECK(decode_protocol_packet_plaintext(body.data(), body.size(),
				server.nodes[0].conn.server_scrk, header, records));
		CHECK(records.size() == 1 && records[0].tag == s2c::GAME_RESET);
	}
	CHECK(server.nodes.size() == 2 && server.nodes[0].queued.empty());

	// The fresh node's joiner sends a keepalive at 60000 ms: the socket leg queues it, and the
	// pump's queue pass in that frame moves its node's receive.
	SessionSequencing joiner;
	std::vector<uint8_t> keepalive_body;
	CHECK(frame_session_packet(joiner,
			SessionCrypto{server.nodes[0].conn.client_scrk, {}, server.nodes[0].conn.server_sk},
			{}, keepalive_body));
	const std::vector<uint8_t> keepalive =
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(keepalive_body));
	CHECK(inmatch::receive_unstopped_server(server, logs, socket, kPeer, keepalive.data(),
			keepalive.size(), 60000).taken);
	CHECK(server.queue.size() == 1 && server.nodes[0].conn.receive_stamp_ms == 1000);
	CHECK(!inmatch::receive_unstopped_server(server, logs, socket, PeerAddr{0x0100007Fu, 40000},
			keepalive.data(), keepalive.size(), 60000).taken);
	inmatch::pump_unstopped_server(server, logs, socket, 60000);
	CHECK(server.queue.empty());
	CHECK(server.nodes[0].conn.receive_stamp_ms == 60000);
	CHECK(server.nodes[1].conn.receive_stamp_ms == 1000);

	// At 121001 ms the stale node (last receive at 1000) is reaped; the fresh one is kept.
	socket.sent.clear();
	inmatch::pump_unstopped_server(server, logs, socket, 121001);
	CHECK(server.nodes.size() == 1 && server.nodes[0].conn.peer == kPeer);
	const std::vector<uint8_t> reaped = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(0x22222222u, make_disconnect_event(1, 3, 120001, 120000, "", 0,
					"NP.C:PT:SERTMOUT")));
	int reaps = 0;
	int to_stale = 0;
	for (const auto &[to, bytes] : socket.sent) {
		if (!(to == kStale)) continue;
		++to_stale;
		if (bytes == reaped) ++reaps;
	}
	CHECK(reaps == 4);
	CHECK(to_stale == 5); // the interval's keepalive, then the burst
	CHECK(!socket.sent.empty() && socket.sent.back().second == reaped);
	const std::string reaped_text = files.files[inmatch::kInoutLogFileName] + inout.pending();
	CHECK(reaped_text.find("NP.C:PT:SERTMOUT") != std::string::npos);

	// The next reset's StopServer: STOP to the node still held alone.
	CaptureSocket reopened;
	inmatch::stop_unstopped_server(server, logs, &reopened, nullptr);
	const std::vector<uint8_t> stop = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(0x11111111u, make_disconnect_event(1, 9, 0, 0, "", 0,
					"NP.C:SH:STOP")));
	int stops = 0;
	for (const auto &[to, bytes] : reopened.sent) {
		CHECK(to == kPeer);
		if (bytes == stop) ++stops;
	}
	CHECK(stops == 4 && reopened.sent.size() == 4);
	CHECK(!server.running && server.nodes.empty());
}

// D-NET-NEW-L1-26 (V-ROLL-H on L1-s19 A2): the protocol pump destroys a pending node right
// behind that node's own legs, before the next node's run: a held node reaped ahead of a node
// that sends in the same pump bursts its four 0x86 (after its interval keepalive) ahead of that
// node's packet.
// [orig: NapiNPProtocol_Pump @0x62A650 - CNapiNPConnection_PumpFlags called @0x62A6F8, then that
//  node's pending test @0x62A6FD and CNapiNPConnection_Destroy called @0x62A793, then the next
//  node; CNapiNPConnection_TeardownActiveConnection @0x6253C0 (the burst @0x6253EF..0x625424)]
void test_a_reaped_node_goes_before_the_next_nodes_legs() {
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;

	const PeerAddr kStale{0x0100007Fu, 31002};
	inmatch::UnstoppedServer server;
	server.running = true;
	server.session_name = "Held";
	server.nodes.push_back(held_node(kStale, 0x22222222u)); // ahead: silent since 1000 ms
	server.nodes.push_back(held_node(kPeer, 0x11111111u));
	server.nodes[1].conn.receive_stamp_ms = 60000; // heard at 60000 ms: kept
	ProtocolMessage reset = make_protocol_message(s2c::GAME_RESET, {});
	reset.reliable = false;
	server.nodes[1].queued.push_back(reset);

	CaptureSocket socket;
	inmatch::pump_unstopped_server(server, logs, socket, 121001);
	CHECK(server.nodes.size() == 1 && server.nodes[0].conn.peer == kPeer);
	const std::vector<uint8_t> reaped = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(0x22222222u, make_disconnect_event(1, 3, 120001, 120000, "", 0,
					"NP.C:PT:SERTMOUT")));
	int reaps = 0;
	std::size_t last_reap = socket.sent.size();
	std::size_t first_kept = socket.sent.size();
	for (std::size_t i = 0; i < socket.sent.size(); ++i) {
		if (socket.sent[i].first == kStale && socket.sent[i].second == reaped) {
			++reaps;
			last_reap = i;
		}
		if (socket.sent[i].first == kPeer && first_kept == socket.sent.size()) first_kept = i;
	}
	CHECK(reaps == 4);
	CHECK(first_kept < socket.sent.size() && last_reap < first_kept);
}

} // namespace

// D-NET-NEW-L1-26 (L1-s22 row 0): an error-arm exit holds the protocol's hello words with its
// server: the identity, the start stamp and the 0x81's protocol fields as the live host wrote
// them, so the held hello handler answers with the dead session's.
// [orig: NapiNPProtocol_SendServerInfoPacket @0x6204B0 (the protocol's fields, UT from +0x53C
//  @0x62064F..0x620683); PostMenu_RouteMissionExit @0x568460 (the error arms skip the destroy)]
void test_an_error_arm_holds_the_sessions_hello_words() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	const ServerHello live = inmatch::host_server_hello(host->ctx(),
			make_title_client_hello(kJo, 0), PeerAddr{});
	const uint32_t start = host->ctx().np_protocol.host_start_tick;
	host->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
	host->role->close();
	const inmatch::UnstoppedServer &held = host->process->unstopped_server;
	CHECK(held.running && held.identity == &kJo && held.host_start_tick == start);
	CHECK(held.hello.sn == live.sn && held.hello.sn == "close");
	CHECK(held.hello.hk == live.hk && held.hello.hk == host->ctx().np_protocol.host_key);
	CHECK(held.hello.p1 == live.p1 && held.hello.p2 == live.p2 && held.hello.mp == live.mp);
	CHECK(held.hello.sf == live.sf && held.hello.sus1 == live.sus1 &&
			held.hello.sus2 == live.sus2);
	CHECK(held.hello.pn == live.pn && held.hello.pv1 == live.pv1 && held.hello.pv3 == live.pv3);
}

// D-NET-NEW-L1-26 (L1-s22 row 0): the held protocol keeps host_running, and its hello handler
// gates on that alone, never on is_authority, so a 0x41 from any source reaching the process's
// socket (a LAN browser's probe at the port the browse rebound) takes the dead session's 0x81:
// its protocol words as the exit left them, the browser's CI, address and external pair echoed,
// UT against the start stamp, NP and NC the held nodes. The manager's lines follow the handler:
// PROTOCOL for a PM-less probe; a PM-carrying hello is answered and tossed [4]; a hello of
// another identity takes nothing and is tossed [4]. A process holding no server takes no 0x41.
// [orig: NapiNPProtocol_HandleClientHello @0x6213B0 - host_running @0x621454, +0x1F5 @0x621463,
//  +0x1F6 @0x62146B, the PM bypass @0x6217C2, the identity gate @0x6217F1..0x6218B0, the send
//  @0x6218FC, `return protocol_version == 0` @0x62190B; NapiNPProtocol_SendServerInfoPacket
//  @0x6204B0 - NP +0x520 @0x6209D8, NC +0xED4 @0x620A4D, RIP / RPN @0x620A7D..0x620ABC;
//  NapiNPManager_HandlePacket @0x622F10 - cb_on_dispatch @0x622FCD, cb_on_error(4) @0x622FF9]
void test_a_held_server_answers_a_hello() {
	MemoryFiles files;
	inmatch::ConnectLog connect{files};
	connect.set_clock([] { return std::string(); });
	connect.start(inmatch::ConnectLogHeader{});
	inmatch::ServerLogs logs;
	logs.connect = &connect;

	const PeerAddr kBrowser{0x0900000Au, 32768}; // 10.0.0.9, a LAN browser's socket
	inmatch::UnstoppedServer server;
	server.running = true;
	server.session_name = "Held";
	server.protocol_name = kJo.pn;
	server.identity = &kJo;
	server.host_start_tick = 1000;
	server.hello.sn = "Held";
	server.hello.hk = 0x1234ABCDu;
	server.hello.mp = 16;
	server.hello.p1 = 4;
	server.hello.np = 7; // a stale count the answer replaces
	server.hello.nc = 7;
	server.hello.ut = 99;
	server.nodes.push_back(held_node(kPeer, 0x11111111u));
	server.nodes.push_back(held_node(PeerAddr{0x0100007Fu, 31002}, 0x22222222u));

	ClientHello probe = make_title_client_hello(kJo, 5);
	probe.eip = 0xFFFFFFFFu; // the browse's broadcast
	probe.epn = 32768;
	const std::vector<uint8_t> datagram =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(probe));
	CaptureSocket socket;
	CHECK(inmatch::receive_unstopped_server(server, logs, socket, kBrowser, datagram.data(),
			datagram.size(), 6000).taken);
	CHECK(socket.sent.size() == 1);
	if (socket.sent.size() == 1) {
		CHECK(socket.sent[0].first == kBrowser);
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ServerHello reply;
		CHECK(nw_decode_inbound(socket.sent[0].second.data(), socket.sent[0].second.size(),
				opcode, body) && opcode == SESSION_OPCODE_SERVER_HELLO);
		CHECK(parse_server_hello(body.data(), body.size(), reply));
		CHECK(reply.ci == 5 && reply.rip == kBrowser.ip && reply.rpn == 32768);
		CHECK(reply.eip == 0xFFFFFFFFu && reply.epn == 32768);
		CHECK(reply.sn == "Held" && reply.hk == 0x1234ABCDu && reply.mp == 16 && reply.p1 == 4);
		CHECK(reply.ut == 5000 && reply.np == 2 && reply.nc == 2);
	}
	const std::string taken_line = " FROM 10.0.0.9:32768 [" + std::string(kJo.pn) + "]";
	const std::string tossed_line = " FROM 10.0.0.9:32768 [4]";
	std::string text = files.files[inmatch::kConnectLogFile];
	const std::size_t raw = text.find("NAPI GOT RAW ");
	const std::size_t cooked = text.find("NAPI GOT COOKED ");
	const std::size_t taken = text.find(taken_line);
	CHECK(raw < cooked && cooked < taken && taken != std::string::npos);
	CHECK(text.find(tossed_line) == std::string::npos);

	// A PM-carrying hello (a peer host's announce) is answered and tossed.
	ClientHello announce = make_title_client_hello(kJo, 6);
	announce.pm = 1;
	const std::vector<uint8_t> announced =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(announce));
	CHECK(inmatch::receive_unstopped_server(server, logs, socket, kBrowser, announced.data(),
			announced.size(), 6000).taken);
	CHECK(socket.sent.size() == 2);
	text = files.files[inmatch::kConnectLogFile];
	const std::size_t first_tossed = text.find(tossed_line);
	CHECK(first_tossed != std::string::npos && first_tossed > taken);

	// Another identity's PM-less hello takes nothing and is tossed.
	ClientHello other = make_title_client_hello(kJo, 7);
	other.pn = "OTHERPROTOCOL";
	const std::vector<uint8_t> refused =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(other));
	CHECK(inmatch::receive_unstopped_server(server, logs, socket, kBrowser, refused.data(),
			refused.size(), 6000).taken);
	CHECK(socket.sent.size() == 2);
	text = files.files[inmatch::kConnectLogFile];
	CHECK(text.find(tossed_line, first_tossed + tossed_line.size()) != std::string::npos);

	// No server held: the 0x41 is the caller's.
	inmatch::UnstoppedServer none;
	none.identity = &kJo;
	CHECK(!inmatch::receive_unstopped_server(none, logs, socket, kBrowser, datagram.data(),
			datagram.size(), 6000).taken);
	CHECK(socket.sent.size() == 2);
}

namespace {

const PeerAddr kStock{0x0900000Au, 32768}; // 10.0.0.9, a stock client's socket
const char *const kStockScrk = "STOCKCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

// A stock client's 0x42 to the held server's identity and host key.
ClientAuth stock_join(uint32_t host_key, uint32_t ci, uint32_t ck) {
	ClientAuth auth = make_title_client_auth(kJo, ci, ck, host_key, "Stock", kStockScrk);
	auth.cu.push_back(make_client_cu_chunk(2, "MPS", "1000"));
	return auth;
}

std::vector<uint8_t> join_datagram(const ClientAuth &auth) {
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
}

// The datagrams `socket` sent to `to`, in order.
std::vector<std::vector<uint8_t>> sent_to(const CaptureSocket &socket, const PeerAddr &to) {
	std::vector<std::vector<uint8_t>> out;
	for (const auto &[peer, bytes] : socket.sent)
		if (peer == to) out.push_back(bytes);
	return out;
}

bool parse_auth_datagram(const std::vector<uint8_t> &datagram, ServerAuth &reply) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == SESSION_OPCODE_SERVER_AUTH && parse_server_auth(body.data(), body.size(), reply);
}

// A held server's 0x42 through the socket pass (taken, queued, nothing sent) and the browse's
// next pump (the queued pass); returns the server's 0x82 to the stock client.
bool held_join(inmatch::UnstoppedServer &server, const inmatch::ServerLogs &logs,
		CaptureSocket &socket, const ClientAuth &auth, uint32_t now_ms, ServerAuth &reply) {
	const std::vector<uint8_t> datagram = join_datagram(auth);
	const std::size_t sent_before = socket.sent.size();
	CHECK(inmatch::receive_unstopped_server(server, logs, socket, kStock, datagram.data(),
			datagram.size(), now_ms).taken);
	CHECK(socket.sent.size() == sent_before && server.queue.size() == 1);
	inmatch::pump_unstopped_server(server, logs, socket, now_ms);
	CHECK(server.queue.empty());
	// This pump's sends alone: an earlier join's 0x82 on the same socket is not this one's.
	for (std::size_t i = sent_before; i < socket.sent.size(); ++i)
		if (socket.sent[i].first == kStock && parse_auth_datagram(socket.sent[i].second, reply))
			return true;
	return false;
}

} // namespace

// D-NET-NEW-L1-26 (L1-s23 row 0): the held protocol's join handler, end to end from an error-arm
// exit. Its head gate is host_running alone, never is_authority, so a stock client's 0x42 the
// process's socket reads (a joiner of the ghost row the held 0x81 put in its browse) is the held
// protocol's: the socket pass queues it (RAW, COOKED, PROTOCOL; nothing sent), and the browse's
// next Pump(-1) runs the queued pass: the protocol's gates and the validate callback over the
// words the close held (no lock, no ban, the held nodes under the session's cap of 4), the create
// on the process's connection counter (the 0x82's MI), the keys, the new-connection callback (the
// connect log's disabling line after the PROTOCOL line, the joiner's MPS of 1000 under the
// template's 1300), the 0x82 with CR 1, the /INOUT added line, and the two CS updates, built
// in the same pump as the node's first sequenced packet. The new node counts in the next 0x81's
// NP and NC.
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - host_running @0x62B7D7, the queue
//  @0x62C323, the create @0x62BF06, the validate callback called @0x62C0AA, OnStateChange
//  @0x62C2D7;
//  NapiNPProtocol_PumpRecvQueues @0x6266A0 - the handler with 1 @0x6267AD;
//  CNapiNPConnection_Create @0x62ACB0 - the counter @0x62B282..0x62B296;
//  CNapiNPConnection_OnStateChange @0x626060 - connection_id @0x62619C, SendSessionInit
//  @0x6261FC, LogPlayerAdded @0x626204; NapiNPServer_HandleNewConnection @0x4C8040 - the
//  disabling line @0x4C80A7, the MPS @0x4C818D..0x4C81E4, ++np_count @0x4C820D;
//  CNapiNPConnection_SendSessionInit @0x620EF0 - MI @0x620FE8]
void test_a_held_server_admits_a_stock_join() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	host->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
	host->role->close();
	inmatch::UnstoppedServer &held = host->process->unstopped_server;
	CHECK(held.running && held.join_capacity == 4 && !held.lineup_queue_open);
	CHECK(!held.join_locked && held.banned_addresses.empty() && held.server_password.empty());
	CHECK(!held.reject_new_connections && held.node_template.max_packet_bytes == 1300);
	const std::size_t nodes_before = held.nodes.size();

	MemoryFiles files;
	inmatch::ConnectLog connect{files};
	connect.set_clock([] { return std::string(); });
	connect.start(inmatch::ConnectLogHeader{});
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.connect = &connect;
	logs.inout = &inout;
	logs.inout_switch = true;

	CaptureSocket socket;
	const uint32_t index_before = next_connection_index();
	ServerAuth reply;
	CHECK(held_join(held, logs, socket, stock_join(held.hello.hk, 9, 0x51515151u), 6016, reply));
	CHECK(held.nodes.size() == nodes_before + 1);
	const std::vector<std::vector<uint8_t>> to_stock = sent_to(socket, kStock);
	CHECK(to_stock.size() == 2);
	if (held.nodes.size() == nodes_before + 1 && to_stock.size() == 2) {
		const inmatch::NapiNPConnection &node = held.nodes.back().conn;
		CHECK(node.peer == kStock && node.phase == inmatch::ConnectionPhase::Joined);
		CHECK(node.type == inmatch::NapiNPConnection::kTypeServerSide);
		CHECK(node.client_ci == 9 && node.client_ck == 0x51515151u && node.player_name == "Stock");
		CHECK(node.receive_stamp_ms == 6016 && node.timeouts.max_packet_bytes == 1000);
		CHECK(reply.cr == 1 && reply.ci == 9 && reply.ck == 0x51515151u);
		CHECK(reply.mi == index_before + 1 && node.connection_id == reply.mi);
		CHECK(reply.sk == node.server_sk && reply.scrk == node.server_scrk && !reply.scrk.empty());
		CHECK(reply.rip == kStock.ip && reply.rpn == kStock.port && reply.na == "Stock");
		bool ceiling = false;
		for (const CsField &field : reply.client_cs)
			if (field.field_index == 13 && field.value == 1000) ceiling = true;
		CHECK(ceiling);
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> records;
		CHECK(nw_decode_inbound(to_stock[1].data(), to_stock[1].size(), opcode, body) &&
				opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE);
		CHECK(decode_protocol_packet_plaintext(body.data(), body.size(), node.server_scrk, header,
				records));
		CHECK(records.size() == 2 && records[0].flags.settings_update &&
				records[1].flags.settings_update);
	}
	const std::string connect_text = files.files[inmatch::kConnectLogFile];
	const std::size_t taken = connect_text.find(" FROM 10.0.0.9:32768 [" + std::string(kJo.pn) + "]");
	const std::size_t disabled = connect_text.find("NET SERVER DISABLING CONNECTLOG");
	CHECK(taken != std::string::npos && disabled != std::string::npos && taken < disabled);
	CHECK(!connect.enabled());
	const std::string inout_text = files.files[inmatch::kInoutLogFileName] + inout.pending();
	CHECK(inout_text.find("PLAYER ADDED \"Stock\"") != std::string::npos);

	// The next 0x81 counts the new node.
	const std::vector<uint8_t> probe =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(
					make_title_client_hello(kJo, 5)));
	CaptureSocket browser;
	CHECK(inmatch::receive_unstopped_server(held, logs, browser, kStock, probe.data(), probe.size(),
			6032).taken);
	ServerHello hello;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	CHECK(browser.sent.size() == 1 &&
			nw_decode_inbound(browser.sent[0].second.data(), browser.sent[0].second.size(), opcode,
					body) &&
			parse_server_hello(body.data(), body.size(), hello));
	CHECK(hello.np == nodes_before + 1 && hello.nc == nodes_before + 1);
}

// D-NET-NEW-L1-26 (L1-s23 row 0): the queued pass's refusals, each a CR=0 0x82 with the family
// (JFC) and the reason (JFP) and no node: the server password (4, ahead of the create: the
// connection counter does not move), then past the create (the counter moves) the validate
// callback over the dead session's words: the held nodes against the cap (14 / 4, or 14 / 5
// with the lineup queue open), the join lock (14 / 2) and the address bans (14 / 3). A 0x42 of
// another identity is tossed [4] at the socket pass and never queued.
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - PW @0x62BE18, the identity's return 0
//  @0x62B7E6, the create @0x62BF06, the validate callback @0x62C0B1, the refusal @0x62C0E7;
//  CNapiNetwork_ValidateJoinRequest @0x4C61B0 - g_ServerJoinLocked @0x4C61CA, g_BannedIdList
//  @0x4C6217, the count @0x4C623F, 5 @0x4C624E; NapiNPProtocol_SendJoinRejection @0x620CD0]
void test_a_held_server_refuses_a_join() {
	MemoryFiles files;
	inmatch::ConnectLog connect{files};
	connect.set_clock([] { return std::string(); });
	connect.start(inmatch::ConnectLogHeader{});
	inmatch::ServerLogs logs;
	logs.connect = &connect;

	inmatch::UnstoppedServer server;
	server.running = true;
	server.session_name = "Held";
	server.protocol_name = kJo.pn;
	server.identity = &kJo;
	server.hello.hk = 0x1234ABCDu;
	server.server_password = "Secret";
	server.join_capacity = 2;
	server.nodes.push_back(held_node(kPeer, 0x11111111u));
	server.nodes.push_back(held_node(PeerAddr{0x0100007Fu, 31002}, 0x22222222u));
	CaptureSocket socket;
	ServerAuth reply;

	// The password: refused ahead of the create.
	uint32_t index = next_connection_index();
	ClientAuth auth = stock_join(server.hello.hk, 9, 0x51515151u);
	auth.pw = "wrong";
	CHECK(held_join(server, logs, socket, auth, 6000, reply));
	CHECK(reply.cr == 0 && reply.jfc == 4 && reply.jfp == 0 && server.nodes.size() == 2);
	CHECK(next_connection_index() == index + 1);

	// The cap: the two held nodes fill 2; refused past the create.
	auth.pw = "SECRET"; // the compare ignores case
	index = next_connection_index();
	reply = ServerAuth{};
	CHECK(held_join(server, logs, socket, auth, 6000, reply));
	CHECK(reply.cr == 0 && reply.jfc == 14 && reply.jfp == 4 && server.nodes.size() == 2);
	CHECK(next_connection_index() == index + 2);
	server.lineup_queue_open = true;
	reply = ServerAuth{};
	CHECK(held_join(server, logs, socket, auth, 6000, reply));
	CHECK(reply.cr == 0 && reply.jfc == 14 && reply.jfp == 5);

	// Room again: the join lock, then the address bans.
	server.join_capacity = 4;
	server.join_locked = true;
	reply = ServerAuth{};
	CHECK(held_join(server, logs, socket, auth, 6000, reply));
	CHECK(reply.cr == 0 && reply.jfc == 14 && reply.jfp == 2 && server.nodes.size() == 2);
	server.join_locked = false;
	server.banned_addresses.push_back(kStock.ip);
	reply = ServerAuth{};
	CHECK(held_join(server, logs, socket, auth, 6000, reply));
	CHECK(reply.cr == 0 && reply.jfc == 14 && reply.jfp == 3 && server.nodes.size() == 2);

	// Another identity: tossed at the socket pass, nothing queued or sent.
	ClientAuth other = auth;
	other.pn = "OTHERPROTOCOL";
	const std::vector<uint8_t> refused = join_datagram(other);
	const std::size_t sent_before = socket.sent.size();
	CHECK(inmatch::receive_unstopped_server(server, logs, socket, kStock, refused.data(),
			refused.size(), 6000).taken);
	CHECK(server.queue.empty() && socket.sent.size() == sent_before);
	const std::string text = files.files[inmatch::kConnectLogFile];
	CHECK(text.find(" FROM 10.0.0.9:32768 [4]") != std::string::npos);
}

// D-NET-NEW-L1-26 (L1-s23 row 0): a held node at the 0x42's address. With the same CI and CK
// (a retransmit) it is sent its 0x82 again, its keys unchanged, and nothing is created; with
// another CI it is destroyed (its removal line, its four 0x86 under its own CK, no record
// latched) and the 0x42 creates its node at the tail of the held list.
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - FindConnection(1, sender) @0x62BEBA, the
//  retransmit @0x62BEE6, SendSessionInit @0x62BEE9 and its return @0x62BEF1,
//  CNapiNPConnection_Destroy @0x62BEFB, the create @0x62BF06;
//  CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the line @0x6253D2, the burst
//  @0x6253EF..0x625424]
void test_a_held_node_takes_a_retransmitted_join() {
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;

	inmatch::UnstoppedServer server;
	server.running = true;
	server.session_name = "Held";
	server.protocol_name = kJo.pn;
	server.identity = &kJo;
	server.hello.hk = 0x1234ABCDu;
	server.join_capacity = 4;
	server.nodes.push_back(held_node(kStock, 0x51515151u));
	server.nodes[0].conn.client_ci = 9;
	server.nodes.push_back(held_node(kPeer, 0x11111111u));

	// The retransmit: its 0x82 again, nothing created.
	CaptureSocket socket;
	uint32_t index = next_connection_index();
	ServerAuth reply;
	CHECK(held_join(server, logs, socket, stock_join(server.hello.hk, 9, 0x51515151u), 6000, reply));
	CHECK(reply.cr == 1 && reply.sk == 0x0BADF00Du && reply.scrk == server.nodes[0].conn.server_scrk);
	CHECK(sent_to(socket, kStock).size() == 1 && server.nodes.size() == 2);
	CHECK(server.nodes[0].conn.peer == kStock);
	CHECK(next_connection_index() == index + 1);

	// Another CI at the address: the held node goes, the new one is created at the tail.
	socket.sent.clear();
	reply = ServerAuth{};
	CHECK(held_join(server, logs, socket, stock_join(server.hello.hk, 10, 0x52525252u), 6000, reply));
	const std::vector<uint8_t> goodbye = nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
			server_goodbye_to_bytes(0x51515151u, DisconnectEvent{}));
	const std::vector<std::vector<uint8_t>> to_stock = sent_to(socket, kStock);
	CHECK(to_stock.size() >= 5);
	if (to_stock.size() >= 5) {
		for (std::size_t i = 0; i < 4; ++i) CHECK(to_stock[i] == goodbye);
		ServerAuth fresh;
		CHECK(parse_auth_datagram(to_stock[4], fresh) && fresh.cr == 1 && fresh.ci == 10 &&
				fresh.ck == 0x52525252u);
	}
	CHECK(server.nodes.size() == 2 && server.nodes[0].conn.peer == kPeer);
	CHECK(server.nodes.size() == 2 && server.nodes[1].conn.peer == kStock &&
			server.nodes[1].conn.client_ci == 10);
	const std::string inout_text = files.files[inmatch::kInoutLogFileName] + inout.pending();
	const std::size_t removed = inout_text.find("PLAYER REMOVED");
	const std::size_t added = inout_text.find("PLAYER ADDED \"Stock\"");
	CHECK(removed != std::string::npos && added != std::string::npos && removed < added);
}

// D-NET-NEW-L1-26 (L1-s23 row 1): the browse's (or a join's) connection create destroys the
// session connection it finds, which after an error-arm exit is the held listen host's own
// client. It latches {2, 2, 0, 0, "N.C:CSSAC", 0, ""} and writes its CLIENT removal line; its
// callback hands the process latch that record (first wins: a stale one stays), which maps no
// exit; its 0x46 reaches the twin through the loopback, so the twin goes pending with the
// client's record and the browse's pump destroys it (its SERVER line, nothing on the wire). The
// held server's 0x81 then counts the remote node alone, and the next reset has no own pair.
// [orig: CNapiGameSession_CreateServerConnection @0x4C9D30 - the +0xE60 test @0x4C9D6B,
//  CNapiNetwork_DisconnectActiveConnection called @0x4C9D77, the destroy @0x4C9D7D;
//  UI_StartLANSessionSearch @0x558300 - the create @0x55836E / @0x55837E;
//  CNapiNetwork_DisconnectActiveConnection @0x4C9140 - dc 2 @0x4C91EF, the text @0x4C91FF, the
//  latch @0x4C922D..0x4C923E; CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the line
//  @0x6253D2, the client callback @0x6254E7; CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 -
//  the latch @0x4C6535..0x4C6559, the reason test @0x4C6571; Nwu_HandleDisconnect @0x623CE0 -
//  the latch @0x6240A8..0x6240BA, RequestDisconnect called @0x6240C4; NapiNPProtocol_Pump
//  @0x62A650 - the pending walk @0x62A6FD..0x62A793]
void test_a_browse_start_releases_the_held_own_pair() {
	auto host = std::make_unique<ListenHost>();
	if (!host->boot()) {
		++failures;
		return;
	}
	host->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
	host->role->close();
	inmatch::UnstoppedServer &held = host->process->unstopped_server;
	CHECK(held.own_client.has_value() && held.nodes.size() == 2 && held.nodes[0].conn.peer.ip == 0);
	CHECK(!host->process->disconnect_latch.valid);

	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	inout.set_stamp_text([] { return std::string(); });
	inout.set_enabled(true);
	inmatch::ServerLogs logs;
	logs.inout = &inout;
	logs.inout_switch = true;

	inmatch::release_unstopped_own_client(held, logs, &host->process->disconnect_latch);
	CHECK(!held.own_client.has_value());
	const inmatch::ProcessDisconnectLatch &latch = host->process->disconnect_latch;
	CHECK(latch.valid && latch.event.ds == 2 && latch.event.dc == 2 && latch.event.dpc == 0 &&
			latch.event.dstr == "N.C:CSSAC");
	CHECK(latch.disconnect_exit().reason == inmatch::kMissionExitNone &&
			latch.disconnect_exit().input_action == 0);
	CHECK(held.nodes.size() == 2 && held.nodes[0].conn.pending_disconnect &&
			held.nodes[0].conn.disconnect_event_valid &&
			held.nodes[0].conn.disconnect_event.dstr == "N.C:CSSAC");

	// The browse's pump destroys the twin: its SERVER line after the client's, no datagram.
	CaptureSocket socket;
	inmatch::pump_unstopped_server(held, logs, socket, 6000);
	CHECK(held.nodes.size() == 1 && held.nodes[0].conn.peer == kPeer);
	for (const auto &[to, bytes] : socket.sent) CHECK(to.ip != 0);
	const std::string text = files.files[inmatch::kInoutLogFileName] + inout.pending();
	const std::size_t client_line = text.find("CLIENT PLAYER REMOVED");
	const std::size_t server_line = text.find("SERVER PLAYER REMOVED");
	CHECK(client_line != std::string::npos && server_line != std::string::npos &&
			client_line < server_line);
	CHECK(text.find("\"N.C:CSSAC\"", server_line) != std::string::npos);

	// The held 0x81 counts the remote node alone.
	const std::vector<uint8_t> probe = nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO,
			client_hello_to_bytes(make_title_client_hello(kJo, 5)));
	CaptureSocket browser;
	CHECK(inmatch::receive_unstopped_server(held, logs, browser, kStock, probe.data(),
			probe.size(), 6016).taken);
	ServerHello hello;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	CHECK(browser.sent.size() == 1 &&
			nw_decode_inbound(browser.sent[0].second.data(), browser.sent[0].second.size(), opcode,
					body) &&
			parse_server_hello(body.data(), body.size(), hello));
	CHECK(hello.np == 1 && hello.nc == 1);

	// A second create finds nothing to release; the reset then writes no CLIENT line.
	inmatch::release_unstopped_own_client(held, logs, &host->process->disconnect_latch);
	inmatch::stop_unstopped_server(held, logs, nullptr, nullptr);
	const std::string after = files.files[inmatch::kInoutLogFileName] + inout.pending();
	CHECK(after.find("CLIENT PLAYER REMOVED", client_line + 1) == std::string::npos);
	CHECK(!held.running && held.nodes.empty());

	// A latch already holding a record (a kicked joiner's) keeps it.
	auto stale = std::make_unique<ListenHost>();
	if (!stale->boot()) {
		++failures;
		return;
	}
	stale->ctx().mission_exit_reason = inmatch::kMissionExitNovaWorld;
	stale->role->close();
	stale->process->disconnect_latch.latch(make_disconnect_event(2, 2, 0, 0, "", 35, ""));
	inmatch::release_unstopped_own_client(stale->process->unstopped_server, inmatch::ServerLogs{},
			&stale->process->disconnect_latch);
	CHECK(stale->process->disconnect_latch.event.dpc == 35 &&
			stale->process->disconnect_latch.event.dstr.empty());
	CHECK(!stale->process->unstopped_server.own_client.has_value());
}

int main() {
	test_an_empty_latch_takes_the_leave_and_stops_the_server();
	test_a_stale_kick_maps_the_leave_and_skips_the_stop();
	test_a_stale_unlisted_code_keeps_the_stop();
	test_the_router_arm_decides_the_stop();
	test_close_runs_the_pump_before_the_stop();
	test_close_sweeps_post_mission_then_queues_each_slots_reset(inmatch::kMissionExitQuit);
	test_close_sweeps_post_mission_then_queues_each_slots_reset(inmatch::kMissionExitNovaWorld);
	test_close_resets_an_orphaned_row_through_its_net_player();
	test_an_error_arm_leaves_the_server_to_the_next_hosts_reset();
	test_the_process_shutdown_stops_a_held_server_without_a_socket();
	test_the_destroy_arm_leaves_no_server();
	test_the_remapped_exit_reads_the_own_client_record();
	test_close_routes_the_last_entity_pass_first();
	test_the_next_hosts_reset_removes_each_held_player_from_the_novaworld_session();
	test_the_browse_pump_steps_the_held_server();
	test_a_reaped_node_goes_before_the_next_nodes_legs();
	test_an_error_arm_holds_the_sessions_hello_words();
	test_a_held_server_answers_a_hello();
	test_a_held_server_admits_a_stock_join();
	test_a_held_server_refuses_a_join();
	test_a_held_node_takes_a_retransmitted_join();
	test_a_browse_start_releases_the_held_own_pair();
	std::printf("host_close_order: %d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
