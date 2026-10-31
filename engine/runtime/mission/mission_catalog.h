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
// game-type code word (+4392). The list is qsorted case-insensitively by the
// title's key (Title::mission_list_order): Jointops.exe's FILENAME
// [orig: Mission_CompareMapNames @ 0x5628e0 — stricmp on entry+0], dfx2.exe's
// display title [orig: CompareFunction @0x561840 (dfx2.exe), _stricmp on
// entry+1044].

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <base/title/title.h> // MissionListOrder (the title's catalog order)
#include <base/vfs/vfs.h> // VfsLookupPolicy (bms_lookup)

namespace opennova {
class ResourceIndex;
}
namespace opennova::rtxt {
struct File;
}
namespace opennova::pff {
struct PffArchive;
}

namespace opennova::mission_catalog {

// A table entry's campaign words: the campaign it belongs to (-1 none), its place in that
// campaign, and its flags. The scan stores -1 and 0 [orig: MissionList_ScanAndBuildFromFiles
// @0x563170 (+0x10C = 0 @0x56336b, +0x104 = -1 @0x563430); Mission_BuildMapListFromPFF @0x562910
// (@0x562b2f, @0x562bf4)]; only dfx2.exe's campaign reader writes them (runtime/mission/campaign.h),
// and the round end reads them under both (PlayerProfile_SaveRoundEndProgress @0x54d6a0).
inline constexpr uint32_t kRowInCampaign = 2u; // a campaign names it [orig: |= 2 @0x560019 (dfx2.exe)]
inline constexpr uint32_t kRowComplete = 4u;   // won, or the profile says so [orig: @0x54d6e7; @0x56020e (dfx2.exe)]
struct CampaignWords {
	int32_t campaign = -1;        // [orig: entry+0x104]
	int32_t campaign_mission = 0; // [orig: entry+0x108]
	uint32_t flags = 0;           // [orig: entry+0x10C]
};

struct Row {
	std::string file;     // archive/loose entry name [orig: entry+0]
	std::string title;    // display title [orig: entry+1044]
	std::string briefing; // [Info] BRIEFING text [orig: entry+1300]
	// The [Info] WIN_DEBRIEFING and LOSE_DEBRIEFING dfx2.exe's end screens read through the
	// entry's table (+1040), each present when the table holds the key, its text empty or not.
	// The table is the one the scan loads (none for an untitled row), and the lookup searches the
	// override table first, with no table too (world::EpilogMission).
	// [orig: TextResource_FindEntryBySectionAndKey(entry+1040, "Info", ..) @0x575c9e (lose),
	//  @0x574771 (win); the table +1040 @0x5623db; the override arm @0x7269db (dfx2.exe)]
	std::optional<std::string> win_debriefing;
	std::optional<std::string> lose_debriefing;
	// The header's single-select game-mode attrib bit (0 = none/SP). The
	// retail table stores the derived session code word instead
	// [orig: entry+4392 = the AI_GetTaskTypeFromFlags switch @ 0x5631f0..];
	// game_type_of derives it.
	uint32_t game_mode = 0;
	bool loose = false; // loose-scanned, not archived [orig: entry+4380]
	// The header's mission type byte (bms::MissionType: 2 a combat-vehicle mission; 0 with no
	// header) [orig: entry+0x1134, MissionList_ScanAndBuildFromFiles @0x563170 (@0x56335d..
	//  0x563365), Mission_BuildMapListFromPFF @0x562910 (@0x562b29)].
	uint8_t mission_type = 0;
	CampaignWords campaign;
};

// The lookup a mission start reads its .bms through, by its row's source: a loose find from
// disk, an archive row from the archives alone, past the /d loose override either way
// [orig: Game_StartMission @0x524769..0x524774 passes g_MapFileName and g_MissionSourceIsLoose
//  to the BMS load; Mission_LoadBMSFromPFF @ 0x40d43c (archive-only); Mission_LoadBMSFile
//  @0x40F51A..0x40F527 fopens a loose one].
inline VfsLookupPolicy bms_lookup(bool source_is_loose) {
	return source_is_loose ? VfsLookupPolicy::ForceLooseFirst : VfsLookupPolicy::ForceArchiveOnly;
}

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
// files alone. Each TITLE / BRIEFING lookup searches `text_override` (the
// mount's override table, null for none) first, as every section lookup does
// [orig: MissionList_ScanAndBuildFromFiles @0x563170 (the calls @0x563496 / @0x5634E8),
//  Mission_BuildMapListFromPFF @0x562910 (the calls @0x562C62 / @0x562CB8) ->
//  TextResource_FindEntryBySectionAndKey @0x75D250].
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 — the loose walk, then
//  Mission_BuildMapListFromPFF @ 0x562910 over (slot 1, slot 0) and (slot 3,
//  slot 2) @ 0x5635a5..0x5635d8, the qsort @ 0x5635f0]
// The .npj/.npz map-project legs are not ported (D-MNU-25, D-MIS-7): retail's
// archive walk matches a .npj or .npz entry as it does a .bms, and the
// shipped localres.pff carries ASP_G7.npz and the JOX jox01.pff nine more,
// which retail lists and OpenNova cannot load. The rows end in `order`, the
// title's (sort_rows).
std::vector<Row> build(
		const ResourceIndex &index, const rtxt::File *text_override, MissionListOrder order);

// One row of the loose leg as build() makes it, from what an embedder read itself (the editor's
// project, whose files are its own): `file` the `.bms` name, `bms` its bytes (its header read only
// when they open as a BMS the scan takes, else zeroed), `text` its table when one lies loose beside
// it (null: none, the header's embedded mission_name titling it), flagged loose; `text_override` as
// build() takes it.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170, the loose walk]
Row loose_row(const std::string &file, const std::vector<uint8_t> &bms, const rtxt::File *text,
		const rtxt::File *text_override);
// The loose leg's rows as build() makes them, from what an embedder reads itself: each `.bms` among
// `names` (the others passed over, in `names`' order), its bytes from `read_bms` (a failed read: none,
// the header zeroed), titled by the text table `read_text` loads for its text_table_name (false: none;
// which table counts as lying beside it is the caller's rule: build()'s the install root's).
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170, the loose walk]
using ReadBms = std::function<bool(const std::string &name, std::vector<uint8_t> &out)>;
using ReadText = std::function<bool(const std::string &bin, rtxt::File &out)>;
std::vector<Row> loose_rows(const std::vector<std::string> &names, const ReadBms &read_bms,
		const ReadText &read_text, const rtxt::File *text_override);
// One archive pair as build() walks it, over archives an embedder opened itself (a product's
// expansion pair with no base install mounted beside it: the hosted fleet's trees, ADR 0064):
// every `.bms` entry of `missions` (lists_as_mission, the map projects passed over), its header
// read from that archive, titled only when `texts` (null: none) holds its `.bin`, which then
// reads as any table; flagged archived. In walk order: the caller sorts (sort_rows).
// [orig: Mission_BuildMapListFromPFF @ 0x562910, the entry walk @ 0x56295f..0x562d51]
std::vector<Row> archive_rows(const pff::PffArchive &missions, const pff::PffArchive *texts,
		const rtxt::File *text_override);
// The catalog's order, the title's (Title::mission_list_order): by file name without case
// (Jointops.exe's), or by display title without case (dfx2.exe's), rows of the same key in the
// order they came.
// [orig: qsort(list, count, 0x11E8, Mission_CompareMapNames) @ 0x5635f0 -- Mission_CompareMapNames
//  @ 0x5628e0, stricmp on entry+0; build() says why the port's sort is stable]
// [orig: qsort(list, count, 0x11E8, CompareFunction) @0x562550 -- CompareFunction @0x561840,
//  _stricmp on entry+1044 (dfx2.exe)]
void sort_rows(std::vector<Row> &rows, MissionListOrder order);
// The session game-type code word the retail table stamps per row: game_type::for_mission_mode over
// the row's mode bit [orig: entry+4392, the AI_GetTaskTypeFromFlags switch @ 0x5631f0..].
uint32_t game_type_of(const Row &row);
// The `.bin` text table a mission's name pairs with, as the scan forms it (the extension after the
// FIRST '.' replaced).
// [orig: Path_ReplaceOrAppendExtension @ 0x53c780, called with "bin" @ 0x56345b]
std::string text_table_name(const std::string &file);
// The section and the two keys the scan reads of that table: [Info] TITLE, the row's title, and
// BRIEFING, its briefing.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170, the title arm @ 0x563489..0x56354f;
//  Mission_BuildMapListFromPFF @ 0x562910 the same @ 0x562c55..0x562d1f]
inline constexpr const char *kTextInfoSection = "Info";
inline constexpr const char *kTextTitleKey = "TITLE";
inline constexpr const char *kTextBriefingKey = "BRIEFING";
// The two [Info] keys dfx2.exe's end screens read of the same table [orig: Cine_EpilogLoseUpdate
// @0x575c93, Cine_EpilogStateMachineUpdate @0x574766 (dfx2.exe)].
inline constexpr const char *kTextWinDebriefingKey = "WIN_DEBRIEFING";
inline constexpr const char *kTextLoseDebriefingKey = "LOSE_DEBRIEFING";
// Whether the archive walk lists an entry as a mission: its name from the FIRST '.' on is ".bms",
// ".npj" or ".npz" without case, so "a.b.bms" is none and a name with no '.' none. build() lists
// the `.bms` alone (D-MNU-25: OpenNova cannot load a map project).
// [orig: Mission_BuildMapListFromPFF @ 0x562910, File_HasExtension @ 0x53c640 @ 0x5629a0 /
//  @ 0x5629bc / @ 0x5629d8]
bool lists_as_mission(const std::string &name);

// The SP screen's row filter — (code_word & 0xFFFDFFFF) == 0x10020, i.e. the
// waypoint (Co-op) family with the objective bit forgiven — lives with the
// code words as game_type::is_waypoint_family
// [orig: SinglePlayer_PopulateMissionList @ 0x5618b3].

// The list row text: the loose "*" marker plus the title, with the FILENAME
// standing in when no title was resolved
// [orig: SinglePlayer_PopulateMissionList @ 0x5618b9..0x561943].
std::string display_text(const Row &row);

// The catalog's high scores: each table entry's score word (+0x11E4), 0 as the scan builds the
// table, read from hiscore.txt (in the game's folder) at boot and at each expansion reload past the
// rescan, and written back at each of the menu's teardowns (a mission start and the exit, the
// reload's too). Read and written alike by Jointops.exe and dfx2.exe; only dfx2.exe's win epilog
// reads and raises a score (runtime/world/epilog_cine.h).
// [orig: HUD_LoadHighScoreText @ 0x5630e0 (called @ 0x4a72b3 past the scan @ 0x4a72a6, and
//  @ 0x5527d4 past the reload's @ 0x5527ca); HighScore_SaveToFile @ 0x563100 (called
//  @ 0x54e4de / @ 0x54e51a by Menu_TeardownShellAndCloseBinkVideos @ 0x54e430, @ 0x552725 by
//  Game_ReloadExpansionAndMods @ 0x552710)] [orig: HUD_LoadHighScoreText @0x562040,
//  HighScore_SaveToFile @0x562060 (dfx2.exe)]
inline constexpr const char *kHighScoreFile = "hiscore.txt";
class HighScoreTable {
public:
	// The table's rows in catalog order, each score 0.
	void reset(const std::vector<Row> &rows);
	// hiscore.txt's text over the table: each line the walk hands its callback (a first token not
	// starting '/') sets the score of every row whose file is its first token, without case, to its
	// second token read as an unsigned decimal. The token is the file's bytes and a row's file its
	// cp1252 bytes (a loose row's held as UTF-8), compared as cp1252_stricmp folds them (D-MNU-39).
	// [orig: HUD_LoadHighScoreText @ 0x5630e0 -> File_ParseASCIIFileWithCallback @ 0x53d980 with
	//  HighScore_ParseLineCallback @ 0x563070 (the stricmp on entry+0 @ 0x56309d, CRT_strtoul(token
	//  2, 10) into entry+0x11E4 @ 0x5630c2; every row, no break)]
	void apply_text(const std::string &text);
	// hiscore.txt as the save writes it: "%s\t%d\r\n" per row in catalog order through a
	// text-mode "w" stream, which writes each '\n' as CR LF, so a line ends CR CR LF; each file
	// in its cp1252 bytes.
	// [orig: HighScore_SaveToFile @ 0x563100 - fopen("hiscore.txt", "w") @ 0x563110, the fprintf
	//  @ 0x563146 of entry+0 and entry+0x11E4]
	std::string text() const;
	bool empty() const { return rows_.empty(); }
	// The score of the first row whose file is `file` (a row's file, compared as apply_text
	// compares a token); 0 for none. set_score sets that row's.
	int32_t score(const std::string &file) const;
	void set_score(const std::string &file, int32_t score);

private:
	struct Entry {
		std::string file;
		int32_t score = 0;
	};
	std::vector<Entry> rows_;
};

} // namespace opennova::mission_catalog
