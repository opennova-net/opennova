#include <editor/session/import_operation.h>

#include <algorithm>
#include <utility>

#include <editor/session/import_controller.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

ImportOperation::ImportOperation(const ProjectPaths &paths, const ProjectDocument &document,
		std::vector<ImportSource> imports, bool replace, std::unique_ptr<ImportPlanOperation> replan,
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
		for (const ImportSource &import : imports_) {
			const bool planned = std::any_of(new_plan_->rows.begin(), new_plan_->rows.end(), [&import](const ImportPlanRow &row) {
				return row.state != ImportPlanRow::State::NotFound && row.source == import;
			});
			if (!planned) {
				refusals_.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.not_planned",
						import.name() + " is not in the import preview: plan it first.", import.name()));
				phase_ = Phase::Done;
				return true;
			}
		}
		phase_ = imports_.empty() ? Phase::Done : Phase::Write;
		return phase_ == Phase::Done;
	}
	case Phase::Write:
		// Written the whole selection or none of it as far as the disk allows (import_assets); from
		// here on the operation runs to its end.
		result_ = import_assets(imports_, paths_, document_, replace_);
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
	// The plan's files (all of them, once it ended), the write, then the refresh's.
	const uint64_t planned = replan_ ? replan_->progress().total : 0;
	progress.done = planned + (written_ ? 1 : 0) + (refresh_ ? refresh_->files_done() : 0);
	progress.total = planned + 1 + (refresh_ ? refresh_->files_total() : 0);
	progress.label = refresh_ ? refresh_->label() : "Importing the files";
	return progress;
}

OperationOutcome ImportOperation::finish(SessionCore &core) {
	return core.imports().absorb_import(*this);
}

} // namespace opennova::editor
