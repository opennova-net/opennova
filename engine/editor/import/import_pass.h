#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <editor/import/import_run.h>
#include <editor/import/importer.h>
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
// `table` is the importer table a source's importer is found in by its extension: the compiled-in
// importers(), or a test's own rows.
class ImportPass {
public:
	ImportPass(const ProjectPaths &paths, const ProjectDocument &project, bool force = false,
			std::string only = std::string(), const std::vector<Importer> &table = importers());

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
	// What this machine last saw of the project's files an import reads (ADR 0046 d6, S9c, S13 A8):
	// each source's and each input's size and last-write time with the content hash they vouch
	// for, and each source's fingerprint of the import record its outputs under the cache were made
	// from (none: no outputs made on this machine yet). Machine-local and disposable, so the
	// committed sidecar never carries a time a checkout changes.
	struct FileSeen {
		uint64_t size = 0;
		int64_t modified = 0; // the file system's own clock ticks: compared, never shown
		uint64_t hash = 0;
	};
	struct Cache {
		std::map<std::string, FileSeen> files;   // by project-relative path
		std::map<std::string, uint64_t> records; // by the source's project-relative path
	};

	enum class Phase : uint8_t { Start, Listing, Importing, Done };

	// The source at `path` (project-relative `relative`) taken: imported again when stale; the
	// bytes it read and wrote added to `spent`.
	void take_source(const std::filesystem::path &path, const std::string &relative, uint64_t &spent);
	// The content hash of the project file at `file` (`relative`), the cache's while its size and
	// last write hold, else read (its bytes added to `spent`); false when it is not there or cannot
	// be read. What it saw is kept for the cache.
	bool file_hash(const std::string &file, const std::string &relative, uint64_t &hash, uint64_t &spent);
	static Cache load_cache(const std::string &path, std::string &text);
	void save_cache() const;

	ProjectPaths paths_;
	ProjectDocument project_;
	bool force_ = false;
	std::string only_;
	const std::vector<Importer> *table_ = nullptr;
	std::filesystem::path root_;
	std::filesystem::path export_dir_;
	std::filesystem::recursive_directory_iterator walk_;
	std::vector<std::pair<std::string, std::filesystem::path>> listed_; // the sources, by path once listed
	size_t next_ = 0;
	std::string current_;
	std::string cache_text_; // the cache as read, to write it only when it changed
	Cache cache_;
	Cache seen_; // the files this pass looked at: a gone source or input leaves the cache
	ImportRunResult result_;
	Phase phase_ = Phase::Start;
};

} // namespace opennova::editor
