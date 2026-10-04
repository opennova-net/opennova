// nw-lister's admin poll thread (apps/nw_lister/admin_feed.h) against an in-test remote-admin
// server that plays the retail one over loopback TCP: the challenge, the login it decrypts and
// checks, one reply per command in the server's formats, and the QUIT the feed ends with (a FIN
// would leak the server's slot). [orig: CAdminServer_AcceptConnection @0x405580;
// CAdminServer_HandleLogin @0x405870; CAdminServer_DispatchCommand @0x406811 (QUIT)]

#include "admin_feed.h"
#include "admin_server_side.h"

#include "net_sockets.h"

#include <net/admin/admin_protocol.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using native_socket = int;
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

void close_native(native_socket s) {
#if defined(_WIN32)
	closesocket(s);
#else
	::close(s);
#endif
}

bool send_reply(opennova::net::Socket &s, const std::string &text) {
	std::vector<uint8_t> payload(text.begin(), text.end());
	payload.push_back(0);
	const std::vector<uint8_t> packet = opennova::admin_encode_packet(payload.data(), payload.size());
	return opennova::net::tcp_send_all(s, packet.data(), packet.size());
}

bool recv_payload(opennova::net::Socket &s, std::vector<uint8_t> &payload) {
	uint8_t header[opennova::ADMIN_PACKET_HEADER_BYTES];
	size_t size = 0;
	if (!opennova::net::tcp_recv_exact(s, header, sizeof(header)) || !opennova::admin_decode_header(header, size))
		return false;
	payload.resize(size);
	return size == 0 || opennova::net::tcp_recv_exact(s, payload.data(), size);
}

struct ServerLog {
	std::atomic<bool> logged_in{false};
	std::atomic<int> commands{0};
	std::atomic<bool> quit{false};
};

// One admin session: the challenge, the login, then one reply per command until QUIT.
void serve_admin(native_socket listener, ServerLog &log) {
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(listener, &readable);
	timeval tv{5, 0};
	if (::select(static_cast<int>(listener) + 1, &readable, nullptr, nullptr, &tv) <= 0) return;
	opennova::net::Socket client{static_cast<intptr_t>(::accept(listener, nullptr, nullptr))};
	if (!client.is_valid()) return;
	const std::vector<uint8_t> challenge = admin_test::server_challenge(42);
	const std::vector<uint8_t> packet = opennova::admin_encode_packet(challenge.data(), challenge.size());
	opennova::net::tcp_send_all(client, packet.data(), packet.size());
	std::vector<uint8_t> login;
	if (!recv_payload(client, login) || login.size() != opennova::ADMIN_LOGIN_BYTES) {
		opennova::net::close_socket(client);
		return;
	}
	admin_test::server_decrypt_login(login.data(), login.size(), challenge);
	const std::string user(reinterpret_cast<const char *>(login.data()));
	const std::string pass(reinterpret_cast<const char *>(login.data() + 32));
	if (user != "boss" || pass != "pw") { // a refused login: no reply, the close
		opennova::net::close_socket(client);
		return;
	}
	log.logged_in = true;
	send_reply(client, "OK - User: " + user + " successfully logged in.");
	std::vector<uint8_t> command;
	while (recv_payload(client, command)) {
		std::string text;
		opennova::admin_reply_text(command, text);
		++log.commands;
		if (text == "QUIT") {
			log.quit = true;
			break; // QUIT returns 0: the server closes
		}
		if (text == "PLAYER LIST") {
			char rows[256];
			std::snprintf(rows, sizeof(rows), "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"
			                                  "%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n",
			              "Host", 0, 1, 9, 0, 0, 0, "Alpha", 2, 1, 8, 3, 1, 120);
			send_reply(client, rows);
		} else if (text == "MISSION LIST") {
			send_reply(client, "0: CP01.bms - () () () <CURRENT MISSION> <>\n1: CP02.bms - () () () <> <NEXT MISSION>\n");
		} else if (text == "GET GAMESETTINGS") {
			send_reply(client, "Tracers              = 1\nGameTime             = 12/30\n");
		} else {
			send_reply(client, "USAGE - [QUIT | GET | MISSION | PLAYER]");
		}
	}
	opennova::net::close_socket(client);
}

} // namespace

int main() {
	using namespace std::chrono_literals;
	if (opennova::net::startup() != 0) return 1;

	const native_socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
	::listen(listener, 5);
#if defined(_WIN32)
	int len = sizeof(addr);
#else
	socklen_t len = sizeof(addr);
#endif
	::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);

	ServerLog log;
	std::thread server([&] { serve_admin(listener, log); });

	opennova::net::Endpoint endpoint;
	endpoint.ip = {127, 0, 0, 1};
	endpoint.port = ntohs(addr.sin_port);
	opennova::lister::AdminFeed feed;
	feed.start(endpoint, "boss", "pw");
	opennova::lister::AdminSnapshot snapshot;
	for (int i = 0; i < 300 && snapshot.seq == 0; ++i) {
		std::this_thread::sleep_for(10ms);
		snapshot = feed.snapshot();
	}
	expect(log.logged_in, "the server's decrypt recovers the feed's login");
	expect(snapshot.ok, "the first poll answers");
	expect(snapshot.players.size() == 1 && snapshot.players[0].name == "Alpha" && snapshot.players[0].slot == 2 &&
	               snapshot.players[0].team == "1",
	       "the players, without the host's own slot");
	expect(snapshot.mission == "CP01", "the current mission");
	expect(snapshot.time_left_minutes == 12, "the time left");
	expect(log.commands == 3, "one command per field, nothing pipelined");

	feed.stop();
	server.join();
	expect(log.quit, "the feed ends its session with QUIT");

	close_native(listener);
	opennova::net::shutdown();
	if (g_failures == 0) {
		std::printf("OK: the admin feed logs in, polls and quits\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
