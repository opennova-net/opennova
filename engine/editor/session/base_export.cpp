#include <editor/session/base_export.h>

#include <utility>

#include <editor/assets/asset_import.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The errors among `findings`.
std::vector<Diagnostic> errors_of(const std::vector<Diagnostic> &findings) {
	std::vector<Diagnostic> errors;
	for (const Diagnostic &d : findings)
		if (d.severity == DiagnosticSeverity::Error) errors.push_back(d);
	return errors;
}

// The first of `said` in its own words, with how many more there are; `otherwise` for none.
std::string first_of(const std::vector<Diagnostic> &said, const char *otherwise) {
	if (said.empty()) return otherwise;
	std::string words = said.front().message;
	if (said.size() > 1) words += " (and " + std::to_string(said.size() - 1) + " more)";
	return words;
}

// Why the base project's Export did not come to be: what refused its build (the gate's refusals), else
// the build's or the copy's errors, else what its operation came to.
std::string why_not_exported(const ActivityView &activity) {
	if (activity.has_build && activity.last_build) {
		const BuildReport &build = *activity.last_build;
		if (build.refused) return first_of(build.blockers, "its build was refused");
		if (!build.ok) return first_of(errors_of(build.diagnostics), "its build failed");
	}
	if (activity.has_export && activity.last_export)
		return first_of(errors_of(activity.last_export->diagnostics), "its export failed");
	return first_of(errors_of(activity.last_operation.findings), "it did not build");
}

} // namespace

BaseExportOperation::BaseExportOperation(std::string base_root, std::string export_dir, ProjectDocument expansion,
		Preferences preferences, BuildAfterBase after,
		NovaWorldAccount account, NovaWorldTransport *transport) :
		root_(std::move(base_root)),
		export_dir_(std::move(export_dir)),
		expansion_(std::move(expansion)),
		preferences_(std::move(preferences)),
		after_(std::move(after)) {
	std::string ignored;
	accounts_.save(account, ignored);
	transport_ = transport;
}

BaseExportOperation::~BaseExportOperation() = default;

bool BaseExportOperation::base_busy(const StepBudget &budget, bool validation) {
	// One step of its validation and one of its operation (a poll of ms 0), within this step's bytes.
	session_->set_poll_budget(PollBudget{0, budget.bytes});
	session_->poll();
	const ActivityView &activity = session_->view().activity;
	return activity.operation.running() || (validation && activity.validation.running);
}

bool BaseExportOperation::fail(std::string why) {
	reason_ = std::move(why);
	session_.reset();
	phase_ = Phase::Read;
	return false;
}

bool BaseExportOperation::step(const StepBudget &budget) {
	switch (phase_) {
	case Phase::Open: {
		// Opened as the command line opens a project to export it: no import pass (the build's own runs).
		session_ = std::make_unique<ProjectSession>(platform_, preferences_);
		session_->set_workspace_kept(false);
		session_->set_novaworld_accounts(&accounts_);
		session_->set_novaworld_transport(transport_);
		session_->handle(request::open_project(root_, false));
		if (!session_->outcome().done()) return fail(first_of(errors_of(session_->outcome().findings), "it does not open"));
		phase_ = Phase::Settle;
		return false;
	}
	case Phase::Settle: {
		// Its Open, then its validation, stepped: the Export's build then plans over the rows they composed
		// at once, as the editor's Export does over a project it has validated.
		if (base_busy(budget, true)) return false;
		const SessionView &view = session_->view();
		if (!view.project.open) return fail(first_of(errors_of(view.activity.last_operation.findings), "it does not open"));
		// A base project on NovaWorld exports with its lobby's gt.ssc, cached as the service answers for the
		// project (NovaWorldView::gate_tag_file): its ask as it opens where the folder is a working copy here,
		// else its setup as one, due once the slot is free (NovaWorldController::project_opened), which no
		// call shows yet. The export waits for any ask in flight (a fresh answer supersedes the cache) and,
		// while no lobby file is in hand, for the session's NovaWorld work to settle; one that brought none
		// refuses the export, saying so.
		const bool lobby_wanted = after_.gate_tag_file.empty() && !view.novaworld.key.empty() &&
				view.novaworld.gate_tag_file.empty();
		if (view.novaworld.calling || (lobby_wanted && session_->busy()))
			return false;
		auto request = request::export_project();
		request.gate_tag_file = after_.gate_tag_file;
		session_->handle(request);
		if (!session_->outcome().done())
			return fail(first_of(errors_of(session_->outcome().findings), "its export was refused"));
		phase_ = Phase::Export;
		return false;
	}
	case Phase::Export: {
		if (base_busy(budget, false)) return false;
		const ActivityView &activity = session_->view().activity;
		ok_ = activity.has_export && activity.last_export && activity.last_export->ok;
		if (!ok_) return fail(why_not_exported(activity));
		session_.reset(); // the base project closed: its documents and its graph go
		phase_ = Phase::Read;
		return false;
	}
	case Phase::Read:
		// The base game as the expansion reads it (OpenOperation's first steps over the export folder).
		if (!listed_) {
			listed_ = true;
			install_files_ = list_retail_file_names(export_dir_, expansion_);
			base_files_ = list_base_file_names(export_dir_, expansion_);
			layer_ = std::make_unique<BaseLayerBuild>(export_dir_, expansion_);
			return false;
		}
		if (!layer_->step(budget.bytes)) return false;
		phase_ = Phase::Done;
		return true;
	case Phase::Done: return true;
	}
	return true;
}

OperationProgress BaseExportOperation::progress() const {
	if (phase_ == Phase::Read || phase_ == Phase::Done) {
		if (layer_) return {layer_->files_done(), layer_->files_total(), OperationUnit::Files, "Reading the base game's names"};
		return {0, 0, OperationUnit::Steps, "Reading the base game's names"};
	}
	if (!session_) return {0, 0, OperationUnit::Steps, "Opening the base game's project"};
	// What the base project's session runs: its Open, its build or its copy, else its validation.
	const ActivityView &activity = session_->view().activity;
	if (activity.operation.running())
		return {activity.operation.done, activity.operation.total, activity.operation.unit,
		        "Base game: " + activity.operation.label};
	return {activity.validation.done, activity.validation.total, OperationUnit::Files, "Base game: checking its files"};
}

void BaseExportOperation::cancel() {
	// Its session goes, and with it the build and the copy it ran (BuildRun, ExportRun: the export folder as
	// it was).
	session_.reset();
	layer_.reset();
	phase_ = Phase::Done;
}

void BaseExportOperation::join(const EditorRequest &request) {
	if (!request.gate_tag_file.empty())
		after_.gate_tag_file = request.gate_tag_file;
	if (!request.version_file.empty())
		after_.version_file = request.version_file;
	switch (request.kind) {
	case EditorRequestKind::Build:
		after_.out_dir = request.out_dir;
		after_.rehash = after_.rehash || request.rehash;
		after_.panel = request.report;
		break;
	case EditorRequestKind::Export:
		after_.exported = ExportIntent{true, request.export_dir};
		after_.rehash = after_.rehash || request.rehash;
		break;
	case EditorRequestKind::Play:
		// Its mode resolved as it joined (join_operation), as BuildOperation takes it.
		after_.play.wanted = true;
		after_.play.mission = request.mission;
		after_.play.behind = request.behind;
		after_.play.fresh = request.fresh;
		after_.play.start = request.start;
		after_.play.mode = request.play_mode.value_or(PlayMode::Runtime);
		break;
	default: return;
	}
	after_.wanted = true;
}

OperationOutcome BaseExportOperation::finish(SessionCore &core) {
	return core.absorb_base_export(*this);
}

} // namespace opennova::editor
