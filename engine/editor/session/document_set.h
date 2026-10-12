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
#include <editor/session/view/documents_view.h>

namespace opennova::editor {

class MissionItemCache;
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
	~DocumentSet();
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
	// documents_dirty(), or a document whose companion holds what it does not (DocumentBase::companion_unsaved).
	bool documents_unsaved() const;
	// The paths of the open documents with unsaved edits, in the order they were opened.
	// `companions`: a document whose companion holds what it does not (DocumentBase::companion_unsaved) too.
	std::vector<std::string> dirty_files(bool companions) const;
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
	// The open documents in their order, each with the locator of the record selected in it ("" none): the
	// active one's the view's selection, every other's the one it kept (the UX round's project lane: what a
	// project reopens with).
	std::vector<std::pair<std::string, std::string>> open_with_selection() const;

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
	// TextureOperation (ADR 0046 S18): the texture document at path (opened first when open_first says so)
	// made anew by the operation over the bytes it holds (documents/texture_operations.h), applied as one
	// step whose words are the operation's. Refused, nothing changed (texture.operation): a file an import
	// makes (its import's options make it), an operation no row names, one the file cannot take.
	void texture_operation(const EditorRequest &request);
	// WaveOperation (round S23 lane A): the wave document at path (opened first when open_first says so) made anew by
	// the operation over the bytes it holds (documents/wave_document.h), applied as one step whose words are the
	// operation's. Refused, nothing changed (wave.operation): a file an import makes, an operation it does not take,
	// one the wave cannot take.
	void wave_operation(const EditorRequest &request);
	void revert_to_saved(const EditorRequest &request);
	// The text of the string a field's string id names (SetStringText, the plain-words lane): the table
	// that defines the id as the game's lookup reaches it (the graph's), opened in the background where it
	// is not, its string's text set there as one undo step of that table.
	void set_string_text(const EditorRequest &request);
	// Copy and Cut.
	void copy(const EditorRequest &request);
	void paste(const EditorRequest &request);
	void duplicate(const EditorRequest &request);
	// Undo and Redo (S25: across documents, document_set_scope.cpp): in the view of the document at `path` ("" the
	// active one), the step made last among it and the files its terrain tools edited (the highest serial, StepMark),
	// and with it the top step of every other document that carries the same link (a stroke, its bake landing and the
	// entities that followed it: one Ctrl+Z); a linked document edited since keeps its later edit and is named, a
	// closed one is skipped and named. Redo the same, the lowest serial first. Never refused for what it leaves.
	void undo_redo(const EditorRequest &request);
	// The coalesced group, or the gesture, of the document at `path` ends ("" the active one); `gesture` (S25): every
	// open gesture carrying that token ends too, in whatever document its batches changed; `cancel`: it ends dropping
	// its step in each (DocumentBase::cancel_gesture), the documents as it found them.
	void end_edit(const std::string &path, uint64_t gesture = 0, bool cancel = false);
	// Save: `over` writes over a file changed outside the editor (ADR 0046 DI-01: document.conflict's Keep my
	// edits), what the other program saved there lost. S25: the document's scope with it (the terrain files its view
	// edited and the documents a step of theirs links, the missions that followed a stroke: save_scope), one status
	// line naming them.
	void save(const std::string &path, bool over = false);
	void save_all();

	// Writes each open document at `paths` that has unsaved edits (`rewrite`: an explicit Save,
	// which also writes one with none whose file holds other bytes than it would write), past a
	// failure, then one refresh; false when one could not be written. `over`: as save's.
	bool save_documents(const std::vector<std::string> &paths, bool rewrite, bool over = false, bool name_files = false);
	// What a document's last save said beyond writing, an Output line each; how many lines.
	size_t note_save(const DocumentBase &document);
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

	// --- S25: the terrain tools' documents (document_set_scope.cpp) -------------------------------------------

	// The document at `path` opened for an edit another view makes (a terrain stroke's file, a mission that follows the
	// ground), the active document kept: a tab with its dirty dot, a status line and an Output line naming `why`. The
	// open document (one open already as it is), null where it does not open (the refusal reported).
	DocumentBase *open_quietly(const std::string &path, const std::string &why);
	// The document at `edited` was edited from the view of the document at `view` (a terrain stroke, a tile tool): it
	// joins the view's undo, save and close scope.
	void note_edited_from(const std::string &view, const std::string &edited);
	// The open documents of a view's scope: the view's own first, then those edited from it, in the order noted.
	std::vector<std::string> scope_of(const std::string &view) const;
	// What a Save in a view writes: its document, the documents of its scope with unsaved edits, and those a gesture
	// of theirs changed with unsaved edits too (the missions that followed a stroke).
	std::vector<std::string> save_scope(const std::string &view) const;
	// What a Close of the document at `path` lists beside it: the documents of its scope with unsaved edits.
	std::vector<std::string> scope_unsaved(const std::string &path) const;
	// A batch the terrain tools planned applied to `document` as edit_record applies one (refused as it refuses one:
	// an import's output, a blocked document), nothing selected, the document noted as edited from `view`; `words`
	// name its step (what Undo says). False when refused.
	bool apply_terrain_edits(DocumentBase &document, const std::vector<Edit> &edits, const std::string &view,
	                         const std::string &words);
	// A heights bake's landing (session/terrain_sculpt, S25): its edit applied to the heights document as part of the
	// stroke's step it bakes (it amends that step), the view updated; false where the document refuses it (its step gone).
	bool apply_bake_landing(DocumentBase &document, const std::vector<Edit> &edits);
	// The documents the batches of the gesture `gesture` changed, in the order they did (S25: a link's), open as the
	// instances it changed (`gone`: those closed or read again since, which hold none of its steps).
	std::vector<std::string> linked(uint64_t gesture, std::vector<std::string> *gone = nullptr) const;

	// The one position rule (S13 A2): where a record placed right after `record` goes, as Paste
	// after the selection and Duplicate place it. A nested record: its owner's collection
	// (`parent` the owner's identity, 0 when the row holds it) at the index after it; a row: the
	// rows at the index after it (`parent` 0). False for a record the document does not hold.
	static bool position_after(const Document &document, const NodeAddress &record, NodeId &parent, size_t &position);

private:
	// The name a file create_file made defines (its request's define, DI-33), added to its open document as
	// its type's Add makes one (an edit_record of the type's batch, opened first, selecting it); false, said
	// in Problems and on the status line, where the type defines no name of the kind there.
	bool define_in_made(const std::string &name, const ReferenceSubject &define);
	// `select_made` false (edit_record's keep_selection, S24): what the batch adds is not selected nor its document
	// made the active one; the selection is repaired as for any other edit.
	bool apply_edits(DocumentBase &document, const std::vector<Edit> &edits, bool select_made = true);
	// The mission's own text table (<mission>.bin) made from the mission text blank where the project has none
	// (S24, set_string_text's create; as a new mission's create_file makes it): its project path, "" with the
	// refusal reported.
	std::string make_mission_text_table(const std::string &mission);
	// set_string_text's create (S24): the next free key of the field's form in the mission's own table, its text,
	// and the field set to its number.
	void create_string(const EditorRequest &request, const Document &document, const FieldUse &use, const std::string &text);
	// The status line's words for a batch (ADR 0046 S15): its records by their titles, a field set by
	// its name (on `named`, its records as they read before) and the new value's words; "" where a
	// batch is none it words.
	std::string records_words(const Document &document, const std::vector<Edit> &edits) const;
	std::string edit_words(const Document &document, const std::vector<Edit> &edits, const std::string &named) const;
	// The step the last edit of `document` made named with the status line's words (what Undo says).
	void name_step(DocumentBase &document) const;
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
	// A request's path that names no file of the project, as its refusal (SessionCore::refuse_request).
	Diagnostic missing_file(const std::string &path) const;
	// The open document at exactly `path` (activate's), or null.
	const DocumentBase *open_at(const std::string &path) const;
	// `before` read again as `after` (reload_changed, a Reload): the records selected in it, the active
	// selection or the one it kept while another is active, selected again in `after` by their places
	// (Document::locator, as a project reopens them: ADR 0046 DI-01); those it no longer has drop out.
	void keep_selection(const DocumentBase &before, const DocumentBase &after);
	// The gesture open in the document at `path` ends (none open there: nothing), and with
	// `validate` the validation its edits left waiting is due: an EndEdit, an Undo or a Redo of the
	// document, a Save of it, the document closed, discarded or read again.
	void end_gesture_in(const std::string &path, bool validate);
	// Every open gesture ends (every edit group ended, the project closed).
	void end_gestures(bool validate);
	// S25 (document_set_scope.cpp): what Undo or Redo in the view of `view` takes next; the view's undo_next and
	// redo_next made again for the active document; a document closed or discarded let go of the scopes (the clean
	// files opened quietly for its view closed with it unless another view's scope holds them) and of the links.
	DocumentsView::StepNext next_step(const std::string &view, bool undo) const;
	void update_step_next();
	void release_scope(const std::string &path);

	// A mission's records of an item take its catalog row (D-MIS-10): the AI class a save writes handed to the
	// document, before an edit and a save (MissionDocument::set_item_classes); the rows read through `rows_`.
	void hand_item_classes(DocumentBase &document);
	// A document's companion (DocumentType::companions, S24 Names: a mission's .mis): where it is (beside the
	// document's file, in its folder: the project's file there of the document's base name and its extension, in
	// any case, else that name), "" for a type with none; read and handed to the document before it reads its file
	// (read_companion: none there, its bytes, or the file system's refusal); written after its save where the
	// document says to, `written` taking its path (write_companion: never failing the document's save).
	std::string companion_path(AssetKind kind, const std::string &relative) const;
	void read_companion(DocumentBase &document, AssetKind kind, const std::string &relative) const;
	void write_companion(DocumentBase &document, std::vector<std::string> &written);
	// A mission's base heights for its .mis, handed to the document before the write (MissionDocument::
	// set_base_heights): each entity's, in the writer's order, the terrain's height under it as the original editor
	// samples it (mission::mis_terrain_height) over the terrain as it reads it (the .trn alone, the .cpt it names:
	// base_terrain_, kept for the last mission's terrain), 0 where no sector lies under it or no terrain reads.
	void hand_base_heights(DocumentBase &document);

	SessionCore &core_;
	SessionView &view_;
	const ProjectPaths &paths_;
	std::unique_ptr<MissionItemCache> rows_;
	struct BaseTerrain;
	std::unique_ptr<BaseTerrain> base_terrain_;
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
	// S25: the documents each view's terrain tools edited (by the view document's path), the documents each gesture's
	// batches changed (by its token: its link), and the documents opened quietly for a view and not shown since.
	std::map<std::string, std::vector<std::string>> edited_from_;
	// A link's documents keep the instance it changed: one closed or read again since holds none of its steps.
	struct LinkedDocument {
		std::string path;
		uint64_t identity = 0;
	};
	std::map<uint64_t, std::vector<LinkedDocument>> links_;
	std::set<std::string> quiet_;
};

} // namespace opennova::editor
