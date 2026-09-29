#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The editor's plain file plumbing: whole-file reads and the atomic write every
// editor save uses (write `<path>.tmp`, then rename over `path`, so a crash leaves
// either the old file or the new one). `error` carries the OS reason on failure.
bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out, std::string &error);
bool read_file_text(const std::string &path, std::string &out, std::string &error);
bool write_file_atomic(const std::string &path, const void *data, size_t size, std::string &error);
bool write_file_atomic(const std::string &path, const std::string &text, std::string &error);

// The written file at `from` put in the place of `to`, replacing it (one rename); false with the
// OS reason when it is refused (a write-protected `to`), `from` then left where it is.
bool replace_file(const std::string &from, const std::string &to, std::string &error);
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

// A directory whose name starts with '.' (.opennova/, .git/): the walks over the
// project tree (the scan, the import pass) never enter one.
bool is_dot_directory(const std::filesystem::path &path);

// A path as the editor names it to the author (Output's lines): relative to the project's
// folder `root` when it lies inside it, "/"-separated (".opennova/build/play"), else as it
// is (a folder outside the project, another drive). As it is for an empty `root`.
std::string shown_path(const std::string &path, const std::string &root);

// A name someone chose for a file the editor is about to write into the project: a
// created document, an imported file, a rename's new name (ADR 0046 d6: the identity
// is the flat logical name). True when `name` is a plain file name (no folder, not "."
// or ".."), fits the game's archives when `kind` is one the build packs
// (logical_name_fits_archive; a loose kind such as a video takes any length), names
// `kind` by itself (asset_name_fits_kind) and, placed in `dir` (project-relative, "" =
// the root), stays inside the project at `root`. Unknown (a kind not decided yet: an
// import before its bytes are read) skips the archive and kind checks. Otherwise
// `problem` is "name", "kind" or "path" (a caller's finding code is "<area>.<problem>")
// and `message` says what is wrong. check_file_name is the same rule without the place:
// the name alone, nothing read from the disk (what a fix offers is checked with it).
bool check_project_file_name(const std::string &root, const std::string &dir, const std::string &name, AssetKind kind,
                             std::string &problem, std::string &message);
bool check_file_name(const std::string &name, AssetKind kind, std::string &problem, std::string &message);

} // namespace opennova::editor
