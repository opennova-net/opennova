#pragma once

#include <memory>
#include <string>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The project file (ADR 0046 d6): `project.opennova` at the project root, versioned
// UTF-8 JSON. Unknown schema versions are rejected outright; pre-1.0 there are no
// migration readers. Everything machine-local (paths, window state) lives in
// `.opennova/local.json` instead, so this file is what a modder commits. Schema 2 (S16) added
// the expansion object; a schema 1 file is refused with what changed.
inline constexpr int kProjectSchemaVersion = 2;
inline constexpr const char *kProjectFileName = "project.opennova";
inline constexpr const char *kProjectCacheDirName = ".opennova";
inline constexpr const char *kLocalSettingsFileName = "local.json";
inline constexpr const char *kImportSidecarSuffix = ".import";
inline constexpr const char *kImportCacheFileName = "import_cache.json";
inline constexpr const char *kBuildCacheFileName = "build_cache.json";
inline constexpr const char *kDefaultTargetGame = "jo";
inline constexpr const char *kDefaultExportOutput = "build/export";

// Which requirement groups the project turns on (ADR 0046 d7): the menu set is always
// required, the mission set only when the project enables missions.
struct ProjectFeatures {
	bool menu = true;
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
struct ProjectExpansion {
	std::string name;      // builds as expansion\<name>\ ("" = standalone)
	std::string builds_on; // an installed expansion ("" = the base game); requires `name`

	bool standalone() const { return name.empty(); }
	bool operator==(const ProjectExpansion &other) const {
		return name == other.name && builds_on == other.builds_on;
	}
	bool operator!=(const ProjectExpansion &other) const { return !(*this == other); }
};

struct ProjectDocument {
	int schema_version = kProjectSchemaVersion;
	std::string project_id;   // a UUID minted at creation, stable for the project's life
	std::string title;
	std::string target_game = kDefaultTargetGame; // a gameprofile code: jo, jodemo, dfx, dfx2, bhd
	ProjectFeatures features;
	ProjectExportSettings export_settings;
	ProjectExpansion expansion; // `"expansion": {"name", "builds_on"}`, absent when standalone
};

// Where a project keeps things, derived from its root. `cache_dir` and everything
// under it is disposable and self-ignored (`.opennova/.gitignore`).
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
	std::string run_dir;     // where Play runs the game: a numbered directory a run (run/run_directory.h)
	std::string staging_dir; // an import's files before they are published (import_assets)
	// Where the files are read from: the root folder (null), or a source of their own, each file by its
	// logical name (the game install as the game is served it, which the game's own data's fold validates
	// as a project of its own: session/original_files.h).
	std::shared_ptr<const FileSource> files;
	// The game install's files an expansion's Play copied where they could not be linked into its run
	// directory (another volume), a folder per install, kept for the next (prepare_expansion_run).
	std::string install_copy_dir;

	static ProjectPaths for_root(const std::string &root);
	// The project-relative export output resolved against the root.
	std::string export_dir(const ProjectDocument &doc) const;
};

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

// A fresh random UUID (version 4 text form).
std::string make_project_id();

// Whether a project could be made at `root` for `target_game` (a gameprofile code) as `expansion`
// (the expansion's rule, check_project_expansion: what an install has is the caller's to weigh,
// expansion_install_findings): the directory must not already hold a project file. Nothing is
// written; create_project asks the same first.
bool can_create_project(const std::string &root, const std::string &target_game, Diagnostic &error,
                        const ProjectExpansion &expansion = ProjectExpansion());

// Create a project at `root`: the directory (created if missing) must not already hold a
// project file. Writes `project.opennova` and the self-ignoring cache directory, returns
// the new document. `target_game` must be a gameprofile code.
bool create_project(const std::string &root, const std::string &title, const std::string &target_game,
                    ProjectDocument &out, Diagnostic &error,
                    const ProjectExpansion &expansion = ProjectExpansion());

// The project rooted at `root` (its project file must exist).
bool open_project(const std::string &root, ProjectDocument &out, Diagnostic &error);

} // namespace opennova::editor
