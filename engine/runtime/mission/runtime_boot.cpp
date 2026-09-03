// S9 (ADR 0028): the mission boot's file-resolution rules — structural
// translations of the shell resolvers they replace; each carries its witness.
// The boot ORDER is MissionKernel::boot (ADR 0043 slice E9).
#include <runtime/mission/runtime_boot.h>
#include <base/io/strutil.h>

#include <formats/aip/aip.h>

#include <cctype>
#include <cstring>

namespace opennova::mission {

namespace {

// Lowercased, whitespace-trimmed copy of a fixed char field (the shell
// resolver's strip_edges().to_lower()); the field is an ASCII .bms name, so
// strutil's C-locale whitespace set matches the shell's.
std::string ascii_lower(const char *data, std::size_t max_len) {
	std::size_t len = 0;
	while (len < max_len && data[len] != '\0') ++len;
	return strutil::to_lower(strutil::trim_view(std::string_view(data, len)));
}

} // namespace

MissionTextSource resolve_mission_text(const BootFileSource &files,
		const std::string &mission_file_basename, std::vector<uint8_t> &out) {
	out.clear();
	if (!files.valid()) return MissionTextSource::kNone;
	// Preserve the original fallback [orig: TextResource_LoadMissionTextBin
	// @ 0x51ed90]: medmssn.bin is used only when <mission>.bin does not
	// exist; a present but malformed table is passed through and rejected
	// downstream without fallback.
	if (!mission_file_basename.empty()) {
		const std::string mission_bin = mission_file_basename + ".bin";
		if (files.has_file(mission_bin)) {
			(void)files.read_file(mission_bin, out);
			return MissionTextSource::kMission;
		}
	}
	if (files.read_file("medmssn.bin", out) && !out.empty())
		return MissionTextSource::kFallback;
	out.clear();
	return MissionTextSource::kNone;
}

std::vector<PromoteOptions::AiProfileRow> resolve_ai_profiles(
		const BootFileSource &files, const bms::File &mission,
		const std::function<PromoteOptions::AiProfileDefaults(int32_t)> &
				ai_profile_defaults) {
	std::vector<PromoteOptions::AiProfileRow> rows;
	if (!files.valid()) return rows;
	auto have = [&rows](const std::string &profile) {
		for (const PromoteOptions::AiProfileRow &r : rows)
			if (r.profile == profile) return true;
		return false;
	};
	// The same entity walk order as the shell resolver it replaces (markers,
	// items, buildings, organics) — first occurrence wins the dedup, so the
	// order is semantic.
	struct Group { const std::vector<bms::Entity> *entities; bool placed_item; };
	const Group groups[] = {
			{&mission.markers, false}, {&mission.items, true},
			{&mission.buildings, false}, {&mission.organics, false}};
	for (const Group &group : groups) {
		for (const bms::Entity &entity : *group.entities) {
			// The name retail's AI init would load for this record: the
			// ai_textfile, else (a placed vehicle item) the def's default_aip
			// or "helo1" — lowercase; first occurrence wins.
			const std::string profile = ai_profile_name_for(
					entity, group.placed_item, ai_profile_defaults);
			if (profile.empty() || have(profile)) continue;
			const std::string file_name = profile + ".aip";
			if (!files.has_file(file_name)) continue;
			std::vector<uint8_t> text;
			if (!files.read_file(file_name, text) || text.empty()) continue;
			// The parse itself is format knowledge (engine/formats/aip,
			// partial-port documented there); this resolver owns only the
			// profile walk and the install row.
			aip::Profile parsed = aip::parse_profile(text.data(), text.size());
			// A file that parsed no witnessed field contributes no row, like
			// the speeds-only resolver this extends (its dictionary stayed
			// empty for such files).
			if (parsed.type == 0 && parsed.patrol_speed == -1 &&
					parsed.combat_speed == -1)
				continue;
			PromoteOptions::AiProfileRow row;
			row.profile = profile;
			row.data = std::move(parsed);
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

} // namespace opennova::mission
