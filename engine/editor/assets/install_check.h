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
// which Play in the game install starts (run/launch_plan.h, kInstallExecutable).
struct InstallCheck {
	struct Expansion {
		std::string name;  // its folder's name ("jox01")
		std::string title; // the Mods list's name ("Escalation")
	};
	std::string root;        // as asked, absolute (absolute_install_path); "" when none was named
	std::string game;        // the game profile code it was read as ("jo")
	bool exists = false;     // a folder is there
	bool mounts = false;     // the game's archives open: an install the editor imports from
	size_t files = 0;        // the files it serves, as the import lists them
	bool executable = false; // the game's program is beside them (Play in the game install)
	std::vector<Expansion> expansions;
	std::string why; // why it does not mount, in the view's words ("" when it does)

	bool ok() const { return mounts; }
	// What was found, in a line: "Joint Operations: 9,290 files, the expansion Escalation (jox01)."; or why
	// the folder is no install the editor can use ("No game here: ...").
	std::string words() const;
};

// The folder `root` read as an install of the game `game` (a gameprofile code; "" for jo). Mounting it
// costs a listing of its archives' tables, never their files' bytes.
InstallCheck check_install(const std::string &root, const std::string &game);

} // namespace opennova::editor
