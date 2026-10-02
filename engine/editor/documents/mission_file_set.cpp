// The mission's file set (mission_file_set.h): the sidecar table's rows as the references the
// mission makes, and the members a scan has for a mission.
#include "mission_file_set.h"

#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

namespace {

// The rows, by the sidecar table's roles [orig: each reader builds the mission's base name plus its
// own extension, Game_StartMission @0x524360]: the string table (else medmssn.bin, which the
// mission's text falls back to [orig: TextResource_LoadMissionTextBin @0x51ed90]; with neither the
// game runs and every key the mission reads answers "", a warning of its own kind); the script (40 of
// 115 shipped missions have one: the game runs without); the loading image (else loadscrn.pcx); the
// tile placement (every shipped mission has one; the game loads the terrain's own without it, a
// warning); the dialog bank and its sounds (17 of 115: the game plays no dialog without them; a
// record that uses a dialog names the bank itself, tolerated as a warning). The sounds are read only
// beside the .dbf (GraphEdge::needs): without one they are no reference at all, and with one they are
// no longer optional, the bank's dialogs playing silent without them (a warning, the sound bank
// kind's: review F5, the data lane's m13).
constexpr MissionFileSetRow kRows[] = {
	{"text", ReferenceKind::MissionStrings, false, "its string table"},
	{"script", ReferenceKind::Script, true, "its script"},
	{"loading_image", ReferenceKind::LoadingImage, true, "its loading image"},
	{"tiles", ReferenceKind::TilePlacement, false, "its tile placement"},
	{"dialog", ReferenceKind::DialogBank, true, "its dialog bank"},
	{"dialog_sounds", ReferenceKind::SoundBank, false, "its dialog bank's sounds"},
};

} // namespace

const std::vector<MissionFileSetRow> &mission_file_set() {
	static const std::vector<MissionFileSetRow> rows = [] {
		std::vector<MissionFileSetRow> out;
		// In the sidecar table's order, every role of it a row here (the table is the witness).
		for (const mission::Sidecar &sidecar : mission::sidecars())
			for (const MissionFileSetRow &row : kRows)
				if (std::string(row.role) == sidecar.role) out.push_back(row);
		return out;
	}();
	return rows;
}

const MissionFileSetRow *mission_file_set_row(const std::string &role) {
	for (const MissionFileSetRow &row : mission_file_set())
		if (role == row.role) return &row;
	return nullptr;
}

std::vector<MissionFileSetMember> mission_file_set_members(const AssetScan &scan, const std::string &mission,
                                                           const std::string &new_mission) {
	std::vector<MissionFileSetMember> out;
	for (const MissionFileSetRow &row : mission_file_set()) {
		const mission::Sidecar *sidecar = mission::sidecar_for_role(row.role);
		if (!sidecar) continue;
		// A row the reader reads only beside another's file (the dialog's sounds, beside its .dbf
		// [orig: DialogSystem_Init @ 0x5275e0, the exists check @ 0x527648]): without that file a file
		// of its name is no member (a game bank a mission is named like).
		if (sidecar->needs) {
			const mission::Sidecar *needed = mission::sidecar_for_role(sidecar->needs);
			if (!needed || !scan.find(mission::sidecar_name(mission, *needed))) continue;
		}
		for (const std::string &name : {mission::sidecar_name(mission, *sidecar), mission::sidecar_alternate_name(mission, *sidecar)}) {
			if (name.empty()) continue;
			const AssetEntry *entry = scan.find(name);
			if (!entry) continue;
			MissionFileSetMember member;
			member.role = row.role;
			member.path = entry->relative_path;
			member.old_name = entry->logical_name;
			// The mission's new base name with the file's own extension (the alternate's where that
			// was found), the file's case of the extension kept.
			const std::string extension = entry->logical_name.substr(mission::mission_base_name(entry->logical_name).size());
			member.new_name = mission::mission_base_name(new_mission) + extension;
			out.push_back(std::move(member));
			break;
		}
	}
	return out;
}

} // namespace opennova::editor
