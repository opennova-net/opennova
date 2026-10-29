#include "host_limits.h"

#include <base/title/title.h>

#include <cstdint>

namespace opennova::novaworld_server::hosted {

namespace {

// The host screen's ranges, the same code in Joint Operations and Delta Force Xtreme 2
// (docs/titles/dfx2-vs-jo-re.md DFX2-8): the live settings accept retail SET's signed words
// (including the kill/score "no limit" sentinel 500); Respawn clamps negative values to zero.
// The MAX_PLAYERS cap is the title's (host_limits_for_title).
constexpr HostLimits kHostScreenLimits = {
	{ 1, 0 }, // max_players (the title's cap)
	{ INT32_MIN, INT32_MAX }, // retail SET words, including sentinels
	{ INT32_MIN, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
	{ 0, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
};

} // namespace

HostLimits host_limits_for_title(std::string_view title) {
	// The title's host screen's MAX_PLAYERS cap (Title::max_players_host_screen): Joint
	// Operations' 64, Delta Force Xtreme 2's 100 (the cap word dword_835F0C, which the host
	// settings' read, slider map and fill take [orig: ReadSettings @0x553dd0, the slider map
	// @0x5540b0, PopulateHostSettings @0x5544cd (dfx2.exe)]; a hosted server is a dedicated
	// host). An unknown title code is the default title's.
	const Title *named = title_by_code(title);
	const Title &resolved = named != nullptr ? *named : default_title();
	HostLimits limits = kHostScreenLimits;
	limits.max_players.max = resolved.max_players_host_screen;
	return limits;
}

} // namespace opennova::novaworld_server::hosted
