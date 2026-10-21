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
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h> // client_goodbye_to_bytes, make_disconnect_event
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/remote_admin.h>
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
}

static const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

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
// [orig: Chat_SendTeamMessage @0x49A900 - the peer test @0x49A979, QueueReliableMessage(0xD, 1,
//  310) @0x49A9B4; CAdminServer_HandleCmdCommand @0x404997 -> Client_ProcessConsoleCommand
//  @0x520E80 (QueueReliableMessage(4, 1, 0) @0x520f29); CGameSession_SetConnectionMode
//  @0x4C49F0 (mode 2); Input_HandleActionBinding case 3 @0x49AF26, @0x49AF38..0x49AF42 ->
//  CNapiNetwork_DisconnectActiveConnection @0x4C9140 (role 2 for a client @0x4C91CF..0x4C91E8,
//  DC 2 @0x4C91EF, DP1 / DP2 0 @0x4C91F7 / @0x4C91FB, the tag @0x4C91FF, DPC 0 @0x4C9213, DDSTR
//  "" @0x4C921A) -> CNapiNPConnection_TeardownActiveConnection @0x6253C0 (the burst
//  @0x625405..0x625424, recv_max_per_tick 4 for the JO template); CNapiGameSession_FullDestroy
//  @0x4C977A (g_GameType = 0); GameType_GetAbbreviation @0x5211B2 (STROVER29)]
void test_joiner_console() {
	const std::string client_scrk = "CLIENT-ADMIN-JOINER-SCRK";
	const std::string server_scrk = "SERVER-ADMIN-JOINER-SCRK";
	uint64_t now = 7000;
	auto joiner = std::make_unique<inmatch::JoinerRole>();
	joiner->runtime = std::make_unique<inmatch::ClientRuntime>("AdminJoiner", [&now] { return now; });
	inmatch::ClientRuntime &client = *joiner->runtime;
	CaptureSocket socket;
	joiner->set_socket(&socket, PeerAddr{0x0100007Fu, 32123});
	client.set_title(&kJo);
	client.seed_session(0x41444D4Eu, 1u, client_scrk, server_scrk, 1, 0, 0x0002,
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
	(void)c.command("GOTO MENUSTATE");
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

	// Back in the menus: the destroyed session zeroed g_GameType, the map file stays.
	consoles.bind(inmatch::RemoteAdminBinding{}, seams);
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	const std::string report = consoles.status_report();
	expect(report.find("CURRENT MAP: TDM01.BMS\r\nCURRENT GAME TYPE: Game Type Zero\r\n") != std::string::npos,
	       "back in the menus: the last map file and game type 0's abbreviation");
	// The teardown cleared g_SessionStatus; the time of day keeps the last mission's clock.
	// [orig: Game_TeardownMission @0x5223FA]
	expect(report.find("UP-TIME: 0 00:00:00\r\nCURRENT TOD: 3840\r\n") != std::string::npos,
	       "back in the menus: no session clock, the last time of day");
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

// A paused single player's frame is the Game Loop's under its in-game menu: its exit router
// reads the stored reason ahead of the pump, so a reason stored by the last pump leaves the
// mission on the next frame, which pumps nothing; with no reason it pumps. No other idle frame
// runs the router. [orig: Game_ProcessMainFrame @0x5267F8, @0x52685E..0x526867, ahead of
//  CAdminServer_ProcessFrame @0x5268F1]
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

} // namespace

int main() {
	test_port_and_log_text();
	test_consoles();
	test_idle_scene();
	test_joiner_console();
	test_listen_host_and_single_player();
	test_idle_frame_exit();
	if (g_failures != 0) {
		std::fprintf(stderr, "remote_admin: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("remote_admin: ok\n");
	return 0;
}
