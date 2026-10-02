#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class ImportOperation;
class ImportPlanOperation;
class SessionCore;
struct ImportPlan;
struct ProjectPaths;
struct SessionView;

// Imports in the project session (ADR 0046 S8, S11g, S13 A2): the import dialog's preview (the
// files to choose from, the files chosen, the plan of importing them with the files they need,
// planned again whenever it is asked for and before it is written), the editor's "Include the
// files these need", the import written (import_assets) and the refresh after it, a source
// imported again, and the game install's file names the Import fixes read. The unsaved guard asks
// it which documents with unsaved edits an import would write over. The plans, the import and a
// source imported again run as operations (S13 A3: ImportPlanOperation, ImportOperation,
// RefreshOperation), each finishing here or in the core.
class ImportController {
public:
	explicit ImportController(SessionCore &core);
	ImportController(const ImportController &) = delete;
	ImportController &operator=(const ImportController &) = delete;

	// PreviewImport: the picked files, a loose one chosen, an archive's members listed.
	void preview_files(const EditorRequest &request);
	// PlanImport: the files chosen, planned again, the list kept.
	void plan(const EditorRequest &request);
	// PreviewInstallImport: the game install's files, those named alone and chosen, or every one
	// listed to choose from.
	void preview_install(const EditorRequest &request);
	void set_dependencies(bool with_dependencies);
	void cancel();
	void import_files(const EditorRequest &request);
	void reimport(const std::string &source, bool force);
	// The game install's file names, for the Import fixes (problem_fixes.h).
	void refresh_install_files();
	// Those an Open read (OpenOperation).
	void set_install_files(std::vector<std::string> names);
	// The documents with unsaved edits an ImportFiles would write over (the unsaved guard's).
	void unsaved_files(const EditorRequest &request, std::vector<std::string> &files);
	// The project closes: the dialog, the imported sources and the install's names go.
	void clear();

	// An import's plan done (ImportPlanOperation's finish): the plan the dialog shows.
	OperationOutcome absorb_plan(ImportPlanOperation &operation);
	// An import done (ImportOperation's finish): the plan made again shown, and nothing more when it
	// was not the one shown (import.changed) or lacks a row asked for (import.not_planned); else the
	// dialog closed, the open documents whose files it replaced read again, the refresh after it
	// taken into the view, what it imported said.
	OperationOutcome absorb_import(ImportOperation &operation);

private:
	void preview(std::vector<ImportChoice> choices, std::vector<ImportChoice> roots, bool with_dependencies);
	// The open preview planned again as an operation (ImportPlanOperation): the dialog shows its
	// files at once and the plan once it is made; a plan that runs gives way to the new one (the
	// rows' Supersede).
	void start_plan();
	// A plan made for the dialog, shown: its rows, whether it is not `shown` (the plan an Import was
	// shown), the ImportPlanned event on which the dialog takes its checks again, the status line.
	void show_plan(std::shared_ptr<const ImportPlan> plan, const ImportPlan *shown);

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
};

} // namespace opennova::editor
