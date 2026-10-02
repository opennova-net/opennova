#pragma once

// What a menu screen reads besides its own windows, read through a file source: the
// string tables its windows name and the shell's %VAR% list. One rule for the game's
// frame, the editor's preview and the editor's asset graph. Witness record:
// docs/mnu/menu-re.md ("Menu strings", "The shell's stylesheets").

#include <base/vfs/file_source.h>
#include <formats/mnu/mnu.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/menu/menu_style.h>
#include <runtime/menu/menu_text_tables.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace opennova::menu {

// A file a configure read or looked for, with the stamp it had then (0: the name did
// not resolve). A holder reconfigures when a recorded stamp moves.
struct MenuDependency {
	std::string name;
	uint64_t stamp = 0;
};

// The TEXT_RSRC a window's string ids read: its own when it authors one, else
// `fallback`'s, null when neither does [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0
// returns the topmost ancestor's value, so the fallback is the root window the window
// hangs under]. The window a part parsed before it is attached falls back to is the part
// it belongs to (a SPINUP / SPINDOWN reads its own only); a combo's LIST_BOX, attached
// before its parse, falls back to the root window, never to the combo [orig:
// CComboWnd_ParseXMLDefinition @ 0x65c0d0 -> CWnd_SetParentAndAttach].
const std::string *window_text_rsrc(const mnu::Window &window, const mnu::Window *fallback);

// Every TEXT_RSRC name the windows of a screen author (roots, children and parts),
// each once in document order: the tables its compile can read.
std::vector<std::string> screen_text_rsrc_names(const mnu::Screen &screen);

// The string tables a screen reads, each read through a file source and parsed, kept by
// (name, stamp) for the session [orig: CUIStringTable_LookupString @ 0x6527c0 keeps a
// table it loaded, matched by stricmp]. A name that does not resolve is recorded as not
// loaded and looked for again on the next load (a failed load is not kept); one that
// resolves but does not parse is not read again until its stamp moves.
class MenuTextTableLoader {
public:
	// Fill `out` for `screen`: the override table (the expansion's `<exp>.bin`, borrowed,
	// null for none) and every table the screen's windows name. The files it read or
	// looked for are appended to `dependencies` when given.
	void load(const mnu::Screen &screen, const FileSource &files, const rtxt::File *override_table,
	          MenuTextTables &out, std::vector<MenuDependency> *dependencies = nullptr);
	void clear();

private:
	struct Loaded {
		uint64_t stamp = 0;
		std::shared_ptr<const rtxt::File> file; // null: the bytes did not parse
	};
	std::map<std::string, Loaded> tables_; // lowercased name
};

// The shell's %VAR% list read through a file source: menu_style.mns into an empty list,
// then brand.mns onto it (load_shell_style), read again only when either sheet's stamp
// moved.
class MenuStyleSource {
public:
	// The list as the compiler takes it (MenuFrameCompiler::set_style_vars).
	const std::map<std::string, std::string> &vars(const FileSource &files);
	// What the last read made of each sheet.
	const ShellStyle &style() const { return style_; }
	// The two sheets with the stamps they were read at.
	void dependencies(std::vector<MenuDependency> &out) const;
	void clear();

private:
	bool loaded_ = false;
	uint64_t stamps_[2] = {0, 0};
	ShellStyle style_;
	std::map<std::string, std::string> vars_;
};

} // namespace opennova::menu
