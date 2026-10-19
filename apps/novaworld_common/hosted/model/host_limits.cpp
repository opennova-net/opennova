#include "host_limits.h"

namespace opennova::novaworld_server::hosted {

namespace {

// Joint Operations' host screen. Service policy, not a witnessed limit (the
// host screen's spin ranges are unread): 64 is the screen's MAX_PLAYERS cap,
// and 500 the kill limit's and the max score's "no limit".
constexpr HostLimits kJointOperationsLimits = {
	{ 2, 64 },  // max_players
	{ 1, 300 }, // time_limit (minutes)
	{ 1, 500 }, // kill_limit
	{ 1, 500 }, // max_score
	{ 0, 300 }, // respawn (seconds)
	{ 0, 300 }, // start_delay (seconds)
};

// Delta Force Xtreme 2's host screen: Joint Operations' ranges but for its
// MAX_PLAYERS cap, 100 to JO's 64 (the cap word dword_835F0C, which the host
// settings' read, slider map and fill take [orig: ReadSettings @0x553dd0,
// the slider map @0x5540b0, PopulateHostSettings @0x5544cd (dfx2.exe)]; a
// hosted server is a dedicated host). Its kill limit's and max score's "no
// limit" 500 is the same code as JO's; the other ranges are unread in both.
// docs/titles/dfx2-vs-jo-re.md DFX2-8.
constexpr HostLimits kDeltaForceXtreme2Limits = {
	{ 2, 100 }, // max_players
	{ 1, 300 }, // time_limit (minutes)
	{ 1, 500 }, // kill_limit
	{ 1, 500 }, // max_score
	{ 0, 300 }, // respawn (seconds)
	{ 0, 300 }, // start_delay (seconds)
};

} // namespace

const HostLimits &host_limits_for_title(std::string_view title) {
	if (title == "dfx2") return kDeltaForceXtreme2Limits;
	// "jo", and OpenNova's own data, which speaks Joint Operations' identity.
	return kJointOperationsLimits;
}

} // namespace opennova::novaworld_server::hosted
