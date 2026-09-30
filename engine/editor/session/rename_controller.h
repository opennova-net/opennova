#pragma once

#include <string>
#include <vector>

#include <editor/graph/rename_transaction.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

class RenameOperation;
class SessionCore;
struct ProjectPaths;
struct SessionView;

// Renames in the project session (ADR 0046 S7, S12, S13 A2): a file renamed with every reference
// to it rewritten (graph/rename_transaction), a requirement satisfied by renaming a file of its
// kind to the name the engine demands, and a defined name renamed everywhere, previewed (every
// site before and after, the refusals) and committed. The open documents a commit rewrites are
// read again, and the one the modder was in stays active, through DocumentSet. The unsaved guard
// asks it which documents with unsaved edits a rename would rewrite or leave behind. A rename is
// planned in its request and committed as an operation (S13 A3, RenameOperation: a file at a time,
// then the one step that writes), whose finish reads again what it touched (absorb_rename).
class RenameController {
public:
	explicit RenameController(SessionCore &core);
	RenameController(const RenameController &) = delete;
	RenameController &operator=(const RenameController &) = delete;

	void rename_asset(const std::string &file, const std::string &new_name);
	void assign_requirement(const std::string &role, const std::string &file);
	// PreviewRename: what a rename would do, planned into the view (nothing written, nothing
	// reported): a name's rename everywhere, or a file's (no field).
	void preview(const EditorRequest &request);
	void rename_symbol(const EditorRequest &request);
	// A rename committed (RenameOperation's finish): the files it touched read again (the scan
	// updated, or an import source's refresh taken), the open documents it rewrote read again, the
	// one the modder was in active again, its findings and what it did said.
	OperationOutcome absorb_rename(RenameOperation &operation);

	// The documents with unsaved edits a RenameAsset, an AssignRequirement or a RenameSymbol would
	// rewrite or leave behind (the unsaved guard's).
	void unsaved_files(const EditorRequest &request, std::vector<std::string> &files);

private:
	// A name's rename everywhere planned from a PreviewRename's or a RenameSymbol's fields (the
	// definition found by its file, locator and field in the graph, current first).
	SymbolRenamePlan plan_symbol(const EditorRequest &request);
	bool saved_file_uses(const std::string &path, const SymbolRenamePlan &plan) const;
	void rename_unsaved(const std::string &file, const std::string &new_name, std::vector<std::string> &files);

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
};

} // namespace opennova::editor
