#include <editor/session/open_operation.h>

#include <utility>

#include <editor/assets/asset_import.h>
#include <editor/project/base_project.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

OpenOperation::OpenOperation(ProjectPaths paths, LocalSettings local, ProjectDocument document, bool import_pass,
		std::string seed, std::string run_install) :
		paths_(std::move(paths)),
		local_(std::move(local)),
		document_(std::move(document)),
		seed_(std::move(seed)),
		run_install_(std::move(run_install)),
		refresh_(paths_, document_, false, std::string(), import_pass) {}

bool OpenOperation::step(const StepBudget &budget) {
	if (!listed_) {
		// The game install's names first, the project's own install (its local.json, else the one
		// the editor last chose): the Import fixes read them. For an expansion of a project's base game
		// (T5), that project's export in its place (SessionCore::base_game).
		std::string base = local_.game_install;
		if (document_.expansion.on_base_project()) {
			Diagnostic why;
			base_project_game_dir(paths_.root, document_.expansion, document_.target_game, base, why);
		}
		install_files_ = list_retail_file_names(base, document_);
		// And an expansion's base game's, which its build's gate reads (ADR 0046 S16).
		base_files_ = list_base_file_names(base, document_);
		listed_ = true;
		return false;
	}
	return refresh_.step(budget.bytes);
}

OperationProgress OpenOperation::progress() const {
	if (!listed_) return {0, 0, OperationUnit::Files, "Reading the game install's file names"};
	return {refresh_.files_done(), refresh_.files_total(), OperationUnit::Files, refresh_.label()};
}

OperationOutcome OpenOperation::finish(SessionCore &core) {
	return core.absorb_open(*this);
}

} // namespace opennova::editor
