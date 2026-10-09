#pragma once

// THE MISSION TEXT TABLE: what a running match reads of the mounted
// <mission>.bin RTXT (or its medmssn.bin fallback, runtime_boot.h) — the
// briefing pages the host's initial-state burst streams (S2C 0x7E), the
// [Locations] LOCATION%03i deploy-map labels (S2C 0x0F) and the
// [PeopleNames] STRNAME%03i display names promote resolves (D-HUD-20). The
// values keep the table's raw cp1252 bytes so the wire payloads stay
// byte-exact for localized text.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <formats/mission/bms.h>

namespace opennova::mission {

struct MissionText {
	bool loaded = false;
	std::string briefing3;
	// briefing2, or briefing when the table has no briefing2 (the witnessed
	// fallback signal).
	std::string briefing2;
	// Numeric suffix -> [Locations] LOCATION%03i value; the host bring-up
	// resolves these against the type-2044 markers in spawn order.
	std::unordered_map<int32_t, std::string> location_texts;
	// Numeric suffix -> [PeopleNames] STRNAME%03i value.
	std::unordered_map<int32_t, std::string> people_names;

	// The authored display name for a record's name_index; empty = none.
	std::string people_name(int32_t index) const;
};

// Harvests the RTXT bytes into `out` (reset first). False with `error` on a
// rejected table; `out` then stays unloaded.
bool parse_mission_text(const uint8_t *bytes, std::size_t size, MissionText &out,
		std::string &error);

// The [Locations] number of each of a mission's markers, in the pool's order:
// each named-location marker (def type 2044) registers the next location name
// as it spawns, from 1, in spawn order (its LOCATION%03i key); 0 for any other
// marker. [orig: Entity_SpawnFromBMSRecord @0x40f182..0x40f221]
std::vector<int32_t> location_numbers(const std::vector<bms::Entity> &markers);

} // namespace opennova::mission
