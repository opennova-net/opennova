#pragma once

#include <string>
#include <vector>

namespace opennova::editor {

// The files an import never takes and a build never packs (ADR 0046 S14): what the game keeps for
// the player or for this machine beside its archives, and what it writes there while it runs. One
// table, which the import plan (a row it cannot take), the import itself (a source it refuses), the
// game install's listing (a loose file it never lists) and the build (a project file it leaves out)
// all read. A mission found loose in a game's folder sits beside these, so an import that looks for
// what a mission needs there must never bring one.
struct PlayerFile {
	std::string name; // the file's name as the game opens it
	std::string what; // what it is, in words ("the player's profiles")
	std::string orig; // the witness: the reader or the writer
};

// The table: the manifest's player files (gameprofile's RES_F_PLAYER_FILE rows: the configuration,
// the saves, the stored credentials, the high scores), then the files the game writes as it runs.
const std::vector<PlayerFile> &player_files();

// Whether a file of this logical name (of any folder, in any case) is one: a row of the table, a
// save (any .sav: the profiles the game writes [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0]), or a
// screenshot ("SS" and five digits, of any extension [orig: Screenshot_CaptureToFile @ 0x5221a0,
// "SS%0.5d.%s"]).
bool is_player_file(const std::string &logical_name);

// What the file is, in words, for a refusal ("the player's profiles"); "" for a file that is none.
std::string player_file_words(const std::string &logical_name);

} // namespace opennova::editor
