#pragma once

#include <string>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// An expansion project's base game when it is a project of its own (ADR 0046 T5; ProjectExpansion::
// base_project): a modder's game (a standalone project, built and exported) with an expansion beside it,
// as the game's own data ships an expansion over its base. The game mounts the expansion's two archives
// over whatever three boot archives its working directory holds [orig: PFF_OpenAllArchives @ 0x4a4310
// over the name table @ 0x829f90; Expansion_LoadAssets @ 0x4a4730], so the base game an expansion plays
// over is a folder laid out as an install: the base project's export (export.output, which the editor's
// Export and `opennova-project export` fill: its boot archives, its loose files, export.json), the base
// game as it ships. Everything the editor reads of the base game for such an expansion reads that folder
// in place of the game install's: the import's listing, the base's names its gate reads, lean packing's
// comparison, the Play run's base. The game install stays the program the game install's Play starts.

// The base project's folder: `base_project` taken from `project_root` when relative, normalized, with
// no trailing separator; "" for an expansion on the game install's base game.
std::string base_project_root(const std::string &project_root, const ProjectExpansion &expansion);

// The base game's folder of an expansion on a project's base game: the base project's export folder
// (ProjectPaths::export_dir of its document). False with `error` (project.base_project) where the
// folder holds no project, its project does not read, it builds as an expansion itself (an expansion
// stands over a base game, never over another expansion: the game mounts one), or it is a project of
// another game; `out` is then the folder the export would be in where the project reads, else "".
// Whether the export is there is the caller's to weigh (an export not made yet: export the base first).
bool base_project_game_dir(const std::string &project_root, const ProjectExpansion &expansion,
                           const std::string &target_game, std::string &out, Diagnostic &error);

// Whether the folder holds a base game an expansion can play over (the export's three boot archives, an
// install's) is base/vfs's vfs_has_boot_archive.

} // namespace opennova::editor
