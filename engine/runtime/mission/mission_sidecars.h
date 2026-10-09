#pragma once

// The files the game finds by a mission's NAME at mission start: the witnessed by-name
// table, once. Nothing in a `.bms` names them; each reader builds `<mission base>` plus its
// own extension and exists-checks it, so an absent one is skipped
// [orig: Game_StartMission @ 0x524360, docs/required-resources.md "Mission start"].
//
// The rows are data for whoever needs the set (the editor's mission file set, its rename and
// its import; a packer). Of the runtime's own loaders the mission text's and the tile info's
// read their rows (runtime_boot's resolve_mission_text and read_placed_tiles); the others still
// spell their names at their port sites (wac_layered_load, the loading screen, the dialog bank)
// and come to read a row on touch.

#include <functional>
#include <string>
#include <vector>

namespace opennova::mission {

struct Sidecar {
	// What the file is to the mission: "text", "script", "loading_image", "tiles", "dialog",
	// "dialog_sounds". Stable tokens.
	const char *role;
	// The extension the reader appends to the mission's base name, dot included.
	const char *extension;
	// The extension the reader tries when no file of the first exists; null for none.
	const char *alternate;
	// The literal file the reader uses instead when the mission has none; null for none.
	const char *fallback;
	// The role whose file must exist for this row to be read at all; null for none.
	const char *needs;
};

// The table, one row a reader (not the order the game reads them in: it opens the .dbf before
// the .bin and the scripts):
//
//   text           <base>.bin, else medmssn.bin: the mission's string table
//                  [orig: TextResource_LoadMissionTextBin @ 0x51ed90, the name @ 0x51edd9];
//                  the mission list's title and briefing read the same file's [Info] TITLE
//                  and BRIEFING [orig: MissionList_ScanAndBuildFromFiles @ 0x563170].
//   script         <base>.wac, compiled after game.wac and server.wac into one buffer
//                  [orig: WacScript_InitAndLoad @ 0x4f91f0, the name @ 0x4f953a].
//   loading_image  <base>.pcx, else loadscrn.pcx [orig: Render_LoadingScreen @ 0x521d10,
//                  the sidecar probe @ 0x521db5, the fallback @ 0x521e20].
//   tiles          <base>.til, the per-mission tile info, read loose first, on the authority
//                  alone, else the terrain's own (runtime_boot's read_placed_tiles)
//                  [orig: Terrain_Init @ 0x60fbe0, the name @ 0x60fd0c;
//                  Terrain_LoadTileInfoFile @ 0x60a740, the policy force @ 0x60a74e;
//                  PolyTrn_LoadTerrainConfig @ 0x60e6c9..0x60e6e5].
//   dialog         <base>.dbf, the dialog bank [orig: DialogSystem_Init @ 0x5275e0, the name
//                  @ 0x52763e].
//   dialog_sounds  <base>.lwf, else <base>.pwf, the dialog bank's sounds, read only when the
//                  .dbf exists [orig: DialogManager_LoadFromFile @ 0x44e650, the .pwf arm
//                  @ 0x44e7f5], into the dialog bank, not one of the six bank slots.
const std::vector<Sidecar> &sidecars();

// The row of a role; null for an unknown one.
const Sidecar *sidecar_for_role(const std::string &role);

// The mission's base name as every reader takes it: the file name up to its FIRST dot (the
// whole name when it has none), case kept, as the game's extension swap replaces from there
// (Path_ReplaceOrAppendExtension; the dialog bank's sounds take strtok's first token). So
// "maps/00TRa.bms" and "00TRa.bms" both read "00TRa", and "op.v2.bms" reads "op".
std::string mission_base_name(const std::string &mission_file);

// The name a row's reader builds for a mission: its base name plus the row's extension, the
// mission's case kept ("00TRa.bms" -> "00TRa.wac").
std::string sidecar_name(const std::string &mission_file, const Sidecar &sidecar);
// The same with the row's alternate extension; "" when the row has none.
std::string sidecar_alternate_name(const std::string &mission_file, const Sidecar &sidecar);

// The dialog bank a mission loads: the name in its header's second terrain slot (the original
// editor's cnv_file) where it holds one, else the mission's own name, its extension swapped for
// .dbf either way [orig: DialogSystem_Init @ 0x5275e0 -- the slot byte_A76224, g_BmsHeaderBlock +
// 0x54, copied @ 0x52760c..0x52761f, the mission's name @ 0x527623..0x527632,
// Path_ReplaceOrAppendExtension(path, "dbf") @ 0x52763e]. No shipped mission fills the slot, so
// every shipped bank is <base>.dbf. `header_slot` the slot as read ("" for none).
std::string dialog_bank_name(const std::string &mission_file, const std::string &header_slot);
// The dialog bank's sounds: its own base name (strtok's first token) plus .lwf, else (`alternate`)
// plus .pwf, which the game opens only where no .lwf of the name exists [orig:
// DialogManager_LoadFromFile @ 0x44e7bb..0x44e807].
std::string dialog_sounds_name(const std::string &dialog_bank, bool alternate = false);

// The names a row's reader opens for a mission: its own name, then its alternate where it tries one
// ("" none), and the file it needs beside it before it reads at all ("" none); the dialog rows by the
// dialog bank the header picks (dialog_bank_name, its sounds dialog_sounds_name's). `header_slot` the
// header's dialog bank slot as read ("" for none, as every shipped mission).
struct SidecarNames {
	std::string name;
	std::string alternate;
	std::string needs;
};
SidecarNames sidecar_names(const std::string &mission_file, const Sidecar &sidecar,
		const std::string &header_slot = std::string());
// Whether a row's reader reads for the mission at all: a row that needs another's file beside it
// reads only where `has_file` finds that file [orig: DialogSystem_Init @ 0x5275e0, the exists check
// @ 0x527648 before DialogManager_LoadFromFile @ 0x44e650 opens the sounds].
bool sidecar_reads(const SidecarNames &names, const std::function<bool(const std::string &)> &has_file);
// The row whose reader opens `file` for the mission (its name or its alternate, compared as the
// archives compare names, pff::normalized_logical_name), among the rows that read (sidecar_reads);
// null where none does.
const Sidecar *sidecar_naming(const std::string &mission_file, const std::string &file,
		const std::function<bool(const std::string &)> &has_file,
		const std::string &header_slot = std::string());

} // namespace opennova::mission
