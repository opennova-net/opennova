#include <editor/session/import_operation.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include <editor/model/core_finding.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/session/import_controller.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

ImportOperation::ImportOperation(const ProjectPaths &paths, const ProjectDocument &document,
		std::vector<ImportChoice> imports, bool replace, std::unique_ptr<ImportPlanOperation> replan,
		std::shared_ptr<const ImportPlan> shown) :
		paths_(paths),
		document_(document),
		imports_(std::move(imports)),
		replace_(replace),
		replan_(std::move(replan)),
		shown_(std::move(shown)) {
	if (!replan_) phase_ = imports_.empty() ? Phase::Done : Phase::Write;
}

ImportOperation::ImportOperation(const ProjectPaths &paths, const ProjectDocument &document,
		std::unique_ptr<ImportPlanOperation> plan) :
		paths_(paths), document_(document), replace_(true), own_plan_(true), replan_(std::move(plan)) {
	if (!replan_) phase_ = Phase::Done;
}

bool ImportOperation::step(const StepBudget &budget) {
	switch (phase_) {
	case Phase::Plan: {
		if (!replan_->step(budget)) return false;
		new_plan_ = std::make_shared<const ImportPlan>(std::move(replan_->plan()));
		// Its own plan: every row the project can take, each file the project holds of its name written over
		// (a row it cannot take left out, as a planned import leaves it out).
		if (own_plan_) {
			imports_ = import_selection(*new_plan_, import_default_checks(*new_plan_, true), true).sources;
			phase_ = imports_.empty() ? Phase::Done : Phase::Write;
			return phase_ == Phase::Done;
		}
		// Planned again before it writes: when that is not the import the dialog showed (a dependency
		// new or gone, a file found in another place, a file that no longer reads), nothing is
		// written; a row the plan does not have is refused.
		changed_ = shown_ && !same_import(*shown_, *new_plan_);
		if (changed_) {
			phase_ = Phase::Done;
			return true;
		}
		// The sources the plan takes, looked up once (a mission's closure asks for thousands).
		std::set<std::string> taken;
		const auto key_of = [](const ImportChoice &source) {
			return source.path + '\n' + source.entry + '\n' + (source.install ? '1' : '0') + (source.native ? '1' : '0') +
			       '\n' + source.as + '\n' + source.expansion;
		};
		for (const ImportPlanRow &row : new_plan_->rows)
			if (row.state != ImportPlanRow::State::NotFound) taken.insert(key_of(row.source));
		for (const ImportChoice &import : imports_) {
			const bool planned = taken.count(key_of(import)) > 0;
			if (!planned) {
				refusals_.push_back(make_finding(CoreFinding::ImportNotPlanned, DiagnosticSeverity::Error, import.name() + " is not in the import preview: plan it first.", import.name()));
				phase_ = Phase::Done;
				return true;
			}
		}
		phase_ = imports_.empty() ? Phase::Done : Phase::Write;
		return phase_ == Phase::Done;
	}
	case Phase::Write:
		// Written the whole selection or none of it as far as the disk allows, a file a step
		// (AssetImport: each source read, checked and staged, then each file published); once it
		// publishes its first file the operation runs to its end.
		if (!import_) import_ = std::make_unique<AssetImport>(imports_, paths_, document_, replace_);
		if (!import_->step(budget.bytes)) return false;
		write_done_ = import_->files_done();
		write_total_ = import_->files_total();
		result_ = import_->take();
		import_.reset();
		written_ = true;
		record_archives();
		if (result_.imported.empty() && !recorded_) {
			phase_ = Phase::Done;
			return true;
		}
		refresh_ = std::make_unique<ProjectRefresh>(paths_, document_);
		phase_ = Phase::Refresh;
		return false;
	case Phase::Refresh:
		if (!refresh_->step(budget.bytes)) return false;
		phase_ = Phase::Done;
		return true;
	case Phase::Done: return true;
	}
	return true;
}

OperationProgress ImportOperation::progress() const {
	OperationProgress progress;
	progress.unit = OperationUnit::Files;
	if (phase_ == Phase::Plan) {
		progress = replan_->progress();
		progress.label = (own_plan_ ? "Planning the import: " : "Planning the import again: ") + progress.label;
		return progress;
	}
	// The plan's files (all of them, once it ended), the write's (each source checked and each
	// file published), then the refresh's.
	const uint64_t planned = replan_ ? replan_->progress().total : 0;
	const uint64_t write_done = import_ ? import_->files_done() : write_done_;
	const uint64_t write_total = import_ ? import_->files_total() : written_ ? write_total_ : imports_.size();
	progress.done = planned + write_done + (refresh_ ? refresh_->files_done() : 0);
	progress.total = planned + std::max(write_total, write_done) + (refresh_ ? refresh_->files_total() : 0);
	// The write's count is its steps (each source checked, each file published), not files: the label names the
	// files the import makes and how far it is (S26 C15: "6383 of 15683" read as files, of a 9300-file import).
	const std::string files = std::to_string(imports_.size()) + (imports_.size() == 1 ? " file" : " files");
	progress.label = refresh_         ? refresh_->label()
	                 : write_done > 0 ? "Importing " + files + ": " +
	                                            std::to_string(write_done * 100 / std::max<uint64_t>(write_total, 1)) + "%"
	                                  : "Importing " + files;
	return progress;
}

bool ImportOperation::cancellable() const {
	return !written_ && !(import_ && import_->publishing());
}

void ImportOperation::cancel() {
	// What it staged goes; nothing was published (cancellable says so).
	if (import_) import_->abandon();
}

void ImportOperation::record_archives() {
	ProjectDocument document = document_;
	for (const auto &[name, slot] : result_.archives) {
		auto &held = document.archives[name];
		if (held != slot) {
			held = slot;
			recorded_ = true;
		}
	}
	if (!recorded_) return;
	// Saved before the rescan, so the project the rescan reads, and the session takes, has it; one that
	// cannot be written is said, the files kept.
	Diagnostic error;
	if (!save_project_document(paths_.project_file, document, error)) {
		result_.diagnostics.push_back(error);
		recorded_ = false;
		return;
	}
	document_ = std::move(document);
}

OperationOutcome ImportOperation::finish(SessionCore &core) {
	return core.imports().absorb_import(*this);
}

} // namespace opennova::editor
