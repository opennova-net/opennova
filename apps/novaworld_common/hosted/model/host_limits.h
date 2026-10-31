#pragma once

// The ranges a hosted server's whole-number settings take: the host screen's of
// the title the server's product speaks (products/product_repository.h: a game's
// `title`, the title code opennova-serve's /game takes). Service policy, not a
// port: the host screens' spin ranges are unread; the cap is the title's screen's
// MAX_PLAYERS (base/title/title.h Title::max_players_host_screen).

#include <string_view>

namespace opennova::novaworld_server::hosted {

// A whole-number setting's range, both ends included.
struct SettingRange {
	int min = 0;
	int max = 0;
};

// The ranges a hosted server's whole-number settings take, the host screen's
// for its product's title.
struct HostLimits {
	SettingRange max_players;
	SettingRange time_limit;
	SettingRange kill_limit;
	SettingRange max_score;
	SettingRange respawn;
	SettingRange start_delay;
};

// The limits of title `title`: the host screen's ranges with the title's
// MAX_PLAYERS cap. A code no installed title has aborts, named (require_title_code).
HostLimits host_limits_for_title(std::string_view title);

} // namespace opennova::novaworld_server::hosted
