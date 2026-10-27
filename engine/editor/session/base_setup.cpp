#include <editor/session/base_setup.h>

#include <algorithm>
#include <utility>

#include <editor/session/novaworld_controller.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The first error among `findings` in its own words, `otherwise` for none.
std::string first_error(const std::vector<Diagnostic> &findings, const char *otherwise) {
	for (const Diagnostic &d : findings)
		if (d.severity == DiagnosticSeverity::Error)
			return d.message;
	return otherwise;
}

} // namespace

BaseSetupOperation::BaseSetupOperation(NovaWorldController &controller, BaseSetup setup,
		std::string install, Preferences preferences) :
		controller_(controller),
		setup_(std::move(setup)),
		install_(std::move(install)),
		preferences_(std::move(preferences)) {}

BaseSetupOperation::BaseSetupOperation(
		NovaWorldController &controller, BaseSetup setup, std::unique_ptr<NovaWorldPull> pull) :
		controller_(controller), setup_(std::move(setup)), pull_(std::move(pull)) {}

BaseSetupOperation::~BaseSetupOperation() = default;

bool BaseSetupOperation::fail(std::string why) {
	reason_ = std::move(why);
	session_.reset();
	done_ = true;
	return true;
}

bool BaseSetupOperation::step(const StepBudget &budget) {
	if (done_)
		return true;
	if (pull_) {
		// Got as a get's pull gets a project into an empty folder: its working copy at the head.
		if (pull_->step(std::max<uint64_t>(budget.bytes, 1)) == NovaWorldJobState::Running)
			return false;
		if (pull_->state() != NovaWorldJobState::Done)
			return fail(pull_->error().empty() ? "the get was cancelled" : pull_->error());
		if (pull_->up_to_date())
			return fail(setup_.key + " has no revision yet");
		ok_ = true;
		done_ = true;
		return true;
	}
	if (!started_) {
		// Made as New project makes a game you own: the project, then every file of its game imported as
		// the Open that follows it ends.
		started_ = true;
		session_ = std::make_unique<ProjectSession>(platform_, preferences_);
		session_->set_workspace_kept(false);
		session_->handle(request::new_stock_project(setup_.dir, setup_.title, install_));
		if (!session_->outcome().done())
			return fail(first_error(session_->outcome().findings, "it could not be made"));
		return false;
	}
	// Its Open, then its import, one poll at a time within this step's bytes; its validation is the
	// project's own, left to whoever opens it.
	session_->set_poll_budget(PollBudget{ 0, budget.bytes });
	session_->poll();
	const ActivityView &activity = session_->view().activity;
	if (activity.operation.running())
		return false;
	const OperationOutcome &last = activity.last_operation;
	if (last.kind != OperationKind::ImportApply || last.end != OperationEnd::Done)
		return fail(first_error(last.findings, "its game's files were not imported"));
	session_.reset(); // the project closed: its documents and its graph go
	ok_ = true;
	done_ = true;
	return true;
}

OperationProgress BaseSetupOperation::progress() const {
	if (pull_) {
		const NovaWorldJobProgress p = pull_->progress();
		return { std::min(p.done, p.total), p.total,
			p.total == 0 ? OperationUnit::Steps
			: p.bytes	 ? OperationUnit::Bytes
						 : OperationUnit::Files,
			setup_.title + ": " + p.label };
	}
	if (!session_)
		return { 0, 0, OperationUnit::Steps, "Making " + setup_.title + "'s project" };
	const ActivityView &activity = session_->view().activity;
	return { activity.operation.done, activity.operation.total, activity.operation.unit,
		setup_.title + ": " + activity.operation.label };
}

void BaseSetupOperation::cancel() {
	if (pull_)
		pull_->cancel();
	session_.reset();
	done_ = true;
}

OperationOutcome BaseSetupOperation::finish(SessionCore &) {
	return controller_.finish_base(*this);
}

} // namespace opennova::editor
