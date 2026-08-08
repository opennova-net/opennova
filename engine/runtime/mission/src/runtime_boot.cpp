// S9 (ADR 0028): the mission boot policy — see runtime_boot.h for the order
// contract. The file-resolution rules here are structural translations of the
// shell resolvers they replace; each carries its witness.
#include "mission/runtime_boot.h"

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
	// Preserve the original fallback: medmssn.bin is used only when
	// <mission>.bin does not exist; a present but malformed table is passed
	// through and rejected downstream without fallback.
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

void parse_aip_profile_speeds(const std::vector<uint8_t> &text,
		PromoteOptions::AiProfileSpeeds &row) {
	// Line-oriented "<key> <value>" with tabs as spaces, keys compared
	// case-insensitively — the two witnessed AIProfile fields only.
	// [orig: AIProfile_ParseProperty "patrol_speed" @0x45E6DF..0x45E717 /
	//  "combat_speed" -> profile+0xC4]
	const char *p = reinterpret_cast<const char *>(text.data());
	const std::size_t n = text.size();
	std::size_t i = 0;
	while (i < n) {
		std::size_t end = i;
		while (end < n && p[end] != '\n') ++end;
		// Tokenize the line on spaces/tabs/CR.
		std::string key;
		std::string value;
		std::size_t t = i;
		auto skip_ws = [&] {
			while (t < end && (p[t] == ' ' || p[t] == '\t' || p[t] == '\r')) ++t;
		};
		auto take_token = [&] {
			std::string tok;
			while (t < end && p[t] != ' ' && p[t] != '\t' && p[t] != '\r')
				tok.push_back(p[t++]);
			return tok;
		};
		skip_ws();
		key = take_token();
		skip_ws();
		value = take_token();
		if (!key.empty() && !value.empty()) {
			for (char &c : key)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			// atoi-shape numeric read: leading sign + digits, junk tails
			// ignored (the shell resolver's int(String) behaved the same for
			// the authored corpus).
			const auto parse_int = [](const std::string &s) {
				int32_t v = 0;
				bool neg = false;
				std::size_t k = 0;
				if (k < s.size() && (s[k] == '-' || s[k] == '+')) {
					neg = s[k] == '-';
					++k;
				}
				for (; k < s.size() && s[k] >= '0' && s[k] <= '9'; ++k)
					v = v * 10 + (s[k] - '0');
				return neg ? -v : v;
			};
			if (key == "patrol_speed") row.patrol_speed = parse_int(value);
			else if (key == "combat_speed") row.combat_speed = parse_int(value);
		}
		i = end + 1;
	}
}

std::vector<PromoteOptions::AiProfileSpeeds> resolve_ai_profile_speeds(
		const BootFileSource &files, const bms::File &mission) {
	std::vector<PromoteOptions::AiProfileSpeeds> rows;
	if (!files.valid()) return rows;
	auto have = [&rows](const std::string &profile) {
		for (const PromoteOptions::AiProfileSpeeds &r : rows)
			if (r.profile == profile) return true;
		return false;
	};
	// The same entity walk order as the shell resolver it replaces (markers,
	// items, buildings, organics) — first occurrence wins the dedup, so the
	// order is semantic.
	const std::vector<bms::Entity> *groups[] = {
			&mission.markers, &mission.items, &mission.buildings,
			&mission.organics};
	for (const std::vector<bms::Entity> *group : groups) {
		for (const bms::Entity &entity : *group) {
			// ai_textfile, lowercase; first occurrence wins.
			const std::string profile =
					ascii_lower(entity.name2, sizeof(entity.name2));
			if (profile.empty() || have(profile)) continue;
			const std::string file_name = profile + ".aip";
			if (!files.has_file(file_name)) continue;
			std::vector<uint8_t> text;
			if (!files.read_file(file_name, text) || text.empty()) continue;
			PromoteOptions::AiProfileSpeeds row;
			row.profile = profile;
			parse_aip_profile_speeds(text, row);
			// A profile carrying neither key contributes no row, exactly like
			// the shell resolver this replaces (its dictionary stayed empty).
			if (row.patrol_speed == -1 && row.combat_speed == -1) continue;
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

BootAbort run_mission_boot(const BootParams &params, const BootSteps &steps) {
	if (params.has_resource_root && params.has_item_db)
		steps.install_seat_specs();
	if (params.has_resource_root) steps.install_ai_profile_speeds();
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
