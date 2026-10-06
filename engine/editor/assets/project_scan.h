#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// What a step of a walk spends on a file besides the bytes it reads (ADR 0046 S13 A3): a
// directory entry listed, or a file visited (its size and last write asked, its name
// classified), counts as this many bytes of the step's budget.
inline constexpr uint64_t kWalkEntryCost = 4096;
// A step budget no walk exhausts: the run-to-end wrappers' (scan_project_assets, run_imports).
inline constexpr uint64_t kWholeWalkStep = UINT64_MAX;

// The walk's rule for the project file at `relative` (project-relative, '/'-separated), what
// scan_project_assets makes of each file it meets (asset_registry.h): the file's entry, or an
// import record's outputs, and its findings. False, nothing made, for a file that is not there or
// that the walk does not reach (the project file, a file under a dot-directory or the export
// output directory). `key` is the path the walk lists it by (its case on disk); `bytes_read`, when
// given, what it read (a `.bin` classified by its content, an import record).
bool scan_project_file(const ProjectPaths &paths, const ProjectDocument &doc, const std::string &relative,
		std::string &key, AssetScan::Visit &out, uint64_t *bytes_read = nullptr);

// The walk's rule for one folder of the project (`folder` project-relative, '/'-separated; "" the
// project's own): the files it lists there and the folders it descends into, project-relative and
// each as the file system spells it, in the order of their paths (ADR 0046 DI-01: a folder whose last
// write moved listed again, assets/disk_changes.h). False, nothing listed, for a folder that is not
// there or that the walk does not reach (a dot-directory, the export folder, or one under them).
bool list_project_folder(const ProjectPaths &paths, const ProjectDocument &doc, const std::string &folder,
		std::vector<std::string> &files, std::vector<std::string> &folders);

// The project's files walked a few at a time (ADR 0046 S13 A3), so an editor opening or reading
// again a large project keeps drawing: scan_project_assets' walk as a cursor, stepped by a budget
// of bytes. The files are listed first (each directory entry kWalkEntryCost), then visited in the
// order of their paths (each kWalkEntryCost and the bytes it reads), then the scan is made from the
// visits (AssetScan::set_visits), so a scan stepped at any budget equals one made in a call. It
// reads the disk as it goes: a file changed between its listing and its visit is visited as it is
// then, one gone by then is not listed. Single-threaded, like the session that steps it.
class ProjectScan {
public:
	ProjectScan(const ProjectPaths &paths, const ProjectDocument &doc);

	// One step: files listed or visited until `budget` bytes are spent (at least one, whatever the
	// budget); true once the scan is made (take() then hands it over).
	bool step(uint64_t budget);
	bool done() const { return phase_ == Phase::Done; }
	// Where it stands: the files listed so far and those visited of them; while it lists, the
	// listing is not done.
	bool listing() const { return phase_ == Phase::Start || phase_ == Phase::Listing; }
	size_t files_listed() const { return listed_.size(); }
	size_t files_visited() const { return next_; }
	// The file it visited last, project-relative ("" while it lists).
	const std::string &current() const { return current_; }
	// The scan, once done (an empty one before).
	AssetScan take();

private:
	enum class Phase : uint8_t { Start, Listing, Visiting, Done };

	ProjectPaths paths_;
	std::filesystem::path root_;
	std::filesystem::path export_dir_;
	std::filesystem::recursive_directory_iterator walk_;
	std::vector<std::pair<std::string, std::filesystem::path>> listed_; // by path once listed
	size_t next_ = 0;
	std::string current_;
	std::map<std::string, AssetScan::Visit> visits_;
	std::map<std::string, int64_t> folders_; // each folder descended into, its stamp (AssetScan::folders)
	AssetScan scan_;
	Phase phase_ = Phase::Start;
};

} // namespace opennova::editor
