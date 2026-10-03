#include <editor/project/expansion_files.h>

#include <iterator>

#include <base/io/strutil.h>
#include <runtime/audio/music_policy.h>

namespace opennova::editor {

namespace {

using R = ExpansionFileRole;
using P = ExpansionPlacement;

constexpr ExpansionFileRow kRows[] = {
	// Loose only: the table's read is path-qualified, which no archive entry matches; the Mods list's
	// read takes the same loose file first (docs/vfs/vfs-pff-mount-re.md § Expansions item 1).
	{ R::Table, "expansion_table", "", ".bin", false, nullptr, AssetKind::Strings, P::Folder,
	  "the expansion's text table, which its strings override the game's by and the Mods list names it by",
	  "[orig: TextResource_LoadOverrideTable(\"expansion\\<n>\\<n>.bin\") @ 0x4a49de through File_LoadResource "
	  "@ 0x75b540; Expansion_ScanAndRegister's [exp_info] read @ 0x4a455b]" },
	{ R::Version, "expansion_version", "version.txt", "", true, nullptr, AssetKind::Text, P::Folder,
	  "the expansion's version text, whose CRC a joiner must match",
	  "[orig: Expansion_LoadAssets @ 0x4a4858..0x4a4885; Server_ValidatePlayerJoinRequest @ 0x51232f]" },
	{ R::MenuMusicBank, "expansion_menumus_sbf", audio::kMenuMusicExpansionPrefix, ".sbf", false, "MENUMUS.SBF",
	  AssetKind::MusicBank, P::Folder, "the expansion's menu music bank",
	  "[orig: Expansion_LoadAssets \"expansion\\%s\\M%s.sbf\" @ 0x4a4906]" },
	{ R::MenuMusicScript, "expansion_menumus_bin", audio::kMenuMusicExpansionPrefix, ".bin", false, "MENUMUS.BIN",
	  AssetKind::MusicScript, P::ByKind, "the expansion's menu music script",
	  "[orig: Expansion_LoadAssets \"M%s.bin\" @ 0x4a491d]" },
	{ R::GameMusicBank, "expansion_gamemus_sbf", audio::kGameMusicExpansionPrefix, ".sbf", false, "GAMEMUS.SBF",
	  AssetKind::MusicBank, P::Folder, "the expansion's mission music bank",
	  "[orig: Expansion_LoadAssets \"expansion\\%s\\G%s.sbf\" @ 0x4a4936]" },
	{ R::GameMusicScript, "expansion_gamemus_bin", audio::kGameMusicExpansionPrefix, ".bin", false, "GAMEMUS.BIN",
	  AssetKind::MusicScript, P::ByKind, "the expansion's mission music script",
	  "[orig: Expansion_LoadAssets \"G%s.bin\" @ 0x4a494a]" },
	{ R::LocalBank, "expansion_locl_lwf", "", "L.lwf", false, nullptr, AssetKind::SoundBank, P::ByKind,
	  "the expansion's first mission sound bank",
	  "[orig: Expansion_LoadAssets \"%sL.lwf\" @ 0x4a4989 into the bank table @ 0x82a5b0, @ 0x4a499d]" },
	{ R::Bank, "expansion_lwf", "", ".lwf", false, nullptr, AssetKind::SoundBank, P::ByKind,
	  "the expansion's second mission sound bank",
	  "[orig: Expansion_LoadAssets \"%s.lwf\" @ 0x4a495e into the bank table, @ 0x4a4972]" },
};

static_assert(std::size(kRows) == kExpansionFileRoleCount, "one row per ExpansionFileRole");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kExpansionFileRoleCount; ++i)
		if (static_cast<size_t>(kRows[i].role) != i) return false;
	return true;
}
static_assert(rows_in_order(), "the rows follow ExpansionFileRole's order");

} // namespace

const ExpansionFileRow &expansion_file_row(ExpansionFileRole role) {
	const size_t index = static_cast<size_t>(role);
	return kRows[index < kExpansionFileRoleCount ? index : 0];
}

const ExpansionFileRow *expansion_file_row_for_manifest_role(const std::string &token) {
	for (const ExpansionFileRow &row : kRows)
		if (token == row.manifest_role) return &row;
	return nullptr;
}

std::string expansion_file_name(const ExpansionFileRow &row, const std::string &name) {
	if (row.fixed) return row.prefix;
	// The music pairs by the runtime's own naming (audio::music_pair_names), the rest by the row.
	switch (row.role) {
	case R::MenuMusicBank: return audio::menu_music_pair_names(name).bank_file;
	case R::MenuMusicScript: return audio::menu_music_pair_names(name).script_file;
	case R::GameMusicBank: return audio::game_music_pair_names(name).bank_file;
	case R::GameMusicScript: return audio::game_music_pair_names(name).script_file;
	default: return std::string(row.prefix) + name + row.suffix;
	}
}

std::vector<ExpansionFile> expansion_files(const std::string &name) {
	std::vector<ExpansionFile> out;
	if (name.empty()) return out;
	for (const ExpansionFileRow &row : kRows) out.push_back({ &row, expansion_file_name(row, name) });
	return out;
}

const ExpansionFileRow *expansion_file_for(const std::string &name, const std::string &logical_name) {
	if (name.empty()) return nullptr;
	for (const ExpansionFileRow &row : kRows)
		if (strutil::iequals(expansion_file_name(row, name), logical_name)) return &row;
	return nullptr;
}

} // namespace opennova::editor
