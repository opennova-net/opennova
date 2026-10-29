// The process's remote-admin half both embedders share (runtime/inmatch/remote_admin.h): the
// 16-bit port read, the log file's text-mode line, the scene a frame outside the mission's main
// frame pumps in (or none), and the console the admin server dispatches to, session-less in the
// menus (the game's case: retail opens the console in every process), over a host context once
// one binds, and over a joiner's own client as the joined session's peer. Driven by the client
// half (net/admin/admin_protocol.h) over in-memory bytes; the protocol itself is admin_server's
// (the same login and verb vectors).
// [orig: Game_InitSubsystems @0x4A72C2..0x4A72D9; CAdminServer_Construct @0x402C84;
//  CAdminServer_DispatchCommand @0x406720; Menu_UpdateFrame @0x552901;
//  Game_ProcessMainFrame @0x5268F1]

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <base/io/crt_rand.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/rtxt/rtxt.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h> // FrameUpdate
#include <net/npwire/ingame_encode.h> // encode_frame_update
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h> // client_goodbye_to_bytes, make_disconnect_event
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h> // start_host_session (the process clock's install)
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/inmatch/napi_np_protocol.h>   // host_server_hello (the 0x81's UT)
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/remote_admin.h>
#include <runtime/inmatch/role_feeds.h> // session_rules_word
#include <runtime/inmatch/server_tick.h> // Server_TickUpdate
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/runtime_boot.h> // BootFileSource
#include <runtime/world/local_player.h> // the round end's view words
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

void expect_eq(const std::string &got, const std::string &want, const char *what) {
	if (got != want) {
		std::fprintf(stderr, "FAIL: %s\n  got:  [%s]\n  want: [%s]\n", what, got.c_str(), want.c_str());
		++g_failures;
	}
}

constexpr uint32_t kLoopback = 0x0100007Fu; // 127.0.0.1 as the platform stores it

// One logged-in client over the server's bytes.
struct Client {
	AdminServer &server;
	uint32_t id = 0;
	std::vector<uint8_t> challenge;

	static std::string reply_text(const std::vector<uint8_t> &packet) {
		size_t size = 0;
		if (packet.size() < ADMIN_PACKET_HEADER_BYTES || !admin_decode_header(packet.data(), size) ||
				packet.size() != ADMIN_PACKET_HEADER_BYTES + size)
			return "<bad packet>";
		const std::vector<uint8_t> payload(packet.begin() + ADMIN_PACKET_HEADER_BYTES, packet.end());
		std::string text;
		admin_reply_text(payload, text);
		return text;
	}

	bool connect_and_login(const std::string &user, const std::string &pass) {
		const AdminServer::Accepted a = server.accept(kLoopback);
		if (!a.admitted || a.send.size() < ADMIN_PACKET_HEADER_BYTES) return false;
		id = a.connection;
		challenge.assign(a.send.begin() + ADMIN_PACKET_HEADER_BYTES, a.send.end());
		const auto login = admin_encode_login(challenge, user, pass);
		const std::vector<uint8_t> packet = admin_encode_packet(login.data(), login.size());
		const AdminServer::Received r = server.receive(id, packet.data(), packet.size());
		return r.send.size() == 1;
	}

	std::vector<std::string> command(const std::string &line) {
		const std::vector<uint8_t> body = admin_encode_command(line);
		const std::vector<uint8_t> packet = admin_encode_packet(body.data(), body.size());
		const AdminServer::Received r = server.receive(id, packet.data(), packet.size());
		std::vector<std::string> out;
		for (const std::vector<uint8_t> &p : r.send) out.push_back(reply_text(p));
		return out;
	}

	std::string one(const std::string &line) {
		const std::vector<std::string> r = command(line);
		return r.size() == 1 ? r.front() : "<" + std::to_string(r.size()) + " replies>";
	}
};

admincfg::AdminConfig users() {
	const std::string text = "boss pw 7F\r\nip_restrict = 127.0.0.*\r\n";
	return admincfg::parse(text.data(), text.size());
}

// The port: the 16-bit word game.cfg stores; 65536 parses to 0, which opens nothing.
void test_port_and_log_text() {
	gamecfg::GameCfg cfg = gamecfg::defaults();
	expect(inmatch::remote_admin_listen_port(cfg) == 0, "the shipped cfg opens no listener");
	const std::string set = "remote_admin_port = 4711\r\n";
	gamecfg::parse(set.data(), set.size(), cfg, {});
	expect(inmatch::remote_admin_listen_port(cfg) == 4711, "a set port");
	const std::string wrapped = "remote_admin_port = 65536\r\n";
	gamecfg::parse(wrapped.data(), wrapped.size(), cfg, {});
	expect(inmatch::remote_admin_listen_port(cfg) == 0, "65536 is the word 0: no listener");
	expect_eq(inmatch::admin_log_file_text("User command (boss): GET\n"),
	          "User command (boss): GET\r\n", "a log line reaches the file as CR LF");
	expect_eq(inmatch::admin_log_file_text("a\nb\n"), "a\r\nb\r\n", "every LF");
}

// The console in the menus, then over a host's context, then session-less again.
void test_consoles() {
	inmatch::RemoteAdminConsoles consoles;
	io::CrtRand rand;
	rand.seed(7);
	AdminServer server(users(), consoles, rand);
	Client c{server};
	expect(c.connect_and_login("boss", "pw"), "the login");
	// No console bound yet: the line is taken and nothing is answered.
	expect(c.command("GET GAMESTATE").empty(), "an unbound console answers nothing");

	gamecfg::GameCfg block = gamecfg::defaults();
	block.game_name = "Menu Block";
	block.armory_reuse_time = 45;
	int saves = 0;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	seams.save_config = [&] { ++saves; };
	const inmatch::RemoteAdminBinding menus;
	consoles.bind(menus, seams);
	expect(consoles.bound_to(menus), "the session-less console");
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Menus", "the menus' state");
	const std::string settings = c.one("GET GAMESETTINGS");
	expect(settings.find("ServerName           = Menu Block\n") != std::string::npos,
	       "the menus' settings read the process block's name");
	expect(settings.find("ArmoryTimer          = 45\n") != std::string::npos, "and its armory timer");
	// No mission start has applied the block: the settings globals are the image's zeros.
	// [orig: g_VoteKickMinPlayers @0x24D2270, zero in the image; CAdminServer_HandleGet]
	expect(settings.find("VoteNumPlayersReq    = 0\n") != std::string::npos &&
	               settings.find("ChangeTeamInterval   = 0\n") != std::string::npos,
	       "before any mission the settings globals read zero");
	expect_eq(c.one("SET ServerName Renamed"), "OK - Setting Changed.", "a SET in the menus");
	// Each value token followed by one space [orig: CAdminServer_HandleSetCommand @0x4060BF..0x40615A].
	expect(block.game_name == "Renamed " && saves == 1, "writes the block and saves it");
	expect_eq(c.one("GOTO MENUSTATE"), "<2 replies>", "GOTO MENUSTATE in the menus: the error and the cycle tail");

	auto host_ptr = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::NapiNPServerCtx &host = *host_ptr;
	host.config.server_name = "Hosted";
	host.config.voting_min_players = 9;
	inmatch::RemoteAdminBinding hosted;
	hosted.host = &host;
	hosted.session = 1;
	consoles.bind(hosted, seams);
	expect(consoles.bound_to(hosted) && !consoles.bound_to(menus), "rebound over the host");
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Game", "the game loop's state");
	inmatch::RemoteAdminBinding next = hosted;
	next.session = 2;
	expect(!consoles.bound_to(next), "another session over the same context rebinds");
	// GOTO MENUSTATE in game: input action 3, exit reason 1 on the host's context, read by the
	// role's session_lost [orig: Input_HandleActionBinding case 3 @0x49AF26].
	(void)c.command("GOTO MENUSTATE");
	expect(host.mission_exit_reason == inmatch::kMissionExitQuit, "a host's quit: the context's exit reason 1");
	consoles.bind(menus, seams);
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Menus", "back in the menus");
	// The session's settings globals stay the process's; a SET writes the round clock in any
	// scene. [orig: CAdminServer_HandleSetCommand @0x405E6A, @0x405E80..0x405E86]
	expect(c.one("GET GAMESETTINGS").find("VoteNumPlayersReq    = 9\n") != std::string::npos,
	       "the menus keep the last session's settings");
	expect_eq(c.one("SET GameTime 10"), "OK - Setting Changed.", "SET GameTime in the menus");
	expect(c.one("GET GAMESETTINGS").find("GameTime             = 10/10\n") != std::string::npos,
	       "the round clock restarted at the new limit");
}

// The scene a frame outside the running mission's main frame pumps in: the menus' frame is the
// MainMenu's whatever the process keeps; a joiner's connect (the PreMenu mode), the mission's
// blocking load, its held start and its teardown pump nothing; a paused single player is the
// Game Loop's frame under its in-game menu. No frame pumps as "Unknown".
// [orig: Menu_UpdateFrame @0x552901; Game_ProcessMainFrame @0x5268F1; MultiPlayer_JoinSessionStateMachine
//  @0x56A320; GameLoop_Init @0x526377; PostMenu_RouteMissionExit @0x568460; Input_HandleActionBinding
//  @0x49B3B6]
void test_idle_scene() {
	using inmatch::AdminScene;
	using inmatch::RemoteAdminIdleFrame;
	using inmatch::State;
	const auto scene = [](bool mission, State state, bool join) {
		RemoteAdminIdleFrame frame;
		frame.mission = mission;
		frame.session_state = state;
		frame.join_pending = join;
		return inmatch::remote_admin_idle_scene(frame);
	};
	expect(scene(false, State::Unloaded, false) == AdminScene::MainMenu, "the menus pump as MainMenu");
	expect(!scene(false, State::Unloaded, true).has_value(), "a joiner's connect pumps nothing");
	expect(!scene(true, State::Loading, true).has_value(), "nor its preload's load");
	expect(scene(true, State::Paused, false) == AdminScene::GameLoop,
	       "a paused single player is the Game Loop's frame");
	expect(!scene(true, State::Running, false).has_value(),
	       "a running mission's unticked frame (the held start) pumps nothing here");
	expect(!scene(true, State::Loading, false).has_value(), "a load pumps nothing");
	expect(!scene(true, State::Connecting, false).has_value(), "nor a connect");
	expect(!scene(true, State::Stopping, false).has_value(), "nor a teardown");
	expect(!scene(true, State::Failed, false).has_value(), "nor a failed session");
	for (int s = 0; s <= static_cast<int>(State::Failed); ++s)
		for (int m = 0; m < 2; ++m)
			for (int j = 0; j < 2; ++j)
				expect(scene(m != 0, static_cast<State>(s), j != 0) != AdminScene::Other,
				       "no frame pumps as Other (\"Unknown\")");
	// The NovaWorld menu's play or host wait blocks inside its menu frame: no pump while it
	// waits, the menus' pump again once it is over. [orig: CNapiGameSession_ConnectOrHost
	//  @0x4D4F10 - the host loop @0x4D5180..0x4D519D, the play loop @0x4D54C0..0x4D54EE;
	//  Menu_UpdateFrame @0x552901]
	RemoteAdminIdleFrame waiting;
	waiting.novaworld_wait = true;
	expect(!inmatch::remote_admin_idle_scene(waiting).has_value(),
	       "a NovaWorld play or host wait pumps nothing");
	waiting.novaworld_wait = false;
	expect(inmatch::remote_admin_idle_scene(waiting) == AdminScene::MainMenu,
	       "the menus pump again once the wait is over");
}

static const Protocol &kJo = *title_protocol("jo");

// The joiner's dialed socket: what its role writes.
struct CaptureSocket final : IDatagramSocket {
	std::vector<std::vector<uint8_t>> sent;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
	}
};

// The C2S messages a joiner's frame sends, decoded with its own session key.
std::vector<ProtocolMessage> joiner_sends(inmatch::ClientRuntime &client, const std::string &scrk,
		uint32_t tick) {
	std::vector<ProtocolMessage> out;
	for (const std::vector<uint8_t> &datagram : client.Client_ProcessNetworkFrame(tick)) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) ||
				opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
				!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, header, messages))
			continue;
		for (ProtocolMessage &m : messages) out.push_back(std::move(m));
	}
	return out;
}

// A joiner's console runs over the joined session as its peer: CHAT SEND queues the global
// sender's C2S 0x0D channel 1 on its connection, CMD queues the line as C2S 0x04 [line][NUL]
// (the client's own console sender), CHAT GET reads its HUD window, and GOTO MENUSTATE is input
// action 3: exit reason 1 on its client and the user leave inside the action, its four 0x46
// goodbyes written at once with the "I.C:CIDEMIS" record, after which no frame sends. Back in the
// menus the report keeps the last map file while the destroyed session zeroed the game type.
// [orig: Chat_SendGlobalMessage @0x49A900 - the peer test @0x49A979, QueueReliableMessage(0xD, 1,
//  310) @0x49A9B4; CAdminServer_HandleCmdCommand @0x404997 -> Client_ProcessConsoleCommand
//  @0x520E80 (QueueReliableMessage(4, 1, 0) @0x520f29); CGameSession_SetConnectionMode
//  @0x4C49F0 (mode 2); Input_HandleActionBinding case 3 @0x49AF26, @0x49AF38..0x49AF42 ->
//  CNapiNetwork_DisconnectActiveConnection @0x4C9140 (role 2 for a client @0x4C91D9..0x4C91E0,
//  DC 2 @0x4C91EF, DP1 / DP2 0 @0x4C91F7 / @0x4C91FB, the tag @0x4C91FF, DPC 0 @0x4C9213, DDSTR
//  "" @0x4C921A) -> CNapiNPConnection_TeardownActiveConnection @0x6253C0 (the client's state-5 arm: the
//  clamp @0x62549E..0x6254B2, the burst @0x6254B4..0x6254D3, recv_max_per_tick 4 for the JO
//  template, the leave callback @0x6254D5..0x6254E7); CNapiGameSession_FullDestroy
//  @0x4C96A0 (g_GameType = 0 @0x4C977A); GameType_GetAbbreviation @0x520FD0 (the STROVER29
//  arm @0x5211A2..0x5211B2)]
void test_joiner_console() {
	const std::string client_scrk = "CLIENT-ADMIN-JOINER-SCRK";
	const std::string server_scrk = "SERVER-ADMIN-JOINER-SCRK";
	uint64_t now = 7000;
	auto joiner = std::make_unique<inmatch::JoinerRole>();
	joiner->runtime = std::make_unique<inmatch::ClientRuntime>("AdminJoiner", [&now] { return now; });
	inmatch::ClientRuntime &client = *joiner->runtime;
	CaptureSocket socket;
	joiner->set_socket(&socket, PeerAddr{0x0100007Fu, 32123});
	client.set_protocol(&kJo);
	joiner->seed_session(0x41444D4Eu, 1u, client_scrk, server_scrk, 1, 0, 0x0002,
	                    world::kPlayerInfantryTypeId, game_type::kTeamDeathmatch, 0x00100000u,
	                    /*replay_mode=*/false);
	(void)joiner_sends(client, client_scrk, 1);

	inmatch::RemoteAdminConsoles consoles;
	io::CrtRand rand;
	rand.seed(11);
	AdminServer server(users(), consoles, rand);
	Client c{server};
	expect(c.connect_and_login("boss", "pw"), "the joiner's admin login");
	gamecfg::GameCfg block = gamecfg::defaults();
	block.time_limit = 25;
	block.max_kills = 500;
	block.autobalance_on_recycle_enabled = 1;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	std::vector<std::string> echoes;
	seams.chat_echo = [&](const std::string &text) { echoes.push_back(text); };
	seams.chat_window = [] { return std::vector<std::string>{"Ann: hi", "", "Bob: yo"}; };
	seams.game_text = [](std::string_view section, std::string_view key) -> std::string {
		if (section == "Overlays" && key == "STROVER64") return "Team Deathmatch";
		if (section == "Overlays" && key == "STROVER29") return "Game Type Zero";
		return {};
	};
	// The joiner's world: its clock at 15:00, as the S2C 0x0A left it.
	auto world = std::make_unique<world::World>();
	world->weather.tod_fixed24 = 0x0F000000u;
	world->rules.mpattrib = 0x200; // the joined mission's S2C 0x64 word: NoFriendlyFire
	inmatch::RemoteAdminBinding joined;
	joined.client = &client;
	joined.joiner = joiner.get();
	joined.world = world.get();
	joined.session = 7;
	consoles.bind(joined, seams);
	expect(consoles.bound_to(joined), "the joiner's console");
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);

	// GET GAMESETTINGS reads the globals the joiner's own mission start copied from its block,
	// the joined mission's rules word and the received round clock (none yet: -1, 0 minutes);
	// its slot table is empty. [orig: Game_ApplySessionSettingsToGlobals @0x551B6D..0x551B75
	//  (500 -> 65000), @0x551BCE, @0x551CD2; CAdminServer_HandleGet @0x403449, @0x4038C5,
	//  @0x403915; Server_InitNewRoundState @0x51C939, @0x51C946]
	const std::string settings = c.one("GET GAMESETTINGS");
	expect(settings.find("AutoBalanceOnRecycle = 1\n") != std::string::npos, "the block's auto-balance word");
	expect(settings.find("GameTime             = 0/25\n") != std::string::npos,
	       "the block's time limit; no round clock received");
	expect(settings.find("KillLimit            = 65000\n") != std::string::npos, "max_kills 500 reads 65000");
	expect(settings.find("FriendlyFire         = 0\n") != std::string::npos, "the joined mission's rules word");
	expect(consoles.status_report().find("CURRENT # PLAYERS: 0\r\n") != std::string::npos,
	       "a joiner's slot table is empty");

	expect_eq(c.one("CHAT SEND hello there"), "OK - Chat sent.", "CHAT SEND on a joiner");
	std::vector<ProtocolMessage> sent = joiner_sends(client, client_scrk, 2);
	bool chat = false;
	for (const ProtocolMessage &m : sent)
		chat = chat || (m.tag == c2s::CHAT_MESSAGE &&
		                m.payload == std::vector<uint8_t>{1, 'h', 'e', 'l', 'l', 'o', ' ', 't', 'h', 'e', 'r',
		                                                  'e', ' ', 0});
	expect(chat, "a joiner's CHAT SEND leaves as C2S 0x0D [1][\"hello there \"][NUL]");
	// The same line again inside the flood window: refused, echoed into the window.
	expect_eq(c.one("CHAT SEND hello there"), "OK - Chat sent.", "a repeat is answered the same");
	expect(echoes.size() == 1 && echoes[0] == "hello there ", "the refused repeat is echoed");
	(void)joiner_sends(client, client_scrk, 3);

	expect_eq(c.one("CMD SERVER kick 2"), "OK - Command executed.", "CMD on a joiner");
	bool line = false;
	for (const ProtocolMessage &m : joiner_sends(client, client_scrk, 4))
		line = line || (m.tag == c2s::ADMIN_NETLOG_COMMAND &&
		                m.payload == std::vector<uint8_t>{'S', 'E', 'R', 'V', 'E', 'R', ' ', 'k', 'i', 'c', 'k',
		                                                  ' ', '2', ' ', 0});
	expect(line, "a joiner's CMD leaves as C2S 0x04 [\"SERVER kick 2 \"][NUL]");

	expect_eq(c.one("CHAT GET"), "Ann: hi\r\nBob: yo\r\n", "CHAT GET reads the joiner's window");

	// The status report prints the process's g_MapFileName and g_GameType: the joined
	// session's (the S2C 0x2C's map file, the seeded 0x08 game type).
	// [orig: CAdminServer_HandleStatus @0x402FF6, @0x403052; NapiNPClientMsg_MissionMapNames
	//  @0x427E6C]
	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> body;
	SessionCrypto crypto{server_scrk, {}, 1u};
	const std::vector<uint8_t> names = {'H', 'o', 's', 't', 'e', 'd', 0,
	                                    'T', 'D', 'M', '0', '1', '.', 'B', 'M', 'S', 0};
	expect(frame_session_packet(server_seq, crypto,
	                            {make_protocol_message(s2c::MISSION_MAP_NAMES, names)}, body),
	       "the 0x2C frames");
	const std::vector<uint8_t> datagram =
			nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	client.receive(datagram.data(), datagram.size());
	(void)joiner_sends(client, client_scrk, 5);
	const std::string joined_lines = "CURRENT MAP: TDM01.BMS\r\nCURRENT GAME TYPE: Team Deathmatch\r\n";
	expect(consoles.status_report().find(joined_lines) != std::string::npos,
	       "a joiner's report: the joined session's map file and game type");

	// UP-TIME is g_SessionStatus' clock: none before the S2C 0x58; then the 0x58's uptime plus
	// the GetTickCount time since its receipt (90061 s on receipt, 2 s later: 1 day 01:01:03).
	// CURRENT TOD is the high word of the joiner's g_EnvCurTimeFixed24 (0x0F000000 -> 3840).
	// [orig: CSessionTimer_GetElapsedMS @0x52D5F0; SessionStatus_ParseFromBuffer @0x530ED0 (the
	//  uptime +0x70 @0x531003, the stamp +0x74 @0x5310A7); CAdminServer_HandleStatus @0x402ECD
	//  (the d / h / m / s split), the TOD's high word @0x402F6F]
	expect(consoles.status_report().find("UP-TIME: 0 00:00:00\r\nCURRENT TOD: 3840\r\n") != std::string::npos,
	       "no 0x58 yet: the session clock reads 0; the world's time of day");
	std::vector<uint8_t> status = {'H', 'o', 's', 't', 'e', 'd', 0, 'T', 'D', 'M', '0', '1', 0, 0, 0, 0};
	const uint32_t uptime_ms = 90061000u;
	for (int i = 0; i < 4; ++i) status.push_back(static_cast<uint8_t>(uptime_ms >> (8 * i)));
	status.resize(status.size() + 39 * 4, 0);
	status.push_back(0); // no option pairs
	body.clear();
	expect(frame_session_packet(server_seq, crypto, {make_protocol_message(s2c::SESSION_STATUS, status)}, body),
	       "the 0x58 frames");
	const std::vector<uint8_t> status_datagram =
			nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	client.receive(status_datagram.data(), status_datagram.size());
	(void)joiner_sends(client, client_scrk, 6);
	now += 2000;
	expect(consoles.status_report().find("UP-TIME: 1 01:01:03\r\n") != std::string::npos,
	       "the 0x58's uptime plus the time since its receipt");

	expect(client.mission_exit_reason() == inmatch::kMissionExitNone, "no exit yet");
	expect(socket.sent.empty(), "the joiner's role has written nothing yet");
	// The cycle tail answers from the frame's game mode, still the Game Loop's after the quit:
	// x5's retail joiner replied `OK - Server is cycling...`. [orig: CAdminServer_HandleGotoCommand
	//  @0x4049D0 - Input_HandleActionBinding(3) @0x404A5D, then the Game Loop test @0x404A7E,
	//  Server_ProcessRoundEnd(0) @0x404A95, the 620 @0x404AA5, the reply @0x404AAF]
	const std::vector<std::string> goto_menu = c.command("GOTO MENUSTATE");
	expect(goto_menu.size() == 1 && goto_menu[0] == "OK - Server is cycling...",
	       "GOTO MENUSTATE on a joiner in the Game Loop: the cycle tail's one reply");
	expect(client.state().spawn_success_gate && client.state().end_round_linger_ticks == 620,
	       "the joiner's own round end: its round-over latch and the 620 linger");
	expect(client.mission_exit_reason() == inmatch::kMissionExitQuit, "GOTO MENUSTATE: the joiner's exit reason 1");
	const std::vector<uint8_t> goodbye = client_goodbye_to_bytes(
			0x41444D4Eu, make_disconnect_event(2, 2, 0, 0, "I.C:CIDEMIS", 0, ""));
	expect(socket.sent.size() == 4, "the action writes the four-datagram goodbye burst at once");
	for (const std::vector<uint8_t> &datagram : socket.sent) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		expect(nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
		               opcode == SESSION_OPCODE_CLIENT_GOODBYE && body == goodbye,
		       "each is the keyed 0x46 with the CIDEMIS record");
	}
	expect(client.session_lost(), "the connection is torn down");
	expect(client.Client_ProcessNetworkFrame(7).empty(), "the next frame sends no C2S");
	joiner->close();
	expect(socket.sent.size() == 4, "the teardown's close sends nothing more");

	// Back in the menus through the Post Menu's reason-1 arm: the destroyed session zeroed
	// g_GameType, the map file stays.
	consoles.route_mission_exit(client.mission_exit_reason());
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	const std::string report = consoles.status_report();
	expect(report.find("CURRENT MAP: TDM01.BMS\r\nCURRENT GAME TYPE: Game Type Zero\r\n") != std::string::npos,
	       "back in the menus: the last map file and game type 0's abbreviation");
	// The teardown cleared g_SessionStatus; the time of day keeps the last mission's clock.
	// [orig: Game_TeardownMission @0x522350 (@0x5223FA)]
	expect(report.find("UP-TIME: 0 00:00:00\r\nCURRENT TOD: 3840\r\n") != std::string::npos,
	       "back in the menus: no session clock, the last time of day");
	// The PLAYER_INFO screen's portrait draws: its 16:00 (0x10000000) is the process's time of
	// day from then on, never restored (0x10000000 >> 16 = 4096).
	// [orig: PlayerInfo_RenderPlayerPreview3D @0x5609C0, the store @0x560AF5;
	//  CAdminServer_HandleStatus @0x402F6F]
	consoles.player_preview_drawn();
	expect(consoles.status_report().find("CURRENT TOD: 4096\r\n") != std::string::npos,
	       "after the player preview drew: 16:00");
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	expect(consoles.status_report().find("CURRENT TOD: 4096\r\n") != std::string::npos,
	       "kept across the menus' frames");
}

// The session's end into the menus zeroes g_GameType only where the session is destroyed: the
// Post Menu's destroy arm (reason 1, the session's own end, a rotation's end) and the pre-game
// menu's cancel; its reset (2) and error arms (5, 6, 7, the mapped disconnects 9..19, the
// NovaWorld session's end 12, 20) skip the destroy, so the menus keep printing the last
// session's game type. A joiner kicked with a DPC 37 record leaves with exit reason 10.
// [orig: PostMenu_RouteMissionExit @0x568460 - reason 2 @0x56846F..0x5684A7, the error arms
//  @0x5684A8..0x5685BD -> loc_5686DA..0x568715, the destroy @0x568683 reached from @0x5684AB
//  (reason 1), @0x5685C2..0x5685D6 and @0x56864F (the rotation's end);
//  CNapiGameSession_FullDestroy @0x4C96A0 (ebp zeroed @0x4C96ED, g_GameType = 0 @0x4C977A);
//  CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 (DPC 36..45 -> reasons 9..18);
//  PreMenu_OnAbortCancel @0x568CB4 (state 9) and UI_PreGameMenuStateMachine's BACK arm
//  @0x568B57..0x568B61 (state 9) -> MultiPlayer_JoinSessionStateMachine @0x56A3E3;
//  UI_EnumerateAndJoinSession @0x56A207 (the row's type copied ahead of the choice, which the
//  destroy zeroes too);
//  GameType_GetAbbreviation @0x520FD0 (the STROVER29 arm @0x5211A2..0x5211B2)]
void test_session_destroy_routes() {
	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	seams.game_text = [](std::string_view section, std::string_view key) -> std::string {
		if (section == "Overlays" && key == "STROVER64") return "Team Deathmatch";
		if (section == "Overlays" && key == "STROVER29") return "Game Type Zero";
		return {};
	};
	const std::string kept = "CURRENT GAME TYPE: Team Deathmatch\r\n";
	const std::string zero = "CURRENT GAME TYPE: Game Type Zero\r\n";
	uint64_t now = 1000;
	uint64_t session = 0;
	// A joiner in a Team Deathmatch session, then its exit's route into the menus.
	auto menus_after = [&](int32_t reason) {
		inmatch::ClientRuntime client("RouteJoiner", [&now] { return now; });
		client.seed_session(0x41444D4Eu, 1u, "CLIENT-ROUTE-SCRK", "SERVER-ROUTE-SCRK", 1, 0, 0x0002,
		                    world::kPlayerInfantryTypeId, game_type::kTeamDeathmatch, 0x00100000u,
		                    /*replay_mode=*/false);
		inmatch::RemoteAdminBinding joined;
		joined.client = &client;
		joined.session = ++session;
		consoles.bind(joined, seams);
		consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
		expect(consoles.status_report().find(kept) != std::string::npos, "the joined session's game type");
		consoles.route_mission_exit(reason);
		consoles.bind(inmatch::RemoteAdminBinding{}, seams);
		consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
		return consoles.status_report();
	};
	const int32_t kicked = inmatch::mission_exit_for_disconnect(
			make_disconnect_event(1, 2, 0, 0, "", 37, "")).reason;
	expect(kicked == 10, "a DPC 37 record maps to exit reason 10");
	expect(menus_after(kicked).find(kept) != std::string::npos,
	       "reason 10: the error arm keeps the last session's game type");
	expect(menus_after(inmatch::kMissionExitQuit).find(zero) != std::string::npos,
	       "reason 1: the destroy zeroes it");
	for (const int32_t reason : {2, 5, 6, 7, 9, 12, 19, 20})
		expect(menus_after(reason).find(kept) != std::string::npos, "the reset and error arms keep it");
	for (const int32_t reason : {3, 4})
		expect(menus_after(reason).find(zero) != std::string::npos, "the rotation's end and the rest destroy");
	// A kept game type (A6's kick), then the pre-game menu's cancel of the next join (A7's BACK
	// or ABORT_CANCEL; ours: NetSessionController._cancel_join_role_choice ->
	// GameWorld.pre_game_cancelled -> session_destroyed): its cleanup destroys.
	expect(menus_after(kicked).find(kept) != std::string::npos, "kept again");
	consoles.session_destroyed();
	expect(consoles.status_report().find(zero) != std::string::npos, "the pre-game cancel zeroes it");
}

// The menus' console reads the process's gametext, which the process start loads once: a fresh
// process's QUERY prints game type 0's STROVER29 text, as it does after a destroyed session (x5's
// A4 / A5 / A6, where ours printed an empty CURRENT GAME TYPE: the menus' console had no gametext).
// Each lookup searches the mount's override table first; a mount with no gametext gives no seam
// (GameText_GetString's "" with no table). The expected report is x5's A4 retail reply (a fresh
// Jointops.exe in the main menu), byte for byte.
// [orig: Game_InitSubsystems @0x4A6FE3..0x4A6FED (g_TextGameText, its one writer);
//  GameText_GetString @0x51EBD0; GameType_GetAbbreviation @0x520FD0 (the STROVER29 arm
//  @0x5211A2..0x5211B2); CAdminServer_HandleStatus @0x402E30]
void test_menus_game_text() {
	rtxt::File table;
	table.sections.push_back({"Overlays", 2});
	table.entries.push_back({"STROVER29", "Deathmatch", {}, 0});
	table.entries.push_back({"STROVER64", "Team Deathmatch", {}, 0});
	std::vector<uint8_t> bin;
	std::string error;
	expect(rtxt::write(table, bin, error), "the gametext table writes");
	mission::BootFileSource files;
	files.has_file = [](const std::string &name) { return name == "gametext.bin"; };
	files.read_file = [&bin](const std::string &name, std::vector<uint8_t> &out) {
		if (name != "gametext.bin") return false;
		out = bin;
		return true;
	};
	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	seams.game_text = inmatch::process_game_text(files, nullptr);
	expect(static_cast<bool>(seams.game_text), "a mounted gametext answers");
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(consoles.status_report(),
	          "SERVER: \r\nUP-TIME: 0 00:00:00\r\nCURRENT TOD: 0\r\nCURRENT # PLAYERS: 0\r\nCURRENT MAP: \r\n"
	          "CURRENT GAME TYPE: Deathmatch\r\nCURRENT PLAYERS: \r\n"
	          "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\nMAP QUEUE: \r\nNo missions in queue.",
	          "a fresh process's menus: x5's A4 retail QUERY");

	auto override_table = std::make_shared<rtxt::File>();
	override_table->sections.push_back({"Overlays", 1});
	override_table->entries.push_back({"STROVER29", "Expansion Deathmatch", {}, 0});
	seams.game_text = inmatch::process_game_text(files, override_table);
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	expect(consoles.status_report().find("CURRENT GAME TYPE: Expansion Deathmatch\r\n") != std::string::npos,
	       "the override table's entry first");

	mission::BootFileSource bare;
	bare.has_file = [](const std::string &) { return false; };
	bare.read_file = [](const std::string &, std::vector<uint8_t> &) { return false; };
	expect(!inmatch::process_game_text(bare, nullptr), "no gametext in the mount: no seam");
}

// The local view's four words the round end clears, raised (and the body pose's raise, which it
// keeps). [orig: Server_ProcessRoundEnd @0x5168EA..0x5168FC]
void raise_view_words(world::LocalPlayer &local) {
	local.view.binoculars_view_active = true;
	local.view.binoculars_requested = true;
	local.view.binoculars_raised = true;
	local.view.scope_settled = true;
	local.hud_map_control.mode = 3;
}

bool view_words_cleared(const world::LocalPlayer &local) {
	return !local.view.binoculars_view_active && !local.view.binoculars_requested &&
	       !local.view.scope_settled && local.hud_map_control.mode == 0 &&
	       local.view.binoculars_raised;
}

// A joiner's MISSION CYCLE: the cycle tail is gated on the Game Loop alone, so a joiner's console
// cycles too. Its own round end runs over its empty slot table (nothing to send): its board build
// rewrites the end-round words its overlay reads (the winner 0; a Team Deathmatch's team words from
// its team records, which nothing on a joiner scored, 0 and 0, so a draw; rows 0..2 cleared; the
// 0x1D's own player index kept), raises its round-over latch, which draws the overlay, and clears
// its view's four words; the cycle stores the 620 linger over the round end's 2790, which the
// joiner's client frame counts down into exit 4. In the menus it is refused.
// [orig: CAdminServer_HandleMissionCommand @0x4065E7..0x40662A (Server_ProcessRoundEnd(0)
//  @0x40660B, the store @0x40661F); Server_ProcessRoundEnd @0x5164F0 (the board build call
//  @0x516590, the 2790 @0x5166C4, g_RoundOver @0x5168E4, the view words @0x5168EA..0x5168FC);
//  Server_BuildEndOfRoundScoreboard @0x508F30 (the memset @0x508F3F, the winner @0x508F77, the
//  team records @0x508FA7 / @0x508FB8, the team draw @0x5092AD); HUD_DrawOverlayPanels @0x5C0060
//  (@0x5C0072); Client_ProcessNetworkFrame @0x42C3AB..0x42C3D3]
void test_joiner_cycle() {
	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	uint64_t now = 1000;
	inmatch::ClientRuntime client("CycleJoiner", [&now] { return now; });
	client.seed_session(0x41444D4Eu, 1u, "CLIENT-CYCLE-SCRK", "SERVER-CYCLE-SCRK", 1, 0, 0x0002,
	                    world::kPlayerInfantryTypeId, game_type::kTeamDeathmatch, 0x00100000u,
	                    /*replay_mode=*/false);
	auto world = std::make_unique<world::World>();
	auto local = std::make_unique<world::LocalPlayer>(*world);
	world->local_player_state = local.get();
	raise_view_words(*local);
	// A stale header in the words (the build's memset clears the rows; the player index is the
	// 0x1D's own byte, which the build leaves).
	EndRoundHeader &stale = client.state().end_round.header;
	stale.winner_team = 2;
	stale.team_score_0 = 7;
	stale.team_score_1 = 3;
	stale.player_index = 4;
	stale.player_names[0] = "Ann";
	stale.player_scores[0] = 9;
	inmatch::RemoteAdminBinding joined;
	joined.client = &client;
	joined.world = world.get();
	joined.session = 1;
	consoles.bind(joined, seams);
	const AdminSession admin{"boss", 0xFFFFFFFFu};
	const auto line = [&](const std::string &text) {
		std::vector<std::string> replies;
		(void)consoles.dispatch(admin, text, replies);
		return replies.size() == 1 ? replies[0] : std::string("<" + std::to_string(replies.size()) + ">");
	};
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(line("MISSION CYCLE"), "ERROR - Must be in Game State to cycle server.", "a joiner's CYCLE outside");
	expect(!client.state().spawn_success_gate && client.state().end_round_linger_ticks == 0 &&
	               !client.state().end_round.header_known && local->view.binoculars_view_active,
	       "refused: the joiner's words untouched");
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	expect_eq(line("MISSION CYCLE"), "OK - Server is cycling...", "a joiner's CYCLE in the Game Loop");
	expect(client.state().spawn_success_gate && client.state().end_round_linger_ticks == 620,
	       "the joiner's round-over latch and the 620 linger");
	const replication::ClientEndRoundStats &er = client.state().end_round;
	expect(er.header_known && er.header_updates == 0, "the build's words, no 0x1D landed");
	expect(er.header.winner_team == 0 && er.header.team_score_0 == 0 && er.header.team_score_1 == 0 &&
	               er.header.draw == 1,
	       "the winner 0, the team records' 0 and 0, a team draw");
	expect(er.header.player_names[0].empty() && er.header.player_scores[0] == 0 &&
	               er.header.player_index == 4,
	       "rows cleared; the 0x1D's player index kept");
	const inmatch::EndRoundSessionState shown = inmatch::end_round_session_state(
			inmatch::RoleView{nullptr, &client, nullptr, true});
	expect(shown.header_known && shown.winner == 0 && shown.draw,
	       "the overlay announces the build's words");
	expect(view_words_cleared(*local), "the joiner's four view words cleared, the body pose kept");
	expect(client.mission_exit_reason() == inmatch::kMissionExitNone, "no exit until the linger runs out");
}

// An A&S joiner's cycle: the board build's team words count the zone chain's zones by the team
// byte each zone carries on the joiner (a received record's over the mission's), 2 for team 1
// and 1 for team 2: no draw. [orig: Server_BuildEndOfRoundScoreboard @0x508F30 (the game type
// test @0x508F8D, the counts @0x508FBF..0x508FE0, the team draw @0x5092AD);
// CTeamSlotManager_CountByTeam @0x4A2A40 (the entity+354 team byte @0x4A2A96)]
void test_joiner_cycle_zones() {
	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	uint64_t now = 1000;
	inmatch::ClientRuntime client("ZoneJoiner", [&now] { return now; });
	client.seed_session(0x41444D4Eu, 1u, "CLIENT-ZONE-SCRK", "SERVER-ZONE-SCRK", 1, 0, 0x0002,
	                    world::kPlayerInfantryTypeId, game_type::kAdvanceAndSecure, 0x00100000u,
	                    /*replay_mode=*/false);
	auto world = std::make_unique<world::World>();
	world->registry.configure_pool(1, 4);
	world::Entity zone;
	zone.kind = world::EntityKind::Item;
	zone.team = 2;
	zone.zone_number = 1;
	const world::EntityHandle taken = world->registry.spawn(1, zone); // the mission's 2, received 1
	zone.zone_number = 2;
	world->zones.chain.zones.push_back(taken);
	world->zones.chain.zones.push_back(world->registry.spawn(1, zone)); // 2, no record
	zone.team = 1;
	zone.zone_number = 3;
	world->zones.chain.zones.push_back(world->registry.spawn(1, zone)); // 1, no record
	replication::ClientEntityState &row = client.state().upsert(taken.packed);
	row.team = 1;
	row.team_known = true;
	inmatch::RemoteAdminBinding joined;
	joined.client = &client;
	joined.world = world.get();
	joined.session = 1;
	consoles.bind(joined, seams);
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	std::vector<std::string> replies;
	(void)consoles.dispatch(AdminSession{"boss", 0xFFFFFFFFu}, "MISSION CYCLE", replies);
	const EndRoundHeader &header = client.state().end_round.header;
	expect(header.team_score_0 == 2 && header.team_score_1 == 1 && header.draw == 0 &&
	               header.winner_team == 0,
	       "the zones the joiner holds: 2 for team 1, 1 for team 2, no draw");
}

// The authority's own round end clears its local view's four words too (single player and a
// session host alike), ahead of the single-player tail. [orig: Server_ProcessRoundEnd @0x5164F0
// (g_RoundOver @0x5168E4, the view words @0x5168EA..0x5168FC, the SP tail @0x51691D)]
void test_authority_round_end_view() {
	for (const bool session : {false, true}) {
		auto world = std::make_unique<world::World>();
		world->registry.configure_pool(0, 8);
		world->rules.mp_session = session;
		auto local = std::make_unique<world::LocalPlayer>(*world);
		world->local_player_state = local.get();
		raise_view_words(*local);
		auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
		ctx->world = world.get();
		ctx->is_authority = 1;
		ctx->is_in_session = 1;
		gamecfg::GameCfg block = gamecfg::defaults();
		inmatch::AdminConsole::Seams seams;
		seams.config_block = &block;
		inmatch::AdminConsole console(*ctx, std::move(seams));
		console.set_scene(inmatch::AdminScene::GameLoop);
		std::vector<std::string> replies;
		(void)console.dispatch(AdminSession{"boss", 0xFFFFFFFFu}, "MISSION CYCLE", replies);
		expect(world->match.outcome().ended, "the authority's round ends");
		expect(view_words_cleared(*local), "the authority's four view words cleared, the body pose kept");
	}
}

// A listen host's console is a peer over its host context: CMD leaves through its own client
// over the loopback as C2S 0x04; single player with no host context quits through its world.
void test_listen_host_and_single_player() {
	inmatch::RemoteAdminConsoles consoles;
	io::CrtRand rand;
	rand.seed(5);
	AdminServer server(users(), consoles, rand);
	Client c{server};
	expect(c.connect_and_login("boss", "pw"), "the host's admin login");
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;

	replication::LoopbackChannel host_loop;
	inmatch::ClientRuntime own(host_loop);
	own.view().set_mp_session(true);
	inmatch::NapiNPServerCtx host;
	host.is_authority = 1;
	host.is_mp_session_peer = 1;
	host.is_in_session = 1;
	inmatch::RemoteAdminBinding listen;
	listen.host = &host;
	listen.client = &own;
	listen.session = 3;
	consoles.bind(listen, seams);
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	expect_eq(c.one("CMD NETLOG"), "OK - Command executed.", "CMD on a listen host");
	own.Client_ProcessNetworkFrame(1);
	replication::Datagram datagram;
	bool saw_line = false;
	while (host_loop.host_recv(datagram))
		saw_line = saw_line || (datagram.tag == c2s::ADMIN_NETLOG_COMMAND &&
		                        datagram.body == std::vector<uint8_t>{'N', 'E', 'T', 'L', 'O', 'G', ' ', 0});
	expect(saw_line, "a listen host's CMD leaves its own client over the loopback as C2S 0x04");

	auto world = std::make_unique<world::World>();
	inmatch::RemoteAdminBinding single;
	single.world = world.get();
	single.session = 4;
	consoles.bind(single, seams);
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	(void)c.command("GOTO MENUSTATE");
	expect(world->mission_exit_reason == inmatch::kMissionExitQuit,
	       "a single player's quit: the world's exit reason 1");
}

// Single player's MISSION CYCLE: the round ends at once (no winner) and the 620-tick linger is
// stored, but single player is out of a session (its network type 0), so its server tick never
// drains the linger: no exit, the player stays on the round's end (the lose epilog, until the
// player's key or the screen's 18600-frame timeout [orig: Server_ProcessRoundEnd @0x5164F0 (the
// Cine_StartPlayback call @0x51697B)]). A session
// host's tick drains the same linger into exit 3. (Ours keeps the context in session; the world's
// mp_session word is single player's is_in_session.)
// [orig: CAdminServer_HandleMissionCommand @0x40660B (Server_ProcessRoundEnd(0)), @0x40661F (the
//  620 store); Server_TickUpdate @0x51D9A5 (the is_in_session test ahead of the drain), @0x51D9B1,
//  the drain @0x51D9BD..0x51D9CC, 3 @0x51DB63; SinglePlayer_StartMission @0x561BD4]
void test_single_player_cycle() {
	for (const bool session : {false, true}) {
		auto world = std::make_unique<world::World>();
		world->registry.configure_pool(0, 8);
		world->rules.mp_session = session;
		auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
		ctx->world = world.get();
		ctx->is_authority = 1;
		ctx->is_in_session = 1;
		gamecfg::GameCfg block = gamecfg::defaults();
		inmatch::AdminConsole::Seams seams;
		seams.config_block = &block;
		inmatch::AdminConsole console(*ctx, std::move(seams));
		console.set_scene(inmatch::AdminScene::GameLoop);
		std::vector<std::string> replies;
		(void)console.dispatch(AdminSession{"boss", 0xFFFFFFFFu}, "MISSION CYCLE", replies);
		expect(replies.size() == 1 && replies[0] == "OK - Server is cycling...", "the cycle runs");
		expect(world->match.outcome().ended && ctx->round_end_linger_ticks == 620,
		       "the round ends with the 620 linger");
		for (int i = 0; i < 640; ++i) inmatch::Server_TickUpdate(*ctx);
		if (session)
			expect(ctx->mission_exit_reason == inmatch::kMissionExitMapCycle,
			       "a session host drains the linger into exit 3");
		else
			expect(ctx->mission_exit_reason == inmatch::kMissionExitNone && ctx->round_end_linger_ticks == 620,
			       "single player never drains it");
	}
}

// A paused single player's frame is the Game Loop's under its in-game menu: its exit router
// reads the stored reason ahead of the pump, so a reason stored by the last pump leaves the
// mission on the next frame, which pumps nothing; with no reason it pumps. No other idle frame
// runs the router. [orig: Game_ProcessMainFrame @0x5267F8, @0x52685E..0x526867, ahead of
//  the pump @0x5268F1 (CAdminServer_ProcessFrame @0x406F50)]
void test_idle_frame_exit() {
	using inmatch::RemoteAdminIdleFrame;
	using inmatch::State;
	RemoteAdminIdleFrame paused;
	paused.mission = true;
	paused.session_state = State::Paused;
	expect(!inmatch::remote_admin_idle_frame_exits(paused), "no reason: the paused frame pumps");
	paused.mission_exit_reason = inmatch::kMissionExitQuit;
	expect(inmatch::remote_admin_idle_frame_exits(paused), "reason 1: the paused frame leaves the mission");
	RemoteAdminIdleFrame menus;
	menus.mission_exit_reason = inmatch::kMissionExitQuit;
	expect(!inmatch::remote_admin_idle_frame_exits(menus), "the menus' frame has no router");
	RemoteAdminIdleFrame loading = paused;
	loading.session_state = State::Loading;
	expect(!inmatch::remote_admin_idle_frame_exits(loading), "nor a load");
}

// The session's exit frame: retail's last main frame runs its whole tick ahead of its exit test,
// which routes to the Post Menu without pumping the admin, so the menus' report reads that
// frame's round clock and time of day (no teardown writes them); UP-TIME's g_SessionStatus is
// cleared by the teardown. Ours' frame stops before its pump, so the console over the session
// takes them at the stop. Retail's words are process globals whatever the console, so an exiting
// session whose console never pumped (its first frame was its last) is bound first and read.
// [orig: Game_ProcessMainFrame @0x5263F0 - g_RoundTimeRemaining @0x526602, Server_TickUpdate
//  @0x5266B6, Environment_UpdateWeatherTick @0x526774, the exit test @0x52685E ahead of
//  CAdminServer_ProcessFrame @0x5268F1; the NovaWorld check @0x52655D..0x52657C on frame 1
//  (g_NetQualityUpdateCountdown zeroed by Game_StartMission @0x524360 (@0x52439E));
//  Game_TeardownMission @0x522350 (@0x5223FA); Menu_UpdateFrame @0x5528A0 (its pump @0x552901);
//  CAdminServer_HandleGet @0x403390 (g_RoundTimeRemaining @0x40388D, / 60 / 62)]
void test_exit_frame_words() {
	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	auto world = std::make_unique<world::World>();
	auto host = std::make_unique<inmatch::NapiNPServerCtx>();
	host->world = world.get();
	host->is_authority = 1;
	host->is_in_session = 1;
	host->config.round_time_limit_minutes = 5;
	inmatch::RemoteAdminBinding hosted;
	hosted.host = host.get();
	hosted.world = world.get();
	hosted.session = 5;
	consoles.bind(hosted, seams);
	// The last pumped frame: five minutes on the round clock (5 x 3720 ticks).
	world->weather.tod_fixed24 = 0x0C000000u;
	world->match.set_remaining_ticks(18600);
	host->np_protocol.host_run_duration_ms = 60000u;
	consoles.refresh_process_mission();
	expect(consoles.process_mission().tod_fixed24 == 0x0C000000u &&
	               consoles.process_mission().session_elapsed_ms == 60000u &&
	               consoles.process_mission().round_ticks_remaining == 18600,
	       "the pumped frame's words");
	// The exit frame ticks once more (the round clock's step, the weather) and stops before its
	// pump: the console over the session is kept and takes its words.
	world->weather.tod_fixed24 = 0x0C000100u;
	world->match.set_remaining_ticks(18599);
	host->np_protocol.host_run_duration_ms = 60016u;
	const inmatch::AdminConsole *console = consoles.console();
	consoles.session_exit_frame(hosted, seams);
	expect(consoles.console() == console, "the console over the exiting session is kept");
	expect(consoles.process_mission().tod_fixed24 == 0x0C000100u &&
	               consoles.process_mission().session_elapsed_ms == 60016u &&
	               consoles.process_mission().round_ticks_remaining == 18599,
	       "the exit frame's words");
	// Back in the menus: UP-TIME cleared by the teardown; the round clock and the time of day
	// the exit frame's, GameTime reading 18599 / 60 / 62 = 4 whole minutes.
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	consoles.refresh_process_mission();
	expect(consoles.process_mission().tod_fixed24 == 0x0C000100u &&
	               consoles.process_mission().session_elapsed_ms == 0u &&
	               consoles.process_mission().round_ticks_remaining == 18599,
	       "the menus keep the exit frame's round clock and time of day");
	std::vector<std::string> replies;
	(void)consoles.dispatch(AdminSession{"boss", 0xFFFFFFFFu}, "GET GAMESETTINGS", replies);
	expect(replies.size() == 1 && replies[0].find("GameTime             = 4/5\n") != std::string::npos,
	       "the menus' GameTime reads the exit frame's clock");

	// A session whose first frame is its last (a NovaWorld session ended during its load): no
	// pump bound its console, the menus' is still bound; its exit frame binds and reads it.
	auto world2 = std::make_unique<world::World>();
	auto host2 = std::make_unique<inmatch::NapiNPServerCtx>();
	host2->world = world2.get();
	host2->is_authority = 1;
	host2->is_in_session = 1;
	host2->session_title = "Second Session";
	host2->config.mission_file = "SECOND.BMS";
	world2->weather.tod_fixed24 = 0x0E000000u;
	world2->match.set_remaining_ticks(37200);
	host2->np_protocol.host_run_duration_ms = 16u;
	inmatch::RemoteAdminBinding first_frame;
	first_frame.host = host2.get();
	first_frame.world = world2.get();
	first_frame.session = 6;
	consoles.session_exit_frame(first_frame, seams);
	expect(consoles.bound_to(first_frame), "the exiting session's console is bound");
	expect(consoles.process_mission().tod_fixed24 == 0x0E000000u &&
	               consoles.process_mission().round_ticks_remaining == 37200 &&
	               consoles.process_mission().map_file == "SECOND.BMS" &&
	               consoles.process_mission().session_title == "Second Session",
	       "the never-pumped session's words: time of day, round clock, map and title");
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	consoles.refresh_process_mission();
	expect(consoles.process_mission().tod_fixed24 == 0x0E000000u &&
	               consoles.process_mission().round_ticks_remaining == 37200 &&
	               consoles.process_mission().map_file == "SECOND.BMS" &&
	               consoles.process_mission().session_elapsed_ms == 0u,
	       "the menus read that session's words");
}

// A joiner's admin SET writes the globals its own session reads, as retail's SET writes them in
// place: a rule bit lands in g_RulesFlags, the joiner world's copy of the mission data's word,
// with the caches its readers take (the friendly fire, the round spawn's tracers, the claymore
// sweep) and the HUD's session_rules_word; GameTime lands in g_RoundTimeRemaining, the client's
// S2C 0x0A clock. Both hold across the frames; the next mission's data copy replaces the word.
// [orig: CAdminServer_HandleSetCommand @0x405A60 - FriendlyFire @0x405EB4 / @0x405EC3, Tracers
//  @0x405F7A / @0x405F86, TeamTriggerClaymore @0x405F38 / @0x405F47, GameTime @0x405E80..0x405E86;
//  NapiNPClientMsg_HandleMissionDataChunk @0x432410 (the memcpy @0x432470, g_RulesFlags at +44);
//  Projectile_DamagePairEligible @0x4E74F0 (@0x4E74FD); RoundData_SpawnRound @0x4EC0D0
//  (@0x4EC72F / @0x4EC79D); Entity_FindEnemyInCone @0x43CBA0 (@0x43CC2C);
//  HUD_DrawFriendlyTagsPass @0x5A4480]
void test_joiner_set_holds() {
	const std::string client_scrk = "CLIENT-ADMIN-SET-SCRK";
	const std::string server_scrk = "SERVER-ADMIN-SET-SCRK";
	auto kernel = std::make_unique<mission::MissionKernel>();
	kernel->world.registry.configure_pool(0, 16);
	auto role = std::make_unique<inmatch::JoinerRole>();
	role->bind(*kernel);
	CaptureSocket socket;
	role->set_socket(&socket, PeerAddr{0x0100007Fu, 32123});
	role->set_protocol(&kJo);
	role->create_runtime("SetJoiner", inmatch::JoinRole::Player, "", "");
	role->kit_seams.apply_authoritative = [] {};
	role->kit_seams.reseed_on_side_change = [] { return false; };
	role->kit_seams.push = [] {};
	role->poll_preload();
	inmatch::ClientRuntime &client = *role->runtime;
	role->seed_session(0x41444D4Eu, 1u, client_scrk, server_scrk, 1, 0, 0x0005,
	                    world::kPlayerInfantryTypeId, game_type::kTeamDeathmatch, 0x00100000u,
	                    /*replay_mode=*/false);
	// The joined mission's data: the default cfg word 0x3A02 (NoFriendlyFire 0x200, tracers on,
	// no claymore preference), copied at the mission start.
	client.view().set_mp_attributes(0x3A02u);
	(void)role->bring_up();
	world::World &world = kernel->world;
	inmatch::RoleView view;
	view.kernel = kernel.get();
	view.runtime = &client;
	view.joiner = true;
	expect(world.rules.mpattrib == 0x3A02u && world.rules.no_friendly_fire &&
	               !world.round_sim.no_tracers_rule && !world.throwables.team_trigger_claymore &&
	               inmatch::session_rules_word(view) == 0x3A02u,
	       "the mission start's copy of the rules word");

	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	inmatch::RemoteAdminBinding joined;
	joined.client = &client;
	joined.joiner = role.get();
	joined.world = &world;
	joined.session = 9;
	consoles.bind(joined, seams);
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	const AdminSession admin{"boss", 0xFFFFFFFFu};
	const auto line = [&](const std::string &text) {
		std::vector<std::string> replies;
		(void)consoles.dispatch(admin, text, replies);
		return replies.size() == 1 ? replies[0] : std::string("<" + std::to_string(replies.size()) + ">");
	};
	expect_eq(line("SET FriendlyFire 1"), "OK - Setting Changed.", "SET FriendlyFire on a joiner");
	expect_eq(line("SET Tracers 0"), "OK - Setting Changed.", "SET Tracers on a joiner");
	expect_eq(line("SET TeamTriggerClaymore 1"), "OK - Setting Changed.", "SET TeamTriggerClaymore on a joiner");
	expect_eq(line("SET GameTime 7"), "OK - Setting Changed.", "SET GameTime on a joiner");
	// 0x3A02 & ~0x200 | 0x1 | 0x8000 = 0xB803; 7 x 3720 = 26040 ticks.
	const auto set_words_hold = [&](const char *what) {
		expect(world.rules.mpattrib == 0xB803u && !world.rules.no_friendly_fire &&
		               world.round_sim.no_tracers_rule && world.throwables.team_trigger_claymore &&
		               inmatch::session_rules_word(view) == 0xB803u &&
		               client.state().round_time_remaining_ticks == 26040,
		       what);
	};
	set_words_hold("the SET words reach the joiner's world, its HUD word and its round clock");
	inmatch::TickInput input;
	role->run_tick(input);
	role->run_tick(input);
	consoles.refresh_process_mission();
	set_words_hold("they hold across the joiner's frames");
	const std::string settings = line("GET GAMESETTINGS");
	expect(settings.find("GameTime             = 7/7\n") != std::string::npos &&
	               settings.find("FriendlyFire         = 1\n") != std::string::npos &&
	               settings.find("Tracers              = 0\n") != std::string::npos &&
	               settings.find("TeamTriggerClaymore  = 1\n") != std::string::npos,
	       "GET reads the held words");

	// A joiner never counts its round clock down (the main frame's decrement is the authority's),
	// so the SET's minutes hold until the host's next phase-1 0x0A timer block folds its own clock
	// back in: 28 minutes = 1680 s = 104160 ticks reads 28/7. Retail's host sends that block on
	// every fourth 0x0A (x5's TDM cells: about 766 ms apart), so a GET 0.6 s after the SET reads
	// either value on either joiner (x5: retail 5/5 once and 28/5 once, ours 28/5 twice).
	// [orig: Game_ProcessMainFrame @0x5265DA (`cmp is_authority`), the decrement @0x526602;
	//  CAdminServer_HandleSetCommand @0x405E80..0x405E86; NapiNPClientMsg_0x00A @0x430174 (off the
	//  authority), the phase-1 fold @0x43021E..0x430235]
	FrameUpdate timer;
	timer.carried_handle = 0xFFFF;
	timer.local_tail_present = true;
	timer.flags2 = 1; // the encoder keys the sub-block on flags2
	timer.sub_block = 1;
	timer.timer.present = true;
	timer.timer.timer_seconds = 1680;
	client.view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(timer));
	expect(client.state().round_time_remaining_ticks == 104160, "the phase-1 fold replaces the SET's clock");
	expect(line("GET GAMESETTINGS").find("GameTime             = 28/7\n") != std::string::npos,
	       "GET reads the folded clock over the SET's limit");

	// The next mission's data (a map change's S2C 0x64, word 0x3A06, which the connection folds
	// into the view) is copied again at that mission's start and replaces the word and its caches;
	// the round clock stays the 0x0A's.
	client.view().set_mp_attributes(0x3A06u);
	(void)role->bring_up();
	expect(world.rules.mpattrib == 0x3A06u && world.rules.no_friendly_fire &&
	               !world.round_sim.no_tracers_rule && !world.throwables.team_trigger_claymore &&
	               inmatch::session_rules_word(view) == 0x3A06u,
	       "the next mission data's copy replaces the SET word");
	consoles.refresh_process_mission();
	expect(consoles.process_mission().rules_word == 0x3A06u, "the admin reads the new copy");
}

// D-NET-1024: the host's wall-clock readers take the process clock, GetTickCount's, so a
// load and a held start count. The start stamps it (its +0x53C) and the round init (its
// g_RoundStateInitTick) at the create; 185000 ms of clock later, of which the host ran 35000,
// the status report's UP-TIME is 0 00:03:05 and the 0x81's UT 185000.
// [orig: NapiNPProtocol_StartServer @0x62B5E0 - GetTickCount @0x62B612, the store @0x62B619;
//  Server_InitNewRoundState - GetTickCount @0x51CB93, the store @0x51CBA0;
//  Server_BuildStatusReport - GetTickCount @0x530B84, `sub eax, g_RoundStateInitTick`
//  @0x530B86; NapiNPProtocol_SendServerInfoPacket - GetTickCount @0x620657, `sub eax,
//  [esi+53Ch]` @0x62065C]
void test_host_uptime_reads_the_process_clock() {
	auto world = std::make_unique<world::World>();
	auto owner = std::make_unique<inmatch::HostOwner>();
	owner->ctx.world = world.get();
	uint32_t clock = 7000;
	inmatch::HostConfig cfg;
	cfg.protocol = &kJo;
	cfg.config.server_name = "Clocked";
	cfg.config.max_players = 8;
	cfg.socket_mode = inmatch::SocketMode::Socketless;
	cfg.serve_and_play = false;
	cfg.clock_ms = [&clock] { return clock; };
	(void)inmatch::start_host_session(*owner, cfg);
	// The session onto its World (the create reads none): its rules, the own add.
	inmatch::bind_host_session_world(*owner, cfg);
	inmatch::NapiNPServerCtx &ctx = owner->ctx;
	expect(ctx.np_protocol.host_start_tick == 7000u && ctx.round_state_init_tick == 7000u,
	       "the start and the round init stamp the process clock at the create");
	clock += 185000u;
	ctx.np_protocol.host_run_duration_ms = 35000u; // the held start ran no tick
	expect(inmatch::session_uptime_ms(ctx) == 185000u, "UP-TIME counts the clock, not the run");

	inmatch::RemoteAdminConsoles consoles;
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	inmatch::RemoteAdminBinding hosted;
	hosted.host = &ctx;
	hosted.session = 1;
	consoles.bind(hosted, seams);
	expect(consoles.status_report().find("UP-TIME: 0 00:03:05\r\n") != std::string::npos,
	       "the status report's UP-TIME reads the process clock");
	const ServerHello hello =
			inmatch::host_server_hello(ctx, make_title_client_hello(kJo, 1), PeerAddr{0x0100007Fu, 32110});
	expect(hello.ut == 185000u, "the 0x81's UT reads the process clock less the start's stamp");
}

} // namespace

int main() {
	test_port_and_log_text();
	test_consoles();
	test_idle_scene();
	test_joiner_console();
	test_session_destroy_routes();
	test_menus_game_text();
	test_joiner_cycle();
	test_joiner_cycle_zones();
	test_authority_round_end_view();
	test_listen_host_and_single_player();
	test_single_player_cycle();
	test_idle_frame_exit();
	test_exit_frame_words();
	test_joiner_set_holds();
	test_host_uptime_reads_the_process_clock();
	if (g_failures != 0) {
		std::fprintf(stderr, "remote_admin: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("remote_admin: ok\n");
	return 0;
}
