#pragma once

#include "net_sockets.h"

#include <net/admin/admin_protocol.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::nw_lister {

// The retail admin client's default port (the server has none: remote_admin_port is 0, off, until
// set).
inline constexpr uint16_t kAdminDefaultPort = ADMIN_CLIENT_DEFAULT_PORT;
inline constexpr int kAdminPollSeconds = 15;

using AdminPlayer = opennova::AdminPlayer;

// What the last poll of the game server's admin port read.
struct AdminSnapshot {
	bool ok = false;      // the last poll answered
	uint64_t seq = 0;     // counts up whenever the content changes; 0 = no poll finished yet
	std::vector<AdminPlayer> players;
	std::string mission;  // "" = keep the listing file's
	int time_left_minutes = -1;
	std::string status;   // why the last poll failed
};

// Polls the game server's remote-admin port (the engine's admin codec over one TCP connection,
// kept logged in between polls) on its own thread, so a slow or dead server never stalls the
// NovaWorld session. Read-only commands only; the admin.cfg user needs the GET, MISSION and
// PLAYER rights (0x01, 0x04, 0x08).
class AdminFeed {
public:
	~AdminFeed() { stop(); }
	void start(const net::Endpoint &server, std::string user, std::string password);
	// Ends the poll thread, which says QUIT on a logged-in connection. Waits for at most one
	// poll's I/O timeout.
	void stop();
	AdminSnapshot snapshot() const;

private:
	void run();
	bool log_in(net::Socket &socket, std::string &error);
	// Fills `next` (ok when every command answered); false when the connection is gone.
	bool poll(net::Socket &socket, AdminSnapshot &next, std::string &error);
	static void close(net::Socket &socket, bool logged_in);
	void publish(AdminSnapshot next);

	net::Endpoint server_;
	std::string user_;
	std::string password_;
	std::atomic<bool> stop_{false};
	std::thread thread_;
	mutable std::mutex mutex_;
	AdminSnapshot snapshot_;
};

} // namespace opennova::nw_lister
