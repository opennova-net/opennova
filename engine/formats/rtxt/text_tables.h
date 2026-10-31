#pragma once

// THE NAMED STRING TABLES: the process's registry of loaded RTXT tables by name, where every
// reader of a game text finds the table it reads. Retail keeps one global per table it loads at
// init: gametext.bin (g_TextGameText), gameerr.bin, vmacros.bin and keyhelp.bin
// [orig: Game_InitSubsystems @0x4A6CD0], the menu resource (game.bin, the menu tokens' table
// [orig: Menu_InitShellResources @0x552510]) and the mission's text bin
// [orig: TextResource_LoadMissionTextBin @0x51ED90]. The embedder registers each table it loads
// under the name its readers spell; a name is matched without case.
//
// Every lookup here searches the process's text-override table first (rtxt.h
// find_with_text_override, retail's g_TextOverrideTable: the active expansion's text bin
// [orig: TextResource_LoadOverrideTable @0x75D5C0]); with no table registered under the name, the
// override table is not searched (GameText_GetString's order: no table, no search).

#include <formats/rtxt/rtxt.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace opennova::rtxt {

// The registered names the game reads by.
inline constexpr const char *kTableGameText = "gametext"; // gametext.bin
inline constexpr const char *kTableMission = "mission";   // <mission>.bin, else medmssn.bin
inline constexpr const char *kTableMenuTxt = "menutxt";   // the menu resource's table
inline constexpr const char *kTableGameUi = "gameui";     // the menu's own text table
// keyhelp.bin: the "Keys" table every key-binding label resolves through (the embedder installs
// it as the binding labels' table when it registers it).
inline constexpr const char *kTableKeyHelp = "keyhelp";
// gameerr.bin: the "Generic Error Strings" the post-mission error dialog shows.
inline constexpr const char *kTableGameErr = "gameerr";

// The sections the game reads by name.
inline constexpr const char *kSectionOverlays = "Overlays";
inline constexpr const char *kSectionWpNames = "WPNames";
inline constexpr const char *kSectionClient = "Client";
inline constexpr const char *kSectionLoadingText = "LoadingText";
inline constexpr const char *kSectionMenu = "Menu";
inline constexpr const char *kSectionMisc = "Misc";
inline constexpr const char *kSectionGenericErrors = "Generic Error Strings";

// Register `table` under `name` (null unregisters it).
void register_table(std::string_view name, std::shared_ptr<const File> table);
// The table registered under `name`, or null.
std::shared_ptr<const File> registered_table(std::string_view name);
// Unregister every table.
void clear_tables();

// The section lookup over the table registered under `name`, the override table first: the
// entry's text, or false with no table or no entry.
bool table_text(std::string_view name, const std::string &section, const std::string &key,
		std::string &out);
// That text, or `fallback` (GameText_GetStringWithFallback's form: the HUD's and the menu
// tokens') [orig: GameText_GetStringWithFallback @0x51EB90].
std::string table_text_or(std::string_view name, const std::string &section,
		const std::string &key, const std::string &fallback);
// The lookup whose miss is the visible "??section:key??" marker (rtxt.h lookup_with_override:
// the override table alone answers with no table registered) [orig: GameErr_GetString @0x4C2C60].
std::string table_lookup(std::string_view name, const std::string &section,
		const std::string &key);
// table_lookup with the {hot} accelerator marker stripped for display (the miss marker carries
// no {hot} token, so stripping the result is exact).
std::string table_lookup_display(std::string_view name, const std::string &section,
		const std::string &key);

// The process's text-override table from the bytes of the file that serves it (base/vfs
// vfs_expansion_override_table, docs/interface/rtxt-strings-re.md "The override table's
// source"): a table that parses becomes the one every lookup searches first, anything else
// (no bytes, a file that does not parse) leaves none [orig: TextResource_LoadOverrideTable
// @0x75D5C0].
void install_text_override_bytes(const uint8_t *data, size_t size);

// A menu UI token: the menu resource's "Menu" section, then the menu's own table's, else the
// fallback (the armory, player-info and DEATH-screen tokens all resolve this way)
// [orig: TextResource_GetStringWithFallback(resource, "Menu", key) @0x562EE0].
std::string menu_text(const std::string &key, const std::string &fallback);
// The menu token's text by that chain, or false when neither table has it.
bool find_menu_text(const std::string &key, std::string &out);

} // namespace opennova::rtxt
