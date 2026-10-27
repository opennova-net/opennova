#pragma once

// A working copy's files as the content store keeps them (ADR 0069): every file the project walk
// reaches (assets/project_walk.h: no dot-directory, no export folder, so never the machine's own
// `.opennova/`), the project file with them, and none a machine makes beside a project
// (content_skips_file), each by its path, its SHA-256 and its size: the manifest a push would send
// (base/project/project_manifest.h). Walked a few files at a time, as the project scan is (ADR
// 0046 S13 A3): the files are listed first (each directory entry kWalkEntryCost), then hashed in
// the order of their paths a block at a time (each block its bytes), so a large file spreads
// over steps and the editor keeps drawing. Each file's SHA-256 is kept in the hash cache
// (ContentPaths::hashes_file) by its size and last write, and trusted by them alone only when that
// last write lies kFileStampSettle before the pass (io::file_stamp_settled, git's racy rule, as
// the build's cache), so a warm status of 9,000 files reads no file's bytes. Single-threaded,
// like the session that steps it.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <editor/content_store/content_state.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

inline constexpr int kContentHashCacheSchemaVersion = 1;

// Whether a file named `name` (a path's last part) is one a machine makes beside a project's files,
// never one of them: Windows' Thumbs.db and desktop.ini, macOS's .DS_Store, Blender's numbered
// backups (`.blend1`, `.blend2`, ...) and the `.tmp` an interrupted atomic save left behind
// (io::write_file_atomic). Its letters' case aside.
bool content_skips_file(std::string_view name);

class ContentScan {
public:
	// The working copy at `paths.root` whose export folder is `export_dir`
	// (ProjectPaths::export_dir;
	// "" for a folder with no project file yet).
	ContentScan(const ProjectPaths &paths, std::string export_dir);

	// One step: files listed or bytes hashed until `budget` bytes are spent (at least one entry or
	// block, whatever the budget); true once the scan is done.
	bool step(uint64_t budget);
	bool done() const { return phase_ == Phase::Done; }
	bool listing() const { return phase_ == Phase::Start || phase_ == Phase::Listing; }
	size_t files_listed() const { return listed_.size(); }
	size_t files_hashed() const { return next_; }
	// The bytes the files to hash hold, and those hashed so far (both 0 while it lists).
	uint64_t bytes_to_hash() const { return bytes_to_hash_; }
	uint64_t bytes_hashed() const { return bytes_hashed_; }

	// Once done: the files, sorted by path, and what kept a file out of them (one line each: a file
	// it could not read, a path no store takes, two paths one file on Windows and macOS).
	const std::vector<project::ProjectFile> &files() const { return files_; }
	const std::vector<std::string> &problems() const { return problems_; }
	// The paths of the files it listed but could not read: in the folder, though not in files(), so
	// no comparison takes one for a file deleted.
	const std::vector<std::string> &unread() const { return unread_; }
	// Why the list lacks files the folder may hold (a folder the walk could not enter, the folder
	// itself among them; an entry whose type, place or size it could not tell, as in a folder it
	// may list but not look into), "" when it listed them all: files() then lacks files a push
	// would send as deleted and a pull would plan as deleted, and a status counts as none.
	const std::string &incomplete() const { return incomplete_; }

private:
	enum class Phase : uint8_t { Start, Listing, Hashing, Done };
	struct Listed {
		std::string path; // project-relative, '/'-separated, as the file system spells it
		std::filesystem::path system; // the system path the scan reads it by
		uint64_t size = 0;
		int64_t modified = 0; // io::file_modified_ticks
	};
	struct Cached {
		uint64_t size = 0;
		int64_t modified = 0;
		std::string sha256;
	};

	void load_cache();
	void finish();
	// The listed file `next_`: its SHA-256 from the cache, or a block of it read; true once it is
	// done (hashed, or given up on with a problem).
	bool hash_next(uint64_t &spent);

	ProjectPaths paths_;
	ContentPaths content_;
	std::filesystem::path root_;
	std::filesystem::path export_dir_;
	std::filesystem::recursive_directory_iterator walk_;
	std::vector<Listed> listed_;
	size_t next_ = 0;
	std::map<std::string, Cached> cache_;
	std::string cache_text_; // the cache as read, so an unchanged one is not written again
	std::map<std::string, Cached> seen_; // every file hashed or taken from the cache this pass
	int64_t pass_began_ = 0;
	// The file being read: its stream and its hash so far.
	std::ifstream reading_;
	io::Sha256 hashing_;
	std::vector<char> block_;
	bool open_ = false;
	uint64_t bytes_to_hash_ = 0;
	uint64_t bytes_hashed_ = 0;
	std::vector<project::ProjectFile> files_;
	std::vector<std::string> problems_;
	std::vector<std::string> unread_;
	std::string incomplete_;
	Phase phase_ = Phase::Start;
};

// A working copy's files read to the end in one call (ContentScan with no budget's bound).
ContentScan scan_content(const ProjectPaths &paths, const std::string &export_dir);

} // namespace opennova::editor
