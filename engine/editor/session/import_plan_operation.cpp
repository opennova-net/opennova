#include <editor/session/import_plan_operation.h>

#include <utility>

#include <editor/session/import_controller.h>
#include <editor/session/problems_service.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

namespace {

// The three steps after the scan (the graph, the install, the plan), each one unit of progress.
constexpr uint64_t kPlanSteps = 3;

} // namespace

ImportPlanOperation::ImportPlanOperation(ProblemsService &problems, const ProjectPaths &paths,
		const ProjectDocument &document, const AssetGraph &graph,
		const std::vector<std::shared_ptr<const DocumentBase>> &open, std::vector<ImportChoice> roots,
		bool with_dependencies, std::string install) :
		problems_(problems),
		paths_(paths),
		document_(document),
		graph_(graph),
		open_(open),
		roots_(std::move(roots)),
		with_dependencies_(with_dependencies),
		install_(std::move(install)),
		walk_(paths_, document_) {}

bool ImportPlanOperation::step(const StepBudget &budget) {
	switch (phase_) {
	case Phase::Validation:
		// The validation left due, joined: the plan copies the graph, which then holds every edit.
		if (problems_.advance(budget.bytes)) phase_ = Phase::Scan;
		return false;
	case Phase::Scan:
		// The project read again (a scan writes nothing, unlike a refresh, which runs the import
		// pass): the view's scan may be older than a change made outside the editor.
		if (walk_.step(budget.bytes)) {
			scan_ = walk_.take();
			phase_ = Phase::Graph;
		}
		return false;
	case Phase::Graph:
		copy_ = std::make_unique<AssetGraph>(graph_);
		copy_->update(paths_, document_, scan_, open_);
		phase_ = Phase::Mount;
		return false;
	case Phase::Mount:
		// The game install where the plan looks for the files the chosen ones need: one that does not
		// mount is the plan's to say (it mounts it again and reports why).
		if (with_dependencies_ && !install_.empty()) {
			auto install = std::make_shared<ImportOrigin>();
			std::string error;
			if (install->open(ImportOrigin::Kind::GameInstall, install_, document_, error)) mounted_ = std::move(install);
		}
		phase_ = Phase::Plan;
		return false;
	case Phase::Plan:
		plan_ = plan_import(roots_, with_dependencies_, paths_, document_, scan_, *copy_, install_, kImportPlanFileCap,
				mounted_);
		copy_.reset();
		mounted_.reset();
		phase_ = Phase::Done;
		return true;
	case Phase::Done: return true;
	}
	return true;
}

OperationProgress ImportPlanOperation::progress() const {
	const uint64_t listed = walk_.files_listed();
	const uint64_t after = phase_ == Phase::Graph ? 0 : phase_ == Phase::Mount ? 1 : phase_ == Phase::Plan ? 2 : 3;
	OperationProgress progress;
	progress.unit = OperationUnit::Files;
	progress.total = walk_.listing() ? 0 : listed + kPlanSteps;
	progress.done = walk_.listing() ? 0 : walk_.files_visited() + (phase_ == Phase::Scan ? 0 : after);
	switch (phase_) {
	case Phase::Validation: progress.label = "Validating the project first"; break;
	case Phase::Scan: progress.label = walk_.listing() ? "Listing the project's files" : "Scanning " + walk_.current(); break;
	case Phase::Graph: progress.label = "Reading what the project's files name"; break;
	case Phase::Mount: progress.label = "Mounting the game install"; break;
	case Phase::Plan:
	case Phase::Done: progress.label = "Planning the import"; break;
	}
	return progress;
}

OperationOutcome ImportPlanOperation::finish(SessionCore &core) {
	return core.imports().absorb_plan(*this);
}

} // namespace opennova::editor
