#include <editor/assets/disk_changes.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <utility>

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <editor/assets/project_scan.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The folder a project-relative path is in ("" the project's own).
std::string folder_of(const std::string &relative) {
	const size_t slash = relative.rfind('/');
	return slash == std::string::npos ? std::string() : relative.substr(0, slash);
}

// Whether `relative` lies under `folder` at any depth ("" the project's own: every path).
bool under_folder(const std::string &relative, const std::string &folder) {
	return folder.empty() || (relative.size() > folder.size() && relative.compare(0, folder.size(), folder) == 0 &&
	                          relative[folder.size()] == '/');
}

// The scan's files directly in `folder`, in the order of their paths.
std::vector<std::string> scanned_in(const AssetScan &scan, const std::string &folder) {
	std::vector<std::string> out;
	const std::string prefix = folder.empty() ? std::string() : folder + "/";
	for (auto it = scan.visits().lower_bound(prefix); it != scan.visits().end(); ++it) {
		const std::string &path = it->first;
		if (path.compare(0, prefix.size(), prefix) != 0) break;
		if (path.size() > prefix.size() && path.find('/', prefix.size()) == std::string::npos) out.push_back(path);
	}
	return out;
}

// The folder `folder` on disk, as the system's calls take it.
fs::path folder_on_disk(const ProjectPaths &paths, const std::string &folder) {
	return system_path(folder.empty() ? paths.root : join_path(paths.root, folder));
}

} // namespace

DiskStamp disk_stamp(const ProjectPaths &paths, const std::string &relative) {
	DiskStamp out;
	std::error_code ec;
	const fs::directory_entry entry(system_path(join_path(paths.root, relative)), ec);
	if (ec || !entry.is_regular_file(ec) || ec) return out;
	const uintmax_t size = entry.file_size(ec);
	if (ec) return out;
	const fs::file_time_type modified = entry.last_write_time(ec);
	if (ec) return out;
	out.present = true;
	out.size_bytes = static_cast<uint64_t>(size);
	out.modified_ticks = static_cast<int64_t>(modified.time_since_epoch().count());
	return out;
}

DiskStamp scanned_stamp(const AssetScan &scan, const std::string &relative) {
	DiskStamp out;
	const auto visit = scan.visits().find(relative);
	if (relative.empty() || visit == scan.visits().end()) return out;
	out.present = true;
	out.size_bytes = visit->second.size_bytes;
	out.modified_ticks = visit->second.modified_ticks;
	return out;
}

DiskChanges::Look DiskChanges::look(const std::string &relative, const DiskStamp &now, const DiskStamp &scanned,
                                    int64_t now_ms, int64_t now_ticks) {
	if (now == scanned) {
		// As the scan took it (it moved back, or the scan read it since): nothing to read.
		looked_.erase(relative);
		ready_.erase(relative);
		return Look::Same;
	}
	// A last write that lies far enough back to be no program's write in progress: read at once.
	if (now.present && io::file_stamp_settled(now.modified_ticks, now_ticks)) {
		looked_.erase(relative);
		ready_.insert(relative);
		return Look::Ready;
	}
	const auto it = looked_.find(relative);
	if (it == looked_.end() || it->second.stamp != now) {
		looked_[relative] = Looked{now, now_ms}; // moved (again): held still from now
		return Look::Waiting;
	}
	if (now_ms - it->second.since_ms < kDiskHoldStillMs) return Look::Waiting;
	looked_.erase(it);
	ready_.insert(relative);
	return Look::Ready;
}

DiskChanges::Look DiskChanges::look_file(const ProjectPaths &paths, const AssetScan &scan, const std::string &file,
                                         int64_t now_ms, int64_t now_ticks) {
	const DiskStamp now = disk_stamp(paths, file);
	const DiskStamp scanned = scanned_stamp(scan, file);
	const auto visit = scan.visits().find(file);
	// The scan's stamp, taken within the file system's tick of the write: once it has settled, the content
	// the scan read compared, once per visit.
	if (now == scanned && now.present && visit != scan.visits().end() && visit->second.racy &&
	    io::file_stamp_settled(now.modified_ticks, now_ticks)) {
		const auto confirmed = confirmed_.find(file);
		if (confirmed == confirmed_.end() || confirmed->second != visit->second.read_ticks) {
			std::vector<uint8_t> bytes;
			std::string unread;
			if (io::read_file_bytes(join_path(paths.root, file), bytes, unread) &&
			    io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size()) != visit->second.racy_fingerprint) {
				confirmed_.erase(file);
				looked_.erase(file);
				ready_.insert(file);
				return Look::Ready;
			}
			confirmed_[file] = visit->second.read_ticks;
		}
	}
	return look(file, now, scanned, now_ms, now_ticks);
}

void DiskChanges::look_at(const ProjectPaths &paths, const AssetScan &scan, const std::vector<std::string> &files,
                          int64_t now_ms, int64_t now_ticks) {
	std::set<std::string> once;
	for (const std::string &file : files) {
		if (file.empty() || !once.insert(file).second) continue;
		look_file(paths, scan, file, now_ms, now_ticks);
	}
}

std::vector<std::string> DiskChanges::folder_changes(const ProjectPaths &paths, const ProjectDocument &doc,
                                                     const AssetScan &scan, int64_t now_ticks, size_t *listed) {
	// Another walk's scan: its stamps are the ones to compare with from now on.
	if (scan.folders() != walked_) {
		walked_ = scan.folders();
		folders_ = walked_;
	}
	std::vector<std::string> out;
	size_t count = 0;
	const std::map<std::string, int64_t> before = folders_; // folders_ changes as folders are listed
	for (const auto &[folder, stamp] : before) {
		if (!folders_.count(folder)) continue; // gone with the folder it was in
		std::error_code ec;
		const fs::path dir = folder_on_disk(paths, folder);
		if (!fs::is_directory(dir, ec)) {
			drop_folder(scan, folder, out);
			continue;
		}
		if (io::file_modified_ticks(dir) == stamp && io::file_stamp_settled(stamp, now_ticks)) continue;
		relist(paths, doc, scan, folder, out, count);
	}
	if (listed) *listed = count;
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

// A folder listed (again): its stamp taken first, so a file made in it while it is listed moves it past
// the stamp kept; its files against the scan's (only those that differ are looked at, never every file
// of a folder whose stamp an editor's own save moved); a folder in it the stamps do not know listed the
// same way (it is new), one they know that is gone dropped.
void DiskChanges::relist(const ProjectPaths &paths, const ProjectDocument &doc, const AssetScan &scan,
                         const std::string &folder, std::vector<std::string> &out, size_t &listed) {
	folders_[folder] = io::file_modified_ticks(folder_on_disk(paths, folder));
	std::vector<std::string> files, folders;
	if (!list_project_folder(paths, doc, folder, files, folders)) {
		// Gone between its stat and its listing, or no longer a folder the walk descends into.
		drop_folder(scan, folder, out);
		return;
	}
	++listed;
	const std::vector<std::string> held = scanned_in(scan, folder);
	std::set_difference(files.begin(), files.end(), held.begin(), held.end(), std::back_inserter(out)); // made
	std::set_difference(held.begin(), held.end(), files.begin(), files.end(), std::back_inserter(out)); // gone
	for (const std::string &sub : folders)
		if (!folders_.count(sub)) relist(paths, doc, scan, sub, out, listed);
	std::vector<std::string> gone_folders;
	for (const auto &[known, stamp] : folders_) {
		(void)stamp;
		if (!known.empty() && folder_of(known) == folder && !std::binary_search(folders.begin(), folders.end(), known))
			gone_folders.push_back(known);
	}
	for (const std::string &gone : gone_folders) drop_folder(scan, gone, out);
}

void DiskChanges::drop_folder(const AssetScan &scan, const std::string &folder, std::vector<std::string> &out) {
	for (const auto &[path, visit] : scan.visits()) {
		(void)visit;
		if (!path.empty() && under_folder(path, folder)) out.push_back(path);
	}
	for (auto it = folders_.begin(); it != folders_.end();)
		it = it->first == folder || under_folder(it->first, folder) ? folders_.erase(it) : std::next(it);
}

void DiskChanges::begin_sweep(std::vector<std::string> files) {
	sweep_ = std::move(files);
	sweep_next_ = 0;
}

bool DiskChanges::step_sweep(const ProjectPaths &paths, const AssetScan &scan, uint64_t budget, int64_t now_ms,
                             int64_t now_ticks) {
	uint64_t spent = 0;
	while (sweep_next_ < sweep_.size()) {
		const std::string &file = sweep_[sweep_next_++];
		look_file(paths, scan, file, now_ms, now_ticks);
		spent += kWalkEntryCost;
		if (spent >= budget) break;
	}
	if (sweep_next_ < sweep_.size()) return false;
	sweep_.clear();
	sweep_next_ = 0;
	return true;
}

std::vector<std::string> DiskChanges::pending_files() const {
	std::vector<std::string> out(ready_.begin(), ready_.end());
	for (const auto &[path, looked] : looked_) {
		(void)looked;
		out.push_back(path);
	}
	return out;
}

std::vector<std::string> DiskChanges::take_ready() {
	std::vector<std::string> out(ready_.begin(), ready_.end());
	ready_.clear();
	return out;
}

void DiskChanges::clear() {
	looked_.clear();
	ready_.clear();
	folders_.clear();
	walked_.clear();
	sweep_.clear();
	sweep_next_ = 0;
	confirmed_.clear();
}

} // namespace opennova::editor
