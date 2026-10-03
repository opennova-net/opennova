#pragma once

// The front-end mission catalog — the scan retail runs at boot and expansion
// reload to build the mission table every menu list reads
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 (loose FindFirstFile
// walk over *.bms), Mission_BuildMapListFromPFF @ 0x562910 (per-archive
// entry walk), called from Game_InitSubsystems @ 0x4a72a6 and
// Game_ReloadExpansionAndMods @ 0x5527ca].
//
// Each 0x11E8-byte retail table entry carries the filename (+0), the display
// title (+1044), the [Info] BRIEFING text pointer (+1300), the loose-scan
// flag (+4380: 1 for loose finds, 0 for archive entries), and the session
// game-type code word (+4392). The list is qsorted case-insensitively by
// FILENAME [orig: Mission_CompareMapNames @ 0x5628e0 — stricmp on entry+0].

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {
class ResourceIndex;
}

namespace opennova::mission_catalog {

struct Row {
	std::string file;     // archive/loose entry name [orig: entry+0]
	std::string title;    // display title [orig: entry+1044]
	std::string briefing; // [Info] BRIEFING text [orig: entry+1300]
	// The header's single-select game-mode attrib bit (0 = none/SP). The
	// retail table stores the derived session code word instead
	// [orig: entry+4392 = the AI_GetTaskTypeFromFlags switch @ 0x5631f0..];
	// that derivation is npwire's game_type::for_mission_mode, which the
	// net-linking consumer applies — the runtime layer never includes net
	// headers (the spawn_select.h layering rule).
	uint32_t game_mode = 0;
	bool loose = false; // loose-scanned, not archived [orig: entry+4380]
};

// Build the catalog from the mounted install. Retail scans the working
// directory (FindFirstFile *.bms) and then each archive volume in
// localres/language pairs; the mount stack serves the same population, with
// each row's loose flag taken from the serving mount and the sibling
// <mission>.bin resolved through the stack rather than the paired volume
// (identical on retail data, where the mission .bins live in the language
// archives). The title is the .bin's [Info] TITLE; when NO .bin exists the
// BMS header's embedded mission_name stands in; either miss leaves it empty
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 title/briefing arm].
// The .npj/.npz map-project legs are not ported (D-MIS-7: the shipped
// localres.pff carries ASP_G7.npz and the JOX jox01.pff nine more, which
// retail lists and OpenNova does not).
std::vector<Row> build(const ResourceIndex &index);

// The SP screen's row filter — (code_word & 0xFFFDFFFF) == 0x10020, i.e. the
// waypoint (Co-op) family with the objective bit forgiven — lives with the
// code words as game_type::is_waypoint_family
// [orig: SinglePlayer_PopulateMissionList @ 0x5618b3].

// The list row text: the loose "*" marker plus the title, with the FILENAME
// standing in when no title was resolved
// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9..0x561943].
std::string display_text(const Row &row);

} // namespace opennova::mission_catalog
