#pragma once

#include <functional>
#include <string>
#include <vector>

#include <formats/mns/mns.h>

namespace opennova::menu {

// The menu shell's stylesheets, in the order the shell loads them into its one
// variable list [orig: Menu_InitShellResources @ 0x552500: "menu_style.mns" @ 0x552604
// with append 0 (@ 0x5525f7), then "brand.mns" @ 0x552616 with append 1 (@ 0x552614),
// each through NapiConfigMap_LoadIncludeFile @ 0x63b970 -> NapiConfigMap_ParseKeyValueBuffer @
// 0x639870]. Nothing else names a stylesheet: the game reads no other .mns.
struct ShellStylesheet {
	const char *name;
	bool append; // false: the list is cleared before the file is read
};
extern const ShellStylesheet kShellStylesheets[2];
// True for a name the shell reads (compared case-blind, as the game's file lookup does).
bool is_shell_stylesheet(const std::string &name);

// What the shell made of one of its stylesheets.
struct ShellSheetRead {
	std::string name;
	bool present = false;   // a file with bytes (a missing or empty one changes nothing)
	mns::ReadResult read;   // where the reader stopped, when it did
	int stopped_line = 0;   // that place's line (0 = read to the end)
};

// The variable list the menus expand %VAR% through, and how each sheet went.
struct ShellStyle {
	mns::KeyValueList list;
	std::vector<ShellSheetRead> sheets;
	bool any_present() const;
};

// Reads a stylesheet's bytes by name; false when there is no such file.
using ShellStyleReader = std::function<bool(const std::string &name, std::string &bytes)>;

// The shell's load, as the game does it: menu_style.mns into an empty list, then
// brand.mns onto it, a later definition replacing the value and keeping the first
// spelling. A missing or empty file changes nothing [orig: NapiConfigMap_LoadIncludeFile
// @ 0x63b989]; a sheet the reader stops in keeps what it read before (every caller
// ignores the result). A sheet retail would stop responding on is read up to that place
// (docs/mnu/menu-re.md "The shell's stylesheets").
ShellStyle load_shell_style(const ShellStyleReader &read);

// One include file into `list` [orig: NapiConfigMap_LoadIncludeFile @ 0x63b970].
ShellSheetRead load_include_file(const std::string &name, const ShellStyleReader &read, bool append,
                                 mns::KeyValueList &list);

} // namespace opennova::menu
