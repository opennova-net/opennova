// The remote-admin console over a real loopback TCP socket: the apps' pump
// (apps/common/admin_tcp_server.h) feeding the engine's connection machine
// (net/admin/admin_server.h) and its console (runtime/inmatch/admin_console.h), and a client
// speaking the admin codec (net/admin/admin_protocol.h) over apps/common's TCP helpers: the
// challenge, the login, the commands, QUIT closing the slot from the server's side, a client's
// FIN freeing its slot, and the admin_log.txt lines.
// [orig: CAdminServer_Listen @0x406E00; CAdminServer_ProcessFrame @0x406F50;
//  CAdminServer_AcceptConnection @0x405580; CAdminServer_HandleLogin @0x405870]

#include "admin_tcp_server.h"
#include "net_sockets.h"

#include <base/io/crt_rand.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

bool recv_payload(net::Socket &s, std::vector<uint8_t> &payload) {
	uint8_t header[ADMIN_PACKET_HEADER_BYTES];
	size_t size = 0;
	if (!net::tcp_recv_exact(s, header, sizeof(header)) || !admin_decode_header(header, size)) return false;
	payload.resize(size);
	return size == 0 || net::tcp_recv_exact(s, payload.data(), size);
}

bool send_payload(net::Socket &s, const std::vector<uint8_t> &payload) {
	const std::vector<uint8_t> packet = admin_encode_packet(payload.data(), payload.size());
	return net::tcp_send_all(s, packet.data(), packet.size());
}

std::string command(net::Socket &s, const std::string &text) {
	if (!send_payload(s, admin_encode_command(text))) return "<send failed>";
	std::vector<uint8_t> payload;
	if (!recv_payload(s, payload)) return "<closed>";
	std::string reply;
	admin_reply_text(payload, reply);
	return reply;
}

template <typename Pred>
bool wait_for(Pred pred) {
	for (int i = 0; i < 500; ++i) {
		if (pred()) return true;
		std::this_thread::sleep_for(5ms);
	}
	return pred();
}

} // namespace

int main() {
	if (net::startup() != 0) return 1;

	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	gamecfg::GameCfg block;
	std::atomic<int> saves{0};
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	seams.save_config = [&] { ++saves; };
	inmatch::AdminConsole console(ctx, std::move(seams));
	console.set_scene(inmatch::AdminScene::MainMenu);

	const std::string cfg_text = "op secret 3\r\nip_restrict = 127.0.0.1\r\n";
	io::CrtRand rand;
	std::mutex log_mutex;
	std::vector<std::string> log;
	AdminServer server(admincfg::parse(cfg_text.data(), cfg_text.size()), console, rand, [&](std::string_view line) {
		std::lock_guard<std::mutex> lock(log_mutex);
		log.emplace_back(line);
	});
	net::AdminTcpServer tcp(server);
	expect(tcp.listen(0, /*loopback_only=*/true) && tcp.port() != 0, "the console listens on an ephemeral port");
	std::atomic<bool> stop{false};
	std::thread pump([&] {
		while (!stop) {
			tcp.pump();
			std::this_thread::sleep_for(1ms);
		}
	});

	net::Endpoint endpoint;
	endpoint.ip = {127, 0, 0, 1};
	endpoint.port = tcp.port();
	{
		net::ScopedSocket client(net::tcp_connect(endpoint, 3000));
		expect(client.is_valid(), "the client connects");
		std::vector<uint8_t> challenge;
		expect(recv_payload(client.get(), challenge) && admin_challenge_valid(challenge), "the challenge");
		const auto login = admin_encode_login(challenge, "op", "secret");
		expect(send_payload(client.get(), std::vector<uint8_t>(login.begin(), login.end())), "the login sent");
		std::vector<uint8_t> reply;
		std::string text;
		expect(recv_payload(client.get(), reply) && admin_reply_text(reply, text) && admin_login_accepted(text),
		       "the login accepted");
		expect(command(client.get(), "GET GAMESTATE") == "OK - Current State = Menus", "GET over the wire");
		expect(command(client.get(), "SET StartDelay 5") == "OK - Setting Changed." && block.startdelay == 5,
		       "SET over the wire writes the cfg block");
		expect(saves == 1, "and saves it");
		expect(command(client.get(), "MISSION LIST") == "USAGE - [QUIT | GET | SET]", "rights 3 refuse MISSION");
		expect(send_payload(client.get(), admin_encode_command("QUIT")), "QUIT sent");
		uint8_t byte = 0;
		expect(net::tcp_recv(client.get(), &byte, 1) == 0, "QUIT: the server closes with no reply");
	}
	expect(wait_for([&] { return tcp.connection_count() == 0 && server.connection_count() == 0; }),
	       "QUIT freed the slot");
	{
		net::ScopedSocket client(net::tcp_connect(endpoint, 3000));
		std::vector<uint8_t> challenge;
		expect(recv_payload(client.get(), challenge), "a second client gets its challenge");
		expect(wait_for([&] { return server.connection_count() == 1; }), "its slot is held");
	}
	expect(wait_for([&] { return server.connection_count() == 0; }), "a client's FIN frees its slot (D-NET-360)");
	stop = true;
	pump.join();
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		bool logged_in = false;
		bool command_logged = false;
		for (const std::string &line : log) {
			if (line == "User logged in (op/secret)\n") logged_in = true;
			if (line == "User command (op) - SET StartDelay 5\n") command_logged = true;
		}
		expect(logged_in && command_logged, "admin_log.txt's login and command lines");
	}
	tcp.close();
	net::shutdown();
	if (g_failures == 0) {
		std::printf("admin_server_tcp: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
