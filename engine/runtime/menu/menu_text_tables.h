#pragma once

// The menu's string tables: which table a window's string ids read and how one
// is looked up [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0;
// CUIStringTable_LookupString @ 0x6527c0 -> TextResource_FindEntryBySectionAndKey
// @ 0x75d250]. Witness record: docs/mnu/menu-re.md ("Menu strings").

#include <formats/rtxt/rtxt.h>

#include <map>
#include <memory>
#include <string>

namespace opennova::menu {

// The section every menu lookup reads [orig: CUIStringTable_LookupString @ 0x6527c0
// passes "menu" to TextResource_FindEntryBySectionAndKey @ 0x75d250, which searches the
// first section of that name].
inline constexpr const char *kMenuTextSection = "menu";

// The string tables a screen reads, by TEXT_RSRC name. A lookup names a table
// (null: the window and its root author none) and a key; it finds the key in the
// first "menu" section (case-insensitive, first match), searching the expansion's
// override table first, and only when the named table loaded [orig:
// CUIStringTable_LookupString @ 0x6527c0 returns 0 for a NULL name or a file that
// does not load; TextResource_FindEntryBySectionAndKey @ 0x75d250 recurses into
// g_TextOverrideTable @ 0x33429B0 first]. There is no shell fallback table: a miss
// is null and the caller shows the key.
class MenuTextTables {
public:
	void clear();
	// The expansion's table (expansion\<exp>\<exp>.bin), borrowed; null for none
	// [orig: TextResource_LoadOverrideTable @ 0x75d5c0 from Expansion_LoadAssets].
	void set_override(const rtxt::File *table) { override_ = table; }
	// A TEXT_RSRC name's file; null records that it did not load. Names compare
	// case-insensitively [orig: the per-file cache's stricmp].
	void set_table(const std::string &name, std::shared_ptr<const rtxt::File> file);
	bool has_table(const std::string &name) const;
	// Whether the table `name` names loaded (a lookup through it can find a key).
	bool loaded(const std::string &name) const;
	// The text for `key` through the table `name` names, null on a miss.
	const std::string *lookup(const std::string *name, const std::string &key) const;

private:
	const rtxt::File *override_ = nullptr;
	std::map<std::string, std::shared_ptr<const rtxt::File>> tables_; // lowercased name
};

} // namespace opennova::menu
