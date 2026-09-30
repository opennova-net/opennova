#pragma once

#include <string>
#include <vector>

#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_refresh.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

// Opening a project as the session's operation (ADR 0046 S13 A3): the game install's file names the
// Import fixes read (one step: the install's archives listed), then the project's files read as
// every refresh reads them (ProjectRefresh: the import pass, the scan, the requirements), each
// walk stepped by bytes within the poll's budget. The project it opens is not the open one until
// it finishes: nothing of it reaches the view before, so a cancel between two steps leaves the
// session with no project open and nothing of this one in the view (the import pass may have
// imported sources on disk, as a refresh stopped there would have). finish() makes it the open
// project (SessionCore::absorb_open), whose validation the poll then steps.
class OpenOperation : public SessionOperation {
public:
	OpenOperation(ProjectPaths paths, LocalSettings local, ProjectDocument document);

	OperationKind kind() const override { return OperationKind::Open; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	void cancel() override {}
	OperationOutcome finish(SessionCore &core) override;

	const ProjectPaths &paths() const { return paths_; }
	const LocalSettings &local() const { return local_; }
	const ProjectDocument &document() const { return document_; }
	std::vector<std::string> &install_files() { return install_files_; }
	ProjectRefresh &refresh() { return refresh_; }

private:
	ProjectPaths paths_;
	LocalSettings local_;
	ProjectDocument document_;
	std::vector<std::string> install_files_;
	bool listed_ = false; // the game install's names read
	ProjectRefresh refresh_;
};

} // namespace opennova::editor
