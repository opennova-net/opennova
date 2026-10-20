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
#include <formats/env/env.h>

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
	// The game install's files as the project imports them (assets/install_view.h: as the game is served them, with
	// `/exp` for a project that builds on an installed expansion and under the project's names for one that builds as
	// an expansion), by logical name.
	std::shared_ptr<InstallFileSource> files;
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
	// The round under way: the files it validates and the names they are of; its missions added once the graph
	// holds the install (add_scope_missions).
	ValidationScope scope;
	std::set<std::string> round;
	bool scope_missions = false;
	std::chrono::steady_clock::duration spent{}, round_spent{};

	ValidationInput input() const { return {paths, *document, *scan, open, &scope}; }
};

OriginalFiles::OriginalFiles() : data_(std::make_shared<const OriginalData>()) {}

OriginalFiles::~OriginalFiles() = default;

void OriginalFiles::want(const std::string &install, const std::shared_ptr<const ProjectDocument> &document,
                         const void *scan, const std::vector<std::string> &names) {
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
		wanted_.insert(names.begin(), names.end());
		if (!install_.empty() && document_) start();
		return;
	}
	document_ = document;
	// The names not wanted before, validated at the next round (one under way goes on).
	for (const std::string &name : names)
		if (wanted_.insert(name).second && !covered_.count(name)) pending_.insert(name);
	if (scan == scan_) return;
	scan_ = scan;
	// A new scan (a Refresh, a save, an import): the install's folder looked at again, and validated again
	// once it moved (a patch over it). One under way goes on.
	if (phase_ == Phase::Done && folder_state() != folder_) start();
}

void OriginalFiles::start() {
	run_.reset();
	folder_ = folder_state();
	phase_ = Phase::Mount;
	mounted_ = false;
	covered_.clear();
	pending_ = wanted_;
}

void OriginalFiles::begin_round() {
	Run &run = *run_;
	run.round = wanted_;
	pending_.clear();
	run.scope.paths.clear();
	run.scope.tiles = false;
	for (const std::string &name : run.round)
		if (const AssetEntry *entry = run.scan->find(name)) {
			run.scope.paths.insert(entry->relative_path);
			run.scope.tiles = run.scope.tiles || entry->kind == AssetKind::TileInfo;
		}
	run.scope.taken();
	run.scope_missions = false;
	run.validation = std::make_unique<ProjectValidation>(run.graph, run.cache, run.readings);
	run.slot = 0;
	run.begun = false;
	run.round_spent = {};
	phase_ = Phase::Validate;
}

void OriginalFiles::add_scope_missions() {
	Run &run = *run_;
	run.scope_missions = true;
	// The terrain gate's findings (graph/use_checks.cpp check_terrain_admission) land on a mission, on the line of
	// overcast.def or of its environment that set what the gate refuses, and take a terrain's own refusal back
	// where every mission reading it loads it: each mission whose terrain or environment is one of the scope's
	// files is judged with them (every mission for overcast.def, which every terrain's load reads).
	std::set<std::string> missions;
	for (const std::string &path : run.scope.paths) {
		const AssetEntry *entry = run.scan->at_path(path);
		if (!entry) continue;
		if (pff::normalized_logical_name(entry->logical_name) == pff::normalized_logical_name(env::kOvercastFile)) {
			for (const AssetEntry &each : run.scan->entries)
				if (each.kind == AssetKind::Mission) missions.insert(each.relative_path);
			continue;
		}
		if (entry->kind != AssetKind::Terrain && entry->kind != AssetKind::Environment) continue;
		for (const GraphEdge *edge : run.graph.referrers_of_file(path)) {
			const AssetEntry *source = run.scan->at_path(edge->source);
			if (source && source->kind == AssetKind::Mission) missions.insert(source->relative_path);
		}
	}
	const size_t before = run.scope.paths.size();
	run.scope.paths.insert(missions.begin(), missions.end());
	if (run.scope.paths.size() != before) run.scope.taken();
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
		if (!run_) return;
		const auto took = std::chrono::steady_clock::now() - began;
		run_->spent += took;
		run_->round_spent += took;
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
			pending_.clear();
			phase_ = Phase::Done;
			OriginalData none;
			none.ready = true;
			publish(std::move(none));
			return true;
		}
		mounted_ = true;
		run_->files = std::make_shared<InstallFileSource>(std::move(game));
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
			listed_ = run_->scan->entries.size();
			begin_round();
		}
		spent();
		return false;
	}
	case Phase::Validate: {
		const bool done = run_->validation->step(run_->input(), bytes);
		// The graph holds the install from its update's step on: the scope's missions, before the uses' headers
		// and the files' own findings are asked.
		if (!run_->scope_missions && run_->validation->graph_read()) add_scope_missions();
		if (done) phase_ = Phase::Checks;
		spent();
		return false;
	}
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
		// Every finding about a file of a name the round covers, keyed, by the file's logical name. The run is
		// kept (its listing, graph, files' findings and checks) for the names a later row is about.
		std::vector<Diagnostic> rows = project_rows(run_->input(), run_->graph, run_->cache);
		run_->checks.append_findings(rows);
		OriginalData data;
		data.ready = true;
		data.scoped = true;
		data.covered = run_->round;
		for (const Diagnostic &d : rows) {
			if (d.asset.empty()) continue;
			const std::string name = pff::normalized_logical_name(basename_of(d.asset));
			if (data.covered.count(name)) ++data.findings[name][original_finding_key(d)];
		}
		spent();
		files_ = run_->scope.paths.size();
		last_ms_ = std::chrono::duration<double, std::milli>(validations_ ? run_->round_spent : run_->spent).count();
		++validations_;
		covered_ = run_->round;
		for (const std::string &name : wanted_)
			if (!covered_.count(name)) pending_.insert(name);
		// The mount let go, the install's archives free for a patch: the next round mounts it again.
		run_->files.reset();
		run_->paths.files.reset();
		phase_ = Phase::Done;
		publish(std::move(data));
		return pending_.empty();
	}
	case Phase::Done:
		// Names wanted since the last round: another round over the run as it stands, the install mounted again
		// (one that moved since starts over: what the run holds is of its files as they were).
		if (run_ && mounted_ && !pending_.empty()) {
			const auto began_round = std::chrono::steady_clock::now();
			if (folder_state() != folder_) {
				start();
				return false;
			}
			auto game = std::make_unique<InstallView>();
			std::string why;
			if (!game->open(install_spec(install_, *document_), why)) {
				start(); // the next step finds it does not mount
				return false;
			}
			run_->files = std::make_shared<InstallFileSource>(std::move(game));
			run_->paths.files = run_->files;
			begin_round();
			run_->round_spent = std::chrono::steady_clock::now() - began_round;
			run_->spent += run_->round_spent;
			return false;
		}
		break;
	case Phase::Idle: break;
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
	mounted_ = false;
	wanted_.clear();
	covered_.clear();
	pending_.clear();
	publish(OriginalData());
}

} // namespace opennova::editor
