#pragma once

#include <runtime/inmatch/mission_exit.h> // kMissionExitNovaWorld

#include <cstdint>

namespace opennova::inmatch {

// The NovaWorld UDP (NWU) session's hosting/playing word (session+0x128, dword_B60108).
inline constexpr int32_t kNwuSessionRoleHosting = 2;
inline constexpr int32_t kNwuSessionRolePlaying = 3;

// The main frame's NovaWorld exit, inside the 62-frame block while in a session and ahead of
// the quality sample: a NovaWorld network type whose NWU session is in use and whose
// hosting/playing word holds neither 2 nor 3 ends the mission.
// [orig: Game_ProcessMainFrame @0x52654f..0x52657c — `cmp is_in_session` @0x52654f,
//  `cmp transport_mode, 1` @0x52655d, `cmp dword_B5FD2C, 0` @0x526565, the word
//  @0x52656d..0x52657a]
inline bool novaworld_session_ended(bool novaworld, bool nwu_in_use, int32_t nwu_session_role) {
	return novaworld && nwu_in_use && nwu_session_role != kNwuSessionRoleHosting &&
			nwu_session_role != kNwuSessionRolePlaying;
}

} // namespace opennova::inmatch
