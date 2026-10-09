#pragma once

#include <filesystem>
#include <string>

#include <base/io/file_io.h>
#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's path conversions and the rule for a name it writes into the project. The
// whole-file reads and the atomic write every editor save uses (write `<path>.tmp`, then
// rename over `path`, so a crash leaves either the old file or the new one) are
// base/io/file_io.h's (io::read_file_bytes, io::write_file_atomic, ...), which reach the
// system through io::os_path.

// Every path the editor keeps is a UTF-8 string: what the Shell hands it (String.utf8()), what it
// shows, stores and compares, and what the process seam widens for a child (CP_UTF8). On Windows
// std::filesystem's narrow conversions read and write the ANSI code page instead (a path made from
// a std::string, a std::string joined with operator/, string(), generic_string()), so a project
// under a folder named outside ASCII (C:/Users/José/...) was made and read in another folder, and
// a name the code page cannot hold threw. Every conversion between the two goes through these,
// base/io/os_path.h's UTF-8 ones: path_of is the UTF-8 string as a path, lexically, as spelled
// (system_path is the one a system call takes); utf8_of a path as UTF-8, '/'-separated, without
// a \\?\ prefix; join_path the two joined as std::filesystem joins them. Elsewhere a path's
// bytes are its name and they are plain conversions.
std::filesystem::path path_of(const std::string &utf8);
std::string utf8_of(const std::filesystem::path &path);
std::string join_path(const std::string &dir, const std::string &name);

// The path the system's own file calls take for the UTF-8 `path` (ADR 0046 S13 A8): on Windows an
// absolute path in its extended-length form (`\\?\C:\...`, a share's `\\?\UNC\server\share\...`),
// which MAX_PATH does not bound, so a build under a deep folder writes as one under a short one
// does; elsewhere the path as it is. For the call alone: a path the editor keeps, compares or
// shows stays as it was given.
std::filesystem::path system_path(const std::string &path);

// A directory whose name starts with '.' (.opennova/, .git/): the walks over the
// project tree (the scan, the import pass) never enter one.
bool is_dot_directory(const std::filesystem::path &path);

// A path as the editor names it to the author (Output's lines): relative to the project's
// folder `root` when it lies inside it, "/"-separated (".opennova/build/play"), else as it
// is (a folder outside the project, another drive). As it is for an empty `root`.
std::string shown_path(const std::string &path, const std::string &root);

// A path's file name, its last step ("menus/main.mnu" -> "main.mnu"): the flat logical name
// a project path names (ADR 0046 d6).
std::string basename_of(const std::string &path);

// A name someone chose for a file the editor is about to write into the project: a
// created document, an imported file, a rename's new name (ADR 0046 d6: the identity
// is the flat logical name). True when `name` is a plain file name (no folder, not "."
// or ".."), fits the game's archives when `kind` is one the build packs
// (logical_name_fits_archive; a loose kind such as a video takes any length), names
// `kind` by itself (asset_name_fits_kind) and, placed in `dir` (project-relative, "" =
// the root), stays inside the project at `root`. Unknown (a kind not decided yet: an
// import before its bytes are read) skips the archive and kind checks. Otherwise
// `problem` says which rule it breaks (a caller reports it under its own area's code: an
// import's import.name, a rename's rename.kind, a new document's document.path) and `message`
// what is wrong. check_file_name is the same rule without the place: the name alone, nothing
// read from the disk (what a fix offers is checked with it).
enum class FileNameProblem {
	None,
	Name, // not a plain file name, or past the archives' name limit for a kind the build packs
	Kind, // not a name of its kind
	Path, // placed where it would land outside the project
};
bool check_project_file_name(const std::string &root, const std::string &dir, const std::string &name, AssetKind kind,
                             FileNameProblem &problem, std::string &message);
bool check_file_name(const std::string &name, AssetKind kind, FileNameProblem &problem, std::string &message);

} // namespace opennova::editor
