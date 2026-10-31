#pragma once

#include <map>
#include <memory>
#include <string>

#include <base/io/json.h>
#include <base/resource_index/file_kind.h>
#include <base/vfs/file_source.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The project file (ADR 0046 d6): `project.opennova` at the project root, versioned
// UTF-8 JSON. Unknown schema versions are rejected outright; pre-1.0 there are no
// migration readers. Everything machine-local (paths, window state) lives in
// `.opennova/local.json` instead, so this file is what a modder commits. Schema 2 (S16) added
// the expansion object; a schema 1 file is refused with what changed. The optional `novaworld` key
// (ADR 0069) came later within schema 2: a file without it reads as before.
inline constexpr int kProjectSchemaVersion = 2;
inline constexpr const char *kProjectFileName = "project.opennova";
inline constexpr const char *kProjectCacheDirName = ".opennova";
inline constexpr const char *kLocalSettingsFileName = "local.json";
inline constexpr const char *kImportSidecarSuffix = ".import";
inline constexpr const char *kImportCacheFileName = "import_cache.json";
inline constexpr const char *kBuildCacheFileName = "build_cache.json";
inline constexpr const char *kTextureHeaderCacheFileName = "texture_headers.json";
inline constexpr const char *kDefaultExportOutput = "build/export";

// The game a project that names none targets: the default title's code (base/title/title.h
// default_title).
std::string default_target_game();

// Which requirement groups the project turns on (ADR 0046 d7): the menu set is always
// required, so no switch names it; the mission set only when the project enables missions.
struct ProjectFeatures {
	bool mission = false;
	bool multiplayer = false;
};

struct ProjectExportSettings {
	std::string output = kDefaultExportOutput; // project-relative export directory
	bool include_runtime = false;              // copy the runtime beside the data on export
};

// What the project is to the game's expansions (ADR 0046 S16): built as one of its own, played with
// `/exp <name>` from `expansion\<name>\`, and built on an installed one (the import mounts the base
// game and that expansion as the game does). Both empty: a standalone project of the base game. The
// game reads an expansion's own files (its table, banks and music) only under `/exp`, so a project
// builds on an installed expansion only as an expansion of its own [orig: Expansion_LoadAssets
// @ 0x4a4906..0x4a49de]. Each name keeps expansion_name.h's rule.
//
// `base_project` (ADR 0046 T5) names the base game as a project of its own rather than the game
// install's: the folder of a standalone project of the same game, taken from this project's folder
// when relative ('/'-separated, so a clone finds it where the repository keeps it). The base game is
// then that project's export (its project.opennova's export.output: the base game as it ships), which
// the expansion imports from, compares its files with, and plays over, where a project on the install's
// base game uses the install's archives (project/base_project.h). It needs a name, and an expansion of
// a project's base builds on no installed expansion.
struct ProjectExpansion {
	std::string name;         // builds as expansion\<name>\ ("" = standalone)
	std::string builds_on;    // an installed expansion ("" = the base game); requires `name`
	std::string base_project; // the base game's project ("" = the game install's); requires `name`

	bool standalone() const { return name.empty(); }
	bool on_base_project() const { return !base_project.empty(); }
	bool operator==(const ProjectExpansion &other) const {
		return name == other.name && builds_on == other.builds_on && base_project == other.base_project;
	}
	bool operator!=(const ProjectExpansion &other) const { return !(*this == other); }
};

// The archive each of the game's own files came from (2026-10-24): an import of a game install records,
// for each archive member it copies, the archive that served it, and a build packs the file there rather
// than where its kind's row would (archive_routing.h). Keyed by the normalized logical name
// (pff::normalized_logical_name); Language, Localres or Resource, an expansion's Language its <b>L.pff and
// the others its <b>.pff. The game finds a name in any of its archives, the first that holds it
// [orig: FileSystem_OpenFile @ 0x75b1c0 over the slots PFF_OpenAllArchives @ 0x4a4310 fills], so what the
// record keeps is the install's own layout, for what reads a file by its archive: a retail patch writes a
// file into the archive a stock install holds it in, and a copy in another archive stands behind the stock
// one where that archive comes first (NovaLogic packs a kind across archives: JO:CA's voice lines in
// language.pff, its other waves in localres.pff). A file the project adds goes by its kind.
using ArchiveRecord = std::map<std::string, ArchiveSlot>;

struct ProjectDocument {
	int schema_version = kProjectSchemaVersion;
	std::string project_id;   // a UUID minted at creation, stable for the project's life
	std::string title;
	std::string target_game = default_target_game(); // a title code (base/title/title.h): jo, dfx2, ...
	ProjectFeatures features;
	ProjectExportSettings export_settings;
	ProjectExpansion expansion; // `"expansion": {"name", "builds_on", "base_project"?}`, absent when standalone
	// `"archives": {"language": [names], "localres": [...], "resource": [...]}`, absent when empty
	ArchiveRecord archives;
	// The NovaWorld project this one is (ADR 0069), by its key (`"novaworld": "ACIDGAMBIT"`, absent when
	// "": not on NovaWorld). Every copy of the project names it, on every machine, so each folder of it finds
	// its project on whatever service it signs in to; the folder's own state there is under
	// `.opennova/novaworld/` (novaworld/novaworld_state.h).
	std::string novaworld;
};

// Whether `key` is a NovaWorld project's key as the service takes one: 1 to 16 of A-Z, 0-9 and '_'.
bool is_novaworld_key(const std::string &key);

// Where a project keeps things, derived from its root. `cache_dir` and everything
// under it is the machine's own and self-ignored (`.opennova/.gitignore`), and disposable but
// for `novaworld/<host>/`, each service's base: lose that and a pull has nothing to merge over.
struct ProjectPaths {
	std::string root;
	std::string project_file;
	std::string cache_dir;
	std::string local_settings_file;
	std::string imported_dir;
	std::string import_cache_file; // what this machine last saw of each import source
	std::string index_dir;
	std::string build_dir;
	std::string build_cache_file; // each file's content hash by its size and last write (BuildRun)
	// Each texture's header as a reader reads it, by its size and last write (graph/texture_checks.cpp).
	std::string texture_headers_file;
	std::string run_dir;     // where Play runs the game: a directory per mode, a numbered one a run in it
	                         // (`<mode>/<n>`, run/run_directory.h)
	std::string staging_dir; // an import's files before they are published (import_assets)
	// Where the files are read from: the root folder (null), or a source of their own, each file by its
	// logical name (the game install as the game is served it, which the game's own data's fold validates
	// as a project of its own: session/original_files.h).
	std::shared_ptr<const FileSource> files;
	// The game install's files an expansion's Play copied where they could not be linked into its run
	// directory (another volume), a folder per install, kept for the next (prepare_expansion_run).
	std::string install_copy_dir;
	// Where a file the editor deletes goes (DI-25, project/project_trash.h): a numbered folder per delete,
	// the files under it at their project-relative paths, never emptied by the editor.
	std::string trash_dir;

	static ProjectPaths for_root(const std::string &root);
	// The project-relative export output resolved against the root, with no trailing separator (an
	// export's staging and set-aside folders are its siblings, named after it).
	std::string export_dir(const ProjectDocument &doc) const;
};

// The folders beside an export folder that an export stages in and sets the last one aside in
// (project_build/export_build.h): `<folder>.tmp`, `<folder>.old`. A project's scan passes over them as it
// passes over the export folder, so one an export left behind is no file of the project's.
inline constexpr const char *kExportStagingSuffix = ".tmp";
inline constexpr const char *kExportPreviousSuffix = ".old";

// The cache directory with its self-ignore file (`.opennova/.gitignore` = `*`). The
// file ignores itself too, so a clone never has it: whatever writes under the cache
// (project creation, the import pass, the build) comes through here first, and the
// cache stays out of the modder's repository on every machine.
bool ensure_project_cache_dir(const ProjectPaths &paths, std::string &error);

io::JsonValue project_document_to_json(const ProjectDocument &doc);
// False with `error` set for a wrong schema version, a missing/invalid field, an
// unknown target game or an expansion the rule refuses (check_project_expansion: one the
// target game has none of, a name the game cannot take); `out` is left untouched on failure.
bool project_document_from_json(const io::JsonValue &json, ProjectDocument &out, Diagnostic &error);

bool load_project_document(const std::string &project_file, ProjectDocument &out, Diagnostic &error);
// Atomic (tmp + rename); the text is json_write's deterministic form.
bool save_project_document(const std::string &project_file, const ProjectDocument &doc,
                           Diagnostic &error);

// Whether a project at `root` would overlap a game install, said in `error` (project.in_install):
// `root` holds the game's boot archives (vfs_has_boot_archive: an install, or a game exported from
// one), sits inside a folder that does, or holds `install` (the install the project is made on or
// opens with; "" for none). The editor writes every file of its project, so a project there would
// rewrite the game's own files in the writers' canonical form. can_create_project and open_project
// ask it with no install; the session asks again with the one it knows.
bool project_overlaps_install(const std::string &root, const std::string &install, Diagnostic &error);

// Whether a project could be made at `root` for `target_game` (a title code) as `expansion`
// (the expansion's rule, check_project_expansion: what an install has is the caller's to weigh,
// expansion_install_findings): the directory must not already hold a project file nor overlap a
// game install (project_overlaps_install). Nothing is written; create_project asks the same first.
bool can_create_project(const std::string &root, const std::string &target_game, Diagnostic &error,
                        const ProjectExpansion &expansion = ProjectExpansion());

// Create a project at `root`: the directory (created if missing) must not already hold a
// project file. Writes `project.opennova` and the self-ignoring cache directory, returns
// the new document. `target_game` must be a title code; `novaworld` the NovaWorld project's
// key it is made for ("" none).
bool create_project(const std::string &root, const std::string &title, const std::string &target_game,
                    ProjectDocument &out, Diagnostic &error,
                    const ProjectExpansion &expansion = ProjectExpansion(),
                    const std::string &novaworld = std::string());

// The project rooted at `root` (its project file must exist, and the folder overlap no game
// install: project_overlaps_install).
bool open_project(const std::string &root, ProjectDocument &out, Diagnostic &error);

} // namespace opennova::editor
