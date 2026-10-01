#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's plain file plumbing: whole-file reads and the atomic write every
// editor save uses (write `<path>.tmp`, then rename over `path`, so a crash leaves
// either the old file or the new one). `error` carries the OS reason on failure. Every
// one of them calls the system through system_path, so none is bound by MAX_PATH.

// The path the system's own file calls take for `path` (ADR 0046 S13 A8): on Windows an absolute
// path in its extended-length form (`\\?\C:\...`, a share's `\\?\UNC\server\share\...`), which
// MAX_PATH does not bound, so a build under a deep folder writes as one under a short one does;
// elsewhere the path as it is. For the call alone: a path the editor keeps, compares or shows
// stays as it was given.
std::filesystem::path system_path(const std::string &path);

bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out, std::string &error);
bool read_file_text(const std::string &path, std::string &out, std::string &error);
bool write_file_atomic(const std::string &path, const void *data, size_t size, std::string &error);
bool write_file_atomic(const std::string &path, const std::string &text, std::string &error);

// The written file at `from` put in the place of `to`, replacing it (one rename, rename_with_retry);
// false with the OS reason when it is refused (a write-protected `to`), `from` then left where it
// is. The file replaced, its last write moves past the one it had, however soon after it the new
// one was written (the stamps that tell a file changed are its size and last write).
bool replace_file(const std::string &from, const std::string &to, std::string &error);

// Whether a refused rename may go through when tried again a moment later: Windows' access denied
// and sharing violation, the refusals a scanner or an indexer holding one of the files open for a
// moment gives (S13 A3 measured 3 of 400 replace_file refusals clearing on a 2 ms retry).
bool rename_refusal_passes(const std::error_code &ec);
// std::filesystem::rename of the system paths, tried again a few times within some 30 ms while
// its refusal may pass (rename_refusal_passes); false with the last refusal in `ec`. Every
// rename that puts a file or a build in place takes it: replace_file, the build's publish.
bool rename_with_retry(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &ec);

// A file made at `path` and opened for writing, only when no file of that name is there: null,
// and nothing touched, when one is (or it cannot be made). A build's copies are made through it,
// so a copy never writes through a name another file may stand behind (a hard link to the last
// good build's archive: S13 A8).
std::FILE *create_new_file(const std::string &path);

// The file's last write set to now: a file put in place by a copy or a rename that kept the last
// write of the file it came from (a rename's copy, an import's publish) reads as written now to
// every cache that keys a file's content by its size and last write (S13 A8).
bool refresh_last_write(const std::string &path, std::string &error);
using FileReplace = std::function<bool(const std::string &from, const std::string &to, std::string &error)>;

// Several files written as one (a rename everywhere): every file's bytes read and every text
// written beside its file (`<path>.tmp`) first, nothing replaced when one of these fails; then
// each put in its file's place in turn (`replace`, replace_file but in a test). When one is not,
// its text and the ones after it are removed and the files already replaced get the bytes they
// held back: false, `problems` saying why (the refusal first, then a file whose bytes could
// not be put back, which holds the new text).
struct FileText {
	std::string path;
	std::string text;
};
bool write_files_together(const std::vector<FileText> &files, std::vector<std::string> &problems,
                          const FileReplace &replace = replace_file);

// mkdir -p; true when the directory exists afterwards.
bool ensure_directory(const std::string &path, std::string &error);

// The file at `from` given a second name, `to`: a hard link, one file under two names with no
// byte copied (a build's archive its content left as the last build packed it, a run directory's
// archives: S13 A8). False, with the OS reason, where the file system will not (another volume, a
// file system without links, `to` taken): the caller copies the file instead.
bool link_file(const std::string &from, const std::string &to, std::string &error);

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
