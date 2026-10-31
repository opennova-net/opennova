// D-NET-969: input action 3 on a listen host disconnects its own client as a user leave.
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
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h> // AdminSession
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/client_runtime.h> // the own client's death-screen latch
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
#include "../common/title_fixture.h"

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
	bool ctx_jo = test_titles::jo_config(ctx.config);
	w::World world;
	bool world_jo = test_titles::jo_world(world);
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

// D-NET-823 (L1-s16 FOUND 1): the admin path's teardown runs the own client's
// disconnected callback too: a stale kick record in the process latch (DPC 35) maps the
// admin's exit 1 to 7, at the exit itself (the client's teardown runs inside the action's
// disconnect, L1-s32 row 1), as the input pass's path maps it; the next receive pump keeps it.
// [orig: Input_HandleActionBinding (the admin's call @0x404A5D) case 3 -> the own client's
//  teardown (CNapiNPConnection_RequestDisconnect @0x61E0F0 - SetState(6) @0x61E0FA ->
//  CNapiNPConnection_SetState @0x626250 - @0x62628F), CNapiNPConnection_TeardownActiveConnection's
//  client callback called @0x6254E7; CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 - the latch
//  @0x4C6535..0x4C6559, 35 -> 7 @0x4C65A2]
void test_admin_exit_runs_the_own_client_callback() {
	auto host = std::make_unique<ListenHost>(true);
	inmatch::ProcessDisconnectLatch latch;
	latch.latch(make_disconnect_event(1, 2, 0, 0, "", 35, "")); // a received kick, DPC 35
	host->ctx.client_disconnect_latch = &latch;
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	inmatch::input_action_exit(binding, inmatch::ExitSite::Admin);
	CHECK(host->ctx.mission_exit_reason == inmatch::kMissionExitPirate);
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

// L1-s32 row 1: the own client's callback runs inside the exit's disconnect, and the input
// action its switch queues over a stale record runs where the exit's input pass drains the
// queue after it: the quit dialog's yes in the key scan and a console code 3 in the drain (the
// same pass: the drain re-reads the count), the admin's at the next frame's main pass; a menu
// screen's confirmation closes its screen first, whose input reset drops it. Action 3 stores exit
// reason 1 and plays the Exit Mission tone again (the listen host is a session peer), action 36
// stores 5; a stored reason (35 -> 7) maps every site at the exit.
// [orig: CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 - Input_QueueEvent(36) @0x4C6596,
//  Input_QueueEvent(3) @0x4C67A4, 35 -> 7 @0x4C65A2; Input_QueueEvent @0x4979C0 (the in-session
//  gate @0x497A0B passes both); Input_ProcessFrame @0x49D520 - Input_ProcessPlayerFrame
//  @0x49D58B, the drain @0x49D599..0x49D5E7; Input_HandleSpecialKeys @0x49C5C0 - the quit
//  dialog's yes @0x49C5FA; Input_HandleActionBinding @0x49AD40 - case 3: reason 1 @0x49AF26, the
//  tone @0x49AF30; case 36: reason 5 @0x49B780; Sound_PlayInterfaceTriggerSet @0x527BE0 - the
//  peer gate @0x527BEB; UI_IngameConfirmExitCommand @0x555460 - Game_CloseInGameScreens
//  @0x55546A; UI_ProcessMenuScreenFrame @0x54E650 - UI_CloseMenuScreen @0x54E659]
struct StaleExit {
	int32_t reason = 0;
	std::size_t tones = 0;
};

StaleExit exit_over_stale_record(uint32_t dpc, inmatch::ExitSite site) {
	auto host = std::make_unique<ListenHost>(true);
	inmatch::ProcessDisconnectLatch latch;
	latch.latch(make_disconnect_event(1, 2, 0, 0, "", dpc, "")); // a received kick
	host->ctx.client_disconnect_latch = &latch;
	inmatch::RemoteAdminBinding binding;
	binding.host = &host->ctx;
	host->world.out.script_sounds.clear();
	inmatch::input_action_exit(binding, site);
	StaleExit out;
	out.reason = host->ctx.mission_exit_reason;
	for (const w::ScriptSoundEvent &sound : host->world.out.script_sounds)
		if (sound.kind == w::ScriptSoundEvent::Kind::Interface &&
				sound.name == inmatch::kExitMissionSoundset)
			++out.tones;
	CHECK(latch.valid && latch.event.dpc == dpc); // the first record wins
	host->ctx.client_disconnect_latch = nullptr;
	return out;
}

void test_a_stale_records_queued_action_runs_where_the_pass_drains() {
	using inmatch::ExitSite;
	for (const ExitSite site : {ExitSite::Input, ExitSite::Admin, ExitSite::AdminOverMenuScreen,
				 ExitSite::MenuScreen}) {
		// The admin's GOTO over a menu screen closes it first: the next frame's screen pass
		// resets the queue (D-NET-1107).
		const bool drains = site == ExitSite::Input || site == ExitSite::Admin;
		// DPC 34 queues action 36: exit reason 5 where it runs, the leave's 1 where it is dropped.
		StaleExit out = exit_over_stale_record(34, site);
		CHECK(out.reason == (drains ? inmatch::kMissionExitCdTrouble : inmatch::kMissionExitQuit));
		CHECK(out.tones == 0);
		// DPC 33 (the kick, the deploy-idle punt) queues action 3: reason 1 and a second tone
		// where it runs (the first is the shell's, ahead of the action).
		out = exit_over_stale_record(33, site);
		CHECK(out.reason == inmatch::kMissionExitQuit);
		CHECK(out.tones == (drains ? 1u : 0u));
		// A stored reason maps at the exit whatever the site.
		out = exit_over_stale_record(35, site);
		CHECK(out.reason == inmatch::kMissionExitPirate);
		CHECK(out.tones == 0);
	}
}

// D-NET-1107 (L1-s32 FOUND): the admin's GOTO MENUSTATE closes the in-game screens ahead of
// its input action 3. With a menu screen up (the embedder's seam) the next frame runs that screen's
// pass, whose close resets the input queue ahead of its pass-through drain, and no main pass: the
// action a stale record's callback queued is dropped and the leave's reason 1 stands. With none up
// the close request finds no screen and the next frame's main pass runs it (DPC 34: action 36,
// reason 5). Either way the own goodbye waits for the next receive pump, the admin pump's.
// [orig: CAdminServer_HandleGotoCommand @0x404A20 - Game_CloseInGameScreens @0x404A4E,
//  Input_HandleActionBinding(3) @0x404A5D; Game_CloseInGameScreens @0x54B940 -
//  g_MenuScreenCloseRequest = 1 @0x54B94A; UI_ProcessMenuScreenFrame @0x54E650 - UI_CloseMenuScreen
//  @0x54E659 -> Input_ResetAllState @0x54E63F, the pass-through drain @0x54E686..0x54E695;
//  Game_ProcessMainFrame @0x5263F0 - UI_IsMenuScreenOpen @0x52660D, Input_ProcessFrame
//  @0x52661D; CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 - Input_QueueEvent(36) @0x4C6596;
//  Input_HandleActionBinding case 36: reason 5 @0x49B780]
void test_admin_goto_over_a_menu_screen_drops_the_queued_action() {
	for (const bool screen_up : {false, true}) {
		auto host = std::make_unique<ListenHost>(true);
		inmatch::ProcessDisconnectLatch latch;
		latch.latch(make_disconnect_event(1, 2, 0, 0, "", 34, "")); // a received kick, DPC 34
		host->ctx.client_disconnect_latch = &latch;
		// No world under the console: the GOTO's cycle tail runs no round end here.
		host->ctx.world = nullptr;
		gamecfg::GameCfg block = gamecfg::defaults();
		inmatch::AdminConsole::Seams seams;
		seams.config_block = &block;
		seams.config_title = opennova::title_by_code("jo");
		seams.menu_screen_open = [screen_up] { return screen_up; };
		inmatch::RemoteAdminBinding binding;
		binding.host = &host->ctx;
		binding.session = 1;
		inmatch::RemoteAdminConsoles consoles;
		consoles.bind(binding, seams);
		consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
		AdminSession admin;
		admin.user = "boss";
		admin.rights = 0x7F;
		std::vector<std::string> replies;
		CHECK(consoles.dispatch(admin, "GOTO MENUSTATE", replies));
		CHECK(host->ctx.mission_exit_reason ==
				(screen_up ? inmatch::kMissionExitQuit : inmatch::kMissionExitCdTrouble));
		const inmatch::NapiNPConnection *own = host->own();
		CHECK(own != nullptr && own->own_goodbye == inmatch::OwnGoodbye::NextReceivePump);
		CHECK(latch.valid && latch.event.dpc == 34); // the first record wins
		host->ctx.client_disconnect_latch = nullptr;
		host->ctx.world = &host->world;
	}
}

} // namespace

// A menu screen's confirmation (the death screen's YES, the in-game CONFIRM_YES) and the admin's
// GOTO close the in-game screens ahead of action 3, which drops the DEATH screen's open latch
// with no close leg, so the triggers' clear runs no title's close call; the input pass's own
// action 3 (the HUD quit dialog's yes, a console code 3) closes none.
// [orig: Game_CloseInGameScreens @0x54B940 - the zero @0x54B954; DeathScreen_OnConfirmYes
//  @0x55353A, UI_IngameConfirmExitCommand @0x55546A, CAdminServer_HandleGotoCommand @0x404A4E,
//  each ahead of its action 3; Input_HandleActionBinding case 3 @0x49AF42 closes none]
void test_a_screens_confirmation_drops_the_death_menu_latch() {
	using inmatch::ExitSite;
	for (const ExitSite site : {ExitSite::MenuScreen, ExitSite::Admin,
				ExitSite::AdminOverMenuScreen, ExitSite::Input}) {
		ns::LoopbackChannel loop;
		auto own = std::make_unique<inmatch::ClientRuntime>(loop);
		test_titles::jo_folds(*own);
		ns::ClientState &state = own->state();
		CHECK(state.take_death_menu_open(/*triggered=*/true, /*menu_open=*/false));
		inmatch::RemoteAdminBinding binding;
		binding.client = own.get();
		inmatch::input_action_exit(binding, site);
		const bool dropped = site != ExitSite::Input;
		CHECK(state.death_menu_open_latch == !dropped);
		// The triggers' clear runs the close leg over a latch left up alone.
		(void)state.take_death_menu_open(/*triggered=*/false, /*menu_open=*/false);
		CHECK(state.take_death_menu_close() == !dropped);
	}
}

// The in-game RESTART, and dfx2.exe's DEATH-screen RESTART_LEVEL that runs it, close the in-game
// screens ahead of exit reason 4: out of a session the DEATH screen's open latch drops with no
// close leg (the screen's picks are dropped); in a session the command does nothing.
// [orig: UI_IngameRestartCommand @0x555410 - is_in_session @0x555410, Game_CloseInGameScreens
//  @0x555419, g_MissionExitReason = 4 @0x555437]
// [orig: DeathScreen_OnRestartLevel @0x550BF0 - Game_CloseInGameScreens @0x550C20 (dfx2.exe)]
void test_the_restart_drops_the_death_menu_latch() {
	for (const bool session : {false, true}) {
		auto world = std::make_unique<w::World>();
		test_titles::jo_world(*world);
		world->rules.mp_session = session;
		ns::LoopbackChannel loop;
		auto own = std::make_unique<inmatch::ClientRuntime>(loop);
		test_titles::jo_folds(*own);
		ns::ClientState &state = own->state();
		CHECK(state.take_death_menu_open(/*triggered=*/true, /*menu_open=*/false));
		CHECK(inmatch::ingame_restart(*world, own.get()) == !session);
		CHECK(state.death_menu_open_latch == session);
		CHECK((world->mission_exit_reason == w::kWorldMissionExitRestart) == !session);
		// The triggers' clear runs the close leg over the latch a session's command left alone.
		(void)state.take_death_menu_open(/*triggered=*/false, /*menu_open=*/false);
		CHECK(state.take_death_menu_close() == session);
	}
}

int main() {
	test_admin_exit_removes_the_host_player_at_the_next_receive_pump();
	test_a_screens_confirmation_drops_the_death_menu_latch();
	test_the_restart_drops_the_death_menu_latch();
	test_admin_exit_runs_the_own_client_callback();
	test_input_exit_holds_the_twin();
	test_input_exit_leaves_at_the_teardown_pump();
	test_dedicated_host_exit();
	test_single_player_admin_exit_is_path_a();
	test_single_player_input_exit_leaves_the_record_only();
	test_a_stale_records_queued_action_runs_where_the_pass_drains();
	test_admin_goto_over_a_menu_screen_drops_the_queued_action();
	if (failures != 0) {
		std::printf("own_client_exit: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("own_client_exit: ok\n");
	return 0;
}
