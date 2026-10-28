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
// (ProjectPaths::export_dir of its document, which `document` takes when given). False with `error`
// (project.base_project) where the folder holds no project, its project does not read, it builds as an
// expansion itself (an expansion stands over a base game, never over another expansion: the game mounts
// one), or it is a project of another game; `out` is then the folder the export would be in where the
// project reads, else "". Whether the export is there, and the project as it stands, is base_export_of's.
bool base_project_game_dir(const std::string &project_root, const ProjectExpansion &expansion,
                           const std::string &target_game, std::string &out, Diagnostic &error,
                           ProjectDocument *document = nullptr);

// The base game's project of an expansion on a project's base game, and its export as the expansion
// weighs it. The export is current when its record names the base project and keeps the stamp of the
// project's files as they stand (export_build.h's project_files_stamp, which the export recorded as its
// build was planned): the expansion plays over it as it is. Otherwise the editor exports the base project
// again before it reads it (session/base_export.h), as the server's release build exports a base game
// before its expansion, where the export may replace the folder (export_replaces: none there, an empty
// one, an export of the base project); a folder of anything else is the person's, never written over.
// Nothing is read but the base's project file, the export's record and the base's folder listings.
struct BaseExport {
	std::string root;         // the base project's folder ("" for an expansion on the game install's base game)
	std::string export_dir;   // its export folder, where its project reads (base_project_game_dir's `out`)
	Diagnostic refusal;       // project.base_project where it serves as no base game
	bool current = false;     // the export is the base project's, made from its files as they stand
	bool replaceable = false; // an export of the base project may replace the folder
	bool resolved() const { return refusal.code().empty(); }
};
BaseExport base_export_of(const std::string &project_root, const ProjectExpansion &expansion,
                          const std::string &target_game);

// The base_project a project in `project_root` names the folder `folder` by: from the project's folder,
// '/'-separated ("../JO"), when `folder` is absolute and the two share a root; `folder` as it is otherwise (one
// relative already, another drive).
std::string base_project_from(const std::string &project_root, const std::string &folder);


// Whether the folder holds a base game an expansion can play over (the export's three boot archives, an
// install's) is base/vfs's vfs_has_boot_archive.

} // namespace opennova::editor
