#include <editor/session/original_files.h>

#include <utility>

#include <editor/assets/install_view.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

OriginalFiles::OriginalFiles() : files_(std::make_shared<const std::set<std::string>>()) {}

OriginalFiles::~OriginalFiles() = default;

void OriginalFiles::want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document,
                         const std::string &root, const AssetScan &scan, const std::vector<Diagnostic> &findings) {
	const std::string game = document ? document->target_game : std::string();
	if (install != install_ || root != root_ || !document_ || !document || document_->target_game != game) {
		clear();
		install_ = install;
		root_ = root;
		document_ = document;
	}
	queue_.clear();
	if (install_.empty() || !document_) return;
	// The files the findings are about, each as the scan lists it now: one known as it stands is kept,
	// any other checked (again).
	std::set<std::string> asked;
	for (const Diagnostic &d : findings) {
		if (d.asset.empty() || !asked.insert(d.asset).second) continue;
		const AssetEntry *entry = scan.at_path(d.asset);
		if (!entry) continue;
		const auto found = known_.find(entry->relative_path);
		if (found != known_.end() && found->second.size == entry->size_bytes && found->second.modified == entry->modified_ticks)
			continue;
		File file;
		file.path = entry->relative_path;
		file.name = entry->logical_name;
		file.size = entry->size_bytes;
		file.modified = entry->modified_ticks;
		queue_.push_back(std::move(file));
	}
}

bool OriginalFiles::mount() {
	if (!mount_tried_) {
		mount_tried_ = true;
		game_ = std::make_unique<InstallView>();
		std::string error;
		if (!game_->open(install_spec(install_, *document_), error)) game_.reset();
	}
	return game_ != nullptr;
}

bool OriginalFiles::step(uint64_t bytes) {
	if (queue_.empty()) return true;
	const bool mounted = mount();
	bool moved = false;
	uint64_t read = 0;
	do {
		File file = std::move(queue_.back());
		queue_.pop_back();
		// Its bytes as the project holds them against the install's copy as the game is served it.
		std::vector<uint8_t> held, served;
		std::string error;
		bool original = false;
		const InstallFile *served_file = mounted ? game_->find(file.name) : nullptr;
		if (served_file && read_file_bytes(join_path(root_, file.path), held, error) && game_->read(*served_file, served))
			original = held == served;
		read += held.size() + served.size();
		++checked_;
		const auto known = known_.find(file.path);
		moved = moved || (known == known_.end() ? original : known->second.original != original);
		file.original = original;
		known_[file.path] = std::move(file);
	} while (!queue_.empty() && read < bytes);
	if (moved) publish();
	// Nothing left to check: the install let go, its archives closed (the game, or a patch, may write
	// them); the next check mounts it again.
	if (queue_.empty()) {
		game_.reset();
		mount_tried_ = false;
	}
	return queue_.empty();
}

void OriginalFiles::publish() {
	auto files = std::make_shared<std::set<std::string>>();
	for (const auto &[path, file] : known_)
		if (file.original) files->insert(path);
	files_ = std::move(files);
	++generation_;
}

void OriginalFiles::clear() {
	const bool had = !files_->empty();
	install_.clear();
	root_.clear();
	document_.reset();
	game_.reset();
	mount_tried_ = false;
	known_.clear();
	queue_.clear();
	if (had) publish();
}

} // namespace opennova::editor
