#pragma once

// The ranges a hosted server's whole-number settings take: the host screen's of
// the title the server's data set speaks (data_set_repository.h: a base data
// set's `title`, the gameprofile code opennova-serve's /game takes). Two
// tables, Joint Operations' (`jo`, which OpenNova's own data speaks too) and
// Delta Force Xtreme 2's (`dfx2`). Service policy, not a port: the host
// screens' spin ranges are unread; the caps are the screens' MAX_PLAYERS.

#include <string_view>

namespace opennova::novaworld_server::hosted {

// A whole-number setting's range, both ends included.
struct SettingRange {
	int min = 0;
	int max = 0;
};

// The ranges a hosted server's whole-number settings take, the host screen's
// for its data set's title.
struct HostLimits {
	SettingRange max_players;
	SettingRange time_limit;
	SettingRange kill_limit;
	SettingRange max_score;
	SettingRange respawn;
	SettingRange start_delay;
};

// The limits of title `title`: Delta Force Xtreme 2's for "dfx2", Joint
// Operations' for "jo" and any other (every title this service hosts runs
// Joint Operations' engine rules).
const HostLimits &host_limits_for_title(std::string_view title);

} // namespace opennova::novaworld_server::hosted
