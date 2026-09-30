#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project_build/build_plan.h>

namespace opennova::editor {

// Runs a plan into an immutable build directory (ADR 0046 d8):
//   <output_root>/<build-id>/   language.pff localres.pff resource.pff <loose files> build.json
// The build id is content-addressed (a hash over every entry's name and bytes), so an
// unchanged project is the same build and nothing is written; a changed project gets a
// new directory in which unchanged archives are copied from the last good build and only
// changed archives are re-packed. Everything lands in `<build-id>.tmp/` first, is
// re-mounted through the engine's own VFS to prove every name resolves, and is renamed
// into place last, so a failure leaves the last good build untouched. Directories the
// caller names in `protected_dirs` (a running Play child's, a live lease's:
// run/play_lease.h) are never pruned, and pruning only ever deletes a directory that
// proves it is a build (its name is a build id and its record names the same id, or it
// is a marked staging directory): the output root may be any folder the user chose.
inline constexpr int kBuildRecordSchemaVersion = 1;
inline constexpr const char *kBuildRecordFileName = "build.json";
inline constexpr const char *kLastGoodBuildFileName = "last_good.json";
inline constexpr const char *kBuildStagingSuffix = ".tmp";
inline constexpr const char *kBuildStagingMarkerFileName = "build.staging";

// True for a build id's spelling: 16 lower-case hex digits (a build directory's name).
bool is_build_id(const std::string &name);

struct BuildProgress {
	virtual ~BuildProgress() = default;
	// Called once per archive as it is written or reused, and once per loose file.
	virtual void on_step(const std::string &what, size_t done, size_t total) = 0;
};

struct BuildReport {
	bool ok = false;
	std::string build_id;
	std::string build_dir;               // the published directory (empty on failure)
	bool reused_existing = false;        // the same content was already built
	std::vector<std::string> archives_written;
	std::vector<std::string> archives_reused;
	std::vector<std::string> loose_written;
	std::vector<Diagnostic> diagnostics;
};

// One build, advanced a budget of bytes at a time (S13 A1): every file hashed in a stream
// whose FNV-1a state carries across steps, then each archive written through the resumable
// PFF writer (formats/pff/pff_stream_writer.h), each reused archive and loose file copied in
// chunks, then the verification and the publish, one step each. The editor steps it within
// each frame's budget so its window keeps drawing while a large project packs; the command
// line and the tests run it to the end (run_build). No thread is involved, so a build never
// races the project it reads: a file edited mid-build (its size or its last write no longer
// what the hash read) fails that build ("changed while packing") instead of packing half of
// each version.
class BuildRun {
public:
	BuildRun(BuildPlan plan, std::string output_root, std::vector<std::string> protected_dirs = {});
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

	void prepare();
	void hash(uint64_t budget);
	void settle();
	void pack(uint64_t budget);
	void copy_loose(uint64_t budget);
	void publish();
	void fail(Diagnostic error);
	void close_streams();
	// The flat index of an archive's entry (archives in plan order, then the loose files).
	size_t stamp_index(size_t group, size_t entry) const;
	void advance(uint64_t bytes);

	BuildPlan plan_;
	std::string output_root_;
	std::vector<std::string> protected_dirs_;
	Phase phase_ = Phase::Prepare;
	bool cancelled_ = false;
	std::unique_ptr<Streams> streams_;
	std::vector<size_t> group_base_; // each archive's first stamp; the loose files' last
	std::vector<Stamp> stamps_;
	// The hash pass: the group (an archive, or the loose files after the archives), the entry,
	// the running hashes.
	size_t hash_group_ = 0;
	size_t hash_entry_ = 0;
	uint64_t group_hash_ = 0;
	uint64_t build_hash_ = 0;
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
	std::string last_dir_;                           // "" when there is no last good build
	std::string tmp_dir_;
	std::string final_dir_;
	BuildReport report_;
};

// The whole build in one call (the command line, the tests).
BuildReport run_build(const BuildPlan &plan, const std::string &output_root,
                      const std::vector<std::string> &protected_dirs = {},
                      BuildProgress *progress = nullptr);

// The last good build's directory under `output_root` ("" when none).
std::string last_good_build_dir(const std::string &output_root);

} // namespace opennova::editor
