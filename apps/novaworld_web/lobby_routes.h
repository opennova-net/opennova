#pragma once

// The lobby browser's public routes: GET /api/lobbies (every listing, by game),
// GET /api/lobbies/<rid> (one listing: its game info, the players its host's
// status reports name, and a hosted server's scoreboard read over its admin
// link) and GET /api/lobbies/novalogic (the servers on NovaLogic's NovaWorld,
// from the list web keeps of NovaLogic's feed: novalogic_lobbies.h). Each listing
// of ours carries its server's ONE build where the service knows it (`build`: a
// running hosted server's, as the server reports it; null for any other, a
// self-hosted or a retail server's, whose retail listing carries none). None names
// a joiner's address or an owner. Service policy, not a port.

#include "novalogic_lobbies.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

class LobbyRoutes {
public:
	// A hosted server's scoreboard is read at most this often per server: the
	// page polls it, and each read is a command on the server's admin link.
	static constexpr std::chrono::seconds kScoreboardFresh{ 15 };
	// What one read waits for the server's answer before the page goes without.
	static constexpr std::chrono::milliseconds kScoreboardWait{ 1500 };

	explicit LobbyRoutes(db::ConnectionPool &pool) : pool_(pool) {}
	LobbyRoutes(const LobbyRoutes &) = delete;
	LobbyRoutes &operator=(const LobbyRoutes &) = delete;

	// `novalogic` is the list of NovaLogic's servers (it must outlive the
	// routes), or null with ONNET_NOVALOGIC_LOBBIES off: the route then answers
	// enabled false and fetches nothing.
	void register_routes(WebApp &app, NovaLogicLobbies *novalogic = nullptr);

private:
	struct ScoreRow {
		std::string name; // UTF-8
		int team = 0;
		int kills = 0;
		int deaths = 0;
	};
	struct Scoreboard {
		std::chrono::steady_clock::time_point read_at{};
		bool read = false;
		bool in_flight = false;
		std::vector<ScoreRow> rows;
	};

	crow::response list();
	crow::response one(uint64_t rid);
	// The hosted server's players with their scores: a fresh read, the last one
	// while another request reads it, or none when no read has answered.
	std::optional<std::vector<ScoreRow>> scoreboard_of(int64_t server_id);

	db::ConnectionPool &pool_;
	NovaLogicLobbies *novalogic_ = nullptr;
	std::mutex mu_;
	std::map<int64_t, Scoreboard> scoreboards_;
};

} // namespace opennova::novaworld_server
