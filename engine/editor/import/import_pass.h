#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <editor/import/import_run.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// The import pass walked a source at a time (ADR 0046 S13 A3): run_imports' pass (import_run.h,
// its rules) as a cursor, stepped by a budget of bytes, so an editor opening or refreshing a
// project whose sources must be imported again keeps drawing. The project's files are listed first
// (each directory entry kWalkEntryCost, assets/project_scan.h), the import sources among them
// kept; then each source, in the order of their paths, is taken as run_imports takes it (kWalkEntry
// Cost, and the bytes it reads to hash or import it and the bytes its outputs write); then the
// machine-local import cache is written when it changed. A pass stepped at any budget comes to what
// one run in a call comes to. Dropped before it is done, it has imported the sources it reached
// (their outputs and records written, as a pass that stopped there would) and left the cache as it
// was, so the next pass looks at those again. Single-threaded, like the session that steps it.
class ImportPass {
public:
	ImportPass(const ProjectPaths &paths, const ProjectDocument &project, bool force = false,
			std::string only = std::string());

	// One step: files listed or sources taken until `budget` bytes are spent (at least one,
	// whatever the budget); true once the pass is done (take() then hands its result over).
	bool step(uint64_t budget);
	bool done() const { return phase_ == Phase::Done; }
	// Where it stands: whether it still lists, the sources listed and those taken.
	bool listing() const { return phase_ == Phase::Start || phase_ == Phase::Listing; }
	size_t sources_listed() const { return listed_.size(); }
	size_t sources_done() const { return next_; }
	// The source it took last, project-relative ("" while it lists).
	const std::string &current() const { return current_; }
	// What the pass came to, once done (an empty result before).
	ImportRunResult take();

private:
	// What this machine last saw of one source (ADR 0046 d6, S9c): its size and last-write
	// time with the content hash they vouch for, and the fingerprint of the import record
	// the outputs under the cache were made from. Machine-local and disposable, so the
	// committed sidecar never carries a time a checkout changes.
	struct CacheEntry {
		uint64_t size = 0;
		int64_t modified = 0; // the file system's own clock ticks: compared, never shown
		uint64_t hash = 0;
		uint64_t record = 0; // 0 = no outputs made on this machine yet
	};
	using Cache = std::map<std::string, CacheEntry>; // by project-relative source path

	enum class Phase : uint8_t { Start, Listing, Importing, Done };

	// The source at `path` (project-relative `relative`) taken: imported again when stale; the
	// bytes it read and wrote added to `spent`.
	void take_source(const std::filesystem::path &path, const std::string &relative, uint64_t &spent);
	static Cache load_cache(const std::string &path, std::string &text);
	void save_cache() const;

	ProjectPaths paths_;
	ProjectDocument project_;
	bool force_ = false;
	std::string only_;
	std::filesystem::path root_;
	std::filesystem::path export_dir_;
	std::filesystem::recursive_directory_iterator walk_;
	std::vector<std::pair<std::string, std::filesystem::path>> listed_; // the sources, by path once listed
	size_t next_ = 0;
	std::string current_;
	std::string cache_text_; // the cache as read, to write it only when it changed
	Cache cache_;
	Cache seen_; // the sources this pass found: a gone source leaves the cache
	ImportRunResult result_;
	Phase phase_ = Phase::Start;
};

} // namespace opennova::editor
