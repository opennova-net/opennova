// The mission text table — see mission_text.h.

#include <runtime/mission/mission_text.h>

#include <formats/rtxt/rtxt.h>

namespace opennova::mission {

std::string MissionText::people_name(int32_t index) const {
	const auto it = people_names.find(index);
	return it != people_names.end() ? it->second : std::string();
}

namespace {

char fold(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// The numeric-key section harvest: raw cp1252 values with only the ASCII
// section / key interpreted — the first section whose name matches, every
// entry whose key is the prefix plus a decimal suffix; the first value per
// suffix wins.
void harvest_indexed(const rtxt::File &table, const char *section_lc, std::size_t section_len,
		const char *prefix_lc, std::size_t prefix_len,
		std::unordered_map<int32_t, std::string> &out_map) {
	for (std::size_t section_index = 0; section_index < table.sections.size(); ++section_index) {
		const std::string &section_name = table.sections[section_index].name;
		if (section_name.size() != section_len) continue;
		bool is_match = true;
		for (std::size_t i = 0; i < section_len; ++i) {
			if (fold(section_name[i]) != section_lc[i]) {
				is_match = false;
				break;
			}
		}
		if (!is_match) continue;

		for (const rtxt::Entry *entry :
				table.get_section_entries(static_cast<uint32_t>(section_index))) {
			if (entry == nullptr || entry->key.size() <= prefix_len) continue;
			bool valid = true;
			for (std::size_t i = 0; i < prefix_len; ++i) {
				if (fold(entry->key[i]) != prefix_lc[i]) {
					valid = false;
					break;
				}
			}
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

	// [Locations] LOCATION%03i (type-2044 markers by one-based spawn order, the
	// S2C 0x0F deploy-map labels) and [PeopleNames] STRNAME%03i (the D-HUD-20
	// authored entity display names promote resolves from each record's
	// name_index — the witnessed resolve is cited at the promote.cpp port site).
	harvest_indexed(table, "locations", 9, "location", 8, out.location_texts);
	harvest_indexed(table, "peoplenames", 11, "strname", 7, out.people_names);
	out.loaded = true;
	return true;
}

} // namespace opennova::mission
