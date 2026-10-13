// The <mission>.bin RTXT table (runtime/mission/mission_text.h): retail's
// whole-key lookups, the expansion's override table first. The briefing pages
// with the witnessed briefing2 -> briefing fallback [orig: @0x506649..0x506660];
// the [Locations] LOCATION%03i and [PeopleNames] STRNAME%03i reads, each a
// sprintf'd key matched whole by stricmp in the first matching section, the
// first match winning [orig: TextResource_FindKeyInSection @0x75D1E0,
// @0x75D205], so a key whose digits are not the %03i form answers nothing; the
// override table searched first [orig: TextResource_FindEntryBySectionAndKey
// @0x75D250, @0x75D27B], and the location read alone answering without a
// mission table [orig: Entity_SpawnFromBMSRecord @0x40ECCA against @0x40F1A9];
// a rejected table; a location's name by its record's name_index.
#include <formats/rtxt/rtxt.h>
#include <runtime/mission/mission_text.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

struct Row {
	const char *section;
	const char *key;
	const char *text;
};

// Builds a grouped RTXT table from (section, key, text) rows in order.
std::vector<uint8_t> table_bytes(const std::vector<Row> &rows) {
	rtxt::File file;
	for (const Row &row : rows) {
		if (file.sections.empty() || file.sections.back().name != row.section)
			file.sections.push_back({ row.section, 0 });
		rtxt::Entry entry;
		entry.key = row.key;
		entry.text = row.text;
		entry.section_index = static_cast<uint32_t>(file.sections.size() - 1);
		file.entries.push_back(entry);
		++file.sections.back().string_count;
	}
	std::vector<uint8_t> out;
	std::string error;
	if (!rtxt::write(file, out, error)) {
		std::printf("FAIL rtxt::write: %s\n", error.c_str());
		++failures;
	}
	return out;
}

// A table built from the rows, parsed back.
rtxt::File parsed(const std::vector<Row> &rows) {
	const std::vector<uint8_t> bytes = table_bytes(rows);
	rtxt::File file;
	std::string error;
	if (!rtxt::parse(bytes.data(), bytes.size(), file, error)) {
		std::printf("FAIL rtxt::parse: %s\n", error.c_str());
		++failures;
	}
	return file;
}

} // namespace

int main() {
	// The full table: both briefing pages present, mixed-case section and key
	// spellings, keys whose digits are not the %03i form, a duplicate key.
	{
		const std::vector<uint8_t> bytes = table_bytes({
				{ "Info", "Briefing3", "Page three" },
				{ "Info", "BRIEFING2", "Page two" },
				{ "Info", "briefing", "Page one" },
				{ "LOCATIONS", "LOCATION0001", "four digits" }, // not LOCATION001's key
				{ "LOCATIONS", "Location001", "Harbor" },
				{ "LOCATIONS", "LOCATION12", "two digits" }, // not LOCATION012's key
				{ "LOCATIONS", "LOCATION012", "Ridge\xE9" }, // cp1252 byte kept raw
				{ "LOCATIONS", "LOCATION001", "Harbor twin" }, // the first match wins
				{ "LOCATIONS", "LOCATIONX", "not a number" },
				{ "LOCATIONS", "LOCATION", "no suffix" },
				{ "PeopleNames", "STRNAME003", "Sgt. Vega" },
				{ "PeopleNames", "strname7", "two short" },
				{ "PeopleNames", "strname007", "Cpl. Ruiz" },
		});
		mission::MissionText text;
		std::string error;
		CHECK(mission::parse_mission_text(bytes.data(), bytes.size(), text, error));
		CHECK(text.loaded);
		CHECK(text.briefing3() == "Page three");
		CHECK(text.briefing2() == "Page two");
		CHECK(text.location_name(1) == "Harbor");
		CHECK(text.location_name(12) == "Ridge\xE9");
		CHECK(text.location_name(2) == "LOCATION002");
		CHECK(text.people_name(3) == "Sgt. Vega");
		CHECK(text.people_name(7) == "Cpl. Ruiz");
		CHECK(text.people_name(4).empty());
	}

	// No briefing2: briefing stands in; no numeric sections: no names.
	{
		const std::vector<uint8_t> bytes = table_bytes({
				{ "info", "briefing3", "Three" },
				{ "info", "briefing", "One" },
		});
		mission::MissionText text;
		text.loaded = true;
		text.override_table = std::make_shared<rtxt::File>(); // the parse resets the table first
		std::string error;
		CHECK(mission::parse_mission_text(bytes.data(), bytes.size(), text, error));
		CHECK(text.override_table == nullptr);
		CHECK(text.briefing3() == "Three" && text.briefing2() == "One");
		CHECK(text.location_name(1) == "LOCATION001" && text.people_name(1).empty());
	}

	// A rejected table: false with an error, nothing loaded.
	{
		const uint8_t junk[] = { 'J', 'U', 'N', 'K', 0, 0, 0, 0 };
		mission::MissionText text;
		text.loaded = true;
		std::string error;
		CHECK(!mission::parse_mission_text(junk, sizeof(junk), text, error));
		CHECK(!error.empty());
		CHECK(!text.loaded && text.briefing3().empty());
	}

	// The expansion's override table answers every lookup first, its own
	// first section of the name and first key of the name winning; the
	// mission's table answers what it lacks.
	{
		mission::MissionText text;
		text.table = parsed({
				{ "info", "briefing3", "Mission three" },
				{ "info", "briefing2", "Mission two" },
				{ "Locations", "LOCATION001", "Harbor" },
				{ "Locations", "LOCATION002", "Ridge" },
				{ "PeopleNames", "STRNAME003", "Sgt. Vega" },
				{ "PeopleNames", "STRNAME004", "Cpl. Ruiz" },
		});
		text.loaded = true;
		auto override_table = std::make_shared<rtxt::File>(parsed({
				{ "INFO", "Briefing3", "Override three" },
				{ "locations", "location001", "Override harbor" },
				{ "Locations", "LOCATION002", "a second section's" },
				{ "PeopleNames", "STRNAME003", "Override Vega" },
		}));
		text.override_table = override_table;
		CHECK(text.briefing3() == "Override three");
		CHECK(text.briefing2() == "Mission two");
		CHECK(text.location_name(1) == "Override harbor");
		CHECK(text.location_name(2) == "Ridge"); // the override's first Locations lacks it
		CHECK(text.people_name(3) == "Override Vega");
		CHECK(text.people_name(4) == "Cpl. Ruiz");

		// With no mission table the location read still asks the override
		// table; the people names and the briefings test the table first.
		mission::MissionText alone;
		alone.override_table = override_table;
		CHECK(alone.location_name(1) == "Override harbor");
		CHECK(alone.location_name(2) == "LOCATION002");
		CHECK(alone.people_name(3).empty());
		CHECK(alone.briefing3().empty());
	}

	// A location's name: the [Locations] value its record's name_index keys (0 keys LOCATION000
	// like any other number), else the LOCATION%03i key string itself.
	// [orig: Entity_SpawnFromBMSRecord @0x40F196..0x40F20A]
	{
		mission::MissionText text;
		text.table = parsed({
				{ "Locations", "LOCATION000", "SEA ANGEL Base" },
				{ "Locations", "LOCATION004", "Ridge" },
				{ "Locations", "LOCATION1234", "Four digits" },
		});
		text.loaded = true;
		CHECK(text.location_name(0) == "SEA ANGEL Base");
		CHECK(text.location_name(4) == "Ridge");
		CHECK(text.location_name(6) == "LOCATION006");
		CHECK(text.location_name(1234) == "Four digits");
		CHECK(text.location_name(-1) == "LOCATION-01");
		CHECK(mission::MissionText().location_name(0) == "LOCATION000");
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("mission_text_test OK\n");
	return 0;
}
