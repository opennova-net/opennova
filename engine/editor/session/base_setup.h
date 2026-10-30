#pragma once

// A mod's base game's project made on this computer (ADR 0074; session/novaworld_controller.h's
// make_base, the one resolver of the New project form's A mod of and of a got mod's base): the
// project of one of NovaWorld's games made in a folder of its own, as an operation. One the account
// cannot get (a retail game's, private on NovaWorld, which players cannot read) is imported from
// the game install, as a game of its own with the whole install imported is: made and opened in a
// session of its own (no process seam, the editor's preferences in memory, its workspace neither
// read nor written), every file of its game imported, each step of that session one step of this
// operation within the poll's budget, so the editor keeps drawing. It names no NovaWorld project.
// Any other is got from NovaWorld: a pull of its head into the empty folder, which is then its
// working copy. Its finish
// hands it to the controller, which goes on with what waited on it
// (NovaWorldController::finish_base: the mod made on it, or the got mod opened). While it runs the
// folder carries its mark (.opennova/base_setup), gone once the project is in: a folder still
// marked was left half-made (cancelled, failed, the editor closed), which the next setup there
// removes and makes again (base_setup_half_made).

#include <cstdint>
#include <memory>
#include <string>

#include <editor/novaworld/novaworld_jobs.h>
#include <editor/run/null_process_platform.h>
#include <editor/session/editor_request.h>
#include <editor/session/preferences_store.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class NovaWorldController;
class ProjectSession;

// What a base game's project is made as: the folder, the NovaWorld game's key and name, and what goes
// on once it is in (`then`: a new_project of the mod on it, or an open_project of a got mod).
struct BaseSetup {
	std::string dir;
	std::string key;
	std::string title;
	EditorRequest then;
};

// Whether `dir` holds a base game's project a setup began and did not finish (its mark).
bool base_setup_half_made(const std::string &dir);

class BaseSetupOperation : public SessionOperation {
public:
	// The game install `install`'s own game imported as the project (`preferences`: the editor's).
	BaseSetupOperation(NovaWorldController &controller, BaseSetup setup, std::string install,
			Preferences preferences);
	// The project got from NovaWorld by `pull` (a pull of setup.key into setup.dir, at no revision).
	BaseSetupOperation(
			NovaWorldController &controller, BaseSetup setup, std::unique_ptr<NovaWorldPull> pull);
	~BaseSetupOperation() override;

	OperationKind kind() const override { return OperationKind::BaseSetup; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	void cancel() override;
	OperationOutcome finish(SessionCore &core) override;

	const BaseSetup &setup() const { return setup_; }
	bool ok() const { return ok_; }
	// Why the project was not made, in its first refusal's words ("" when it was).
	const std::string &reason() const { return reason_; }
	// The get's job, its account for the controller to take (null for an import).
	NovaWorldPull *pull() { return pull_.get(); }

private:
	bool fail(std::string why);
	// The project is in: its mark removed.
	bool finished();

	NovaWorldController &controller_;
	BaseSetup setup_;
	std::string install_;
	NullProcessPlatform platform_;
	MemoryPreferencesStore preferences_;
	std::unique_ptr<ProjectSession> session_; // the import's, while it makes and imports the project
	bool started_ = false;
	bool marked_ = false;
	std::unique_ptr<NovaWorldPull> pull_;
	bool done_ = false;
	bool ok_ = false;
	std::string reason_;
};

} // namespace opennova::editor
