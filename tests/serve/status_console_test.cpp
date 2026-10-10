// opennova-serve's status console and the client-less host's console (ADR 0051
// PR7), with no retail data:
//   - the page rendered as text over a synthetic feed (a text snapshot), and the
//     print rule (the page's 200 ms throttle; a reprint only when its rows change);
//   - the client-less host's CHAT ring: a player's fanned line posts into it by
//     the handler's channel (13 as 12), and a console line typed into the chat
//     input goes out as S2C 0x14 [10][255][text] to every in-match slot and into
//     the ring; the flood table's echo; the talk debounce;
//   - the /INOUT lines;
//   - the log switches in retail's spellings, and the working-directory devices
//     writing the punt START line and a .sph the decoder reads;
//   - a whole synthetic session over a two-map rotation: the server boots the
//     first map with /PROFILE and /PUNTLOG in a working directory of its own, a
//     LAN joiner joins and deploys, the status console prints the page with the
//     joiner on it, a round end changes the map and the joiner reloads, then the
//     server stops. Each map wrote its own numbered .sph (the teardown closes
//     the log, every mission start opens the next file), and each decodes back
//     with its end chunk: its map, the joiner's roster row and eighth-tick
//     samples, and no disconnect marker (the log closes ahead of the slot
//     disconnects, as retail's teardown closes it).
// [orig: Server_DrawStatusScreen @0x50a2d0; HUD_DrawServerConsoleLines
//  @0x5ba0a0; Chat_SendTeamMessage @0x49a900; NapiNPServer_HandleChatMessage
//  @0x513760; CNapiNPConnection_LogHostStarted @0x61e6a0]
#include "server.h"
#include "server_logs.h"
#include "serve_test_support.h"
#include "status_console.h"

#include <base/io/strutil.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/server_admin_command.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/serverlog_decode.h>
#include <runtime/hud/feed_format.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_chat.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/world/world.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

hud::ServerStatusSlot slot(const char *name, uint8_t team, int32_t cls, int32_t idle) {
	hud::ServerStatusSlot s;
	s.active = true;
	s.in_game = true;
	s.team = team;
	s.class_word = cls;
	s.idle_seconds = idle;
	s.name = name;
	return s;
}

// A dedicated host's page: the slot table with the host's own slot 0 skipped
// (the grid shows slots 1..4), a Team Deathmatch round, two console lines.
hud::ServerStatusPageState synthetic_page() {
	hud::ServerStatusPageState page;
	page.mp_session_peer = false;
	page.capacity = 5;
	page.slot_limit = 5;
	page.slots.resize(5);
	page.slots[1] = slot("Alice", 1, 5, 0);
	page.slots[2] = slot("Bob", 2, 8, 7);
	page.slots[4] = slot("Cy", 1, 6, 0);
	page.slots[4].in_game = false;
	page.slots[4].loading = true;
	page.server_name = "Serve Test";
	page.game_type = 0x10000u;
	page.round_wins_team1 = 2;
	page.round_wins_team2 = 1;
	page.rounds_played = 4;
	page.team_points[0] = 15;
	page.team_points[1] = 9;
	page.round_time_remaining = 62 * (3600 + 125);
	page.frames = 62;
	page.cpu_percent = 3;
	page.pre_round_delay = 75;
	page.total_logins = 6;
	page.console_rows = {{"", 0}, {"", 0}, {"Alice: hi", 0xFF80A0FFu}, {"welcome", 0xFFFFFFFFu}};
	hud::ServerStatusText &t = page.text;
	t.empty_slot = "Open";
	t.server_lan = "LAN";
	t.team_wins = "Wins";
	t.team1 = "Blue";
	t.team2 = "Red";
	t.ties = "Ties";
	t.team_scores = "Scores";
	t.frames = "Frames";
	t.cpu = "CPU";
	t.start_timer = "Start";
	t.total_logins = "Logins";
	t.current_logins = "Players";
	t.game_type_abbreviation = "TDM";
	return page;
}

void test_render_snapshot() {
	const hud::ServerStatusPageState page = synthetic_page();
	// The grid: ceil(5 / 30) = 1 column, doubled to 2 under three; 3 rows. A
	// dedicated host shows slot i + 1, so the six cells are slots 1..6 and the
	// two past the limit (5) print nothing. The loading Cy is a row like any
	// other (grey on the screen; the text carries no colour).
	const std::string expected =
			"LAN Serve Test [TDM]  1:02:05\n"
			"#01 M:Alice    #04 S:Cy\n"
			"#02 R:Bob (7)\n"
			"#03 Open\n"
			"Wins\n"
			"   1 Red\n"
			"   2 Blue\n"
			"   1 Ties\n"
			"Scores\n"
			"   9 Red\n"
			"  15 Blue\n"
			"Frames 63+  CPU 3%  Start 1:15  Logins 6  Players 3\n"
			"Alice: hi\n"
			"welcome\n";
	const std::string text = serve::render_status_page(page);
	if (text != expected) std::printf("--- got ---\n%s--- expected ---\n%s", text.c_str(), expected.c_str());
	CHECK(text == expected);
}

std::string read_file(std::FILE *f) {
	std::fflush(f);
	std::rewind(f);
	std::string out;
	char buf[512];
	size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
	return out;
}

void test_print_rule() {
	std::FILE *out = std::tmpfile();
	CHECK(out != nullptr);
	if (out == nullptr) return;
	serve::StatusConsole console(out);
	hud::ServerStatusPageState page = synthetic_page();
	CHECK(console.present(page, 5000));   // the first page prints
	page.frames = 40;                     // the bottom row alone: no reprint
	page.round_time_remaining -= 62;
	CHECK(!console.present(page, 5400));
	page.console_rows[1].text = "new line"; // a console row changes
	CHECK(!console.present(page, 5500));    // inside the 200 ms throttle
	CHECK(console.present(page, 5700));
	const std::string printed = read_file(out);
	CHECK(printed.find("Frames 40") != std::string::npos);
	CHECK(printed.find("new line") != std::string::npos);
	std::fclose(out);
}

// A client-less host context with two in-match players on loopback wires.
struct ConsoleRig {
	world::World world;
	replication::LoopbackChannel wire_a;
	replication::LoopbackChannel wire_b;
	inmatch::NapiNPServerCtx ctx;
	world::EntityHandle alice;

	ConsoleRig() {
		world.registry.configure_pool(0, 8);
		world::Entity e;
		e.kind = world::EntityKind::Organic;
		e.flags = world::kEntityFlagPlayer;
		e.engine_flags = world::kEntityFlagPlayer;
		e.team = 1;
		e.health = 100;
		e.alive = true;
		alice = world.registry.spawn(0, e);
		const world::EntityHandle bob = world.registry.spawn(0, e);
		ctx.is_authority = 1;
		ctx.is_mp_session_peer = 0;
		ctx.is_in_session = 1;
		ctx.world = &world;
		inmatch::NapiNPConnection a;
		a.connection_id = 3;
		a.type = inmatch::NapiNPConnection::kTypeServerSide;
		a.link.transport = &wire_a;
		a.link.mode = replication::TransportMode::Client;
		a.link.owned_entity = alice;
		a.burst.spawned = true;
		a.phase = inmatch::ConnectionPhase::InMatch;
		a.reply.player_slot = 1;
		a.reply.player_name = "Alice";
		a.peer = peer_addr_from_octets({10, 0, 0, 7}, 2302);
		a.player_name = "Alice";
		ctx.np_protocol.connection_list.push_back(a);
		inmatch::NapiNPConnection b = a;
		b.connection_id = 4;
		b.link.transport = &wire_b;
		b.link.owned_entity = bob;
		b.reply.player_slot = 2;
		b.reply.player_name = "Bob";
		ctx.np_protocol.connection_list.push_back(b);
	}
};

std::vector<std::vector<uint8_t>> drain_chat(replication::LoopbackChannel &wire) {
	std::vector<std::vector<uint8_t>> bodies;
	replication::Datagram dg;
	while (wire.client_recv(dg))
		if (dg.tag == s2c::CHAT_BROADCAST) bodies.push_back(dg.body);
	return bodies;
}

void test_console_chat() {
	ConsoleRig rig;
	inmatch::NapiNPServerCtx &ctx = rig.ctx;

	// A player's line: fanned, then posted into the host's own ring.
	ChatUplink up;
	up.channel = 1;
	up.text = "hi <b>all";
	(void)inmatch::Server_HandleChatMessage(ctx, ctx.np_protocol.connection_list[0], up, 5000,
			rig.world);
	CHECK(ctx.console_chat.size() == 1);
	CHECK(ctx.console_chat.back().text == "Alice: hi all");
	CHECK(ctx.console_chat.back().color == hud::kHudColorLightBlue);
	// Proximity (13) posts as the squad channel (12): magenta.
	up.channel = 13;
	up.text = "near";
	(void)inmatch::Server_HandleChatMessage(ctx, ctx.np_protocol.connection_list[1], up, 9000,
			rig.world);
	CHECK(ctx.console_chat.size() == 2 && ctx.console_chat.back().text == "Bob: near" &&
			ctx.console_chat.back().color == hud::kHudColorMagenta);
	(void)drain_chat(rig.wire_a);
	(void)drain_chat(rig.wire_b);

	// A console line opens the Global talk, types, sends.
	hud::ChatEntry entry;
	// Inside the first eight frames the talk debounce refuses the row.
	uint32_t events = inmatch::server_console_submit(ctx, entry, "too early", 5, {});
	CHECK((events & hud::chat_entry_event::kBegan) == 0);
	CHECK(drain_chat(rig.wire_a).empty());
	events = inmatch::server_console_submit(ctx, entry, "Hello <c00ff00>team", 100, {});
	CHECK((events & hud::chat_entry_event::kSubmitted) != 0);
	for (replication::LoopbackChannel *wire : {&rig.wire_a, &rig.wire_b}) {
		const std::vector<std::vector<uint8_t>> bodies = drain_chat(*wire);
		CHECK(bodies.size() == 1);
		if (bodies.size() != 1) continue;
		const std::vector<uint8_t> expected = {10, 0xFF, 'H', 'e', 'l', 'l', 'o', ' ',
				't', 'e', 'a', 'm', 0};
		CHECK(bodies[0] == expected);
	}
	// The ring keeps the unstripped line, white.
	CHECK(ctx.console_chat.back().text == "Hello <c00ff00>team" &&
			ctx.console_chat.back().color == hud::kHudColorWhite);
	// The same line again inside 0x500 frames: refused, echoed light blue.
	const size_t before = ctx.console_chat.size();
	events = inmatch::server_console_submit(ctx, entry, "Hello <c00ff00>team", 200, {});
	CHECK((events & hud::chat_entry_event::kFloodEcho) != 0);
	CHECK(drain_chat(rig.wire_a).empty());
	CHECK(ctx.console_chat.size() == before + 1 &&
			ctx.console_chat.back().color == hud::kHudColorLightBlue);
	// The status page's rows are the ring's four newest, oldest first.
	const std::vector<inmatch::ServerConsoleLine> rows = inmatch::server_console_rows(ctx.console_chat);
	CHECK(rows.size() == 4 && rows[0].text == "Alice: hi all" && rows[1].text == "Bob: near" &&
			rows[3].text == "Hello <c00ff00>team");
	// The ring holds 40 lines, each cut at 119 characters.
	for (int i = 0; i < 50; ++i) inmatch::server_console_post(ctx.console_chat, std::string(200, 'x'), 1);
	CHECK(ctx.console_chat.size() == 40 && ctx.console_chat.back().text.size() == 119);
}

void test_inout_lines() {
	ConsoleRig rig;
	inmatch::NapiNPServerCtx &ctx = rig.ctx;
	ctx.np_protocol.session_name = "Serve Test";
	CHECK(inmatch::inout_host_line(ctx, true) == "???.???.???.???:????? : HOST STARTED \"Serve Test\".");
	ctx.local_address = PeerAddr{0, 32768};
	ctx.local_address_known = true;
	CHECK(inmatch::inout_host_line(ctx, false) == "0.0.0.0:32768 : HOST STOPPED \"Serve Test\".");
	inmatch::NapiNPConnection &a = ctx.np_protocol.connection_list[0];
	CHECK(inmatch::inout_player_added_line(a) == "10.0.0.7:2302 : SERVER PLAYER ADDED \"Alice\".");
	inmatch::latch_disconnect_event(a, make_disconnect_event(1, 9, 0, 0, "", 0, "NP.C:SH:STOP"));
	CHECK(inmatch::inout_player_removed_line(a) ==
			"10.0.0.7:2302 : SERVER PLAYER REMOVED \"Alice\". [1,9,0,0,\"\",0,\"NP.C:SH:STOP\"]");
}

void test_switches_and_devices() {
	serve::LogSwitches sw;
	std::string error;
	const std::vector<std::string> args = {"/profile", "host", "/PuntLog", "/CHEATLOG", "/PUNT.TXT", "/x"};
	CHECK(serve::parse_log_switch(args, 0, sw, error) == 2 && sw.profile_path == "host");
	CHECK(serve::parse_log_switch(args, 2, sw, error) == 1 && sw.punt_log);
	CHECK(serve::parse_log_switch(args, 3, sw, error) == 1 && sw.cheat_log);
	CHECK(serve::parse_log_switch(args, 4, sw, error) == 1);
	CHECK(serve::parse_log_switch(args, 5, sw, error) == 0);
	CHECK(serve::parse_log_switch({"/PROFILE"}, 0, sw, error) == -1);

	// The devices in a working directory of their own.
	const fs::path dir = fs::temp_directory_path() /
			("opennova_serve_logs_" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(dir);
	const fs::path previous = fs::current_path();
	fs::current_path(dir);
	{
		serve::ServerLogDevices devices;
		serve::LogSwitches on;
		on.profile_path = "host";
		on.punt_log = true;
		devices.arm(on);
		const inmatch::ServerLogs logs = devices.logs();
		CHECK(logs.profile != nullptr && logs.punt != nullptr);
		logs.punt->write_punt(4, "Joe", "ACRC", "");
		CHECK(logs.profile->open("servetst.bms"));
		logs.profile->close();
	}
	std::ifstream punt("_PUNT.TXT", std::ios::binary);
	const std::string punt_text((std::istreambuf_iterator<char>(punt)), std::istreambuf_iterator<char>());
	CHECK(punt_text.find(" : START : \r\n") != std::string::npos);
	CHECK(punt_text.find(" : 04 : \"Joe\" : ACRC : \r\n") != std::string::npos);
	std::ifstream sph("host.sph", std::ios::binary);
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(sph)), std::istreambuf_iterator<char>());
	ServerLogDocument doc;
	CHECK(decode_server_log(bytes.data(), bytes.size(), doc) && doc.mission == "servetst.bms" &&
			doc.ended_clean);
	punt.close();
	sph.close();
	fs::current_path(previous);
	std::error_code ec;
	fs::remove_all(dir, ec);
}


// One `.sph` file decoded: its map, the joiner's one roster row with at least
// five eighth-tick samples, the end chunk and no disconnect marker.
void check_capture(const fs::path &path, const char *map, const char *joiner) {
	std::ifstream in(path, std::ios::binary);
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	ServerLogDocument doc;
	CHECK(!bytes.empty() && decode_server_log(bytes.data(), bytes.size(), doc));
	CHECK(doc.version == 2 && strutil::iequals(doc.mission, map));
	CHECK(doc.roster.size() == 1);
	const uint32_t id = doc.roster.empty() ? 0 : doc.roster[0].net_id;
	CHECK(!doc.roster.empty() && doc.roster[0].name == joiner && id != 0);
	size_t samples = 0;
	for (const ServerLogFrame &f : doc.frames)
		for (const ServerLogEntity &e : f.entities)
			if (e.net_id == id && (e.flags & 0x100u) != 0) ++samples;
	CHECK(doc.frames.size() >= 5 && samples >= 5);
	for (const ServerLogEvent &ev : doc.events) CHECK(ev.kind != ServerLogEventKind::Disconnect);
	CHECK(doc.ended_clean && doc.leftover_clean);
}

void test_serve_session_capture() {
	using namespace serve_test;
	if (net::startup() != 0) {
		CHECK(false);
		return;
	}
	const fs::path dir = fresh_dir("capture");
	CHECK(write_bytes(dir / "SERVELOG.BMS", deathmatch_mission("Serve Log Map")));
	CHECK(write_bytes(dir / "SERVELO2.BMS", deathmatch_mission("Serve Log Map Two")));
	// The rotation: the first line's map starts (the last line names it), the
	// second follows.
	CHECK(write_text(dir / "log.host", "GameName \"Log Test\"\r\nMaxPlayers 8\r\n"
	                                   "Mission servelog.bms\r\nMission servelo2.bms\r\n"
	                                   "Mission servelog.bms\r\n"));
	// The logs land in the working directory.
	ScopedCwd cwd(dir);

	// --lan-port 0: the OS picks the port, which the server holds and reports.
	std::string error;
	serve::ServeOptions options;
	CHECK(serve::parse_serve_options({"--resource-dir", dir.string(), "/HOST", "log.host",
			"--loose-root", "--lan-port", "0", "/PROFILE", "host", "/PUNTLOG"}, options, error) == 0);
	auto holder = std::make_unique<serve::Server>(options);
	const bool started = holder->start(error);
	CHECK(started);
	if (!started) {
		std::printf("start: %s\n", error.c_str());
		cwd.restore();
		net::shutdown();
		return;
	}
	serve::Server &server = *holder;
	CHECK(fs::exists(dir / "host.sph")); // the first mission's start opened it
	constexpr double kFrame = 1.0 / 62.5;
	const net::Endpoint server_ep{{127, 0, 0, 1}, server.bound_port()};

	uint16_t joiner_port = 0;
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	CHECK(joiner_sock.is_valid());
	inmatch::ClientRuntime client("LogJoiner");
	auto ship = [&](const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), server_ep, d.data(), d.size());
	};
	auto drain = [&]() {
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, 5);
			if (n <= 0) break;
			client.receive(rx, static_cast<size_t>(n));
		}
	};
	uint32_t tick = 1;
	ship(client.start());
	auto step = [&]() {
		if (!server.frame(kFrame)) return false;
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
		return true;
	};
	for (int f = 0; f < 900 && !client.in_match(); ++f)
		if (!step()) break;
	CHECK(client.in_match());
	for (int f = 0; f < 600 && client.in_match() && !client.is_deployed(); ++f)
		if (!step()) break;
	CHECK(client.is_deployed());
	for (int f = 0; f < 40; ++f)
		if (!step()) break;

	// The page, printed by the console the server's loop runs (the test
	// directory has no gametext.bin, so its labels are empty): the server
	// line, the joiner in slot 1 (slot 0 is the server's own row, which the
	// dedicated page skips), and the joiner counted in the current logins.
	std::FILE *out = std::tmpfile();
	CHECK(out != nullptr);
	if (out != nullptr) {
		serve::StatusConsole console(out);
		CHECK(console.update(server, 1000));
		const std::string page = read_file(out);
		CHECK(page.find(" Log Test []") != std::string::npos);
		CHECK(page.find("#01 ") != std::string::npos && page.find(":LogJoiner") != std::string::npos);
		CHECK(page.find("   1   1\n") != std::string::npos);
		std::fclose(out);
	}

	// A round end: the map change tears the first map down (its log closes)
	// and starts the second (the next numbered log opens); the joiner reloads.
	CHECK(inmatch::Server_ExecuteServerCommand(server.role().state.host_owner.ctx,
			&server.kernel().world, "Cycle", "", {}).handled);
	bool reloaded = false;
	bool back = false;
	for (int f = 0; f < 4000 && !back; ++f) {
		if (!step()) break;
		const int32_t reason = client.mission_exit_reason();
		if (reason != 0) {
			CHECK(inmatch::main_frame_exit(reason, /*in_session=*/true, /*authority=*/false) ==
					inmatch::MainFrameExit::GameLoop);
			CHECK(client.begin_mission_reload());
			reloaded = true;
			continue;
		}
		back = reloaded && client.in_match() && client.is_deployed() &&
				server.missions_played() == 2;
	}
	CHECK(reloaded && back);
	CHECK(fs::exists(dir / "host1.sph"));
	for (int f = 0; f < 40; ++f)
		if (!step()) break;

	server.stop();
	holder.reset();
	check_capture(dir / "host.sph", "servelog.bms", "LogJoiner");
	check_capture(dir / "host1.sph", "servelo2.bms", "LogJoiner");
	CHECK(!fs::exists(dir / "host2.sph"));
	const std::string punt_text = read_text(dir / "_PUNT.TXT");
	CHECK(punt_text.find(" : START : \r\n") != std::string::npos);

	cwd.restore();
	net::shutdown();
	std::error_code ec;
	fs::remove_all(dir, ec);
}

} // namespace

int main() {
	test_render_snapshot();
	test_print_rule();
	test_console_chat();
	test_inout_lines();
	test_switches_and_devices();
	test_serve_session_capture();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("status console: ok\n");
	return 0;
}
