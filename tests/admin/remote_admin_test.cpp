// The process's remote-admin half both embedders share (runtime/inmatch/remote_admin.h): the
// 16-bit port read, the log file's text-mode line, and the console the admin server dispatches
// to, session-less in the menus (the game's case: retail opens the console in every process) and
// over a host context once one binds. Driven by the client half (net/admin/admin_protocol.h) over
// in-memory bytes; the protocol itself is admin_server's (the same login and verb vectors).
// [orig: Game_InitSubsystems @0x4A72C2..0x4A72D9; CAdminServer_Construct @0x402C84;
//  CAdminServer_DispatchCommand @0x406720; Menu_UpdateFrame @0x552901;
//  Game_ProcessMainFrame @0x5268F1]

#include <base/io/crt_rand.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/remote_admin.h>

#include <cstdio>
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
	consoles.bind(nullptr, seams);
	expect(consoles.bound_to(nullptr), "the session-less console");
	consoles.console()->set_scene(inmatch::AdminScene::MainMenu);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Menus", "the menus' state");
	const std::string settings = c.one("GET GAMESETTINGS");
	expect(settings.find("ServerName           = Menu Block\n") != std::string::npos,
	       "the menus' settings read the process block's name");
	expect(settings.find("ArmoryTimer          = 45\n") != std::string::npos, "and its armory timer");
	expect_eq(c.one("SET ServerName Renamed"), "OK - Setting Changed.", "a SET in the menus");
	// Each value token followed by one space [orig: CAdminServer_HandleSetCommand @0x4060BF..0x40615A].
	expect(block.game_name == "Renamed " && saves == 1, "writes the block and saves it");

	inmatch::NapiNPServerCtx host;
	host.config.server_name = "Hosted";
	consoles.bind(&host, seams);
	expect(consoles.bound_to(&host) && !consoles.bound_to(nullptr), "rebound over the host");
	consoles.console()->set_scene(inmatch::AdminScene::GameLoop);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Game", "the game loop's state");
	consoles.bind(nullptr, seams);
	consoles.console()->set_scene(inmatch::AdminScene::Other);
	expect_eq(c.one("GET GAMESTATE"), "OK - Current State = Unknown", "a pre or post menu");
}

} // namespace

int main() {
	test_port_and_log_text();
	test_consoles();
	if (g_failures != 0) {
		std::fprintf(stderr, "remote_admin: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("remote_admin: ok\n");
	return 0;
}
