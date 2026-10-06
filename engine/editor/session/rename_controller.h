#pragma once

#include <cstdint>
#include <map>
#include <optional>
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
	// MoveAsset (DI-03): the file put in the project's `folder` under its own name, planned as a rename
	// that keeps the name (graph/rename_transaction.h plan_move: no site rewritten), committed as a
	// rename is; its way back the rename's (Edit > Move back).
	void move_asset(const std::string &file, const std::string &folder);
	void assign_requirement(const std::string &role, const std::string &file);
	// PreviewRename: what a rename would do, planned into the view (nothing written, nothing
	// reported): a name's rename everywhere, or a file's (no field).
	void preview(const EditorRequest &request);
	void rename_symbol(const EditorRequest &request);
	// SplitTexture (S18): a texture copied, the uses in the files named moved to the copy
	// (graph/rename_transaction.h plan_split), committed as a rename is; not undoable, and no way back.
	void split_texture(const EditorRequest &request);
	// A rename committed (RenameOperation's finish): the files it touched read again (the scan
	// updated, or an import source's refresh taken), the open documents it rewrote read again, the
	// one the modder was in active again, its findings and what it did said.
	OperationOutcome absorb_rename(RenameOperation &operation);

	// Rename back (the UX round's problems lane): the last rename that finished taken back, its true
	// inverse. Its name, or its file, is found by itself, never by a place an edit may have moved since: the
	// definition the rename gave the new name, in the file and the field that define it (a file's: the
	// renamed file at its new path), offered while it is there; renamed back to the old name at only the
	// sites the rename rewrote, as it left them (each by its file, its record's place and field, a text's
	// line and column), the files it wrote held as it left them (a file changed since cannot say which of
	// its uses the rename wrote, and refuses): a use that named the new name before the rename is left. A
	// PreviewRenameBack plans it into the view's rename_preview (`back`), nothing written, its ask opening
	// the Rename back dialog; a RenameBack commits it as a rename (RenameOperation), which is then the last.
	void preview_back(const EditorRequest &request);
	void rename_back();
	// The project closed: its last rename's way back goes with it.
	void forget() {
		done_.reset();
		backing_ = false;
	}

	// The documents with unsaved edits a RenameAsset, a MoveAsset, an AssignRequirement, a RenameSymbol or
	// a RenameBack would rewrite or leave behind (the unsaved guard's).
	void unsaved_files(const EditorRequest &request, std::vector<std::string> &files);

private:
	// A name's rename everywhere planned from a PreviewRename's or a RenameSymbol's fields (the
	// definition found by its file, locator and field in the graph, current first).
	SymbolRenamePlan plan_symbol(const EditorRequest &request);
	bool saved_file_uses(const std::string &path, const SymbolRenamePlan &plan) const;
	void rename_unsaved(const std::string &file, const std::string &new_name, std::vector<std::string> &files);
	bool unsaved_while_due(std::vector<std::string> &files);
	// The last rename's way back, planned over the graph and the files as they stand (rename_back).
	SymbolRenamePlan plan_back_symbol();
	RenamePlan plan_back_file();
	// Of a plan's sites, those the last rename wrote, as it left them; why the others it wrote are not
	// among them (a file changed since, a use gone) into `refusals`.
	void keep_written(std::vector<RenameSite> &sites, std::vector<Diagnostic> &refusals) const;
	// A project file's bytes as they are on disk, hashed (0 for a file that cannot be read).
	uint64_t hash_of(const std::string &file) const;

	// The last rename that finished, as its way back needs it: a name's (its kind, the file and the field
	// defining it, its scope) or a file's (its new path), the names before and after, the sites it wrote as
	// it left them (a text's columns as the new name's length moved them), and each file it wrote by its
	// bytes' hash after the commit.
	// A move's (`move`): `file` its new path, `from` and `to` the folders it left and went to.
	struct Done {
		bool symbol = false;
		bool move = false;
		ReferenceKind kind = ReferenceKind::None;
		std::string file, field, scope, from, to;
		std::vector<RenameSite> sites;
		std::map<std::string, uint64_t> written;
	};
	std::optional<Done> done_;
	bool backing_ = false; // the rename under way is a way back

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
};

// Whether Edit offers the last rename's way back (ActivityView::last_rename): its name still defined where the
// rename put it (its kind, name, file and field, in the graph), its file still at its new path.
bool rename_back_offered(const SessionView &view);

} // namespace opennova::editor
