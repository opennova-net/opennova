#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// A project file as the disk holds it now, or as the scan's walk took it (ADR 0046 DI-01): there or
// not, its size and its last write (the file system's own ticks), compared for equality, never shown.
struct DiskStamp {
	bool present = false;
	uint64_t size_bytes = 0;
	int64_t modified_ticks = 0;
	bool operator==(const DiskStamp &other) const {
		return present == other.present && size_bytes == other.size_bytes && modified_ticks == other.modified_ticks;
	}
	bool operator!=(const DiskStamp &other) const { return !(*this == other); }
};
// The project file at `relative` (project-relative, '/'-separated) on disk now.
DiskStamp disk_stamp(const ProjectPaths &paths, const std::string &relative);
// The same file as the scan's walk took it (its visit's stamp): not present for one the scan does not list.
DiskStamp scanned_stamp(const AssetScan &scan, const std::string &relative);

// How long a file that moved must hold still before it is read (ADR 0046 DI-01): a look that finds the
// stamp a look this long before found reads it, so a program still writing it (its size or last write
// moving between the looks) is never read half-written. A file whose last write already lies
// io::kFileStampSettle before the look is read at the first look.
inline constexpr int64_t kDiskHoldStillMs = 250;

// What the disk changed of a project's files outside the editor since its scan (ADR 0046 DI-01), found
// by looks: a look at a file compares its stamp on disk with the scan's; one that moved is read once it
// holds still (kDiskHoldStillMs between two looks, or a last write already settled), never while a
// program may still be writing it. The files a look finds moved wait for the next look; those that held
// still are ready, taken by whoever reads them again (take_ready). Which files to look at is the
// caller's: those it watches each time, the folders (a folder's last write moves when a file is made,
// deleted or renamed in it, so only a folder whose stamp moved is listed again, its new files and its
// gone ones looked at), and every file of the scan, swept a few at a time (step_sweep). Single-threaded,
// like the session that holds it.
class DiskChanges {
public:
	// What a look came to.
	enum class Look : uint8_t {
		Same,    // as the scan took it: nothing to read (one that moved back is forgotten)
		Waiting, // moved: read once a later look finds it holding still
		Ready,   // moved and held still: read it again
	};
	// `relative` looked at: `now` its stamp on disk, `scanned` the scan's, `now_ms` the caller's steady
	// clock and `now_ticks` the file system's (io::file_clock_now_ticks).
	Look look(const std::string &relative, const DiskStamp &now, const DiskStamp &scanned, int64_t now_ms,
	          int64_t now_ticks);
	// look() of each file of `files` against `scan`, each once; the files it found ready, in their order.
	void look_at(const ProjectPaths &paths, const AssetScan &scan, const std::vector<std::string> &files,
	             int64_t now_ms, int64_t now_ticks);

	// The folders the scan's walk descended into, each whose last write moved since it was listed (by the
	// walk, or by this since) listed again (list_project_folder): its files the scan does not list (made)
	// and the scan's files it no longer holds (gone), a folder made in it with every file it holds, a
	// folder gone with every file of the scan under it, each folder's stamp kept as taken before it was
	// listed. A stamp that lies within io::kFileStampSettle of `now_ticks` (the file system's clock) is
	// listed again too until it settles: a file made in the same tick of the clock it was taken in leaves
	// it as it was. A scan of another walk (its folders' stamps not the ones followed) starts the stamps
	// again from its. The paths it found, project-relative, for the caller to look at; `listed`, when
	// given, how many folders it listed.
	std::vector<std::string> folder_changes(const ProjectPaths &paths, const ProjectDocument &doc, const AssetScan &scan,
	                                        int64_t now_ticks, size_t *listed = nullptr);

	// A sweep over `files` (every file the scan lists that the caller watches this way: the editor's
	// focus-in), begun anew; a sweep running is replaced.
	void begin_sweep(std::vector<std::string> files);
	bool sweeping() const { return sweep_next_ < sweep_.size(); }
	// The sweep's files looked at against `scan` until `budget` is spent (kWalkEntryCost a file), at least
	// one; true once it has looked at every one (none left).
	bool step_sweep(const ProjectPaths &paths, const AssetScan &scan, uint64_t budget, int64_t now_ms,
	                int64_t now_ticks);

	// The files a look found moved, by path: those waiting for a later look (not yet held still) and
	// those ready and not taken yet (a sweep's, between two checks), which a check looks at again before it
	// takes them (one the scan caught up with meanwhile, an editor's own save, is then Same).
	std::vector<std::string> pending_files() const;
	size_t waiting() const { return looked_.size(); }
	// The files ready to be read again, taken (their looks forgotten), in the order of their paths.
	std::vector<std::string> take_ready();
	size_t ready() const { return ready_.size(); }
	// Everything forgotten: the looks, the folders' stamps, the sweep (the project closed or another opened).
	void clear();

private:
	struct Looked {
		DiskStamp stamp;
		int64_t since_ms = 0; // when a look first found this stamp
	};
	// The folder `folder` listed (again), its stamp taken first: its files the scan does not list and the
	// scan's files directly in it it no longer holds into `out`; a folder in it the stamps do not know
	// listed the same way (it is new), one they know that is gone dropped (drop_folder).
	void relist(const ProjectPaths &paths, const ProjectDocument &doc, const AssetScan &scan, const std::string &folder,
	            std::vector<std::string> &out, size_t &listed);
	// The scan's files under `folder` (every depth) into `out` as gone, its folders' stamps dropped.
	void drop_folder(const AssetScan &scan, const std::string &folder, std::vector<std::string> &out);

	std::map<std::string, Looked> looked_;
	std::set<std::string> ready_;
	std::map<std::string, int64_t> folders_; // each folder's stamp as last listed, by path ("" the project's)
	std::map<std::string, int64_t> walked_;  // the scan's folders the stamps started from (AssetScan::folders)
	std::vector<std::string> sweep_;
	size_t sweep_next_ = 0;
};

} // namespace opennova::editor
