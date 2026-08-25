// S9 (ADR 0028): the mission boot policy — see runtime_boot.h for the order
// contract. The file-resolution rules here are structural translations of the
// shell resolvers they replace; each carries its witness.
#include "mission/runtime_boot.h"

#include <aip/aip.h>

#include <cctype>
#include <cstring>

namespace opennova::mission {

namespace {

// Lowercased, whitespace-trimmed copy of a fixed char field (the shell
// resolver's strip_edges().to_lower()).
std::string ascii_lower(const char *data, std::size_t max_len) {
	std::string out;
	out.reserve(max_len);
	for (std::size_t i = 0; i < max_len && data[i] != '\0'; ++i) {
		char c = data[i];
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		out.push_back(c);
	}
	const auto is_ws = [](char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	};
	std::size_t begin = 0;
	while (begin < out.size() && is_ws(out[begin])) ++begin;
	std::size_t end = out.size();
	while (end > begin && is_ws(out[end - 1])) --end;
	return out.substr(begin, end - begin);
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

BootAbort run_mission_boot(const BootParams &params, const BootSteps &steps) {
	if (params.has_resource_root && params.has_item_db)
		steps.install_seat_specs();
	if (params.has_resource_root) steps.install_ai_profiles();
	if (params.has_terrain_til) steps.install_terrain_til();
	steps.install_mission_text();
	if (!steps.load_mission()) return BootAbort::kLoadFailed;
	if (params.has_terrain) steps.install_terrain_field();
	if (params.has_resource_root) steps.install_sound_profiles();
	if (params.has_resource_root) steps.install_infantry_anim();
	if (params.has_resource_root && params.has_wac) steps.install_wac();
	if (params.playable && !params.is_joiner) steps.spawn_local_player();
	if (params.has_resource_root && params.has_item_db)
		steps.resolve_infantry_adm();
	if (params.has_item_db) steps.resolve_item_traits();
	if (params.has_item_db) {
		if (params.has_resource_root) steps.install_asset_root();
		steps.resolve_collision();
		steps.occlusion_init();
	}
	if (params.has_resource_root) {
		steps.load_weapon_table();
		const bool ammo_ok = steps.load_ammo_table();
		if (ammo_ok && params.has_item_db) steps.resolve_ai_weapons();
	}
	return BootAbort::kNone;
}

} // namespace opennova::mission
