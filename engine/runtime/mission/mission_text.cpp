// The mission text table — see mission_text.h.

#include <runtime/mission/mission_text.h>

#include <base/io/strutil.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/hud/game_text_lookup.h>

#include <string_view>

namespace opennova::mission {

std::string MissionText::people_name(int32_t index) const {
	const auto it = people_names.find(index);
	return it != people_names.end() ? it->second : std::string();
}

std::string MissionText::location_name(int32_t name_index) const {
	const auto it = location_texts.find(name_index);
	return it != location_texts.end() ? it->second : hud::text_key(hud::kLocationKey, name_index);
}

namespace {

// The numeric-key section harvest: raw cp1252 values with only the ASCII
// section / key interpreted — the first section whose name is the form's
// (ASCII case ignored), every entry whose key is the form's prefix (case
// ignored) plus a decimal suffix; the first value per suffix wins.
void harvest_indexed(const rtxt::File &table, const hud::TextKeyForm &form,
		std::unordered_map<int32_t, std::string> &out_map) {
	const std::string_view prefix(form.prefix);
	const std::size_t prefix_len = prefix.size();
	for (std::size_t section_index = 0; section_index < table.sections.size(); ++section_index) {
		if (!strutil::iequals(table.sections[section_index].name, form.section)) continue;

		for (const rtxt::Entry *entry :
				table.get_section_entries(static_cast<uint32_t>(section_index))) {
			if (entry == nullptr || entry->key.size() <= prefix_len) continue;
			bool valid = strutil::iequals(std::string_view(entry->key).substr(0, prefix_len), prefix);
			int32_t index = 0;
			for (std::size_t i = prefix_len; valid && i < entry->key.size(); ++i) {
				const char digit = entry->key[i];
				// Stop one digit short of INT32_MAX: 214748364 * 10 + 8 overflows.
				if (digit < '0' || digit > '9' || index > 214748364 ||
						(index == 214748364 && digit > '7')) {
					valid = false;
					break;
				}
				index = index * 10 + (digit - '0');
			}
			if (valid) out_map.emplace(index, entry->text);
		}
		break;
	}
}

} // namespace

bool parse_mission_text(const uint8_t *bytes, std::size_t size, MissionText &out,
		std::string &error) {
	out = MissionText();
	rtxt::File table;
	if (!rtxt::parse(bytes, size, table, error)) return false;

	// Preserve the table's raw cp1252 bytes. Retail uses an empty briefing2 as
	// the signal to fall back to briefing [orig: @0x506649..0x506660].
	if (const rtxt::Entry *e = table.find_in_section("info", "briefing3")) out.briefing3 = e->text;
	if (const rtxt::Entry *e = table.find_in_section("info", "briefing2")) out.briefing2 = e->text;
	if (out.briefing2.empty()) {
		if (const rtxt::Entry *e = table.find_in_section("info", "briefing")) out.briefing2 = e->text;
	}

	// [Locations] LOCATION%03i (each type-2044 marker's by its name_index, the
	// S2C 0x0F deploy-map labels) and [PeopleNames] STRNAME%03i (the D-HUD-20
	// authored entity display names promote resolves from each record's
	// name_index — the witnessed resolve is cited at the promote.cpp port site).
	harvest_indexed(table, hud::kLocationKey, out.location_texts);
	harvest_indexed(table, hud::kPeopleNameKey, out.people_names);
	out.loaded = true;
	return true;
}

} // namespace opennova::mission
