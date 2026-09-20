#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
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

BuildReport run_build(const BuildPlan &plan, const ProjectDocument &doc, const std::string &output_root,
                      const std::vector<std::string> &protected_dirs = {},
                      BuildProgress *progress = nullptr);

// The last good build's directory under `output_root` ("" when none).
std::string last_good_build_dir(const std::string &output_root);

} // namespace opennova::editor
