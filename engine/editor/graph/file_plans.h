#pragma once

#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

struct ImportedSource;

// Files' chores planned (DI-25, the deep-integration plan): a delete, a duplicate and a folder's rename, each
// planned over the scan before anything is written, so a window can say what it would do and a test can read
// it; the session commits them (session/file_chores.h) and keeps each as one step of the file history.

// The names a copy is given (duplicate_name), from the game's own rules on a name:
// - an archive's entry holds a name of at most 15 characters, its field 16 bytes with the NUL (the entry's
//   +16 name[16], vfs/vfs-pff-mount-re.md; a longer name is never found [orig: PFF_FindEntry @ 0x7685d0]),
//   so a name a copy is given is at most kCopyNameChars long where the build packs its kind (the editor's
//   own check, logical_name_fits_archive, takes the field's 16);
// - a model's stem at most kModelStemChars, so the textures the Blender add-on names after it,
//   `<model>_<i>.tga` and `<model>_<i>n.mdt`, still fit those 15 (ADR 0047's texture names; art/onjo1/README.md
//   "Names": a longer stem is cut to one two models share).
// Taken is a name the project has in any case (the game's lookup uppercases both sides [orig:
// PFF_SortEntries @ 0x768280, PFF_CompareSearchNameToEntry @ 0x768240]: normalized_logical_name), one the base
// game serves under an expansion, and one a file of its set would take (a mission's companions, an import
// source's outputs).
inline constexpr size_t kCopyNameChars = 15;
inline constexpr size_t kModelStemChars = 8;

// A delete: what goes to the trash as one batch, and what is kept.
struct DeletePlan {
	std::string path; // the file, project-relative
	std::string name; // its logical name
	// The project's files that go: the file, a mission's companions (the files the game finds by its name),
	// an import source's record.
	std::vector<std::string> files;
	// An import source's outputs: their logical names, and their folder under the cache, which goes with
	// them ("" none: kept, or no source).
	std::vector<std::string> outputs;
	std::string output_dir;
	// An import source deleted alone: each output kept as a file of the project, from its place under the
	// cache to the place the placement rule gives a file of its kind (assets/project_layout.h).
	bool alone = false;
	std::vector<std::pair<std::string, std::string>> kept;
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
	// Every file whose names the delete takes from the project (the file, its companions, an import source's
	// outputs where they go): what names one of them is left naming nothing.
	std::vector<std::string> named() const;
};
// Refused (file.unknown) for a file the project lacks, (file.imported) for an import's output (its source makes
// it again: delete the source), (file.exists) where a kept output's place holds a file. `alone` asks only of an
// import source.
DeletePlan plan_delete(const ProjectPaths &paths, const AssetScan &scan, const std::string &file, bool alone);

// A duplicate: the file copied under a new name in its own folder, with the files of its set.
struct DuplicatePlan {
	std::string path;     // the file, project-relative
	std::string name;     // its logical name
	std::string new_name; // the copy's
	std::string new_path; // project-relative
	// Each copy, from and to (project-relative), the file first: a mission's companions under the copy's base
	// name, an import source's record beside the copy (its import then makes the copy's outputs, named after
	// the copy).
	std::vector<std::pair<std::string, std::string>> copies;
	bool import = false;  // an import source copied with its record
	std::vector<std::string> outputs; // the outputs the copy's import makes, by name
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
};
// `new_name` "" takes duplicate_name's. Refused (file.unknown) for a file the project lacks, (file.imported)
// for an import's output (copy its source) or a mission's companion made by an import, (file.name) for a name
// the project's rules refuse (check_project_file_name), another extension, or none free, (file.exists) for a
// name taken. `alone`: an import source copied without its record (as a file the scan reads by its name).
DuplicatePlan plan_duplicate(const ProjectPaths &paths, const AssetScan &scan, const std::string &file,
                             const std::string &new_name, bool alone, const BaseNames *base = nullptr);
// The name a copy of `file` takes when none is asked: its stem's number counted on (oncrate1.3di makes
// oncrate2.3di, onjo_m1.bms onjo_m2.bms, items.def items2.def, a number's zeros kept), the stem cut to fit the
// rules above, the first name no file has nor any file of its set would take; "" when none is free.
std::string duplicate_name(const AssetScan &scan, const AssetEntry &file, bool with_record,
                           const BaseNames *base = nullptr);

// A folder renamed: every file of the project in it (at any depth) moved under the new name, each a move as
// Move to folder plans it (plan_move: the rename transaction, no site rewritten, an import source's record
// with it), and its folders, empty ones too, made under the new name.
struct FolderPlan {
	std::string from; // the folder, project-relative
	std::string to;   // its new path
	std::vector<RenamePlan> moves;
	std::vector<std::string> folders; // the folders under `from` (`from` first), as they were
	std::vector<Diagnostic> refusals;
	bool ok() const { return refusals.empty(); }
};
// `new_name` is the folder's own new name (one step, beside it). Refused (file.folder) for the top level, a
// name with folders, a dot-name, the folder's own name in any case or a folder outside the project, the
// export folder's or one the walk never enters; (file.unknown) for a folder that is not there; (file.exists)
// where a folder or a file holds the new name; (file.imported) for an import outside the folder that reads a
// file in it (`imports`); and each file's move refused as Move to folder refuses it, but for an import
// source whose record lists the files its import reads beside it, which moves when they all sit in the
// folder too (they are found from its folder, ImportSidecar::inputs, so they stay found).
FolderPlan plan_folder_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                              const std::string &folder, const std::string &new_name,
                              const std::vector<ImportedSource> *imports = nullptr);
// The project-relative path `path` (a file or a folder) as it is once `from` is renamed `to`: unchanged when
// it is not in `from`.
std::string path_in_renamed(const std::string &path, const std::string &from, const std::string &to);

// A folder a request makes or acts on, in the project's form (normalize_project_folder): refused, with why in
// `why`, for one outside the project, through a dot-folder, or in the export folder (no file of the project's
// sits there).
bool project_folder_of(const ProjectPaths &paths, const ProjectDocument &project, const std::string &text, std::string &out,
                       std::string &why);

} // namespace opennova::editor
