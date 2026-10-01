#include <editor/session/open_operation.h>

#include <utility>

#include <editor/assets/asset_import.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

OpenOperation::OpenOperation(ProjectPaths paths, LocalSettings local, ProjectDocument document, bool import_pass) :
		paths_(std::move(paths)),
		local_(std::move(local)),
		document_(std::move(document)),
		refresh_(paths_, document_, false, std::string(), import_pass) {}

bool OpenOperation::step(const StepBudget &budget) {
	if (!listed_) {
		// The game install's names first, the project's own install (its local.json, else the one
		// the editor last chose): the Import fixes read them.
		install_files_ = list_retail_file_names(local_.game_install, document_);
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
