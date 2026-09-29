#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
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
// caller names in `protected_dirs` (a running Play child's) are never pruned, and
// pruning only ever deletes a directory that proves it is a build (its name is a build
// id and its record names the same id, or it is a marked staging directory): the output
// root may be any folder the user chose.
inline constexpr int kBuildRecordSchemaVersion = 1;
inline constexpr const char *kBuildRecordFileName = "build.json";
inline constexpr const char *kLastGoodBuildFileName = "last_good.json";
inline constexpr const char *kBuildStagingSuffix = ".tmp";
inline constexpr const char *kBuildStagingMarkerFileName = "build.staging";

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

// One build, advanced a unit of work at a time: the hashing and staging, then one
// archive or one loose file per step, then the verification and the publish. The editor
// steps it once per frame so its window keeps drawing while a large project packs; the
// command line and the tests run it to the end (run_build). No thread is involved, so a
// build never races the project it reads: a file edited mid-build fails that build
// ("changed while packing") instead of packing half of each version.
class BuildRun {
public:
	BuildRun(BuildPlan plan, std::string output_root, std::vector<std::string> protected_dirs = {});

	// One unit of work; true once the build has finished (ok or not).
	bool step();
	bool done() const { return phase_ == Phase::Done; }

	// Archives and loose files handled so far, out of how many, and the last one's name.
	size_t steps_done() const { return steps_done_; }
	size_t steps_total() const { return plan_.archives.size() + plan_.loose.size(); }
	const std::string &last_step() const { return last_step_; }

	const BuildReport &report() const { return report_; }

private:
	enum class Phase { Prepare, Archives, Loose, Publish, Done };

	void prepare();
	void pack_next_archive();
	void copy_next_loose();
	void publish();
	void fail(Diagnostic error);

	BuildPlan plan_;
	std::string output_root_;
	std::vector<std::string> protected_dirs_;
	Phase phase_ = Phase::Prepare;
	size_t archive_index_ = 0;
	size_t loose_index_ = 0;
	size_t steps_done_ = 0;
	std::string last_step_;
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
