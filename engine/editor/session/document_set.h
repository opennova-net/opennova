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
#include <editor/session/selection.h>

namespace opennova::editor {

class SessionCore;
struct ProjectPaths;
struct SessionView;

// The project session's open documents (ADR 0046 S13 A2): which are open and which is active, the
// selection each keeps while another is active, the ones whose file changed outside the editor
// (stale, or in conflict with their unsaved edits), reading them, reading them again and closing
// them, saving them and rewriting a file that is not open, the clipboard, the edits (a Set, an
// Add, a batch over any rows, a Revert to saved), Copy, Cut, Paste and Duplicate by one position
// rule (position_after), and the edit groups (a coalesced burst of typing, a gesture) with the
// validation a gesture's edits wait on. It keeps the view's documents, active document, selection,
// clipboard and open gesture (S13 V7), and moves DocumentSet when which documents are open, or which
// of them have unsaved edits, changes (S13 D1).
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
	// SelectFile (ADR 0046 S18): the project file at `path` selected in Files ("" none), leading the
	// Preview window where a viewport draws it whether or not it is open (a texture).
	void select_file(const std::string &path);
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
	// True while a gesture's edits wait for it to end (their validation with them: the view's
	// documents.gestures, S13 V7): the poll steps no validation meanwhile.
	bool gesture_open() const;
	// A batch of the gesture `token` changed the document at `path`, or a sample of a drag of the
	// wire's came for it at `sampled_ms` (SessionCore; before any batch of it changes the document
	// too): the document's open gesture, its last sample's time kept (another gesture open in the
	// document ends first; one in another document stays open).
	void open_gesture(const std::string &path, uint64_t token, int64_t sampled_ms = -1);

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
	bool apply_edits(DocumentBase &document, const std::vector<Edit> &edits);
	// The status line's words for a batch (ADR 0046 S15): its records by their titles, a field set by
	// its name (on `named`, its records as they read before) and the new value's words; "" where a
	// batch is none it words.
	std::string records_words(const Document &document, const std::vector<Edit> &edits) const;
	std::string edit_words(const Document &document, const std::vector<Edit> &edits, const std::string &named) const;
	// After `document` moved from the state (load_generation, revision) by an edit, an undo or a
	// redo: the active selection repaired against the rows that changed since (changes_since; all
	// of them when the document cannot say), a gone primary giving way to `owner`.
	void repair_selection(const DocumentBase &document, uint64_t load_generation, uint64_t revision,
	                      const NodeAddress &owner);
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
	// The gesture open in the document at `path` ends (none open there: nothing), and with
	// `validate` the validation its edits left waiting is due: an EndEdit, an Undo or a Redo of the
	// document, a Save of it, the document closed, discarded or read again.
	void end_gesture_in(const std::string &path, bool validate);
	// Every open gesture ends (every edit group ended, the project closed).
	void end_gestures(bool validate);

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
	std::vector<std::shared_ptr<DocumentBase>> documents_;
	// Each open document's instance and whether it has unsaved edits, as the view last listed
	// them: DocumentSet moves when they differ (update_view).
	std::vector<std::pair<uint64_t, bool>> document_set_;
	// The selection each open document had when another became active (activate), by path.
	std::map<std::string, Selection> remembered_;
	// The open documents whose file changed outside the editor and was not read again, by
	// path: a clean one whose file no longer reads (the reason; it stays open as it was), and
	// one with unsaved edits (reload_changed, or a Save refused as a conflict).
	// Each is a Problems row until the file is read again, saved over or matches again, or
	// the document closes.
	std::map<std::string, Diagnostic> stale_;
	std::set<std::string> conflicts_;
	bool last_edit_ok_ = false;
};

} // namespace opennova::editor
