// The process-wide "Keys" text table every key-binding label resolves through:
// retail's g_TextKeyHelp (keyhelp.bin, loaded at boot beside gametext.bin
// [orig: Game_InitSubsystems @0x4a6cd0]). The three binding formatters
// (controls::format_binding, controls::format_display_string, the per-VK
// controls::key_name) look every prefix, separator, mouse name and key name
// up here with the binary's own literal as the fallback
// [orig: KeyHelp_GetStringWithFallback @0x51ed40].
//
// The shell installs the parsed keyhelp.bin at the boot point that loads the
// text tables; ctests install a synthetic table. Until a table is installed
// every lookup falls back.

#pragma once

#include <string>

#include <formats/rtxt/rtxt.h>

namespace opennova::controls {

// Install (copy) the "Keys" table. Replaces any earlier table.
void set_key_strings(rtxt::File table);

// Forget the installed table: every lookup falls back again.
void clear_key_strings();

bool has_key_strings();

// The "Keys" lookup: the entry text when the installed table's section
// "Keys" carries `key` (case-insensitive, first match), else the fallback
// [orig: KeyHelp_GetStringWithFallback @0x51ed40 -- g_TextGameText null ->
//  fallback @0x51ed47; TextResource_FindEntryBySectionAndKey(g_TextKeyHelp,
//  section, key) miss or null text -> fallback @0x51ed76/@0x51ed7e]. The
// original gates on the GAMETEXT global and returns the fallback RAW (the
// caller literals carry the "XX" untranslated marker, e.g. "XXCtrl - ", so a
// missing table shows the marker on screen); this port gates on its own
// table and strips the "XX" marker from the fallback (it leads the literal or
// follows its leading blanks: "XXCtrl - " -> "Ctrl - ", " XXor " -> " or ")
// -- the one deliberate deviation, kept so a shell that has not installed
// keyhelp.bin shows the marker-stripped English the rest of the catalog
// already uses (see action_class_name). With the shipped table installed
// every label is the retail text ("Ctrl-", "Shift-", " or ", "Key 226", ...).
std::string key_string(const char *key, const char *fallback);

// The same lookup in any section of the table (the help screen's "Text"
// rows: HELPTITLE, PAGE, CHANGE_SCREEN, the class names and each action's
// help text). The literals there carry the "!" / "|" untranslated marker
// ("!Help - %s"), stripped on the fallback like the "XX" one above.
std::string key_help_string(const char *section, const char *key, const char *fallback);
// The same lookup returning the fallback untouched, as the original does (the Tab board's paging hint keeps its
// "!" marker on screen with no table): the entry text where the installed table's section carries `key`, else
// `fallback` [orig: KeyHelp_GetStringWithFallback @0x51ed40, @0x51ed76].
std::string key_help_string_raw(const char *section, const char *key, const char *fallback);

}  // namespace opennova::controls
