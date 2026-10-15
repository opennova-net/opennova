#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The files an expansion's own name forms (ADR 0046 S16): what the game opens for expansion <n>
// under `/exp <n>`, the required-resource manifest's RES_F_EXPANSION rows, which name each file, cite
// its witness and say which of the base game's files it stands in for [orig: Expansion_LoadAssets
// @ 0x4a4730, the expansion arm @ 0x4a4858..0x4a49de]. A project building as an expansion holds them by
// those names, flat as every file of a project is; the build places each where its reader looks.
enum class ExpansionFileRole {
	Table,           // <n>.bin: the text override table and the Mods list's [exp_info]
	Version,         // version.txt: the CRC a joiner must match
	MenuMusicBank,   // M<n>.sbf, in place of MENUMUS.SBF
	MenuMusicScript, // M<n>.bin, in place of MENUMUS.BIN
	GameMusicBank,   // G<n>.sbf, in place of GAMEMUS.SBF
	GameMusicScript, // G<n>.bin, in place of GAMEMUS.BIN
	LocalBank,       // <n>L.lwf, the mission banks' first slot
	Bank,            // <n>.lwf, the second
	kCount
};
inline constexpr size_t kExpansionFileRoleCount = static_cast<size_t>(ExpansionFileRole::kCount);

// Where the game reads a file of the row: loose in the expansion's own folder (by a path there, never
// through the archives), or by its name through the archives, so where its kind packs (the
// expansion's own archives: the build's routing).
enum class ExpansionPlacement { Folder, ByKind };

// The editor's columns of a manifest row: what the file is to it, where the build puts it, its words.
struct ExpansionFileRow {
	ExpansionFileRole role;
	const char *manifest_role; // the manifest row it is (RES_F_EXPANSION): its name, its witness, what it replaces
	AssetKind kind;            // what the file is to the engine
	ExpansionPlacement placement;
	const char *what;          // in words: "the expansion's text table"

	// The manifest row; null only for a role token the manifest lacks (the tests pin every one).
	const gameprofile::RequiredResource *resource() const;
	// A name the expansion's does not form (version.txt).
	bool fixed() const;
	// The base game's file the game reads in its place without /exp; null when none.
	const char *replaces() const;
};

// The rows, in ExpansionFileRole's order.
const ExpansionFileRow &expansion_file_row(ExpansionFileRole role);
// The row whose manifest role is `token`; null when none.
const ExpansionFileRow *expansion_file_row_for_manifest_role(const std::string &token);
// The row's file for expansion `name` ("jxm" -> "Mjxm.sbf"; version.txt whatever the name): the
// manifest's name for it (gameprofile_expansion_file_name).
std::string expansion_file_name(const ExpansionFileRow &row, const std::string &name);

struct ExpansionFile {
	const ExpansionFileRow *row = nullptr;
	std::string name;
};
// Every row's file for expansion `name`, in the rows' order; none for "" (a standalone project).
std::vector<ExpansionFile> expansion_files(const std::string &name);
// The row `logical_name` is a file of for expansion `name` (compared without case, as the game
// compares names); null for none, and for every name when `name` is "".
const ExpansionFileRow *expansion_file_for(const std::string &name, const std::string &logical_name);

} // namespace opennova::editor
