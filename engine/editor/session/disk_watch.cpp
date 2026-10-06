#include <editor/session/disk_watch.h>

#include <set>
#include <utility>

#include <base/io/file_time.h>
#include <base/io/strutil.h>
#include <editor/assets/project_scan.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/document_base.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_files.h>
#include <editor/session/session_core.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

namespace {

// The files an import read (S13 A8), which the import round trip watches for their sources.
std::set<std::string> import_inputs(const SessionView &view) {
	std::set<std::string> out;
	if (view.project.imports)
		for (const ImportedSource &source : *view.project.imports)
			out.insert(source.inputs.begin(), source.inputs.end());
	return out;
}

} // namespace

DiskWatch::DiskWatch(SessionCore &core) : core_(core) {}

bool DiskWatch::follow_project() {
	const SessionView &view = core_.view();
	const std::string root = view.project.open ? view.project.root : std::string();
	if (root != root_) {
		changes_.clear();
		root_ = root;
		unsettled_ = 0;
		publish(0);
	}
	return view.project.open && view.project.scan && view.project.document;
}

bool DiskWatch::imports_watch(const std::string &relative) const {
	const SessionView &view = core_.view();
	if (const AssetEntry *entry = view.project.scan->at_path(relative)) {
		if (!entry->imported_from.empty() || entry->kind == AssetKind::ImportSource) return true;
		if (entry->kind == AssetKind::Texture && strutil::ends_with_icase(relative, ".png")) return true;
	}
	return false;
}

std::vector<std::string> DiskWatch::watched_files() const {
	const SessionView &view = core_.view();
	const AssetScan &scan = *view.project.scan;
	const std::set<std::string> inputs = import_inputs(view);
	std::set<std::string> files;
	for (const std::shared_ptr<const DocumentBase> &document : view.documents.open)
		if (document) files.insert(document->path());
	// What each viewport's picture read (a model's textures, a menu's stylesheets and fonts, a mission's
	// models and terrain), by the names the game reads them by.
	const Viewports &viewports = core_.viewports();
	for (size_t i = 0; i < viewports.size(); ++i)
		if (const FileStamps *read = viewports.at(i).picture_reads())
			for (const FileStamp &file : read->files())
				if (const AssetEntry *entry = scan.find(file.name)) files.insert(entry->relative_path);
	std::vector<std::string> out;
	for (const std::string &file : files)
		if (!imports_watch(file) && !inputs.count(file)) out.push_back(file);
	return out;
}

void DiskWatch::check(bool all) {
	if (!follow_project()) return;
	SessionView &view = core_.view();
	const AssetScan &scan = *view.project.scan;
	const ProjectPaths &paths = core_.paths();
	const int64_t now_ms = core_.platform().now_ms();
	const int64_t now_ticks = io::file_clock_now_ticks();
	// What a look found moved before, the files the editor shows, then what the folders made and lost.
	std::vector<std::string> looks = changes_.pending_files();
	for (std::string &file : watched_files()) looks.push_back(std::move(file));
	for (std::string &file : changes_.folder_changes(paths, *view.project.document, scan, now_ticks))
		looks.push_back(std::move(file));
	changes_.look_at(paths, scan, looks, now_ms, now_ticks);
	if (all) {
		// Every file of the project the import round trip does not watch, a few a poll (step).
		const std::set<std::string> inputs = import_inputs(view);
		std::vector<std::string> every;
		for (const auto &[path, visit] : scan.visits()) {
			(void)visit;
			if (!path.empty() && !imports_watch(path) && !inputs.count(path)) every.push_back(path);
		}
		changes_.begin_sweep(std::move(every));
	}
	// The import round trip's own files, under its own rule (S18).
	const std::vector<ImportedSource> none;
	ExternalChanges changes = external_changes(paths, scan, view.project.imports ? *view.project.imports : none, now_ticks);
	const size_t unsettled = changes.unsettled;
	const std::vector<std::string> ready = changes_.take_ready();
	if (ready.empty() && changes.empty()) return publish(unsettled);
	std::set<std::string> sources(changes.sources.begin(), changes.sources.end());
	std::set<std::string> files(changes.files.begin(), changes.files.end());
	const std::string record_suffix = kImportSidecarSuffix;
	for (const std::string &file : ready) {
		files.insert(file);
		// An import record that changed, or a source that came with its record: the source imported (again).
		if (strutil::ends_with_icase(file, record_suffix)) {
			const std::string source = file.substr(0, file.size() - record_suffix.size());
			if (disk_stamp(paths, source).present) sources.insert(source);
		} else if (importer_for(basename_of(file)) && disk_stamp(paths, file).present &&
		           disk_stamp(paths, file + record_suffix).present) {
			sources.insert(file);
		}
	}
	changes.sources.assign(sources.begin(), sources.end());
	changes.files.assign(files.begin(), files.end());
	if (core_.start_changed_refresh(std::move(changes))) {
		view.activity.status = "Reading what changed outside the editor...";
		core_.touch(ViewConcern::Output);
	}
	publish(unsettled);
}

void DiskWatch::step(const PollBudget &budget) {
	if (!changes_.sweeping() || !follow_project() || core_.operations().running()) return;
	const SessionView &view = core_.view();
	const int64_t started = budget.ms > 0 ? steady_clock_ms() : 0;
	const uint64_t bytes = budget.step_bytes ? budget.step_bytes : kWholeWalkStep;
	// A step at least, then more while the poll's milliseconds last: a project of thousands of files is
	// looked at over a few frames.
	while (!changes_.step_sweep(core_.paths(), *view.project.scan, bytes, core_.platform().now_ms(),
	                            io::file_clock_now_ticks()))
		if (budget.ms <= 0 || steady_clock_ms() - started >= budget.ms) break;
	publish(unsettled_);
}

void DiskWatch::publish(size_t unsettled) {
	unsettled_ = unsettled;
	ProjectView &project = core_.view().project;
	const size_t waiting = changes_.waiting() + changes_.ready() + unsettled;
	const bool sweeping = changes_.sweeping();
	if (project.outside_waiting == waiting && project.outside_sweeping == sweeping) return;
	project.outside_waiting = waiting;
	project.outside_sweeping = sweeping;
	core_.touch(ViewConcern::Files);
}

} // namespace opennova::editor
