// opennova-serve's remote-admin server end to end (ADR 0051 PR5b), with no retail data. The
// server runs in-process on a synthetic two-map rotation (SERVETST, SERVETS2, SERVETST) in a
// temp working directory holding a game.cfg with remote_admin_port set, an admin.cfg with one
// user and a banned.txt; a LAN joiner (a ClientRuntime on a real UDP socket) plays in it. Over
// real loopback TCP the engine's admin client half (net/admin/admin_protocol.h) logs in and:
//   - PLAYER LIST lists the joiner;
//   - MISSION LIST, then MISSION ADD with ONESHOT at a position, SETNEXT onto it and CYCLE: the
//     map changes to the SETNEXT entry, and the next CYCLE removes the one-shot after it played
//     and plays the entry that slid into its slot;
//   - SET ServerName changes the session's name and re-saves ./game.cfg; SET ServerMessage is
//     not a SET key (retail's usage and `Setting Not Found`, no save);
//   - CHAT SEND reaches the joiner as S2C 0x14 [10][255], and CHAT GET lists the host's ring;
//   - PLAYER BAN on the LAN joiner: no PCID, so the error and no punt;
//   - opennova-nw-lister's admin feed polls the live server beside it;
//   - QUIT closes the connection.
// Then the files: admin_log.txt's lines, banned.txt loaded at the session start and saved at
// the exit when dirty. Last, on a fresh server, GOTO MENUSTATE quits the session.
// [orig: Game_InitSubsystems @0x4A72B8..0x4A72D9; CAdminServer_ProcessFrame @0x406F50;
//  CAdminServer_HandleMissionCommand @0x4062E0; MissionList_InsertEntryAtIndex @0x501AD0;
//  MissionList_GetCurrentEntry @0x4FC540; CAdminServer_HandleSetCommand @0x405A60;
//  CAdminServer_HandleChatCommand @0x404AC0; CAdminServer_HandlePlayer @0x403EF0]
#include "server.h"
#include "serve_test_support.h"

#include "admin_feed.h"

#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_protocol.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

void check_eq(const std::string &got, const std::string &want, const char *what) {
	if (got == want) return;
	std::printf("FAIL %s\n  got:  [%s]\n  want: [%s]\n", what, got.c_str(), want.c_str());
	++failures;
}

constexpr double kFrame = 1.0 / 62.5;

// The server and its LAN joiner, stepped on a thread of their own so the test's admin client
// can block on its socket while the server pumps it; the test reads their state under the lock.
struct Rig {
	serve::Server &server;
	net::Endpoint server_ep;
	net::ScopedSocket joiner_sock;
	inmatch::ClientRuntime client{"ServeJoiner"};
	uint32_t tick = 1;
	bool ended = false;
	std::vector<replication::ClientChatLine> chat;
	std::mutex mutex;
	std::atomic<bool> stop{false};
	std::thread thread;

	Rig(serve::Server &s, uint16_t joiner_port) : server(s) {
		server_ep = net::Endpoint{{127, 0, 0, 1}, server.bound_port()};
		joiner_sock = net::ScopedSocket(net::udp_bind(joiner_port));
	}
	void ship(const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), server_ep, d.data(), d.size());
	}
	// One server frame and one joiner frame; the joiner reloads in place at a map change
	// (its stored exit 4 is the Game Loop, inmatch::main_frame_exit).
	void step() {
		if (!ended && !server.frame(kFrame)) ended = true;
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, 0);
			if (n <= 0) break;
			client.receive(rx, static_cast<size_t>(n));
		}
		if (client.mission_exit_reason() != 0) {
			(void)client.begin_mission_reload();
			return;
		}
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
		for (replication::ClientChatLine &line : client.view().drain_chat_lines()) chat.push_back(line);
	}
	void start() {
		ship(client.start());
		// A few frames per lock hold: a sleep is a scheduler quantum on Windows, and the
		// admin CYCLE's 620-tick linger runs at the frame rate.
		thread = std::thread([this] {
			while (!stop) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					for (int i = 0; i < 8; ++i) step();
				}
				std::this_thread::sleep_for(1ms);
			}
		});
	}
	void join() {
		stop = true;
		if (thread.joinable()) thread.join();
	}
	template <typename F>
	auto with(F f) {
		std::lock_guard<std::mutex> lock(mutex);
		return f();
	}
	// Polls `pred` under the lock until it holds or `seconds` pass.
	bool wait(const std::function<bool()> &pred, int seconds) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (std::chrono::steady_clock::now() < deadline) {
			if (with(pred)) return true;
			std::this_thread::sleep_for(5ms);
		}
		return with(pred);
	}
	inmatch::NapiNPServerCtx &ctx() { return server.role().state.host_owner.ctx; }
};

// The admin client half over a blocking loopback socket.
struct AdminClient {
	net::ScopedSocket socket;

	bool connect(uint16_t port) {
		socket = net::ScopedSocket(net::tcp_connect(net::Endpoint{{127, 0, 0, 1}, port}, 10000));
		return socket.is_valid();
	}
	bool recv(std::string &text) {
		uint8_t header[ADMIN_PACKET_HEADER_BYTES];
		size_t size = 0;
		if (!net::tcp_recv_exact(socket.get(), header, sizeof(header)) || !admin_decode_header(header, size))
			return false;
		std::vector<uint8_t> payload(size);
		if (size != 0 && !net::tcp_recv_exact(socket.get(), payload.data(), size)) return false;
		return admin_reply_text(payload, text);
	}
	bool send(const std::vector<uint8_t> &payload) {
		const std::vector<uint8_t> packet = admin_encode_packet(payload.data(), payload.size());
		return net::tcp_send_all(socket.get(), packet.data(), packet.size());
	}
	bool login(const std::string &user, const std::string &pass) {
		uint8_t header[ADMIN_PACKET_HEADER_BYTES];
		size_t size = 0;
		if (!net::tcp_recv_exact(socket.get(), header, sizeof(header)) || !admin_decode_header(header, size))
			return false;
		std::vector<uint8_t> challenge(size);
		if (!net::tcp_recv_exact(socket.get(), challenge.data(), size) || !admin_challenge_valid(challenge))
			return false;
		const auto login = admin_encode_login(challenge, user, pass);
		if (!send(std::vector<uint8_t>(login.begin(), login.end()))) return false;
		std::string reply;
		return recv(reply) && admin_login_accepted(reply);
	}
	// One command and its `count` replies.
	std::vector<std::string> command(const std::string &line, size_t count = 1) {
		std::vector<std::string> replies;
		if (!send(admin_encode_command(line))) return replies;
		for (size_t i = 0; i < count; ++i) {
			std::string text;
			if (!recv(text)) break;
			replies.push_back(text);
		}
		return replies;
	}
	std::string one(const std::string &line) {
		const std::vector<std::string> replies = command(line, 1);
		return replies.empty() ? std::string("<no reply>") : replies[0];
	}
};

std::string rotation_line(int index, const char *file, const char *marks) {
	return std::to_string(index) + ": " + file + " - " + marks + "\n";
}

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);
	const fs::path dir = serve_test::fresh_dir("admin_root");
	const fs::path work = serve_test::fresh_dir("admin_cwd");
	serve_test::ScopedCwd cwd(work);
	CHECK(serve_test::write_bytes(dir / "SERVETST.BMS", serve_test::deathmatch_mission()));
	CHECK(serve_test::write_bytes(dir / "SERVETS2.BMS", serve_test::deathmatch_mission("Serve Test Map Two")));
	// The rotation [SERVETST, SERVETS2, SERVETST]: the last line names the starting map, its
	// first entry; REPLAY off.
	CHECK(serve_test::write_text(dir / "admin.host",
			"GameName \"Admin Test\"\r\n"
			"ServerMessage \"the host file's message\"\r\n"
			"MaxPlayers 8\r\n"
			"Replay 0\r\n"
			"Mission SERVETST.BMS\r\n"
			"Mission SERVETS2.BMS\r\n"
			"Mission SERVETST.BMS\r\n"));
	// The working directory: admin.cfg with one user (GET, SET, MISSION, PLAYER and the 0x40 group:
	// 0x4F) and a whitelist that admits loopback; a banned.txt in the writer's own format.
	CHECK(serve_test::write_text(work / "admin.cfg", "boss pw 4F\r\nip_restrict = 127.0.0.1\r\n"));
	CHECK(serve_test::write_text(work / "banned.txt", "            10.1.2.3   \"Mallory\"\n"));
	// A stale admin_log.txt: the launch truncates it.
	CHECK(serve_test::write_text(work / "admin_log.txt", "a previous run's line\r\n"));

	// A server on free ports, game.cfg written with its admin port first.
	std::string error;
	const auto start_server = [&](uint16_t &admin_port) {
		std::unique_ptr<serve::Server> started;
		for (int attempt = 0; attempt < 5 && !started; ++attempt) {
			gamecfg::GameCfg cfg = gamecfg::defaults(gamecfg::DefaultTexts{});
			const uint16_t udp = serve_test::free_udp_port();
			admin_port = serve_test::free_tcp_port();
			cfg.mp_lan_server_port_min = udp;
			cfg.mp_lan_server_port_max = udp;
			cfg.remote_admin_port = admin_port;
			CHECK(gamecfg::save_file(gamecfg::kFileName, cfg, {}, error));
			serve::ServeOptions options;
			CHECK(serve::parse_serve_options({"--resource-dir", dir.string(), "/HOST",
							  (dir / "admin.host").string(), "--loose-root"},
					options, error) == 0);
			auto server = std::make_unique<serve::Server>(options);
			if (server->start(error) && server->admin_port() == admin_port) {
				started = std::move(server);
			} else if (error.find("bind scan") == std::string::npos && server->running()) {
				std::printf("the admin listener took no port %u\n", admin_port);
			}
		}
		return started;
	};
	uint16_t admin_port = 0;
	std::unique_ptr<serve::Server> holder = start_server(admin_port);
	if (!holder) {
		std::printf("start: %s\n", error.c_str());
		return 1;
	}
	serve::Server &server = *holder;
	CHECK(server.admin_port() == admin_port);
	CHECK(serve_test::read_text(work / "admin_log.txt").empty());
	// banned.txt loaded at the session's round init.
	{
		const banlist::AddressBanList &bans = server.role().state.host_owner.ctx.bans.addresses;
		CHECK(bans.entries.size() == 1 && bans.entries[0].name == "Mallory" &&
				bans.entries[0].address == (10u | (1u << 8) | (2u << 16) | (3u << 24)));
	}

	Rig rig(server, 0);
	CHECK(rig.joiner_sock.is_valid());
	rig.start();
	CHECK(rig.wait([&] { return rig.client.in_match() && rig.client.is_deployed(); }, 60));

	AdminClient admin;
	CHECK(admin.connect(admin_port));
	CHECK(admin.login("boss", "pw"));

	// --- PLAYER LIST: the LAN joiner in slot 1 (no host row: D-NET-352).
	{
		const std::string list = admin.one("PLAYER LIST");
		const std::vector<AdminPlayer> players = admin_parse_player_list(list);
		CHECK(list.rfind("NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n", 0) == 0);
		CHECK(players.size() == 1 && players[0].name == "ServeJoiner" && players[0].slot == 1);
	}

	// --- the rotation: LIST, ADD ONESHOT at 2, SETNEXT 2, CYCLE onto it, CYCLE past it.
	check_eq(admin.one("MISSION LIST"),
			rotation_line(0, "SERVETST.BMS", "() () () <CURRENT MISSION> <>") +
					rotation_line(1, "SERVETS2.BMS", "() () () <> <>") +
					rotation_line(2, "SERVETST.BMS", "() () () <CURRENT MISSION> <>"),
			"MISSION LIST: every entry of the current row marked");
	{
		std::string available;
		for (size_t i = 0; i < server.catalog().size(); ++i)
			available += std::to_string(i) + ". " + server.catalog()[i].file + " (" + server.catalog()[i].title + ")\n";
		CHECK(server.catalog().size() == 2);
		check_eq(admin.one("MISSION AVAILABLE"), available, "MISSION AVAILABLE: the catalog");
	}
	check_eq(admin.one("MISSION ADD servets2.bms 0 2 ONESHOT"), "OK - Entry Added\n", "MISSION ADD ... ONESHOT");
	check_eq(admin.one("MISSION SETNEXT 2"), "OK - Next Mission Set.", "MISSION SETNEXT");
	check_eq(admin.one("MISSION LIST"),
			rotation_line(0, "SERVETST.BMS", "() () () <CURRENT MISSION> <>") +
					rotation_line(1, "SERVETS2.BMS", "() () () <> <>") +
					rotation_line(2, "SERVETS2.BMS", "() () (ONE_SHOT) <> <NEXT MISSION>") +
					rotation_line(3, "SERVETST.BMS", "() () () <CURRENT MISSION> <>"),
			"the inserted one-shot and the SETNEXT mark");
	CHECK(rig.with([&] { return server.rotation().setnext_latch && server.rotation().list.alt_cursor == 2; }));
	check_eq(admin.one("MISSION CYCLE"), "OK - Server is cycling...", "MISSION CYCLE");
	// The joiner is back once it plays on the next map's file (its reload starts at the S2C 0x25).
	CHECK(rig.wait([&] {
		return server.missions_played() == 2 && rig.client.map_file() == "SERVETS2.BMS" &&
				rig.client.in_match() && rig.client.is_deployed();
	}, 60));
	rig.with([&] {
		// The SETNEXT entry: the one-shot SERVETS2 at index 2. The DM map has no launch option,
		// so the latch waits for a launch-option map's teardown (net-re §5.70.5).
		CHECK(server.rotation().list.cursor == 2 && server.rotation().list.count == 4);
		CHECK(rig.ctx().config.mission_file == "SERVETS2.BMS");
		CHECK(rig.client.map_file() == "SERVETS2.BMS");
		CHECK(server.rotation().setnext_latch);
		return 0;
	});
	check_eq(admin.one("MISSION CYCLE"), "OK - Server is cycling...", "the second CYCLE");
	CHECK(rig.wait([&] {
		return server.missions_played() == 3 && rig.client.map_file() == "SERVETST.BMS" &&
				rig.client.in_match() && rig.client.is_deployed();
	}, 60));
	rig.with([&] {
		// The one-shot removed after it played; the entry that slid into its slot plays.
		CHECK(server.rotation().list.count == 3 && server.rotation().list.cursor == 2);
		CHECK(rig.ctx().config.mission_file == "SERVETST.BMS");
		CHECK(rig.client.map_file() == "SERVETST.BMS");
		return 0;
	});
	check_eq(admin.one("MISSION LIST"),
			rotation_line(0, "SERVETST.BMS", "() () () <CURRENT MISSION> <>") +
					rotation_line(1, "SERVETS2.BMS", "() () () <> <>") +
					rotation_line(2, "SERVETST.BMS", "() () () <CURRENT MISSION> <>"),
			"the one-shot gone");
	check_eq(admin.one("MISSION REMOVE 1"), "OK - Mission Removed.", "MISSION REMOVE");
	CHECK(rig.with([&] { return server.rotation().list.count == 2 && server.rotation().list.cursor == 1; }));

	// --- SET: ServerName is a key (the cfg block, the protocol's session name, game.cfg saved);
	// ServerMessage is not (the usage, then the error, and no save).
	check_eq(admin.one("SET ServerName Admin Renamed"), "OK - Setting Changed.", "SET ServerName");
	CHECK(rig.with([&] { return rig.ctx().np_protocol.session_name == "Admin Renamed "; }));
	{
		const gamecfg::LoadResult saved = gamecfg::load_file(gamecfg::kFileName, {});
		CHECK(saved.file_read && saved.cfg.game_name == "Admin Renamed ");
		CHECK(saved.cfg.servermsg == "the host file's message");
	}
	const std::string cfg_before = serve_test::read_text(work / "game.cfg");
	const std::vector<std::string> message = admin.command("SET ServerMessage hello", 2);
	CHECK(message.size() == 2 && message[0].rfind("USAGE -  SET [AutoBalanceOnRecycle", 0) == 0 &&
			message[1] == "ERROR - Setting Not Found.");
	CHECK(rig.with([&] { return rig.ctx().config.custom_text == "the host file's message"; }));
	CHECK(serve_test::read_text(work / "game.cfg") == cfg_before);

	// --- CHAT SEND: S2C 0x14 [10][255] to the joiner and the host's ring; CHAT GET lists it.
	check_eq(admin.one("CHAT SEND hello <b>joiner"), "OK - Chat sent.", "CHAT SEND");
	const bool chat_reached = rig.wait([&] {
		for (const replication::ClientChatLine &line : rig.chat)
			if (line.channel == 10 && line.sender_slot == 0xFF && line.text == "hello joiner ") return true;
		return false;
	}, 30);
	CHECK(chat_reached);
	rig.with([&] {
		if (!chat_reached)
			for (const replication::ClientChatLine &line : rig.chat)
				std::printf("joiner chat: [%d][%u] '%s'\n", line.channel, line.sender_slot, line.text.c_str());
		CHECK(!rig.ctx().console_chat.empty() && rig.ctx().console_chat.back().text == "hello <b>joiner ");
		return 0;
	});
	check_eq(admin.one("CHAT GET"), "hello <b>joiner \r\n", "CHAT GET: the host's CHAT ring");

	// --- PLAYER BAN on a LAN host: the joiner has no PCID, so the error and no punt.
	check_eq(admin.one("PLAYER BAN 1"), "ERROR - Player #1 \"ServeJoiner\" doesn't have a PCID.",
			"PLAYER BAN on a LAN joiner");
	std::this_thread::sleep_for(300ms);
	CHECK(rig.with([&] { return !rig.client.session_lost() && rig.client.in_match(); }));

	// --- opennova-nw-lister's admin feed against the live server, beside this connection.
	{
		nw_lister::AdminFeed feed;
		feed.start(net::Endpoint{{127, 0, 0, 1}, admin_port}, "boss", "pw");
		nw_lister::AdminSnapshot snapshot;
		for (int i = 0; i < 1000 && snapshot.seq == 0; ++i) {
			std::this_thread::sleep_for(10ms);
			snapshot = feed.snapshot();
		}
		CHECK(snapshot.ok);
		CHECK(snapshot.players.size() == 1 && snapshot.players[0].name == "ServeJoiner");
		CHECK(snapshot.mission == "SERVETST");
		feed.stop();
	}

	// --- QUIT: the server closes the connection with no reply.
	CHECK(admin.send(admin_encode_command("QUIT")));
	uint8_t byte = 0;
	CHECK(net::tcp_recv(admin.socket.get(), &byte, 1) == 0);

	// --- the files: admin_log.txt's lines (CR LF), banned.txt saved at the exit when dirty.
	rig.join();
	{
		const std::string log = serve_test::read_text(work / "admin_log.txt");
		CHECK(log.find("New connection accepted (0100007f)\r\n") != std::string::npos);
		CHECK(log.find("User logged in (boss/pw)\r\n") != std::string::npos);
		CHECK(log.find("User command (boss) - MISSION SETNEXT 2\r\n") != std::string::npos);
		CHECK(log.find("a previous run's line") == std::string::npos);
	}
	{
		inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
		ctx.bans.addresses.entries.push_back(banlist::AddressBan{4u | (3u << 8) | (2u << 16) | (1u << 24), "Trent"});
		ctx.bans.addresses_dirty = true;
	}
	server.stop();
	{
		const std::string banned = serve_test::read_text(work / "banned.txt");
		CHECK(banned.find("10.1.2.3   \"Mallory\"") != std::string::npos);
		CHECK(banned.find("4.3.2.1   \"Trent\"") != std::string::npos);
	}
	CHECK(server.admin_port() == 0);

	// --- GOTO MENUSTATE on this Serve Only host, on a fresh server: input action 3 passes its
	// binding gate (record 3 is the exit row, which carries no head-gate bit), so the cycle tail
	// sends its one reply and the next frame's exit reason 1 takes the router's teardown: the
	// session is destroyed and the joiner dropped, as a retail dedicated server leaves its match
	// for its main menu (this one has no menu, so its run ends).
	// [orig: CAdminServer_HandleGotoCommand @0x404A5D; Input_HandleActionBinding case 3
	//  @0x49AF26; PostMenu_RouteMissionExit @0x5684AB -> CNapiGameSession_FullDestroy @0x568683]
	{
		uint16_t quit_port = 0;
		std::unique_ptr<serve::Server> quitter = start_server(quit_port);
		CHECK(quitter != nullptr);
		if (quitter) {
			Rig quit_rig(*quitter, 0);
			CHECK(quit_rig.joiner_sock.is_valid());
			quit_rig.start();
			CHECK(quit_rig.wait([&] { return quit_rig.client.in_match(); }, 60));
			AdminClient quit_admin;
			CHECK(quit_admin.connect(quit_port));
			CHECK(quit_admin.login("boss", "pw"));
			check_eq(quit_admin.one("GOTO MENUSTATE"), "OK - Server is cycling...",
					"GOTO MENUSTATE: the cycle tail's one reply");
			CHECK(quit_rig.wait([&] { return quit_rig.ended; }, 30));
			quit_rig.with([&] {
				CHECK(quitter->quit() && !quitter->rotation_ended() && !quitter->running());
				CHECK(quitter->end_message().find("GOTO MENUSTATE") != std::string::npos);
				CHECK(quitter->missions_played() == 1);
				return 0;
			});
			CHECK(quit_rig.wait([&] { return quit_rig.client.session_lost(); }, 30));
			quit_rig.join();
			CHECK(quitter->admin_port() == 0);
		}
	}

	net::shutdown();
	cwd.restore();
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::remove_all(work, ec);
	if (failures != 0) {
		std::printf("opennova_serve_admin: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_admin: ok\n");
	return 0;
}
