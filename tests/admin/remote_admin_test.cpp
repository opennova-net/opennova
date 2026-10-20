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

#include <base/gameprofile/gameprofile.h>
#include <base/io/crt_rand.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/mission_exit.h>
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
	expect_eq(c.one("SET ServerName Renamed"), "OK - Setting Changed.", "a SET in the menus");
	// Each value token followed by one space [orig: CAdminServer_HandleSetCommand @0x4060BF..0x40615A].
	expect(block.game_name == "Renamed " && saves == 1, "writes the block and saves it");
	expect_eq(c.one("GOTO MENUSTATE"), "<2 replies>", "GOTO MENUSTATE in the menus: the error and the cycle tail");

	inmatch::NapiNPServerCtx host;
	host.config.server_name = "Hosted";
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
// (the client's own console sender), CHAT GET reads its HUD window, and GOTO MENUSTATE stores
// exit reason 1 on its client.
// [orig: Chat_SendTeamMessage @0x49A900 - the peer test @0x49A979, QueueReliableMessage(0xD, 1,
//  310) @0x49A9B4; CAdminServer_HandleCmdCommand @0x404997 -> Client_ProcessConsoleCommand
//  @0x520E80 (QueueReliableMessage(4, 1, 0) @0x520f29); CGameSession_SetConnectionMode
//  @0x4C49F0 (mode 2); Input_HandleActionBinding case 3 @0x49AF26]
void test_joiner_console() {
	const std::string client_scrk = "CLIENT-ADMIN-JOINER-SCRK";
	const std::string server_scrk = "SERVER-ADMIN-JOINER-SCRK";
	uint64_t now = 7000;
	inmatch::ClientRuntime client("AdminJoiner", [&now] { return now; });
	client.set_title(&kJo);
	client.seed_session(0x41444D4Eu, 1u, client_scrk, server_scrk, 1, 0, 0x0002,
	                    world::kPlayerInfantryTypeId, 0, 0x00100000u, /*replay_mode=*/false);
	(void)joiner_sends(client, client_scrk, 1);

	inmatch::RemoteAdminConsoles consoles;
	io::CrtRand rand;
	rand.seed(11);
	AdminServer server(users(), consoles, rand);
	Client c{server};
	expect(c.connect_and_login("boss", "pw"), "the joiner's admin login");
	gamecfg::GameCfg block = gamecfg::defaults();
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	std::vector<std::string> echoes;
	seams.chat_echo = [&](const std::string &text) { echoes.push_back(text); };
	seams.chat_window = [] { return std::vector<std::string>{"Ann: hi", "", "Bob: yo"}; };
	inmatch::RemoteAdminBinding joined;
	joined.client = &client;
	joined.session = 7;
	consoles.bind(joined, seams);
	expect(consoles.bound_to(joined), "the joiner's console");
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);

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
	expect(client.mission_exit_reason() == inmatch::kMissionExitNone, "no exit yet");
	(void)c.command("GOTO MENUSTATE");
	expect(client.mission_exit_reason() == inmatch::kMissionExitQuit, "GOTO MENUSTATE: the joiner's exit reason 1");
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

} // namespace

int main() {
	test_port_and_log_text();
	test_consoles();
	test_idle_scene();
	test_joiner_console();
	test_listen_host_and_single_player();
	if (g_failures != 0) {
		std::fprintf(stderr, "remote_admin: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("remote_admin: ok\n");
	return 0;
}
