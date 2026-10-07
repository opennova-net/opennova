#include <runtime/mission/mission_sidecars.h>

namespace opennova::mission {

const std::vector<Sidecar> &sidecars() {
	// One row a reader, each opened from Game_StartMission's load [orig: Game_StartMission @ 0x524360].
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
	// Every reader replaces the extension from the name's first dot [orig: Path_ReplaceOrAppendExtension
	// @ 0x53c780, the scan @ 0x53c7c4; DialogManager_LoadFromFile's strtok @ 0x44e7bb].
	const size_t dot = name.find('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string sidecar_name(const std::string &mission_file, const Sidecar &sidecar) {
	return mission_base_name(mission_file) + sidecar.extension;
}

std::string sidecar_alternate_name(const std::string &mission_file, const Sidecar &sidecar) {
	if (sidecar.alternate == nullptr) return std::string();
	return mission_base_name(mission_file) + sidecar.alternate;
}

std::string dialog_bank_name(const std::string &mission_file, const std::string &header_slot) {
	// [orig: DialogSystem_Init @ 0x52760c: the header's slot when its first byte is set, else
	//  the mission's file name; the extension swapped from the first dot @ 0x52763e]
	return mission_base_name(header_slot.empty() ? mission_file : header_slot) + ".dbf";
}

std::string dialog_sounds_name(const std::string &dialog_bank, bool alternate) {
	// [orig: DialogManager_LoadFromFile @ 0x44e7bb strtok(filename, "."), "%s.lwf" @ 0x44e7d4, "%s.pwf"
	//  @ 0x44e7f5 when the first does not exist]
	return mission_base_name(dialog_bank) + (alternate ? ".pwf" : ".lwf");
}

} // namespace opennova::mission
