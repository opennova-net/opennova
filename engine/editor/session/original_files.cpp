#include <editor/session/original_files.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

#include <base/vfs/file_source.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/install_view.h>
#include <editor/documents/project_check.h>
#include <editor/documents/project_checks.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// Every run of digits as one '#': a count or a place in words, which an edit elsewhere moves.
std::string without_numbers(const std::string &text) {
	std::string out;
	bool digits = false;
	for (const char c : text) {
		const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
		if (digit && !digits) out += '#';
		if (!digit) out += c;
		digits = digit;
	}
	return out;
}

// The game install's files as the project imports them (assets/install_view.h: as the game is served
// them, with `/exp` for a project that builds on an installed expansion and under the project's names
// for one that builds as an expansion), by logical name.
class InstallFiles final : public FileSource {
public:
	explicit InstallFiles(std::unique_ptr<InstallView> view) : view_(std::move(view)) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const InstallFile *file = view_->find(name);
		return file && view_->read(*file, out);
	}
	uint64_t stamp(const std::string &name) const override { return view_->find(name) ? 1 : 0; }
	// A file's size as stored, without reading it; 0 when it cannot be told.
	uint64_t size_of(const std::string &name) const {
		const InstallFile *file = view_->find(name);
		return file ? view_->size(*file) : 0;
	}
	const InstallView &view() const { return *view_; }

private:
	std::unique_ptr<InstallView> view_;
};

} // namespace

std::string original_finding_key(const Diagnostic &d) {
	std::string key = d.code();
	const auto part = [&key](const std::string &text) {
		key += '\x1f';
		key += text;
	};
	part(d.record_key.empty() ? without_numbers(d.record) : d.record_key);
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
	// A finding of no record (a text document's) by its words, never its line or its numbers.
	if (d.record.empty() && d.record_key.empty()) part(without_numbers(d.message));
	return key;
}

// The install validated: its files listed and read through its mount, its own graph, files' findings
// and project checks (nothing of the session's), its steps' time.
struct OriginalFiles::Run {
	std::shared_ptr<const ProjectDocument> document;
	std::shared_ptr<InstallFiles> files;
	ProjectPaths paths;
	std::vector<std::string> names; // the install's files as the game serves them
	size_t scanned = 0;              // of those, typed into the scan
	std::shared_ptr<AssetScan> scan = std::make_shared<AssetScan>();
	std::vector<std::shared_ptr<const DocumentBase>> open; // none: the install's files as they ship
	AssetGraph graph;
	ValidationCache cache;
	GraphReadings readings;
	std::unique_ptr<ProjectValidation> validation;
	ProjectChecks checks;
	size_t slot = 0;
	bool begun = false;
	std::chrono::steady_clock::duration spent{};

	ValidationInput input() const { return {paths, *document, *scan, open}; }
};

OriginalFiles::OriginalFiles() : data_(std::make_shared<const OriginalData>()) {}

OriginalFiles::~OriginalFiles() = default;

void OriginalFiles::want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document,
                         const void *scan) {
	const std::string game = document ? document->target_game : std::string();
	// The install as the project imports it moves with the project's expansion (install_spec): another one
	// is another install to judge against (ADR 0046 S16).
	const ProjectExpansion expansion = document ? document->expansion : ProjectExpansion();
	if (install != install_ || game != game_ || expansion != expansion_ || !document_ || !document) {
		clear();
		install_ = install;
		game_ = game;
		expansion_ = expansion;
		document_ = document;
		scan_ = scan;
		if (!install_.empty() && document_) start();
		return;
	}
	document_ = document;
	if (scan == scan_) return;
	scan_ = scan;
	// A new scan (a Refresh, a save, an import): the install's folder looked at again, and validated again
	// once it moved (a patch over it). One under way goes on.
	if (phase_ == Phase::Done && folder_state() != folder_) start();
}

void OriginalFiles::start() {
	run_.reset();
	folder_ = folder_state();
	longest_ms_ = 0.0;
	phase_ = Phase::Mount;
}

// The install folder's files (their names, sizes and last writes), what a patch over it moves.
std::string OriginalFiles::folder_state() const {
	std::vector<std::string> entries;
	std::error_code ec;
	fs::directory_iterator it(system_path(install_), ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		std::error_code status;
		if (!it->is_regular_file(status)) continue;
		const uintmax_t size = it->file_size(status);
		const auto written = it->last_write_time(status).time_since_epoch().count();
		entries.push_back(utf8_of(it->path().filename()) + "\x1f" + std::to_string(size) + "\x1f" + std::to_string(written));
	}
	std::sort(entries.begin(), entries.end());
	std::string out;
	for (const std::string &entry : entries) out += entry + "\x1e";
	return out;
}

bool OriginalFiles::step(uint64_t bytes) {
	if (settled()) return true;
	const auto began = std::chrono::steady_clock::now();
	const auto spent = [&] {
		const auto took = std::chrono::steady_clock::now() - began;
		longest_ms_ = std::max(longest_ms_, std::chrono::duration<double, std::milli>(took).count());
		if (run_) run_->spent += took;
	};
	switch (phase_) {
	case Phase::Mount: {
		// The install mounted as a stock launch mounts it and listed as the game serves it: each file by its
		// logical name, read through the mount, its kind as the scan types it (a `.bin` or a name no rule
		// types by what it holds).
		run_ = std::make_unique<Run>();
		run_->document = document_;
		auto game = std::make_unique<InstallView>();
		std::string why;
		if (!game->open(install_spec(install_, *document_), why)) {
			// An install that does not mount: none of the project's findings is the original's.
			run_.reset();
			phase_ = Phase::Done;
			OriginalData none;
			none.ready = true;
			publish(std::move(none));
			return true;
		}
		run_->files = std::make_shared<InstallFiles>(std::move(game));
		run_->paths = ProjectPaths::for_root(install_);
		run_->paths.files = run_->files;
		for (const InstallFile &file : run_->files->view().files()) run_->names.push_back(file.name);
		std::sort(run_->names.begin(), run_->names.end(), [](const std::string &a, const std::string &b) {
			return pff::normalized_logical_name(a) < pff::normalized_logical_name(b);
		});
		phase_ = Phase::Scan;
		spent();
		return false;
	}
	case Phase::Scan: {
		// Each file typed within the budget: by its name, or by what it holds where its name cannot say.
		uint64_t read = 0;
		while (run_->scanned < run_->names.size() && read < bytes) {
			const std::string &name = run_->names[run_->scanned++];
			AssetEntry entry;
			entry.logical_name = name;
			entry.relative_path = name;
			entry.size_bytes = run_->files->size_of(name);
			entry.kind = classify_asset(name, nullptr);
			read += kValidationFileCost;
			if (asset_classification_needs_bytes(name) || entry.kind == AssetKind::Unknown) {
				std::vector<uint8_t> content;
				if (run_->files->read(name, content)) entry.kind = classify_asset(name, &content);
				read += content.size();
			}
			run_->scan->entries.push_back(std::move(entry));
		}
		if (run_->scanned == run_->names.size()) {
			run_->scan->index();
			run_->validation = std::make_unique<ProjectValidation>(run_->graph, run_->cache, run_->readings);
			phase_ = Phase::Validate;
		}
		spent();
		return false;
	}
	case Phase::Validate:
		if (run_->validation->step(run_->input(), bytes)) phase_ = Phase::Checks;
		spent();
		return false;
	case Phase::Checks: {
		// The document types' project checks over the install's files, each stepped (a menu a step).
		const ValidationInput input = run_->input();
		while (run_->slot < run_->checks.slot_count()) {
			if (!run_->begun) {
				run_->checks.begin_slot(run_->slot);
				run_->begun = true;
			}
			bool moved = false;
			if (!run_->checks.step_slot(run_->slot, {input, run_->cache, *run_->files}, bytes, moved)) {
				spent();
				return false;
			}
			run_->begun = false;
			const bool checked = run_->checks.has_check(run_->slot);
			++run_->slot;
			if (checked) {
				spent();
				return false;
			}
		}
		phase_ = Phase::Rows;
		spent();
		return false;
	}
	case Phase::Rows: {
		// Every finding about a file of the install, keyed, by the file's logical name; then everything the
		// run held let go (its graph, its files' findings, its checks, the mount).
		std::vector<Diagnostic> rows = project_rows(run_->input(), run_->graph, run_->cache);
		run_->checks.append_findings(rows);
		OriginalData data;
		data.ready = true;
		for (const Diagnostic &d : rows)
			if (!d.asset.empty()) ++data.findings[pff::normalized_logical_name(basename_of(d.asset))][original_finding_key(d)];
		spent();
		files_ = run_->scan->entries.size();
		last_ms_ = std::chrono::duration<double, std::milli>(run_->spent).count();
		++validations_;
		run_.reset();
		phase_ = Phase::Done;
		publish(std::move(data));
		return true;
	}
	case Phase::Idle:
	case Phase::Done: break;
	}
	return true;
}

void OriginalFiles::publish(OriginalData data) {
	if (*data_ == data) return;
	data_ = std::make_shared<const OriginalData>(std::move(data));
	++generation_;
}

void OriginalFiles::clear() {
	install_.clear();
	game_.clear();
	expansion_ = ProjectExpansion();
	document_.reset();
	folder_.clear();
	scan_ = nullptr;
	run_.reset();
	phase_ = Phase::Idle;
	publish(OriginalData());
}

} // namespace opennova::editor
