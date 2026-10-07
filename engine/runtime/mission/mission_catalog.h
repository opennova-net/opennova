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
namespace opennova::rtxt {
struct File;
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

// Build the catalog from the mounted install, in retail's walk: the loose
// `.bms` files first (flagged loose, titled only from a `.bin` loose in the
// install's root), then two archive pairs, each `.bms` entry of the mission
// archive titled only when the pair's own text archive holds its `.bin`: the
// expansion's <n>.pff with <n>L.pff, then localres.pff with language.pff.
// resource.pff is never walked, a `.bms` in <n>L.pff is never listed, and a
// name both pairs carry lists twice (no dedupe anywhere downstream either).
// The title is the table's [Info] TITLE (empty when the table lacks one);
// with no table the BMS header's embedded mission_name stands in. The pairs
// read the archive slots mount_game fills under RetailTable discovery
// (vfs.h kArchiveSlotCount), so a loose-only or ScanAll mount lists its loose
// files alone.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 — the loose walk, then
//  Mission_BuildMapListFromPFF @ 0x562910 over (slot 1, slot 0) and (slot 3,
//  slot 2) @ 0x5635a5..0x5635d8, the qsort @ 0x5635f0]
// The .npj/.npz map-project legs are not ported (D-MNU-25, D-MIS-7): retail's
// archive walk matches a .npj or .npz entry as it does a .bms, and the
// shipped localres.pff carries ASP_G7.npz and the JOX jox01.pff nine more,
// which retail lists and OpenNova cannot load.
std::vector<Row> build(const ResourceIndex &index);

// One row of the loose leg as build() makes it, from what an embedder read itself (the editor's
// project, whose files are its own): `file` the `.bms` name, `bms` its bytes (its header read only
// when they open as a BMS the scan takes, else zeroed), `text` its table when one lies loose beside
// it (null: none, the header's embedded mission_name titling it), flagged loose.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170, the loose walk]
Row loose_row(const std::string &file, const std::vector<uint8_t> &bms, const rtxt::File *text);
// The `.bin` text table a mission's name pairs with, as the scan forms it (the extension after the
// FIRST '.' replaced).
// [orig: Path_ReplaceOrAppendExtension @ 0x53c780, called with "bin" @ 0x56345b]
std::string text_table_name(const std::string &file);

// The SP screen's row filter — (code_word & 0xFFFDFFFF) == 0x10020, i.e. the
// waypoint (Co-op) family with the objective bit forgiven — lives with the
// code words as game_type::is_waypoint_family
// [orig: SinglePlayer_PopulateMissionList @ 0x5618b3].

// The list row text: the loose "*" marker plus the title, with the FILENAME
// standing in when no title was resolved
// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9..0x561943].
std::string display_text(const Row &row);

} // namespace opennova::mission_catalog
