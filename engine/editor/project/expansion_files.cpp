#include <editor/project/expansion_files.h>

#include <iterator>

#include <base/io/strutil.h>

namespace opennova::editor {

namespace {

using R = ExpansionFileRole;
using P = ExpansionPlacement;

constexpr ExpansionFileRow kRows[] = {
	// Loose only: the table's read is path-qualified, which no archive entry matches; the Mods list's
	// read takes the same loose file first (docs/vfs/vfs-pff-mount-re.md § Expansions item 1).
	{ R::Table, "expansion_table", AssetKind::Strings, P::Folder,
	  "the expansion's text table, which its strings override the game's by and the Mods list names it by" },
	{ R::Version, "expansion_version", AssetKind::Text, P::Folder,
	  "the expansion's version text, whose CRC a joiner must match" },
	{ R::MenuMusicBank, "expansion_menumus_sbf", AssetKind::MusicBank, P::Folder, "the expansion's menu music bank" },
	{ R::MenuMusicScript, "expansion_menumus_bin", AssetKind::MusicScript, P::ByKind,
	  "the expansion's menu music script" },
	{ R::GameMusicBank, "expansion_gamemus_sbf", AssetKind::MusicBank, P::Folder,
	  "the expansion's mission music bank" },
	{ R::GameMusicScript, "expansion_gamemus_bin", AssetKind::MusicScript, P::ByKind,
	  "the expansion's mission music script" },
	{ R::LocalBank, "expansion_locl_lwf", AssetKind::SoundBank, P::ByKind,
	  "the expansion's first mission sound bank" },
	{ R::Bank, "expansion_lwf", AssetKind::SoundBank, P::ByKind, "the expansion's second mission sound bank" },
};

static_assert(std::size(kRows) == kExpansionFileRoleCount, "one row per ExpansionFileRole");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kExpansionFileRoleCount; ++i)
		if (static_cast<size_t>(kRows[i].role) != i) return false;
	return true;
}
static_assert(rows_in_order(), "the rows follow ExpansionFileRole's order");

} // namespace

const gameprofile::RequiredResource *ExpansionFileRow::resource() const {
	return gameprofile::gameprofile_required_resource_by_role(manifest_role);
}

bool ExpansionFileRow::fixed() const { return !gameprofile::gameprofile_expansion_file_formed(resource()); }

const char *ExpansionFileRow::replaces() const {
	const gameprofile::RequiredResource *row = resource();
	return row ? row->replaces : nullptr;
}

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
	return gameprofile::gameprofile_expansion_file_name(row.resource(), name);
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
