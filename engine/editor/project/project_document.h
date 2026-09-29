#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The project file (ADR 0046 d6): `project.opennova` at the project root, versioned
// UTF-8 JSON. Unknown schema versions are rejected outright; pre-1.0 there are no
// migration readers. Everything machine-local (paths, window state) lives in
// `.opennova/local.json` instead, so this file is what a modder commits.
inline constexpr int kProjectSchemaVersion = 1;
inline constexpr const char *kProjectFileName = "project.opennova";
inline constexpr const char *kProjectCacheDirName = ".opennova";
inline constexpr const char *kLocalSettingsFileName = "local.json";
inline constexpr const char *kImportSidecarSuffix = ".import";
inline constexpr const char *kImportCacheFileName = "import_cache.json";
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

struct ProjectDocument {
	int schema_version = kProjectSchemaVersion;
	std::string project_id;   // a UUID minted at creation, stable for the project's life
	std::string title;
	std::string target_game = kDefaultTargetGame; // a gameprofile code: jo, jodemo, dfx, dfx2, bhd
	ProjectFeatures features;
	ProjectExportSettings export_settings;
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
	std::string staging_dir; // an import's files before they are published (import_assets)

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
// False with `error` set for a wrong schema version, a missing/invalid field or an
// unknown target game; `out` is left untouched on failure.
bool project_document_from_json(const io::JsonValue &json, ProjectDocument &out, Diagnostic &error);

bool load_project_document(const std::string &project_file, ProjectDocument &out, Diagnostic &error);
// Atomic (tmp + rename); the text is json_write's deterministic form.
bool save_project_document(const std::string &project_file, const ProjectDocument &doc,
                           Diagnostic &error);

// A fresh random UUID (version 4 text form).
std::string make_project_id();

// Create a project at `root`: the directory (created if missing) must not already hold a
// project file. Writes `project.opennova` and the self-ignoring cache directory, returns
// the new document. `target_game` must be a gameprofile code.
bool create_project(const std::string &root, const std::string &title, const std::string &target_game,
                    ProjectDocument &out, Diagnostic &error);

// The project rooted at `root` (its project file must exist).
bool open_project(const std::string &root, ProjectDocument &out, Diagnostic &error);

} // namespace opennova::editor
