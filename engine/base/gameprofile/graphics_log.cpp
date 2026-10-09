#include <base/gameprofile/graphics_log.h>

namespace opennova::gameprofile {

std::vector<GraphicsLogMission> graphics_log_missions(const std::string &text) {
	std::vector<GraphicsLogMission> out;
	static const std::string kMission = "Mission:\""; // [orig: Game_StartMission @ 0x5252fb, the format @ 0x7d0460]
	static const std::string kComplete = "Mission loading complete"; // [orig: @ 0x5262fd, the text @ 0x7d0314]
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		start = end + 1;
		if (line.compare(0, kMission.size(), kMission) == 0) {
			const size_t close = line.find('"', kMission.size());
			if (close != std::string::npos) out.push_back({line.substr(kMission.size(), close - kMission.size()), false});
		} else if (line.compare(0, kComplete.size(), kComplete) == 0 && !out.empty()) {
			out.back().complete = true;
		}
	}
	return out;
}

} // namespace opennova::gameprofile
