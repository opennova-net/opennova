#pragma once

#include <string>
#include <vector>

namespace opennova::gameprofile {

// The graphics log the game writes in its working directory, made anew by the first line a
// process writes and appended to after, never read by the game [orig: CGfxDevice_WriteLogEntry @
// 0x67cc80, "wt+" @ 0x67cca3, "a" @ 0x67cda6].
inline constexpr const char *kGraphicsLogName = "ghw.txt";

// The missions a graphics log says the game began loading, each with whether it says the load finished:
// a `Mission:"<file>" - ...` line as Game_StartMission begins [orig: Game_StartMission @ 0x5252fb, the
// format @ 0x7d0460], and "Mission loading complete" as it ends [orig: @ 0x5262fd, the text @ 0x7d0314].
struct GraphicsLogMission {
	std::string file;
	bool complete = false;
};
std::vector<GraphicsLogMission> graphics_log_missions(const std::string &text);

} // namespace opennova::gameprofile
