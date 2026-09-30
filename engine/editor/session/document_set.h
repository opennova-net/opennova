#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/edit.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class SessionCore;
struct ProjectPaths;
struct SessionView;

// The project session's open documents (ADR 0046 S13 A2): which are open and which is active, the
// selection each keeps while another is active, the ones whose file changed outside the editor
// (stale, or in conflict with their unsaved edits), reading them, reading them again and closing
// them, saving them and rewriting a file that is not open, the clipboard, the edits (a Set, an
// Add, a batch on one row, a Revert to saved), Copy, Cut, Paste and Duplicate by one position
// rule (position_after), and the edit groups (a coalesced burst of typing, a gesture) with the
// validation a gesture's edits wait on. It keeps the view's documents, active document, selection
// and clipboard, and moves DocumentSet when which documents are open, or which of them have unsaved
// edits, changes (S13 D1).
class DocumentSet {
public:
	explicit DocumentSet(SessionCore &core);
	DocumentSet(const DocumentSet &) = delete;
	DocumentSet &operator=(const DocumentSet &) = delete;

	// --- what is open --------------------------------------------------------------------------

	// The open document at `path` (project-relative, or a logical name), "" the active one; null
	// when none is open. records_for: the record document it is (as_records), null too when the
	// one open is of another kind: what a request that reads or edits records acts on.
	DocumentBase *document_for(const std::string &path = {});
	Document *records_for(const std::string &path = {});
	const std::vector<std::shared_ptr<DocumentBase>> &documents() const { return documents_; }
	bool documents_dirty() const;
	// The paths of the open documents with unsaved edits, in the order they were opened.
	std::vector<std::string> dirty_files() const;
	// True when the last EditRecord, Copy, Cut, Paste or Duplicate went through (a Move that left a
	// record where it is included: it changed nothing, and nothing was wrong).
	bool last_edit_ok() const { return last_edit_ok_; }

	// The view's documents again, the project's files following them (the open documents stand in
	// for theirs), DocumentSet moving when what is open, or what is unsaved, changed.
	void update_view();
	// The one place the active document changes: the one it replaces keeps its selection for when
	// it is active again; `path` takes back the one it kept, repaired against its records as they
	// are now. A caller with a record to show selects it after.
	void activate(const std::string &path);
	// A menu made the active document, or read again, with nothing selected shows its first screen.
	void select_first_screen();

	// --- the files ---------------------------------------------------------------------------

	// A document of its kind's type read from the project's file; null, with `error`, when the file
	// does not load.
	std::shared_ptr<DocumentBase> load(const std::string &relative, AssetKind kind, Diagnostic &error) const;
	// The open documents against their files (Rescan, an import, Build and Play): a clean one
	// whose file changed is read again, one whose file no longer reads stays open as it was
	// (document.stale), one with unsaved edits whose file changed keeps them (document.conflict).
	void reload_changed();
	// The Problems rows of the open documents whose file changed outside the editor and was not
	// read again.
	std::vector<Diagnostic> findings() const;
	// The document at `path` was read again, saved or closed: what its file did outside the editor
	// is no longer a row.
	void forget_file_state(const std::string &path);

	// --- the requests ------------------------------------------------------------------------

	void create_file(const EditorRequest &request);
	// OpenDocument and ReloadDocument.
	void open_document(const EditorRequest &request);
	void show_in_files(const EditorRequest &request);
	void close_document(const std::string &path);
	void select_record(const EditorRequest &request);
	void edit_record(const EditorRequest &request);
	void revert_to_saved(const EditorRequest &request);
	// Copy and Cut.
	void copy(const EditorRequest &request);
	void paste(const EditorRequest &request);
	void duplicate(const EditorRequest &request);
	// Undo and Redo.
	void undo_redo(const EditorRequest &request);
	void end_edit(const std::string &path);
	void save(const std::string &path);
	void save_all();

	// Writes each open document at `paths` that has unsaved edits (`rewrite`: an explicit Save,
	// which also writes one with none whose file holds other bytes than it would write), past a
	// failure, then one refresh; false when one could not be written.
	bool save_documents(const std::vector<std::string> &paths, bool rewrite);
	// EndEdit on every open document: the coalesced groups and the gestures end, and the validation
	// a gesture's edits left waiting is due.
	void end_edit_groups();

	// The unsaved-changes prompt's Discard: the document at `path` dropped unsaved (a Close or a
	// Reload waited on it), or every document (a project switch, Quit).
	void discard(const std::string &path);
	void discard_all();
	// The project closes: every document goes, with the selections they kept and their files' states.
	void close_all();

	// The one position rule (S13 A2): where a record placed right after `record` goes, as Paste
	// after the selection and Duplicate place it. A nested record: its owner's collection
	// (`parent` the owner's identity, 0 when the row holds it) at the index after it; a row: the
	// rows at the index after it (`parent` 0). False for a record the document does not hold.
	static bool position_after(const Document &document, const NodeAddress &record, NodeId &parent, size_t &position);

private:
	// The selection each open document had when another became active (activate), by path.
	struct Selection {
		NodeAddress primary;
		std::vector<NodeAddress> selected;
	};

	bool apply_edits(DocumentBase &document, const std::vector<Edit> &edits);
	// A request that acts on records with none to act on: refused as the file not open
	// (document.not_open, in the words `not_open`), or as the document open there holding no
	// records (document.no_records, S13 D6); nothing when no project is open.
	void refuse_records(const std::string &path, const char *not_open);
	void copy_records(Document &document, bool cut);
	void paste_records(Document &document, const PasteAt &target);
	void duplicate_records(Document &document);
	void rewrite_file(const std::string &path);
	// The open document at exactly `path` (activate's), or null.
	const DocumentBase *open_at(const std::string &path) const;

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
	std::vector<std::shared_ptr<DocumentBase>> documents_;
	// Each open document's instance and whether it has unsaved edits, as the view last listed
	// them: DocumentSet moves when they differ (update_view).
	std::vector<std::pair<uint64_t, bool>> document_set_;
	std::map<std::string, Selection> remembered_;
	// The open documents whose file changed outside the editor and was not read again, by
	// path: a clean one whose file no longer reads (the reason; it stays open as it was), and
	// one with unsaved edits (reload_changed, or a Save refused as a conflict).
	// Each is a Problems row until the file is read again, saved over or matches again, or
	// the document closes.
	std::map<std::string, Diagnostic> stale_;
	std::set<std::string> conflicts_;
	bool gesture_validation_due_ = false; // a gesture's edits wait for it to end
	bool last_edit_ok_ = false;
};

} // namespace opennova::editor
