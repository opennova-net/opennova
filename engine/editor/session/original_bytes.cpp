#include <editor/session/original_bytes.h>

#include <filesystem>
#include <memory>
#include <system_error>

#include <base/io/file_time.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

std::set<std::string> OriginalBytes::identical(const std::string &install, const ProjectDocument &document,
                                               const std::string &root, const std::vector<std::string> &files) {
	std::set<std::string> out;
	const std::string key = root + '\x1f' + install + '\x1f' + document.target_game + '\x1f' + document.expansion.name +
	                        '\x1f' + document.expansion.builds_on;
	if (key != key_) {
		known_.clear();
		key_ = key;
	}
	if (install.empty() || files.empty()) return out;
	std::unique_ptr<InstallView> view;
	bool opened = false;
	const int64_t asked_at = io::file_clock_now_ticks();
	for (const std::string &file : files) {
		const std::string path = join_path(root, file);
		std::error_code ec;
		const uint64_t size = uint64_t(fs::file_size(system_path(path), ec));
		if (ec) continue;
		const int64_t written = io::file_modified_ticks(system_path(path));
		const auto found = known_.find(file);
		if (found != known_.end() && found->second.size == size && found->second.written == written) {
			if (found->second.same) out.insert(file);
			continue;
		}
		if (!opened) {
			opened = true;
			view = std::make_unique<InstallView>();
			std::string why;
			if (!view->open(install_spec(install, document), why)) view.reset();
		}
		if (!view) continue; // an install that does not open: nothing known, asked again next time
		bool same = false;
		const InstallFile *served = view->find(basename_of(file));
		if (served) {
			std::vector<uint8_t> theirs, ours;
			std::string error;
			++reads_;
			same = view->read(*served, theirs) && theirs.size() == size && read_file_bytes(path, ours, error) &&
			       ours == theirs;
		}
		if (io::file_stamp_settled(written, asked_at)) known_[file] = Known{size, written, same};
		else known_.erase(file);
		if (same) out.insert(file);
	}
	return out;
}

void OriginalBytes::clear() {
	key_.clear();
	known_.clear();
}

} // namespace opennova::editor
