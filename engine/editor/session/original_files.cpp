#include <editor/session/original_files.h>

#include <iterator>
#include <utility>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/documents/document_types.h>
#include <editor/documents/project_check.h>
#include <editor/documents/project_checks.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/model/document_base.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

std::string original_finding_key(const Diagnostic &d) {
	std::string key = d.code();
	const auto part = [&key](const std::string &text) {
		key += '\x1f';
		key += text;
	};
	part(d.record);
	part(d.field);
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		part(requirement->role);
		part(requirement->target);
	} else if (const ReferenceSubject *reference = reference_subject(d)) {
		part(std::to_string(static_cast<int>(reference->kind)));
		part(reference->target);
		part(reference->scope);
		part(std::to_string(reference->loader_arg));
	}
	// A text document's finding names no record: its words tell two of one code apart, never its line.
	if (d.record.empty() && d.line) part(d.message);
	return key;
}

OriginalFiles::OriginalFiles() : data_(std::make_shared<const OriginalData>()) {}

OriginalFiles::~OriginalFiles() = default;

void OriginalFiles::want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document,
                         const std::string &root, const std::shared_ptr<const AssetScan> &scan,
                         const std::vector<Diagnostic> &findings, const std::vector<std::shared_ptr<const DocumentBase>> &open) {
	const std::string game = document ? document->target_game : std::string();
	if (install != install_ || root != root_ || !document_ || !document || document_->target_game != game) {
		clear();
		install_ = install;
		root_ = root;
	}
	document_ = document;
	scan_ = scan;
	open_ = open;
	queue_.clear();
	asked_.clear();
	if (install_.empty() || !document_ || !scan_) return;
	// The files the findings are about, each as the scan lists it now: one known as it stands is kept,
	// any other checked (again).
	for (const Diagnostic &d : findings) {
		if (d.asset.empty() || asked_.count(d.asset)) continue;
		const AssetEntry *entry = scan_->at_path(d.asset);
		if (!entry) continue;
		asked_.insert(entry->relative_path);
		const auto found = known_.find(entry->relative_path);
		if (found != known_.end() && found->second.size == entry->size_bytes && found->second.modified == entry->modified_ticks &&
		    found->second.kind == entry->kind)
			continue;
		File file;
		file.path = entry->relative_path;
		file.name = entry->logical_name;
		file.kind = entry->kind;
		file.size = entry->size_bytes;
		file.modified = entry->modified_ticks;
		queue_.push_back(std::move(file));
	}
	// What the files and the documents hold may have moved since the last baseline: brought up again
	// once the files are checked (an update over a shadow that holds them reads only what changed).
	baseline_due_ = true;
}

bool OriginalFiles::mount() {
	if (!mount_tried_) {
		mount_tried_ = true;
		game_ = std::make_unique<Vfs>();
		if (!mount_retail(*game_, install_, *document_)) game_.reset();
	}
	return game_ != nullptr;
}

void OriginalFiles::compare(uint64_t bytes) {
	const bool mounted = mount();
	uint64_t read = 0;
	do {
		File file = std::move(queue_.back());
		queue_.pop_back();
		// Its bytes as the project holds them against the install's copy as the game is served it.
		std::vector<uint8_t> held, served;
		std::string error;
		file.served = mounted && read_served(*game_, file.name, served);
		file.original = file.served && read_file_bytes(join_path(root_, file.path), held, error) && held == served;
		read += held.size() + served.size();
		++checked_;
		known_[file.path] = std::move(file);
	} while (!queue_.empty() && read < bytes);
}

bool OriginalFiles::step(uint64_t bytes, const OriginalProject &project) {
	if (!queue_.empty()) {
		compare(bytes);
		if (!queue_.empty()) return false;
		// The baseline, a step of its own.
		return false;
	}
	if (baseline_due_) {
		baseline_due_ = false;
		baseline(project);
		// Nothing left to do: the install let go, its archives closed (the game, or a patch, may write
		// them); the next check mounts it again.
		game_.reset();
		mount_tried_ = false;
	}
	return true;
}

// The install's copy of a file as a document standing in for it, kept while the project's file stands as
// it was checked: the install's bytes for a file that differs, the project's own (the same bytes) for one
// open with unsaved edits. Null when its kind has no document or its copy does not load: no finding of
// the file is then the original's.
std::shared_ptr<DocumentBase> OriginalFiles::copy_of(const File &file) {
	const auto found = copies_.find(file.path);
	if (found != copies_.end() && found->second.size == file.size && found->second.modified == file.modified &&
	    found->second.from_install == !file.original)
		return found->second.document;
	Copy copy;
	copy.size = file.size;
	copy.modified = file.modified;
	copy.from_install = !file.original;
	const DocumentType *type = document_type_for(file.kind);
	std::vector<uint8_t> bytes;
	std::string error;
	bool read = false;
	if (type && type->make)
		read = file.original ? read_file_bytes(join_path(root_, file.path), bytes, error)
		                     : mount() && read_served(*game_, file.name, bytes);
	if (read) {
		std::shared_ptr<DocumentBase> made = type->make();
		Diagnostic failed;
		if (made->load_bytes(bytes, file.path, file.kind, document_->target_game, failed)) copy.document = std::move(made);
	}
	copies_[file.path] = copy;
	return copy.document;
}

void OriginalFiles::baseline(const OriginalProject &project) {
	OriginalData data;
	if (!document_ || !scan_ || install_.empty()) return publish(std::move(data));
	std::set<std::string> dirty;
	for (const auto &open : open_)
		if (open && open->dirty()) dirty.insert(open->path());
	// The files the install serves: as the install's, and not open with unsaved edits, every finding of
	// theirs is the original's; held otherwise, each is judged against its install copy.
	std::vector<const File *> otherwise;
	for (const std::string &path : asked_) {
		const auto found = known_.find(path);
		if (found == known_.end() || !found->second.served) continue;
		if (found->second.original && !dirty.count(path)) data.files.insert(path);
		else otherwise.push_back(&found->second);
	}
	if (otherwise.empty()) {
		shadow_graph_.reset();
		shadow_cache_.reset();
		copies_.clear();
		return publish(std::move(data));
	}
	// The project as it would be with the game's own copies: the install copies standing in, as open
	// documents, for the files held otherwise; every other open document as it is.
	std::set<std::string> replaced;
	std::vector<std::pair<std::string, std::shared_ptr<DocumentBase>>> standing;
	for (const File *file : otherwise) {
		replaced.insert(file->path);
		data.findings[file->path]; // judged, with none the original's where its copy does not load
		if (std::shared_ptr<DocumentBase> copy = copy_of(*file)) standing.emplace_back(file->path, std::move(copy));
	}
	for (auto it = copies_.begin(); it != copies_.end();)
		it = replaced.count(it->first) ? std::next(it) : copies_.erase(it);
	std::vector<std::shared_ptr<const DocumentBase>> open;
	for (const auto &document : open_)
		if (document && !replaced.count(document->path())) open.push_back(document);
	for (const auto &[path, document] : standing) open.push_back(document);
	// The shadow: the session's graph and files' findings copied the first time, then brought up as the
	// session's are (only what changed is read again).
	if (!shadow_graph_) {
		shadow_graph_ = std::make_unique<AssetGraph>(project.graph);
		shadow_cache_ = std::make_unique<ValidationCache>(project.cache);
	}
	const ValidationInput input{project.paths, *document_, *scan_, open};
	refresh_project(input, *shadow_graph_, *shadow_cache_);
	++baselines_;
	for (const Diagnostic &d : project_rows(input, *shadow_graph_, *shadow_cache_)) {
		const auto found = data.findings.find(d.asset);
		if (found != data.findings.end()) ++found->second[original_finding_key(d)];
	}
	// What the project checks make of each copy alone (a menu's screens compiled as the game draws them),
	// made again only for a new copy or a new scan (the files its renders read).
	const ProjectCheckInput checks{input, *shadow_cache_, project.files};
	for (const auto &[path, document] : standing) {
		Copy &copy = copies_[path];
		if (copy.checked_over != scan_.get()) {
			copy.checked.clear();
			project.checks.findings_of(*document, checks, copy.checked);
			copy.checked_over = scan_.get();
		}
		std::map<std::string, size_t> &keys = data.findings[path];
		for (const Diagnostic &d : copy.checked)
			if (d.asset == path) ++keys[original_finding_key(d)];
	}
	publish(std::move(data));
}

void OriginalFiles::publish(OriginalData data) {
	if (*data_ == data) return;
	data_ = std::make_shared<const OriginalData>(std::move(data));
	++generation_;
}

void OriginalFiles::clear() {
	install_.clear();
	root_.clear();
	document_.reset();
	scan_.reset();
	open_.clear();
	asked_.clear();
	game_.reset();
	mount_tried_ = false;
	known_.clear();
	queue_.clear();
	baseline_due_ = false;
	copies_.clear();
	shadow_graph_.reset();
	shadow_cache_.reset();
	publish(OriginalData());
}

} // namespace opennova::editor
