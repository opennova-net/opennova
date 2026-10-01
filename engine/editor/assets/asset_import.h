#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/import_choice.h>
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
std::vector<ImportChoice> list_import_choices(const std::vector<std::string> &paths,
                                             std::vector<Diagnostic> &diagnostics);
// A game install mounted into `game` as a stock launch of the project's game mounts it
// (mount_install with no /d and no expansion: the witnessed archive table, read alone
// while one of its archives is mounted), its payloads decoded with that game's key: the
// one mount behind the retail listings, a retail source's import and the import plan.
// False when the folder holds none of the game's archives.
bool mount_retail(Vfs &game, const std::string &retail_root, const ProjectDocument &document);
// A file of a mounted install or archive as its game loader is served it (S13 D9): decoded as the
// game's text readers decode a stored file (Vfs::read_file), or, for a kind whose loader takes the
// SCR form under a key of its own and unwraps it itself (a shader: AssetKindRow::scr, ScrForm::Shader),
// the bytes as stored. What an import copies into the project and what its plan reads.
bool read_served(const Vfs &game, const std::string &name, std::vector<uint8_t> &out);
// Every effective file of a game install, mounted as a stock launch mounts it
// (mount_retail): the "Import from game data" list.
std::vector<ImportChoice> list_retail_import_choices(const std::string &retail_root, const ProjectDocument &document,
                                                    std::vector<Diagnostic> &diagnostics);
// The logical names a game install resolves, sorted by their normalized form (the
// Problems Import fixes).
std::vector<std::string> list_retail_file_names(const std::string &retail_root, const ProjectDocument &document);
// Where an import writes a file of `kind` (project-relative): over the project's file of
// the name when it has one (a replace keeps its place), else in the kind's folder
// (AssetKindRow::folder).
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
// An AssetImport run to its end.
ImportResult import_assets(const std::vector<ImportChoice> &sources, const ProjectPaths &paths,
                           const ProjectDocument &document, bool replace_existing);

// The import a step at a time (ADR 0046 S14: a mission's closure is thousands of files and
// hundreds of megabytes; S13 A3's rule for every long job): the project scanned (ProjectScan),
// then a source a step, read, checked as import_assets says and, while none was refused, staged
// at once, its bytes dropped (one file is held at a time, never the selection); then, when none
// was refused, a file published a step. A refusal stages nothing more and, once every source was
// checked (each refusal said), removes the stage: the project is as it was. It can be abandoned
// until it publishes its first file, not after.
class AssetImport {
public:
	AssetImport(std::vector<ImportChoice> sources, const ProjectPaths &paths, const ProjectDocument &document,
	            bool replace_existing);
	~AssetImport();
	AssetImport(const AssetImport &) = delete;
	AssetImport &operator=(const AssetImport &) = delete;

	// One step within `bytes` read and written (at least one source or one file); true once done.
	bool step(uint64_t bytes);
	bool done() const;
	// True from its first published file on: it runs to its end from there.
	bool publishing() const;
	// Stops before it publishes: what it staged is removed. Nothing after it publishes.
	void abandon();
	// Its progress in files: the sources checked and the files published, of the sources and the
	// files they make (known as each is staged, so the total grows while it checks).
	size_t files_done() const;
	size_t files_total() const;
	// What it came to, once done.
	ImportResult take();

private:
	class Run;
	std::unique_ptr<Run> run_;
};

} // namespace opennova::editor
