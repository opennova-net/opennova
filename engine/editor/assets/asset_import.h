#pragma once

#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// A native file: loose, a named member of one PFF, or (install) the effective file of
// a mounted game install, resolved the way a stock launch resolves it (the archive
// table's precedence; a loose file beside the archives is not what it reads, as it is
// only under /d). Importing makes an editable
// project copy; a scene text the Blender add-on writes (`.o3d`, `.o3a`) converts once
// into the native files it makes, the source not kept (editor/import/converter); a
// loose source an importer keeps converting (a PNG from the disk) is copied with its
// sidecar (editor/import/importer), while the game's own file (from the install or an
// archive) is copied as it is, with no record: a PNG of the game stays a texture.
struct ImportSource {
	std::string path;
	std::string entry;    // empty for a loose file; the logical name of a member or an install's
	bool install = false; // `path` is the game install to mount
	// A loose file copied as the game's own, as it is and with no import record (a PNG stays
	// the texture a menu names): a file the import plan found beside the file that names it.
	// A loose file picked from the disk is the author's source otherwise.
	bool native = false;
	std::string name() const;
};

inline bool operator==(const ImportSource &a, const ImportSource &b) {
	return a.path == b.path && a.entry == b.entry && a.install == b.install && a.native == b.native;
}
inline bool operator!=(const ImportSource &a, const ImportSource &b) { return !(a == b); }

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
