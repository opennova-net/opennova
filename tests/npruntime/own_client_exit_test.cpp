// D-NET-NEW-L8-21: input action 3 on a listen host disconnects its own client as a user leave.
// The own client latches {2, 2, 0, 0, "I.C:CIDEMIS", 0, ""} (the first latch wins), its teardown
// writes the /INOUT CLIENT line and sends its goodbye burst to the pair's address 0, the manager's
// loopback. The admin's GOTO MENUSTATE runs after the frame's exit test, so the next tick's
// receive pump takes the goodbye: the server-side twin is set pending and destroyed in that
// pump's connection walk, its /INOUT SERVER line, then the host player's removal (the leave line,
// the body's S2C 0x12 and the 0x46 removal to the others, no 0x5D; the body gone). An exit from
// the frame's input pass waits for the mission teardown's pump (path B): after the pools' destroy,
// that pump's pending walk takes the held goodbye and removes the host's player over the
// destroyed bodies (the leave line, the body's 0x12 by its handle, the 0x46), and the /PROFILE
// marker it writes reaches nothing, the log closed ahead of the pump. An exit from the frame's input pass waits
// for the mission teardown's pump (held here). A dedicated host has no own client: nothing.
// Single player is the same mode-3 session with no wire (ours: the in-process listen server, the
// socketless host role of the SinglePlayer kind): the admin's exit runs the host player's
// disconnect at the next receive pump (its /INOUT SERVER line, the /PROFILE disconnect marker,
// the body's removal); an exit from the input pass leaves the latched record and the client's
// line alone, since single player never runs the teardown pump (out of a session).
// [orig: Input_HandleActionBinding case 3 (the call @0x49AF42);
//  CNapiNetwork_DisconnectActiveConnection @0x4C9140 (the record @0x4C91C3..0x4C921A, the latch
//  @0x4C922D..0x4C923E, RequestDisconnect @0x4C924A); CNapiNPConnection_TeardownActiveConnection
//  @0x6253C0 (LogPlayerRemoved @0x6253D2, the state-5 burst @0x6254BF..0x6254D3, the state-1
//  callback @0x625438); CNapiNPConnection_LogPlayerRemoved @0x61E940; Nwu_HandleDisconnect
//  @0x623CE0 (role 2 @0x624005, the latch @0x6240A8..0x6240BA, RequestDisconnect @0x6240C4);
//  NapiNPProtocol_Pump @0x62A650 (the pending walk @0x62A6FD..0x62A793);
//  Server_HandlePlayerDisconnect @0x51B5C0 (the /PROFILE marker's call @0x51B822);
//  CServerLog_WriteDisconnectMarker @0x4E1C50 (the file test @0x4E1C53..0x4E1C57);
//  Game_TeardownMission @0x522350 (Entity_Destroy over pools 0..2 @0x52236F..0x5223C0, the
//  log's close @0x5223F0, the pump @0x522588);
//  Game_ProcessMainFrame @0x5263F0 (the head receive @0x526528, the admin pump @0x5268F1);
//  Server_PumpNetworkTransport @0x4FD960 (`cmp is_in_session` @0x4FD969, jz @0x4FD970), the
//  teardown's call @0x522588; SinglePlayer_StartMission @0x561AF0 (network type 0 @0x561BD4,
//  CNapiGameSession_CreateSession @0x561E65); CNapiGameSession_CreateSession's success flag
//  ctx+0x68 @0x4C9D16; CNapiNetwork_PumpServerProtocolRecv @0x4C4EE0]
#include "npruntime/conn_fixture.h"

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_decode_session.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/serverlog_decode.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/remote_admin.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/server_files.h>
#include <runtime/inmatch/server_log_recorder.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace opennova;
namespace w = opennova::world;
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

constexpr const char *kStamp = "T: ";
const std::string kCidemis = " [2,2,0,0,\"I.C:CIDEMIS\",0,\"\"]";
// The pair's two /INOUT lines: the client's at the action, the twin's at the pump.
const std::string kClientRemoved = "0.0.0.0:0 : CLIENT PLAYER REMOVED \"Host\"." + kCidemis;
const std::string kServerRemoved = "0.0.0.0:0 : SERVER PLAYER REMOVED \"Host\"." + kCidemis;

// A listen host (its own pair, the host's body in slot 0) and one remote in match (slot 1), or a
// dedicated host with the remote alone. Heap-held: the World.
struct ListenHost {
	inmatch::NapiNPServerCtx ctx;
	w::World world;
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	ns::LoopbackChannel own_loop;
	ns::UdpSessionTransport remote{ns::UdpSessionTransport::Role::Host};
	w::EntityHandle host_body;
	w::EntityHandle remote_body;

	explicit ListenHost(bool own_client, bool with_remote = true) {
		inmatch::set_connection_mode(ctx, own_client ? inmatch::ConnectionMode::HostClient
		                                             : inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.max_players = 4;
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 32);
		ctx.world = &world;
		inout.set_stamp_text([] { return std::string(kStamp); });
		inout.set_enabled(true);
		ctx.logs.inout = &inout;
		w::PlayerSpawn spawn;
		spawn.team = 1;
		if (own_client) {
			host_body = w::spawn_remote_player(world, spawn);
			inmatch::NapiNPConnection own = conn_fixture::make_conn(inmatch::kHostPlayerDcb,
					inmatch::NapiNPConnection::kTypeClientSide, &own_loop,
					ns::TransportMode::Loopback, host_body, true);
			own.player_name = "Host";
			own.reply.player_name = "Host";
			own.reply.player_slot = 0;
			own.assigned_team = 1;
			own.assigned_team_valid = true;
			ctx.np_protocol.connection_list.push_back(std::move(own));
		}
		if (!with_remote) return;
		spawn.position = {10.0f, 0.0f, 0.0f};
		remote_body = w::spawn_remote_player(world, spawn);
		inmatch::NapiNPConnection joiner = conn_fixture::make_conn(inmatch::kFirstJoinerDcb,
				inmatch::NapiNPConnection::kTypeServerSide, &remote, ns::TransportMode::Client,
				remote_body, true);
		joiner.player_name = "Joe";
		joiner.reply.player_name = "Joe";
		joiner.reply.player_slot = 1;
		ctx.np_protocol.connection_list.push_back(std::move(joiner));
	}

	const inmatch::NapiNPConnection *own() const {
		for (const inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeClientSide) return &c;
		return nullptr;
	}
	std::vector<ns::Datagram> to_remote() {
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (remote.pop_outbound(d)) out.push_back(d);
		return out;
	}
	// The /INOUT entries since the last call, without their stamps.
	std::vector<std::string> inout_lines() {
		std::vector<std::string> out;
		const std::string &text = inout.pending();
		size_t at = consumed;
		while (at < text.size()) {
			const size_t end = text.find("\r\n", at);
			if (end == std::string::npos) break;
			std::string line = text.substr(at, end - at);
			if (line.rfind(kStamp, 0) == 0) line = line.substr(std::string(kStamp).size());
			out.push_back(line);
			at = end + 2;
		}
		consumed = at;
		return out;
	}
	size_t consumed = 0;
};

// The admin's GOTO MENUSTATE: the own client's CLIENT line at the action, nothing on the wire
// until the next receive pump, whose walk tears down the twin and removes the host's player.
void test_admin_exit_removes_the_host_player_at_the_next_receive_pump() {
	auto host = std::make_unique<ListenHost>(true);
	(void)host->inout_lines(); // the log's start line
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Admin);
	CHECK(host->ctx.mission_exit_reason == inmatch::kMissionExitQuit);
	const inmatch::NapiNPConnection *own = host->own();
	CHECK(own != nullptr && own->disconnect_event_valid && own->disconnect_event.ds == 2 &&
			own->disconnect_event.dc == 2 && own->disconnect_event.dstr == "I.C:CIDEMIS" &&
			own->own_goodbye == inmatch::OwnGoodbye::NextReceivePump);
	std::vector<std::string> lines = host->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kClientRemoved);
	CHECK(host->to_remote().empty()); // the goodbye rides the loopback
	// A second exit finds the client torn down: nothing more.
	inmatch::input_action_exit(binding, inmatch::ExitSite::Admin);
	CHECK(host->inout_lines().empty());

	// The next tick's receive pump.
	(void)inmatch::tick_connections(host->ctx, 0, 2, /*respect_s2c_send_boundary=*/false,
			/*run_spawn_pump=*/false);
	CHECK(host->own() == nullptr);
	CHECK(host->world.registry.get(host->host_body) == nullptr);
	CHECK(host->world.registry.get(host->remote_body) != nullptr);
	lines = host->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kServerRemoved);
	// The remote hears the host player leave: the leave line, the body's 0x12, then the slot's
	// removal record, and no sweep.
	const std::vector<ns::Datagram> sent = host->to_remote();
	size_t leave = sent.size(), body = sent.size(), removal = sent.size();
	for (size_t i = 0; i < sent.size(); ++i) {
		if (sent[i].tag == s2c::FORMATTED_GAME_TEXT && leave == sent.size()) {
			FormattedGameText text;
			if (decode_formatted_game_text(sent[i].body.data(), sent[i].body.size(), text) &&
					text.subtype == kGameTextPlayerLeaving && text.text == "Host")
				leave = i;
		}
		EntityRemove gone;
		size_t consumed = 0;
		if (sent[i].tag == s2c::ENTITY_REMOVE &&
				decode_entity_remove(sent[i].body.data(), sent[i].body.size(), gone, consumed) &&
				gone.entity_handle == host->host_body.packed)
			body = i;
		if (sent[i].tag == 0x46 && sent[i].body == encode_player_sync_removal(0, false))
			removal = i;
		CHECK(sent[i].tag != s2c::EMPTY_SLOT_SWEEP);
	}
	CHECK(leave < body && body < removal && removal < sent.size());
	// The remote stays.
	CHECK(host->ctx.np_protocol.connection_list.size() == 1);
}

// D-NET-NEW-L1-26 (L1-s16 FOUND 1): the admin path's teardown runs the own client's
// disconnected callback too: a stale kick record in the process latch (DPC 35) maps the
// admin's exit 1 to 7 at the next receive pump, as the input pass's path maps it.
// [orig: Input_HandleActionBinding (the admin's call @0x404A5D) case 3 -> the own client's
//  teardown, CNapiNPConnection_TeardownActiveConnection's client callback called @0x6254E7;
//  CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 - the latch @0x4C6535..0x4C6559, 35 -> 7
//  @0x4C65A2]
void test_admin_exit_runs_the_own_client_callback() {
	auto host = std::make_unique<ListenHost>(true);
	inmatch::ProcessDisconnectLatch latch;
	latch.latch(make_disconnect_event(1, 2, 0, 0, "", 35, "")); // a received kick, DPC 35
	host->ctx.client_disconnect_latch = &latch;
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Admin);
	CHECK(host->ctx.mission_exit_reason == inmatch::kMissionExitQuit);
	(void)inmatch::tick_connections(host->ctx, 0, 2, /*respect_s2c_send_boundary=*/false,
			/*run_spawn_pump=*/false);
	CHECK(host->own() == nullptr);
	CHECK(host->ctx.mission_exit_reason == inmatch::kMissionExitPirate);
	CHECK(latch.valid && latch.event.dpc == 35); // the first record wins
	host->ctx.client_disconnect_latch = nullptr;
}

// An exit from the frame's input pass: the same CLIENT line, the twin held for the mission
// teardown's pump; the next tick's receive pump removes nothing.
void test_input_exit_holds_the_twin() {
	auto host = std::make_unique<ListenHost>(true);
	(void)host->inout_lines();
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Input);
	const inmatch::NapiNPConnection *own = host->own();
	CHECK(own != nullptr && own->own_goodbye == inmatch::OwnGoodbye::TeardownPump);
	std::vector<std::string> lines = host->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kClientRemoved);
	(void)inmatch::tick_connections(host->ctx, 0, 2, false, false);
	CHECK(host->own() != nullptr);
	CHECK(host->world.registry.get(host->host_body) != nullptr);
	CHECK(host->inout_lines().empty());
	CHECK(host->to_remote().empty());
}

// Path B: an exit from the input pass, then the teardown: the pools go, the /PROFILE log closes,
// and the teardown pump's pending walk takes the held goodbye; the remote hears the host
// player's leave line, its body's 0x12 (the destroyed body's handle) and the slot's 0x46; no
// device sweep and no marker reach anything.
void test_input_exit_leaves_at_the_teardown_pump() {
	auto host = std::make_unique<ListenHost>(true);
	inmatch::ServerLogRecorder profile(host->files, "host");
	CHECK(profile.open("mission.bms"));
	host->ctx.logs.profile = &profile;
	(void)host->inout_lines();
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Input);
	(void)host->inout_lines();
	// The teardown's head: the pools' destroy, then the log's close.
	host->world.registry.despawn(host->host_body);
	host->world.registry.despawn(host->remote_body);
	profile.close();
	// A receive pump's walk does not take it; the teardown pump's does, once.
	CHECK(!inmatch::drain_own_goodbye(host->ctx, inmatch::OwnGoodbye::NextReceivePump));
	CHECK(inmatch::drain_own_goodbye(host->ctx, inmatch::OwnGoodbye::TeardownPump));
	CHECK(!inmatch::drain_own_goodbye(host->ctx, inmatch::OwnGoodbye::TeardownPump));
	CHECK(host->own() == nullptr);
	const std::vector<std::string> lines = host->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kServerRemoved);
	std::vector<uint8_t> order;
	for (const ns::Datagram &d : host->to_remote()) {
		if (d.tag == s2c::FORMATTED_GAME_TEXT || d.tag == 0x46 || d.tag == s2c::EMPTY_SLOT_SWEEP)
			order.push_back(d.tag);
		if (d.tag == s2c::ENTITY_REMOVE) {
			EntityRemove gone;
			size_t consumed = 0;
			CHECK(decode_entity_remove(d.body.data(), d.body.size(), gone, consumed) &&
					gone.entity_handle == host->host_body.packed);
			order.push_back(d.tag);
		}
	}
	CHECK(order == (std::vector<uint8_t>{s2c::FORMATTED_GAME_TEXT, s2c::ENTITY_REMOVE, 0x46}));
	// The marker went to a closed log: the file holds no PREM.
	const std::string &bytes = host->files.files["host.sph"];
	const std::vector<uint8_t> file(bytes.begin(), bytes.end());
	ServerLogDocument doc;
	CHECK(decode_server_log(file.data(), file.size(), doc) && doc.events.empty());
	host->ctx.logs.profile = nullptr;
}

// A dedicated host has no own client: the exit stores the reason and nothing else.
void test_dedicated_host_exit() {
	auto host = std::make_unique<ListenHost>(false);
	(void)host->inout_lines();
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Admin);
	CHECK(host->ctx.mission_exit_reason == inmatch::kMissionExitQuit);
	(void)inmatch::tick_connections(host->ctx, 0, 2, false, false);
	CHECK(host->inout_lines().empty());
	CHECK(host->to_remote().empty());
	CHECK(host->ctx.np_protocol.connection_list.size() == 1);
}

// Single player (the socketless host role, its own pair, no remote, /PROFILE and /INOUT on): the
// admin's exit is path A, the host player's disconnect at the next receive pump with its /INOUT
// SERVER line and its /PROFILE disconnect marker; an exit from the input pass is path B, which
// single player never drains: the client's line and the latched record, nothing more.
void test_single_player_admin_exit_is_path_a() {
	auto sp = std::make_unique<ListenHost>(true, /*with_remote=*/false);
	sp->world.rules.mp_session = false; // single player is out of a session (network type 0)
	inmatch::ServerLogRecorder profile(sp->files, "sp");
	CHECK(profile.open("mission.bms"));
	sp->ctx.logs.profile = &profile;
	(void)sp->inout_lines();
	inmatch::RemoteAdminBinding binding;
	binding.host = &sp->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Admin);
	std::vector<std::string> lines = sp->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kClientRemoved);
	(void)inmatch::tick_connections(sp->ctx, 0, 2, false, false);
	CHECK(sp->own() == nullptr);
	CHECK(sp->world.registry.get(sp->host_body) == nullptr);
	lines = sp->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kServerRemoved);
	CHECK(sp->ctx.np_protocol.connection_list.empty());
	sp->ctx.logs.profile = nullptr;
	profile.close();
	const std::string &bytes = sp->files.files["sp.sph"];
	const std::vector<uint8_t> file(bytes.begin(), bytes.end());
	ServerLogDocument doc;
	CHECK(decode_server_log(file.data(), file.size(), doc));
	CHECK(doc.events.size() == 1 && doc.events[0].kind == ServerLogEventKind::Disconnect);
}

void test_single_player_input_exit_leaves_the_record_only() {
	auto sp = std::make_unique<ListenHost>(true, /*with_remote=*/false);
	sp->world.rules.mp_session = false;
	(void)sp->inout_lines();
	inmatch::RemoteAdminBinding binding;
	binding.host = &sp->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Input);
	const inmatch::NapiNPConnection *own = sp->own();
	CHECK(own != nullptr && own->disconnect_event_valid &&
			own->disconnect_event.dstr == "I.C:CIDEMIS" &&
			own->own_goodbye == inmatch::OwnGoodbye::TeardownPump);
	std::vector<std::string> lines = sp->inout_lines();
	CHECK(lines.size() == 1 && lines[0] == kClientRemoved);
	(void)inmatch::tick_connections(sp->ctx, 0, 2, false, false);
	CHECK(sp->own() != nullptr);
	CHECK(sp->world.registry.get(sp->host_body) != nullptr);
	CHECK(sp->inout_lines().empty());
}

} // namespace

int main() {
	test_admin_exit_removes_the_host_player_at_the_next_receive_pump();
	test_admin_exit_runs_the_own_client_callback();
	test_input_exit_holds_the_twin();
	test_input_exit_leaves_at_the_teardown_pump();
	test_dedicated_host_exit();
	test_single_player_admin_exit_is_path_a();
	test_single_player_input_exit_leaves_the_record_only();
	if (failures != 0) {
		std::printf("own_client_exit: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("own_client_exit: ok\n");
	return 0;
}
