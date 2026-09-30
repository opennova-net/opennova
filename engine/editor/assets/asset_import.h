#pragma once

#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/import_source.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

struct ImportResult {
	std::vector<std::string> imported; // project-relative paths, as published
	// After a failure while publishing: the files it did not reach (project-relative), the
	// one that failed first. Empty when every file was published, and when the import
	// failed before publishing (then it wrote nothing).
	std::vector<std::string> not_imported;
	std::vector<Diagnostic> diagnostics;
};

// Expand selected PFFs into selectable members; loose files stay single choices.
std::vector<ImportSource> list_import_sources(const std::vector<std::string> &paths,
                                             std::vector<Diagnostic> &diagnostics);
// A game install mounted into `game` as a stock launch of the project's game mounts it
// (mount_install with no /d and no expansion: the witnessed archive table, read alone
// while one of its archives is mounted), its payloads decoded with that game's key: the
// one mount behind the retail listings, a retail source's import and the import plan.
// False when the folder holds none of the game's archives.
bool mount_retail(Vfs &game, const std::string &retail_root, const ProjectDocument &document);
// Every effective file of a game install, mounted as a stock launch mounts it
// (mount_retail): the "Import from game data" list.
std::vector<ImportSource> list_retail_import_sources(const std::string &retail_root, const ProjectDocument &document,
                                                    std::vector<Diagnostic> &diagnostics);
// The logical names a game install resolves, sorted by their normalized form (the
// Problems Import fixes).
std::vector<std::string> list_retail_file_names(const std::string &retail_root, const ProjectDocument &document);
// Where an import writes a file of `kind` (project-relative): over the project's file of
// the name when it has one (a replace keeps its place), else in the kind's folder
// (blank_placement_dir).
std::string import_destination(const AssetScan &existing, const std::string &name, AssetKind kind);
// What `sources` make copied into the project (a converter's outputs, else the file
// itself), the whole selection or none of it as far as the disk allows (ADR 0046 S11g).
// Every file is read and checked first: its name, its kind, a clash with another selected
// file, a project file of the name (left as it is when the bytes are the same, written over
// only with `replace_existing`), and for an author's loose file an importer converts, the
// place of its import record (made from the importer's defaults when it has none); any
// refusal writes nothing. A converter's model naming a texture the import does not bring
// and the project does not have is a warning (import.texture_not_imported): its textures
// come with it only through its plan (import_plan.h). Then every file, and every record,
// is staged in the import's own folder under the cache (`paths.staging_dir`, on the
// project's volume, never scanned; one a crash left is removed first), and a failure
// staging removes the staged files and the folders made for them: the project is as it
// was. Then each file is renamed over its destination in order, after its record; a failure
// there stops it (a record written for a file that did not publish goes with it), the files
// published before it `imported`, it and the rest `not_imported`, each one a finding.
ImportResult import_assets(const std::vector<ImportSource> &sources, const ProjectPaths &paths,
                           const ProjectDocument &document, bool replace_existing);

} // namespace opennova::editor
