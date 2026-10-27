#pragma once

// A working copy's own state (the content store, ADR 0069): what a project folder knows of the
// content project it is a copy of, kept under its `.opennova/content/`, which never syncs (the
// machine's own folder, ProjectPaths::cache_dir). The binding names the service and the content
// project and the revision the folder's files were last made the same as (its base); the base's
// manifest lists those files, so a change since is told apart from a change the server has; the
// hash cache keeps each file's SHA-256 by its size and last write; a pull lands its downloads under
// `incoming/` before it puts them in place (content_apply.h).

#include <cstdint>
#include <string>
#include <vector>

#include <base/project/project_manifest.h>

namespace opennova::editor {

struct ProjectPaths;

struct ContentPaths {
	std::string dir; // <root>/.opennova/content
	std::string binding_file; // binding.json
	std::string base_file; // base.json: the base revision's manifest, its canonical text
	std::string hashes_file; // hashes.json (content_scan.h)
	std::string incoming_dir; // incoming/<sha256>: a pull's downloads

	static ContentPaths for_project(const ProjectPaths &paths);
};

inline constexpr int kContentBindingSchemaVersion = 1;

// What the folder is a working copy of.
struct ContentBinding {
	std::string server; // the service's address: "https://opennova.net"
	std::string key; // the content project's: "JO"
	int64_t revision = 0; // the base revision, 0 before the folder holds one
};

// The folder's binding: false, `error` empty, when it has none; false with `error` when the file is
// there but no binding.
bool read_content_binding(const ContentPaths &paths, ContentBinding &out, std::string &error);
bool write_content_binding(
		const ContentPaths &paths, const ContentBinding &binding, std::string &error);

// The base revision's files: none (true) while the folder has no base file; false with `error`
// when the file is no manifest.
bool read_content_base(
		const ContentPaths &paths, std::vector<project::ProjectFile> &out, std::string &error);
// The base written (its manifest's canonical text) after the files it lists are in place.
bool write_content_base(const ContentPaths &paths, const std::vector<project::ProjectFile> &files,
		std::string &error);

} // namespace opennova::editor
