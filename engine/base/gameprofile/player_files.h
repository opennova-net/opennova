#pragma once

#include <functional>
#include <string>
#include <vector>

namespace opennova::gameprofile {

// The files an import never takes and a build never packs (ADR 0046 S14): what the game keeps for
// the player or for this machine beside its archives, and what it writes there while it runs. One
// table, which an import plan (a row it cannot take), an import itself (a source it refuses), a
// game install's listing (a loose file it never lists) and a build (a project file it leaves out)
// all read. A mission found loose in a game's folder sits beside these, so an import that looks for
// what a mission needs there must never bring one.
struct PlayerFile {
	std::string name; // the file's name as the game opens it
	std::string what; // what it is, in words ("the player's profiles")
	std::string orig; // the witness: the reader or the writer
};

// The table: the manifest's player files (required_resources' RES_F_PLAYER_FILE rows: the
// configuration, the saves, the stored credentials, the high scores), then the files the game writes
// as it runs.
const std::vector<PlayerFile> &player_files();

// Whether a file of this logical name (of any folder, in any case) is one: a row of the table, a
// save (any .sav: the profiles the game writes [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0]), or a
// screenshot ("SS" and five digits, of any extension [orig: Screenshot_CaptureToFile @ 0x5221a0,
// "SS%0.5d.%s"]).
bool is_player_file(const std::string &logical_name);

// What the file is, in words, for a refusal ("the player's profiles"); "" for a file that is none.
std::string player_file_words(const std::string &logical_name);

// The file the Screen Shot action writes in the game's working directory: the first
// "SS%0.5d.<ext>" of index 0..99998 the directory does not hold (`exists` asks after each
// name), its extension "tga" under the launch's /sstga (the frame through the run-length TGA
// writer, formats/tga/tga.h tga_write_screenshot), else "jpg" (the device's JPEG of the frame);
// "" when all of them are taken, and the action writes nothing.
// [orig: Screenshot_CaptureToFile @0x5221A0 - the arm the action takes (the frame-capture bit
//  of dword_B4C3C0 clear @0x5221BC; only input code 127, which no catalog row carries, sets
//  it), the extension pick @0x522290..0x52229F ("jpg" @0x7D0224, "tga" under g_ScreenshotTga,
//  set by /sstga in Game_ParseCommandLineAndInit @0x4A77D3), sprintf("SS%0.5d.%s") @0x5222B2,
//  File_CheckExists @0x5222BC, the stop at 99999 @0x5222D1, the TGA arm Screenshot_CaptureTGA
//  @0x5201E0 (the call @0x522300); the action's call @0x49AF15]
std::string screenshot_file_name(const std::function<bool(const std::string &)> &exists, bool tga);

} // namespace opennova::gameprofile
