// The mission text table — see mission_text.h.

#include <runtime/mission/mission_text.h>

#include <runtime/hud/game_text_lookup.h>

namespace opennova::mission {

const rtxt::Entry *MissionText::find(const std::string &section, const std::string &key) const {
	// [orig: TextResource_FindEntryBySectionAndKey @0x75D250: the override
	//  table first @0x75D27B (the mission's table is never it), then the
	//  table asked; a null table finds nothing @0x75D27F]
	if (override_table != nullptr) {
		if (const rtxt::Entry *entry = override_table->find_in_section(section, key)) return entry;
	}
	return loaded ? table.find_in_section(section, key) : nullptr;
}

std::string MissionText::get_string(const std::string &section, const std::string &key) const {
	if (!loaded) return std::string(); // [orig: @0x51EB57]
	const rtxt::Entry *entry = find(section, key);
	return entry != nullptr ? entry->text : std::string();
}

std::string MissionText::briefing3() const {
	return get_string("info", "briefing3");
}

std::string MissionText::briefing2() const {
	// An empty briefing2 is the signal to read briefing [orig: @0x506649..0x506660].
	std::string page = get_string("info", "briefing2");
	if (page.empty()) page = get_string("info", "briefing");
	return page;
}

std::string MissionText::people_name(int32_t index) const {
	if (!loaded) return std::string(); // [orig: `test ebx, ebx` @0x40ECCA]
	const rtxt::Entry *entry =
			find(hud::kPeopleNameKey.section, hud::text_key(hud::kPeopleNameKey, index));
	return entry != nullptr ? entry->text : std::string();
}

std::string MissionText::location_name(int32_t name_index) const {
	const std::string key = hud::text_key(hud::kLocationKey, name_index);
	const rtxt::Entry *entry = find(hud::kLocationKey.section, key);
	return entry != nullptr ? entry->text : key;
}

bool parse_mission_text(const uint8_t *bytes, std::size_t size, MissionText &out,
		std::string &error) {
	out = MissionText();
	if (!rtxt::parse(bytes, size, out.table, error)) {
		out.table = rtxt::File();
		return false;
	}
	out.loaded = true;
	return true;
}

} // namespace opennova::mission
