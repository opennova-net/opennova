#pragma once

// The files the game finds by a mission's NAME at mission start: the witnessed by-name
// table, once. Nothing in a `.bms` names them; each reader builds `<mission base>` plus its
// own extension and exists-checks it, so an absent one is skipped
// [orig: Game_StartMission @ 0x524360, docs/required-resources.md "Mission start"].
//
// The rows are data for whoever needs the set (the editor's mission file set, its rename and
// its import; a packer). Of the runtime's own loaders the mission text's reads its row
// (runtime_boot's resolve_mission_text); the others still spell their names at their port
// sites (wac_layered_load, the loading screen, the tile info load, the dialog bank) and come
// to read a row on touch.

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
//   tiles          <base>.til, the per-mission tile info, read loose first
//                  [orig: Terrain_Init @ 0x60fbe0, the name @ 0x60fd0c; Terrain_LoadTileInfoFile
//                  @ 0x60a740, the policy force @ 0x60a74e].
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

} // namespace opennova::mission
