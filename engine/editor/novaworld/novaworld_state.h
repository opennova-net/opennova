#pragma once

// A working copy's own state (NovaWorld, ADR 0069): what a project folder knows of the
// NovaWorld project its project file names (ProjectDocument::novaworld), kept under its
// `.opennova/novaworld/`, which never syncs (the machine's own folder, ProjectPaths::cache_dir).
// Each service has a folder of its own there, by its host (`opennova.net`, `staging.opennova.net`,
// `localhost_8080`): the revision the folder's files were last made the same as on that service
// (its base) and the base's manifest, which lists those files, so a change since is told apart from
// a change the server has. A debug build's staging and a release build's live service each keep
// their own. The hash cache, which keeps each file's SHA-256 by its size and last write, and a
// pull's downloads, landed under `incoming/` before it puts them in place (novaworld_apply.h), are
// every service's.

#include <cstdint>
#include <string>
#include <vector>

#include <base/project/project_manifest.h>

namespace opennova::editor {

struct ProjectPaths;

struct RemoteLobby;

struct NovaWorldPaths {
	std::string server; // normalized service URL, including its scheme and port
	std::string dir; // <root>/.opennova/novaworld
	std::string service_dir; // <dir>/<host>: the service's own ("" with no service)
	std::string state_file; // state.json: the working copy's project and base revision
	std::string base_file; // base.json: the base revision's manifest, its canonical text
	std::string lobby_file; // lobby.json: the service's resolved lobby and encoded runtime file
	std::string hashes_file; // <dir>/hashes.json (novaworld_scan.h)
	std::string incoming_dir; // <dir>/incoming/<sha256>: a pull's downloads

	// The project's, for the service at `server` ("" none: the files every service shares alone).
	static NovaWorldPaths for_project(const ProjectPaths &paths, const std::string &server);
};

// The folder a service's state is kept in: its host and port as `server` names them, in small
// letters, a ':' and anything else no folder name holds as '_' ("https://opennova.net"
// "opennova.net", "http://localhost:8080" "localhost_8080"); "" for no host.
std::string novaworld_service_folder(const std::string &server);

inline constexpr int kNovaWorldStateSchemaVersion = 1;

// What the folder is a working copy of on a service: the project its project file named when the
// state was written (another key since is no working copy of the new one), and the base revision, 0
// before the folder holds one.
struct NovaWorldWorkingCopy {
	std::string key; // "JO"
	int64_t revision = 0;
};

// The folder's state on the service: false, `error` empty, when it has none; false with `error`
// when the file is there but no state this editor writes.
bool read_novaworld_state(
		const NovaWorldPaths &paths, NovaWorldWorkingCopy &out, std::string &error);
bool write_novaworld_state(
		const NovaWorldPaths &paths, const NovaWorldWorkingCopy &copy, std::string &error);

// The base revision's files: none (true) while the folder has no base file; false with `error`
// when the file is no manifest.
bool read_novaworld_base(
		const NovaWorldPaths &paths, std::vector<project::ProjectFile> &out, std::string &error);
// The base written (its manifest's canonical text) after the files it lists are in place.
bool write_novaworld_base(const NovaWorldPaths &paths, const std::vector<project::ProjectFile> &files,
		std::string &error);

// Only transport decoding happens here. The service generates the encoded file. Its
// content-addressed cached copy is a build input, never a source file or a pushed file.
bool cache_novaworld_lobby(const NovaWorldPaths &paths, const std::string &key,
		const RemoteLobby &lobby, std::string &error);
std::string cached_novaworld_gate_tag(
		const NovaWorldPaths &paths, const std::string &key, const std::string &title);

} // namespace opennova::editor
