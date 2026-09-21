// The <mission>.bin RTXT harvest (runtime/mission/mission_text.h): the
// briefing pages with the witnessed briefing2 -> briefing fallback
// [orig: @0x506649..0x506660], the [Locations] LOCATION%03i and [PeopleNames]
// STRNAME%03i numeric-key harvests (case-insensitive section / prefix, the
// first value per suffix wins, non-numeric suffixes skipped, raw cp1252 bytes
// kept), the people-name resolve, and a rejected table.
#include <formats/rtxt/rtxt.h>
#include <runtime/mission/mission_text.h>

#include <cstdio>
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

} // namespace

int main() {
	// The full table: both briefing pages present, mixed-case section and key
	// spellings, a non-numeric and an over-long suffix, a duplicate suffix.
	{
		const std::vector<uint8_t> bytes = table_bytes({
				{ "Info", "Briefing3", "Page three" },
				{ "Info", "BRIEFING2", "Page two" },
				{ "Info", "briefing", "Page one" },
				{ "LOCATIONS", "Location001", "Harbor" },
				{ "LOCATIONS", "LOCATION12", "Ridge\xE9" }, // cp1252 byte kept raw
				{ "LOCATIONS", "LOCATION001", "Harbor twin" }, // the first value wins
				{ "LOCATIONS", "LOCATIONX", "not a number" },
				{ "LOCATIONS", "LOCATION", "no suffix" },
				{ "LOCATIONS", "LOCATION99999999999", "overflow" },
				{ "PeopleNames", "STRNAME003", "Sgt. Vega" },
				{ "PeopleNames", "strname7", "Cpl. Ruiz" },
		});
		mission::MissionText text;
		std::string error;
		CHECK(mission::parse_mission_text(bytes.data(), bytes.size(), text, error));
		CHECK(text.loaded);
		CHECK(text.briefing3 == "Page three");
		CHECK(text.briefing2 == "Page two");
		CHECK(text.location_texts.size() == 2);
		CHECK(text.location_texts.count(1) == 1 && text.location_texts.at(1) == "Harbor");
		CHECK(text.location_texts.count(12) == 1 && text.location_texts.at(12) == "Ridge\xE9");
		CHECK(text.people_names.size() == 2);
		CHECK(text.people_name(3) == "Sgt. Vega");
		CHECK(text.people_name(7) == "Cpl. Ruiz");
		CHECK(text.people_name(4).empty());
	}

	// No briefing2: briefing stands in; no numeric sections: empty maps.
	{
		const std::vector<uint8_t> bytes = table_bytes({
				{ "info", "briefing3", "Three" },
				{ "info", "briefing", "One" },
		});
		mission::MissionText text;
		text.people_names[1] = "stale"; // the parse resets the table first
		std::string error;
		CHECK(mission::parse_mission_text(bytes.data(), bytes.size(), text, error));
		CHECK(text.briefing3 == "Three" && text.briefing2 == "One");
		CHECK(text.location_texts.empty() && text.people_names.empty());
	}

	// A rejected table: false with an error, nothing loaded.
	{
		const uint8_t junk[] = { 'J', 'U', 'N', 'K', 0, 0, 0, 0 };
		mission::MissionText text;
		text.loaded = true;
		text.briefing3 = "stale";
		std::string error;
		CHECK(!mission::parse_mission_text(junk, sizeof(junk), text, error));
		CHECK(!error.empty());
		CHECK(!text.loaded && text.briefing3.empty());
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("mission_text_test OK\n");
	return 0;
}
