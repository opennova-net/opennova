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

namespace opennova::mission {

struct MissionText {
	bool loaded = false;
	std::string briefing3;
	// briefing2, or briefing when the table has no briefing2 (the witnessed
	// fallback signal).
	std::string briefing2;
	// Numeric suffix -> [Locations] LOCATION%03i value; the host bring-up
	// resolves each admitted type-2044 marker's name_index against these.
	std::unordered_map<int32_t, std::string> location_texts;
	// Numeric suffix -> [PeopleNames] STRNAME%03i value.
	std::unordered_map<int32_t, std::string> people_names;

	// The authored display name for a record's name_index; empty = none.
	std::string people_name(int32_t index) const;
	// The deploy-map name a type-2044 marker registers as it spawns: the
	// [Locations] value keyed by the record's own name_index (its +4 dword;
	// 0 keys LOCATION000 like any other number), else the LOCATION%03i key
	// string itself. [orig: Entity_SpawnFromBMSRecord: sprintf("LOCATION%03i",
	// [edi+4]) @0x40F196..0x40F1A4, the Locations lookup @0x40F1B8, the
	// key-string fallback @0x40F1E3..0x40F20A]
	std::string location_name(int32_t name_index) const;
};

// Harvests the RTXT bytes into `out` (reset first). False with `error` on a
// rejected table; `out` then stays unloaded.
bool parse_mission_text(const uint8_t *bytes, std::size_t size, MissionText &out,
		std::string &error);

} // namespace opennova::mission
