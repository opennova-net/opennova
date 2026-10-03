#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project_build/build_plan.h>

namespace opennova::editor {

// Runs a plan into an immutable build directory (ADR 0046 d8):
//   <output_root>/<build-id>/   language.pff localres.pff resource.pff <loose files> build.json
// The build id is content-addressed (a hash over every entry's name, size and content hash, an
// archive's with the format and the writer's version, PFF_WRITER_VERSION), so an unchanged project
// is the same build and nothing is written; a changed project gets a new directory in which
// unchanged archives are linked (copied where the file system cannot link them) from the last good
// build and only changed archives are re-packed. A file's content hash is read from the plan's
// hash cache while its size and last write are those it was hashed at (S13 A8), so a build reads
// the bytes of the files that changed since and no others; the cache keeps no hash of a file whose
// last write lies within io::kFileStampSettle of the pass that read it (io::file_stamp_settled).
// Everything lands in a staging directory first (`<build-id>.tmp/`, or `<build-id>.<n>.tmp/` when
// one left before cannot be emptied: a game running on the last good build holds the archives a
// cancelled build linked there), is re-mounted through the engine's own VFS to prove every name
// resolves, and is renamed into place last (tried again while a scanner holds a file, a few
// seconds at most), so a failure leaves the last good build untouched. No write goes through a
// name already there: a copy makes its file new (create_new_file), so a link to the last good
// build's archive is never written through, and an archive is reused only while its size is the
// one its build recorded. The directories the caller's ProtectedDirs names when the build
// publishes (a running Play child's, those whose lease names a process that may still run:
// run/play_lease.h) are never pruned, and pruning only ever deletes a directory that proves it is
// a build (its name is a build id and its record names the same id, or it is a marked staging
// directory), its proof last: the output root may be any folder the user chose, as deep as it is
// (every call to the system takes a path through system_path, project/project_files.h). A build is
// its project's (BuildPlan::project, in its record and its staging marker): where several projects
// build into one folder (Build to folder), a build reuses, prunes and replaces only its own
// project's, and says how many of the others' it left (BuildReport::others).
// 2: each archive's record carries its size beside its hash (1 kept the hash alone).
// 3: the record names its project.
inline constexpr int kBuildRecordSchemaVersion = 3;
inline constexpr int kBuildCacheSchemaVersion = 1;
inline constexpr const char *kBuildRecordFileName = "build.json";
inline constexpr const char *kLastGoodBuildFileName = "last_good.json";
inline constexpr const char *kBuildStagingSuffix = ".tmp";
inline constexpr const char *kBuildStagingMarkerFileName = "build.staging";

// True for a build id's spelling: 16 lower-case hex digits (a build directory's name).
bool is_build_id(const std::string &name);

// The directories a build must not prune, asked when it publishes (not when it starts: a game
// may start from a build while another packs).
using ProtectedDirs = std::function<std::vector<std::string>()>;
// ProtectedDirs naming `dirs` whenever it is asked (the tests', a caller that knows them).
ProtectedDirs protect_dirs(std::vector<std::string> dirs);

struct BuildProgress {
	virtual ~BuildProgress() = default;
	// Called once per archive as it is written or reused, and once per loose file.
	virtual void on_step(const std::string &what, size_t done, size_t total) = 0;
};

// A file a build published (the UX round's problems lane: the build panel's): an archive with how many
// files it packs and whether the last good build's was kept, or a loose file; its size as published.
struct BuiltFile {
	std::string name;
	uint64_t bytes = 0;
	size_t files = 0;
	bool archive = false;
	bool reused = false;
};

struct BuildReport {
	bool ok = false;
	// Refused by the gate before anything was read (its plan blocked: build_blockers), not failed on the way.
	bool refused = false;
	std::string build_id;
	std::string build_dir;               // the published directory (empty on failure)
	// The expansion the build made (BuildTarget::expansion; "" for the standalone game), which Play
	// runs with /exp (ADR 0046 S16).
	std::string expansion;
	bool reused_existing = false;        // the same content was already built
	std::vector<std::string> archives_written;
	std::vector<std::string> archives_reused; // the last good build's, its content unchanged
	std::vector<std::string> archives_linked; // of those, the ones linked rather than copied
	std::vector<std::string> loose_written;
	// The files whose bytes the hash pass read, and those bytes: every other file's content hash
	// came from the hash cache (BuildPlan::hash_cache).
	size_t files_hashed = 0;
	uint64_t bytes_hashed = 0;
	std::vector<Diagnostic> diagnostics;
	// What the published directory holds, the archives in the plan's order then the loose files.
	std::vector<BuiltFile> built;
	// How long it took, start to finish (its caller's clock: the session's; 0 when none timed it).
	double seconds = 0;
	// The builds of other projects its folder holds (their directories' names), left as they are.
	std::vector<std::string> others;
};

// One build, advanced a budget of bytes at a time (S13 A1): every file hashed in a stream
// whose FNV-1a state carries across steps (or its hash taken from the hash cache, a file's stat
// a step's small cost: S13 A8), then each archive written through the resumable PFF writer
// (formats/pff/pff_stream_writer.h), each reused archive linked or copied in chunks and each loose
// file copied in chunks, then the verification and the publish, one step each (the publish some more
// while a scanner holds a staged file, three seconds at most). The editor steps it within
// each frame's budget so its window keeps drawing while a large project packs; the command
// line and the tests run it to the end (run_build). No thread is involved, so a build never
// races the project it reads: a file edited mid-build (its size or its last write no longer
// what the hash read) fails that build ("changed while packing") instead of packing half of
// each version: a file is checked when it is opened and again when its last byte is read.
class BuildRun {
public:
	BuildRun(BuildPlan plan, std::string output_root, ProtectedDirs protected_dirs = {});
	// A run left unfinished is cancelled: its staging directory goes.
	~BuildRun();
	BuildRun(const BuildRun &) = delete;
	BuildRun &operator=(const BuildRun &) = delete;

	// One step: at most `budget_bytes` bytes read, hashed, written or copied (a file opened
	// counts as a small read, and every step moves: at least one byte, or one file opened), or
	// one of the steps between (the gate, the staging directory, the verification and the
	// publish). True once the build has finished: ok, failed or cancelled.
	bool step(uint64_t budget_bytes);
	bool done() const { return phase_ == Phase::Done; }
	// Stops between two steps: the staging directory is removed, nothing is published and the
	// last good build stays as it was; report() is not ok and names no finding. A finished
	// build is left as it is.
	void cancel();
	bool cancelled() const { return cancelled_; }

	// The bytes hashed and then written or copied so far, out of how many (every entry's
	// bytes twice, once per pass), never going back; and what the last step worked on.
	uint64_t bytes_done() const { return bytes_done_; }
	uint64_t bytes_total() const { return bytes_total_; }
	const std::string &label() const { return label_; }
	// The archives and loose files written or reused so far, out of how many, and each one's
	// name in that order (the command line's lines).
	size_t items_done() const { return items_done_; }
	size_t items_total() const { return plan_.archives.size() + plan_.loose.size(); }
	const std::string &item_name(size_t index) const;

	const BuildReport &report() const { return report_; }

private:
	enum class Phase { Prepare, Hash, Settle, Archives, Loose, Publish, Done };
	struct Stamp {
		uint64_t size = 0;
		int64_t written = 0; // the file's last write, as the hash read it
	};
	struct Streams; // the files a step has open (build_run.cpp)
	// What the hash cache knows of a file: the size and last write its content hash was read at.
	struct Hashed {
		uint64_t size = 0;
		int64_t written = 0;
		uint64_t hash = 0;
	};
	using HashCache = std::map<std::string, Hashed>; // by the file's path (BuildEntry::source_path)

	void prepare();
	void hash(uint64_t budget);
	// The file's name, size and content hash folded into its archive's (or the loose files') hash.
	void fold(const BuildEntry &entry, uint64_t size, uint64_t content);
	static uint64_t archive_hash_seed();
	void load_cache();
	void save_cache() const;
	void settle();
	void pack(uint64_t budget);
	void copy_loose(uint64_t budget);
	void publish();
	// The report's `built`: each published file of the plan with its size in the build directory.
	void list_built();
	// The builds of other projects its folder holds (BuildReport::others), for a build handed back unchanged.
	void list_others();
	void fail(Diagnostic error);
	void close_streams();
	// The flat index of an archive's entry (archives in plan order, then the loose files).
	size_t stamp_index(size_t group, size_t entry) const;
	void advance(uint64_t bytes);

	BuildPlan plan_;
	std::string output_root_;
	ProtectedDirs protected_dirs_;
	Phase phase_ = Phase::Prepare;
	bool cancelled_ = false;
	std::unique_ptr<Streams> streams_;
	std::vector<size_t> group_base_; // each archive's first stamp; the loose files' last
	std::vector<Stamp> stamps_;
	// The hash pass: the group (an archive, or the loose files after the archives), the entry,
	// the running hashes (the file's, its group's, the build's), the cache as read and the files
	// this build hashed or took from it, which the cache keeps when the pass ends.
	size_t hash_group_ = 0;
	size_t hash_entry_ = 0;
	uint64_t file_hash_ = 0;
	uint64_t group_hash_ = 0;
	uint64_t build_hash_ = 0;
	HashCache cache_;
	std::string cache_text_;
	HashCache seen_;
	// The write pass: the archive or loose file under way, and whether it has started.
	size_t archive_index_ = 0;
	size_t loose_index_ = 0;
	bool item_started_ = false;
	bool item_reused_ = false;
	uint64_t item_share_ = 0;      // its bytes in bytes_total()'s write pass
	uint64_t item_share_done_ = 0;
	uint64_t bytes_done_ = 0;
	uint64_t bytes_total_ = 0;
	std::string label_;
	size_t items_done_ = 0;
	std::map<std::string, std::string> hashes_;      // archive file name -> hex content hash
	std::map<std::string, std::string> last_hashes_; // the last good build's
	std::map<std::string, uint64_t> last_sizes_;     // its archives' sizes, as its record keeps them
	std::string last_dir_;                           // "" when there is no last good build
	std::string tmp_dir_;
	std::string final_dir_;
	int64_t pass_began_ = 0; // when the hash pass began (io::file_clock_now_ticks)
	// The publish: its record (written into the staging directory, then the last good one), and
	// when its rename was first refused while the refusal may pass.
	bool publish_ready_ = false;
	std::string record_;
	std::chrono::steady_clock::time_point publish_refused_at_{};
	BuildReport report_;
};

// The whole build in one call (the command line, the tests).
BuildReport run_build(const BuildPlan &plan, const std::string &output_root, ProtectedDirs protected_dirs = {},
                      BuildProgress *progress = nullptr);

// The last good build's directory under `output_root` ("" when none).
std::string last_good_build_dir(const std::string &output_root);

} // namespace opennova::editor
