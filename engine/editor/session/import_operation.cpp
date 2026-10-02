#include <editor/session/import_operation.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include <editor/model/diagnostic.h>
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

bool ImportOperation::step(const StepBudget &budget) {
	switch (phase_) {
	case Phase::Plan: {
		if (!replan_->step(budget)) return false;
		new_plan_ = std::make_shared<const ImportPlan>(std::move(replan_->plan()));
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
			return source.path + '\n' + source.entry + '\n' + (source.install ? '1' : '0') + (source.native ? '1' : '0');
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
		if (result_.imported.empty()) {
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
		progress.label = "Planning the import again: " + progress.label;
		return progress;
	}
	// The plan's files (all of them, once it ended), the write's (each source checked and each
	// file published), then the refresh's.
	const uint64_t planned = replan_ ? replan_->progress().total : 0;
	const uint64_t write_done = import_ ? import_->files_done() : write_done_;
	const uint64_t write_total = import_ ? import_->files_total() : written_ ? write_total_ : imports_.size();
	progress.done = planned + write_done + (refresh_ ? refresh_->files_done() : 0);
	progress.total = planned + std::max(write_total, write_done) + (refresh_ ? refresh_->files_total() : 0);
	progress.label = refresh_         ? refresh_->label()
	                 : write_done > 0 ? "Importing the files: " + std::to_string(write_done) + " of " + std::to_string(write_total)
	                                  : "Importing the files";
	return progress;
}

bool ImportOperation::cancellable() const {
	return !written_ && !(import_ && import_->publishing());
}

void ImportOperation::cancel() {
	// What it staged goes; nothing was published (cancellable says so).
	if (import_) import_->abandon();
}

OperationOutcome ImportOperation::finish(SessionCore &core) {
	return core.imports().absorb_import(*this);
}

} // namespace opennova::editor
