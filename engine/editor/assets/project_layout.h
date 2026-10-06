#pragma once

#include <string>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>

namespace opennova::editor {

// Where the project keeps its files (DI-03, the deep-integration plan): the one placement rule every
// file the editor makes follows (Create missing, New file, an import's destinations and a converter's
// outputs), so a flat project stays flat and one laid out by kind stays laid out by kind. A folder is
// organization only (ADR 0046 d6): the game finds a file by its flat name (the build packs and copies
// it by name alone), so where a file sits never changes what the game reads.

// How the project lays its files out, from the files it has: Flat when more of them sit at the top
// level than in their kind's folder (AssetKindRow::folder), ByKind otherwise (a project with no file
// yet among them: the kinds' folders, as the editor has always made them). A file of a kind kept at
// the top level by its row (a configuration, the score table, a text) says neither, nor does one in a
// folder of the author's own (art/); an import source counts as the kind its name gives (a PNG a
// texture), and an import's output, made under the cache, not at all.
enum class ProjectLayout { ByKind, Flat };
ProjectLayout project_layout(const AssetScan &scan);
// Its wire word: "by_kind" or "flat".
const char *project_layout_token(ProjectLayout layout);

// The folder a new file of `kind` goes in (project-relative, '/'-separated, "" the top level): beside
// the project's files of its kind, in the folder holding the most of them (of two holding as many, the
// kind's own folder, else the top level, else the first by its path); with none, where the project
// keeps most of its files (project_layout: the top level of a flat project, the kind's folder of one
// laid out by kind). An import source is placed as the kind its name gives (a PNG beside the
// textures it makes); an import's outputs are no file of a folder and are never counted.
std::string placement_folder(const AssetScan &scan, AssetKind kind);
// Where a new file named `name` of `kind` goes (project-relative): placement_folder's folder joined
// with the name.
std::string placement_path(const AssetScan &scan, const std::string &name, AssetKind kind);

// A folder a request names as a person writes it ("defs", "defs/", "/textures", "art\\terrain", "" or
// "/" the top level) in the project's form: '/'-separated, no leading or trailing separator, "." steps
// taken out. False, with `why` in words, for one that leaves the project ("..", a drive or a rooted
// path), or goes through a folder the project's walk never enters (a name starting with '.': the cache
// and every other dot-folder), so a file moved there would no longer be one of the project's.
bool normalize_project_folder(const std::string &text, std::string &out, std::string &why);

} // namespace opennova::editor
