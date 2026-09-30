#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class SessionCore;
struct ImportPlan;
struct ProjectPaths;
struct SessionView;

// Imports in the project session (ADR 0046 S8, S11g, S13 A2): the import dialog's preview (the
// files to choose from, the files chosen, the plan of importing them with the files they need,
// planned again whenever it is asked for and before it is written), the editor's "Include the
// files these need", the import written (import_assets) and the refresh after it, a source
// imported again, and the game install's file names the Import fixes read. The unsaved guard asks
// it which documents with unsaved edits an import would write over.
class ImportController {
public:
	explicit ImportController(SessionCore &core);
	ImportController(const ImportController &) = delete;
	ImportController &operator=(const ImportController &) = delete;

	// PreviewImport: the picked files, a loose one chosen, an archive's members listed.
	void preview_files(const EditorRequest &request);
	// PlanImport: the files chosen, planned again, the list kept.
	void plan(const EditorRequest &request);
	// PreviewRetailImport: the game install's files, those named alone and chosen, or every one
	// listed to choose from.
	void preview_retail(const EditorRequest &request);
	void set_dependencies(bool flag);
	void cancel();
	void import_files(const EditorRequest &request);
	void reimport(const std::string &source, bool force);
	// The game install's file names, for the Import fixes (problem_fixes.h).
	void refresh_retail_files();
	// The documents with unsaved edits an ImportFiles would write over (the unsaved guard's).
	void unsaved_files(const EditorRequest &request, std::vector<std::string> &files);
	// The project closes: the dialog, the imported sources and the install's names go.
	void clear();

private:
	void preview(std::vector<ImportSource> choices, std::vector<ImportSource> roots, bool with_dependencies);
	// The open preview planned again; `shown`, the plan an Import was shown, says if it changed.
	void plan_preview(const ImportPlan *shown = nullptr);

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
};

} // namespace opennova::editor
