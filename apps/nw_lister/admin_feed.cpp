#include "admin_feed.h"

#include <base/io/log.h>

#include <chrono>

namespace opennova::nw_lister {

namespace {

constexpr int kAdminIoTimeoutMs = 10000;
// How long a QUIT waits for the server's close before the reset.
constexpr int kAdminQuitTimeoutMs = 2000;

bool send_payload(net::Socket &socket, const uint8_t *payload, size_t size) {
	const std::vector<uint8_t> packet = admin_encode_packet(payload, size);
	return net::tcp_send_all(socket, packet.data(), packet.size());
}

bool recv_payload(net::Socket &socket, std::vector<uint8_t> &payload, std::string &error) {
	uint8_t header[ADMIN_PACKET_HEADER_BYTES];
	if (!net::tcp_recv_exact(socket, header, sizeof(header))) {
		error = "the server closed the connection or did not answer";
		return false;
	}
	size_t size = 0;
	if (!admin_decode_header(header, size)) {
		error = "not a remote-admin packet";
		return false;
	}
	payload.resize(size);
	if (size != 0 && !net::tcp_recv_exact(socket, payload.data(), size)) {
		error = "the connection closed mid-packet";
		return false;
	}
	return true;
}

// One command and its one reply: the server reads one packet at a time and drops whatever
// follows it in the same read, so nothing is pipelined. An ERROR or USAGE reply leaves the
// connection up (`connected`). [orig: ProcessClientData @0x406f3d]
bool command(net::Socket &socket, const char *text, std::string &reply, std::string &error, bool &connected) {
	const std::vector<uint8_t> payload = admin_encode_command(text);
	std::vector<uint8_t> answer;
	if (!send_payload(socket, payload.data(), payload.size()) || !recv_payload(socket, answer, error)) {
		if (error.empty()) error = std::string("cannot send ") + text;
		connected = false;
		return false;
	}
	if (!admin_reply_text(answer, reply) || admin_reply_is_error(reply)) {
		error = std::string(text) + ": " + reply;
		return false;
	}
	return true;
}

bool same_players(const std::vector<AdminPlayer> &a, const std::vector<AdminPlayer> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].slot != b[i].slot || a[i].name != b[i].name || a[i].team != b[i].team) return false;
	}
	return true;
}

} // namespace

void AdminFeed::start(const net::Endpoint &server, std::string user, std::string password) {
	stop();
	server_ = server;
	user_ = std::move(user);
	password_ = std::move(password);
	if (user_.size() > ADMIN_LOGIN_FIELD_MAX_CHARS || password_.size() > ADMIN_LOGIN_FIELD_MAX_CHARS) {
		io::logf(io::LogLevel::kWarn, "[admin] the server reads %zu characters of the user and password",
		         ADMIN_LOGIN_FIELD_MAX_CHARS);
	}
	stop_ = false;
	thread_ = std::thread([this]() { run(); });
}

void AdminFeed::stop() {
	stop_ = true;
	if (thread_.joinable()) thread_.join();
}

AdminSnapshot AdminFeed::snapshot() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return snapshot_;
}

void AdminFeed::publish(AdminSnapshot next) {
	std::lock_guard<std::mutex> lock(mutex_);
	const bool changed = next.ok != snapshot_.ok || next.mission != snapshot_.mission ||
	                     next.time_left_minutes != snapshot_.time_left_minutes ||
	                     !same_players(next.players, snapshot_.players);
	next.seq = snapshot_.seq + ((changed || snapshot_.seq == 0) ? 1 : 0);
	snapshot_ = std::move(next);
}

// Connect and log in: the challenge first (it keys the login), then the encrypted login. A
// refused login gets no reply; the server just closes. [orig: CAdminServer_HandleLogin
//  @0x4059e3 (the failure path returns 0, ProcessFrame closes)]
bool AdminFeed::log_in(net::Socket &socket, std::string &error) {
	socket = net::tcp_connect(server_, kAdminIoTimeoutMs);
	if (!socket.is_valid()) {
		error = "cannot connect to " + net::endpoint_to_string(server_);
		return false;
	}
	std::vector<uint8_t> challenge;
	if (!recv_payload(socket, challenge, error)) {
		error += " (an ip_restrict line in admin.cfg may refuse this address)";
		return false;
	}
	if (!admin_challenge_valid(challenge)) {
		error = "the server's challenge is not a remote-admin challenge";
		return false;
	}
	const auto login = admin_encode_login(challenge, user_, password_);
	std::vector<uint8_t> answer;
	std::string reply;
	if (!send_payload(socket, login.data(), login.size()) || !recv_payload(socket, answer, error)) {
		error = "the admin login was refused";
		return false;
	}
	if (!admin_reply_text(answer, reply) || !admin_login_accepted(reply)) {
		error = "unexpected login reply: " + reply;
		return false;
	}
	io::logf(io::LogLevel::kInfo, "[admin] logged in to %s", net::endpoint_to_string(server_).c_str());
	return true;
}

// One poll: PLAYER LIST (game state only: "ERROR - Not in Game State." in the menus), MISSION
// LIST and GET GAMESETTINGS, each answered by exactly one reply. False when the connection is
// gone. [orig: CAdminServer_HandlePlayer @0x403f63]
bool AdminFeed::poll(net::Socket &socket, AdminSnapshot &next, std::string &error) {
	std::string players;
	std::string missions;
	std::string settings;
	bool connected = true;
	if (command(socket, ADMIN_COMMAND_PLAYER_LIST, players, error, connected) &&
	    command(socket, ADMIN_COMMAND_MISSION_LIST, missions, error, connected) &&
	    command(socket, ADMIN_COMMAND_GET_GAMESETTINGS, settings, error, connected)) {
		next.ok = true;
		next.players = admin_parse_player_list(players);
		next.mission = admin_parse_current_mission(missions);
		next.time_left_minutes = admin_parse_time_left_minutes(settings);
	}
	return connected;
}

// The server keeps a connection's slot when the client closes with a FIN, and its slot table
// corrupts once six are held, so a logged-in session ends with QUIT (the server closes first)
// and anything else with a reset. [orig: ProcessFrame @0x406fa3 (recv() == 0 is not a close);
//  AcceptConnection @0x4056e6..0x4056ed (the regrow copies 4 bytes a slot); DispatchCommand
//  @0x406811 (QUIT returns 0); RAT.exe @0x401475..0x40148d]
void AdminFeed::close(net::Socket &socket, bool logged_in) {
	if (logged_in) {
		const std::vector<uint8_t> quit = admin_encode_command(ADMIN_COMMAND_QUIT);
		uint8_t drain[256];
		if (send_payload(socket, quit.data(), quit.size())) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAdminQuitTimeoutMs);
			while (std::chrono::steady_clock::now() < deadline) {
				const int n = net::tcp_recv(socket, drain, sizeof(drain));
				if (n == 0) {
					net::close_socket(socket);
					return;
				}
				if (n < 0) break;
			}
		}
	}
	net::close_socket_reset(socket);
}

void AdminFeed::run() {
	net::Socket socket;
	bool logged_in = false;
	while (!stop_) {
		AdminSnapshot next;
		std::string error;
		if (!logged_in) logged_in = log_in(socket, error);
		if (logged_in && !poll(socket, next, error)) {
			close(socket, false);
			logged_in = false;
		} else if (!logged_in) {
			close(socket, false);
		}
		next.status = error;
		publish(std::move(next));
		for (int i = 0; i < kAdminPollSeconds * 10 && !stop_; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	if (socket.is_valid()) close(socket, logged_in);
}

} // namespace opennova::nw_lister
