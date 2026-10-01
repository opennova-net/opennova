#include <runtime/mission/mission_sidecars.h>

namespace opennova::mission {

const std::vector<Sidecar> &sidecars() {
	// One row a reader, in mission-start order [orig: Game_StartMission @ 0x524360].
	static const std::vector<Sidecar> rows = {
		// [orig: TextResource_LoadMissionTextBin @ 0x51ed90]
		{"text", ".bin", nullptr, "medmssn.bin", nullptr},
		// [orig: WacScript_InitAndLoad @ 0x4f91f0]
		{"script", ".wac", nullptr, nullptr, nullptr},
		// [orig: Render_LoadingScreen @ 0x521d10]
		{"loading_image", ".pcx", nullptr, "loadscrn.pcx", nullptr},
		// [orig: Terrain_LoadTileInfoFile @ 0x60a740]
		{"tiles", ".til", nullptr, nullptr, nullptr},
		// [orig: DialogSystem_Init @ 0x5275e0]
		{"dialog", ".dbf", nullptr, nullptr, nullptr},
		// [orig: DialogManager_LoadFromFile @ 0x44e650]
		{"dialog_sounds", ".lwf", ".pwf", nullptr, "dialog"},
	};
	return rows;
}

const Sidecar *sidecar_for_role(const std::string &role) {
	for (const Sidecar &row : sidecars()) {
		if (role == row.role) return &row;
	}
	return nullptr;
}

std::string mission_base_name(const std::string &mission_file) {
	const size_t slash = mission_file.find_last_of("/\\");
	const std::string name = slash == std::string::npos ? mission_file : mission_file.substr(slash + 1);
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string sidecar_name(const std::string &mission_file, const Sidecar &sidecar) {
	return mission_base_name(mission_file) + sidecar.extension;
}

std::string sidecar_alternate_name(const std::string &mission_file, const Sidecar &sidecar) {
	if (sidecar.alternate == nullptr) return std::string();
	return mission_base_name(mission_file) + sidecar.alternate;
}

} // namespace opennova::mission
