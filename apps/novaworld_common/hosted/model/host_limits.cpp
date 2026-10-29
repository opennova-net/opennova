#include "host_limits.h"
#include <cstdint>

namespace opennova::novaworld_server::hosted {

namespace {

// Joint Operations' MAX_PLAYERS cap is 64. Other live settings accept retail
// SET's signed words (including the kill/score "no limit" sentinel 500);
// Respawn clamps negative values to zero.
constexpr HostLimits kJointOperationsLimits = {
	{ 1, 64 },
	{ INT32_MIN, INT32_MAX }, // retail SET words, including sentinels
	{ INT32_MIN, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
	{ 0, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
};

// Delta Force Xtreme 2's host screen: Joint Operations' ranges but for its
// MAX_PLAYERS cap, 100 to JO's 64 (the cap word dword_835F0C, which the host
// settings' read, slider map and fill take [orig: ReadSettings @0x553dd0,
// the slider map @0x5540b0, PopulateHostSettings @0x5544cd (dfx2.exe)]; a
// hosted server is a dedicated host). Its kill limit's and max score's "no
// limit" 500 is the same code as JO's; the other ranges are unread in both.
// docs/titles/dfx2-vs-jo-re.md DFX2-8.
constexpr HostLimits kDeltaForceXtreme2Limits = {
	{ 1, 100 },
	{ INT32_MIN, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
	{ 0, INT32_MAX },
	{ INT32_MIN, INT32_MAX },
};

} // namespace

const HostLimits &host_limits_for_title(std::string_view title) {
	if (title == "dfx2") return kDeltaForceXtreme2Limits;
	// "jo", and OpenNova's own data, which speaks Joint Operations' identity.
	return kJointOperationsLimits;
}

} // namespace opennova::novaworld_server::hosted
