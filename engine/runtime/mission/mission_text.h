#pragma once

// THE MISSION TEXT TABLE: what a running match reads of the mounted
// <mission>.bin RTXT (or its medmssn.bin fallback, runtime_boot.h) — the
// briefing pages the host's initial-state burst streams (S2C 0x7E), the
// [Locations] LOCATION%03i deploy-map labels (S2C 0x0F) and the
// [PeopleNames] STRNAME%03i display names promote resolves (D-HUD-20). Every
// read is retail's whole-key lookup, the expansion's override table first;
// the values keep the tables' raw cp1252 bytes so the wire payloads stay
// byte-exact for localized text.

#include <formats/rtxt/rtxt.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace opennova::mission {

struct MissionText {
	// The mission's table parsed (g_TextMission is set) [orig: g_TextMission
	// @0xB4C2B4, TextResource_LoadMissionTextBin @0x51ED90].
	bool loaded = false;
	rtxt::File table;
	// The expansion's text-override table, null for none: every lookup
	// searches it before the mission's table [orig: g_TextOverrideTable
	// @0x33429B0, TextResource_LoadOverrideTable @0x75D5C0].
	std::shared_ptr<const rtxt::File> override_table;

	// The entry a (section, key) lookup finds: the override table's first,
	// else the mission table's (none when it is not loaded); in each, the
	// first section whose name matches and the first key in it that does,
	// both by stricmp. [orig: TextResource_FindEntryBySectionAndKey @0x75D250,
	// g_TextOverrideTable searched first unless it is the table asked
	// @0x75D27B, the section walk @0x75D2B0; TextResource_FindKeyInSection
	// @0x75D1E0, the key's stricmp @0x75D205]
	const rtxt::Entry *find(const std::string &section, const std::string &key) const;

	// MissionText_GetString: the found text, "" with no mission table or no
	// entry [orig: MissionText_GetString @0x51EB50: the g_TextMission test
	// @0x51EB57, the lookup @0x51EB76]. The debug bit (dword_24C1930 & 0x10000
	// -> "&" @0x51EB59..0x51EB6A) is a developer switch this port does not
	// carry.
	std::string get_string(const std::string &section, const std::string &key) const;
	// The briefing pages S2C 0x7E carries: [info] briefing3, and briefing2,
	// or briefing when briefing2 reads empty [orig: NetPacket_WriteBriefingText
	// @0x506620: @0x506647, @0x506649, the empty test @0x506651, @0x506660].
	std::string briefing3() const;
	std::string briefing2() const;
	// The authored display name a record's name_index keys (promote gates
	// name_index 0 and clips the copy): the [PeopleNames] STRNAME%03i text,
	// "" with no mission table or no entry. [orig: Entity_SpawnFromBMSRecord:
	// the mission table's test @0x40ECCA, sprintf("STRNAME%03i") @0x40ECD9,
	// the lookup @0x40ECE9, a null text skipped @0x40ECF7]
	std::string people_name(int32_t index) const;
	// The deploy-map name a type-2044 marker registers as it spawns: the
	// [Locations] text keyed by the record's own name_index (its +4 dword;
	// 0 keys LOCATION000 like any other number), else the LOCATION%03i key
	// string itself. The lookup has no mission-table test, so an override
	// table answers without one. [orig: Entity_SpawnFromBMSRecord:
	// sprintf("LOCATION%03i", [edi+4]) @0x40F196..0x40F1A4, the Locations
	// lookup @0x40F1B8, the key-string fallback @0x40F1E3..0x40F20A]
	std::string location_name(int32_t name_index) const;
};

// Parses the RTXT bytes into `out`'s mission table (reset first, the
// override table with it). False with `error` on a rejected table; `out`
// then stays unloaded.
bool parse_mission_text(const uint8_t *bytes, std::size_t size, MissionText &out,
		std::string &error);

} // namespace opennova::mission
