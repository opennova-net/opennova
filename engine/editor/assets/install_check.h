#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::editor {

// What a folder holds as a game install (the UX round's project lane): checked where a modder names one
// (New project, Project settings, the welcome page), so a wrong folder is said at once rather than at the
// first import. The folder is read as every import reads an install: mounted as a stock launch of the
// project's game mounts it (InstallView over the base game's InstallSpec: the boot archive table, no /d;
// assets/install_view.h), its files counted as the import lists them; its expansions as the Mods list
// finds them (vfs_list_expansions, vfs_expansion_info); and whether the game's own program is beside them,
// which Play in the game install starts (run/launch_plan.h, kInstallExecutable). The game boots on any one
// of its boot table's archives [orig: PFF_OpenAllArchives @ 0x4a4310, the all-failed check @ 0x4a6f44], but
// an install the editor imports from ships all three (a folder of the language archive alone is a language
// pack, not the game); and a build the editor made (its build record beside its archives, or a folder
// inside a project's `.opennova/`) is a project's, not the game's.
struct InstallCheck {
	struct Expansion {
		std::string name;  // its folder's name ("jox01")
		std::string title; // the Mods list's name ("Escalation")
	};
	std::string root;        // as asked, absolute (absolute_install_path); "" when none was named
	std::string game;        // the game profile code it was read as ("jo")
	bool exists = false;     // a folder is there
	bool mounts = false;     // the game's archives open
	size_t files = 0;        // the files it serves, as the import lists them
	bool executable = false; // the game's program is beside them (Play in the game install)
	std::vector<Expansion> expansions;
	// The boot table's archives the folder lacks (vfs.h kBootArchiveTable), in the table's order.
	std::vector<std::string> missing_archives;
	bool archives_present = false; // any of the boot table's archives is there, whether it opens or not
	bool build = false;            // a build the editor made, not the game install
	std::string why;               // why it does not mount, in the view's words ("" when it does)

	// An install the editor imports from: every boot archive there and mounting, and no build of a project's.
	bool ok() const { return mounts && missing_archives.empty() && !build; }
	// What was found, in a line: "Joint Operations: 9,290 files, the expansion Escalation (jox01)."; or why
	// the folder is no install the editor can use ("No game here: ...", "The game's archives here do not
	// open: ...", "Not the whole game: no localres.pff or resource.pff here.").
	std::string words() const;
};

// The folder `root` read as an install of the game `game` (a gameprofile code; "" for jo). Mounting it
// costs a listing of its archives' tables, never their files' bytes.
InstallCheck check_install(const std::string &root, const std::string &game);

} // namespace opennova::editor
