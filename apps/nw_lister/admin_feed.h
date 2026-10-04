#pragma once

// Live server state for the listing, read from the game server's retail
// remote-admin port (TCP, 4000 by default) - the same exchange
// opennova-net/WolfRAT2 drives (wolfrat/admin_session.py):
//
//   packet   = 00 00 0D 0A | u32le total length (header included) | payload
//   server  -> 32-byte challenge + NUL
//   client  -> 65-byte login: user[32] password[32] NUL, mixed with the
//              challenge and the 0x04B05731 LCG (admin_login_response)
//   server  -> "... logged in ..." text
//   then one NUL-terminated ASCII command per packet, one text reply each:
//   "PLAYER LIST", "MISSION LIST", "GET GAMESETTINGS".
//
// The admin port never states the game mode, so game_type stays the
// listing file's.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::lister {

// The 65-byte login answer to a challenge (the challenge's NUL ends the key).
std::array<uint8_t, 65> admin_login_response(const uint8_t *challenge, size_t size, const std::string &user,
                                             const std::string &password);

struct AdminPlayer {
	int slot = 0;
	std::string name;
	std::string team;
};

// "PLAYER LIST": tab-separated NAME # TEAM Class Kills Deaths PING rows. A
// dedicated server's own slot 0 "Host" row is not a player and is dropped.
std::vector<AdminPlayer> parse_admin_players(const std::string &reply);

// "MISSION LIST": the <CURRENT MISSION> row's file name without its
// .bms/.npj/.npz extension, as NovaWorld rows show maps; "" when absent.
std::string parse_admin_current_mission(const std::string &reply);

// "GET GAMESETTINGS": GameTime = remaining/total minutes. Returns the
// remaining minutes, or -1 (shown as no time limit) when there is no limit,
// none is left, or the row is missing.
int parse_admin_time_left(const std::string &reply);

struct AdminSnapshot {
	bool ok = false;           // the last poll answered
	uint64_t seq = 0;          // bumps whenever the content changes
	std::vector<AdminPlayer> players;
	std::string mission;       // "" = keep the listing file's
	int time_left_minutes = -1;
	std::string status;        // last error, for the log
};

// Polls the admin port on its own thread so a slow or dead server never
// stalls the NovaWorld session.
class AdminFeed {
public:
	~AdminFeed() { stop(); }
	void start(std::string host, uint16_t port, std::string user, std::string password, int poll_seconds);
	void stop();
	AdminSnapshot snapshot() const;

private:
	void run();
	void publish(AdminSnapshot next);

	std::string host_, user_, password_;
	uint16_t port_ = 4000;
	int poll_seconds_ = 15;
	std::atomic<bool> stop_{false};
	std::atomic<intptr_t> sock_{-1}; // the open connection, so stop() can cut a blocked read
	std::thread thread_;
	mutable std::mutex mu_;
	AdminSnapshot snap_;
};

} // namespace opennova::lister
