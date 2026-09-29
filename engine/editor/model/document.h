#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <editor/model/edit.h>
#include <editor/model/edit_history.h>
#include <editor/model/node.h>
#include <editor/model/value.h>

namespace opennova::editor {

struct GraphSymbol;
struct ReferenceChoice;
struct SessionView;

// Where "Go to" goes (Document::reference_targets, graph/usage_target): a project file, and
// the record there that defines the name or makes the use (its locator and the field to
// show), or the file itself.
struct ReferenceTarget {
	std::string label;     // what a choice among several says ("style variable X in menu_style.mns")
	std::string file;      // project-relative
	std::string locator;   // the record there (Document::locator); "" = the file itself
	std::string field;     // the field to show there
	bool editable = false; // the editor opens the file's kind; else Files shows the file
};

// An editable file (ADR 0046 d9): the lifecycle every document type shares (load
// and decode, the change fingerprint, undo/redo, the save checkpoint, the conflict
// check, the atomic write) over rows a document type parses from and serializes to
// its native format. The type supplies the record-level hooks below; the session,
// the windows and the shell use only this interface.
//
// Records nest at any depth (ADR 0046 S9g): a row owns collections, and so may each
// record in them; the type declares, per owner, the collections it holds
// (`collections(row, owner)`), and the base derives the walk, a record's placement
// (its owner and its index there), its ancestors, its name, its path and a locator a
// reload finds it again by.
//
// The document keeps the rows and the file-wide state as the last successful load or
// save left them (the saved baseline; committed rows are immutable and keep their
// identities across edits, so it costs a list of pointers), and answers what changed
// since: a field, a record, the file-wide state, what a field held, and the edits that
// give a field back.
class Document {
public:
	Document();
	virtual ~Document() = default;

	// The file at `absolute_path` read, then load_bytes: the document keeps the path and what
	// the file held, which Save writes over and checks against.
	bool load(const std::string &absolute_path, const std::string &relative_path, AssetKind kind,
	          const std::string &game, Diagnostic &error);
	// A file's bytes as stored (the game's loader decodes them), with the name it goes by in
	// the project: what an import reads before anything is written (the import plan follows
	// its references). A document loaded this way has no file: Save refuses it
	// (document.no_file).
	bool load_bytes(const std::vector<uint8_t> &bytes, const std::string &relative_path, AssetKind kind,
	                const std::string &game, Diagnostic &error);
	// Writes serialize()'s text over the file. The file's source findings are then those of
	// the text written (the input the rewrite dropped is no longer reported); the rows, their
	// identities and the history stay, and the saved baseline moves to them.
	bool save(Diagnostic &error);
	// What an explicit Save does with this document when it has no unsaved edits: nothing when
	// it serializes to the bytes the file held when it was loaded or last saved; a write when it
	// serializes to other bytes (the canonical rewrite: the lines the game ignores dropped, the
	// line ends fixed, a table regrouped); a refusal when it does not serialize (Save says why,
	// document.unserializable), never "no changes".
	enum class RewriteNeed { None, Rewrite, Unserializable };
	RewriteNeed rewrite_need() const;
	// True while the file holds the bytes this document was loaded from or last saved (the
	// conflict check Save makes); false once it changed outside the editor or is gone.
	bool matches_file() const;
	// True once Save wrote the file since the load: the file then holds bytes serialize()
	// made, not the bytes the document was read from (a type whose writer normalizes what
	// it read tells the two apart by it).
	bool wrote_file() const { return wrote_file_; }
	// One undoable change. Every address is checked against the document first: a stale
	// identity, or one that names another kind than the address says, is refused
	// (document.selection). A Move that leaves the record where it is, a Clear of a field
	// already left out, a Write of one already written and a Set of the value a field holds
	// (the field as the record reads it, and whether it is written, the same after the Set)
	// succeed with no history step (the document stays clean). A coalesced Set applies to the
	// record as its open group found it (Edit::coalesce), and one that gives the group's
	// fields back the values they held leaves no step; edits sharing a gesture fold into one
	// step (Edit::gesture). The type may refuse any change before it commits
	// (accept_change).
	bool apply(const Edit &edit, Diagnostic &error);
	// A batch: every edit changes the same row (Sets, Clears, Writes, and Adds, Duplicates,
	// Removes, Moves and Pastes inside it), cloned once and committed as one Change, one
	// undo step; nothing is committed when any edit is refused. A later edit may name a
	// record an earlier one made (batch_made). A batch whose edits all
	// coalesce folds with the next batch of the same fields (a drag without a gesture).
	// Adding, removing or moving rows, and a file-wide value, go one edit at a time.
	bool apply(const std::vector<Edit> &edits, Diagnostic &error);
	// The edits that follow from a batch (a record's new name followed into the references
	// of its file, graph/rename_transaction's plan_symbol_rename), planned over the
	// document as the batch finds it.
	using FollowEdits = std::function<std::vector<Edit>(const Document &document, const std::vector<Edit> &edits)>;
	// A batch and what follows from it as one undo step, undone and redone together: an
	// edit that follows on the batch's own row joins its batch, the others go in one batch
	// per row (Sets, Clears, Writes; one the batch makes itself is dropped). A coalesced batch
	// continuing its open group (typing) starts again from what the group found, the step
	// before undone first, so what follows is planned from the group's first state and a
	// value typed on the way (an empty name, a name another record has) leaves nothing
	// behind; the whole group stays one step. Nothing is committed when any batch is
	// refused or the type vetoes any change (a reopened group is put back as it was).
	bool apply(const std::vector<Edit> &edits, const FollowEdits &follow, Diagnostic &error);
	void undo();
	void redo();
	void end_edit_group() { history_.end_edit_group(); }

	const std::string &path() const { return relative_path_; }
	AssetKind kind() const { return kind_; }
	bool dirty() const { return history_.dirty(); }
	bool can_undo() const { return history_.can_undo(); }
	bool can_redo() const { return history_.can_redo(); }
	uint64_t revision() const { return history_.revision(); }
	// This instance, distinct from every other document ever made in the process: a
	// reload is a new document even at the same revision (the graph caches by it).
	uint64_t identity() const { return identity_; }
	// True while the source holds input the typed model cannot carry: editing and
	// saving wait for the source to be corrected and reloaded. Lines the game ignores
	// are only reported (issues) and are dropped on save.
	bool blocked() const { return blocked_; }
	size_t ignored_lines() const;
	const std::vector<SourceIssue> &issues() const { return issues_; }
	const std::vector<std::shared_ptr<const Node>> &rows() const { return rows_; }
	const FileState *file_state() const { return file_state_.get(); }
	// The record the last Add, Duplicate or Paste made (a Paste's first record), and
	// every record it made (a Paste's records, not their descendants).
	NodeId last_added() const { return last_added_; }
	const std::vector<NodeId> &last_added_records() const { return added_; }
	// The address of a row or nested record by identity (kind 0 / row 0 when unknown).
	NodeAddress address_of(NodeId id) const;
	const Node *row(NodeId id) const;

	// --- what a document type declares about its records -------------------------
	// A collection an owner holds: its records are of one kind, in order.
	struct CollectionSpec {
		NodeKind kind = 0;             // the records' kind
		const char *label = "";        // "Actions"
		const char *name_field = "";   // the field that names a record ("" = none)
		const char *kind_name = "";    // the kind's token (kind_from_name, add_record, the MCP, locators)
		// No Add, Duplicate, Remove or Move: the core's own facility (no document type sets it
		// today; the core's tests do).
		bool fixed = false;
		Applicability applies = Applicability::Reads; // does the game read this collection here
		size_t max = 0; // the most records it holds (0 = any number; a menu window's part: 1)
	};
	struct Collection {
		CollectionSpec spec;
		std::vector<NodeId> ids; // index = position
	};
	// Where a nested record sits: its owner (owner.child == 0: the row itself), the
	// collection and its index there.
	struct Placement {
		NodeAddress owner;
		CollectionSpec spec;
		size_t index = 0;
	};
	// A record met by a walk, with its placement. Returning false stops the walk.
	using RecordVisitor = std::function<bool(const NodeAddress &record, const Placement &at)>;
	struct KindSpec {
		NodeKind kind = 0;
		const char *label = ""; // "Add record", "Add carry limit"
	};
	virtual const char *kind_label(NodeKind kind) const = 0;
	virtual NodeKind kind_from_name(const std::string &name) const = 0; // -1 when unknown
	virtual bool is_top_kind(NodeKind kind) const = 0;
	virtual std::vector<KindSpec> top_kinds() const = 0;
	// The collections `owner` holds inside `row` (owner.child == 0: the row's own), in
	// the order the file writes them; none for a record that holds nothing. `row` may be
	// an uncommitted clone (the base resolves a batch's later edits against it).
	virtual std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const = 0;
	// Every nested record of a row in pre-order (a record, then what it holds), with its
	// placement. The default recurses through collections(); a type may walk faster.
	virtual void walk_records(const Node &row, const RecordVisitor &visit) const;
	virtual const std::vector<FieldSchema> &fields(NodeKind kind) const = 0;
	// A field as it applies to one record: whether the game reads it there, the
	// reference it makes given the record's other fields, the scope it resolves in, and the
	// symbol it defines and where a lookup finds that (its scope).
	virtual FieldSchema field_on(const NodeAddress &address, const FieldSchema &field) const {
		(void)address;
		return field;
	}
	// The symbol a record's defining field makes (the graph fills its name, file, record,
	// place, field and scope), as the type's lookup rule sees it: marked inert where no
	// lookup finds this definition (a stylesheet's earlier definition of a name, a menu's
	// screen a later one shadows, a string table's later section of a name, a model's user
	// point past the scan), given the value it stands for where it stands for one (a style
	// variable's). The default: every definition is found.
	virtual void refine_symbol(const NodeAddress &address, GraphSymbol &symbol) const {
		(void)address;
		(void)symbol;
	}
	// Whether an optional field is written (`field` "" = whether the record itself is):
	// read_present over the record's row.
	bool present(const NodeAddress &address, const std::string &field) const;
	// A field of a record as it stands: read over its row. A type adds here what it derives
	// from the whole document rather than the row (a stylesheet line's number).
	virtual bool get(const NodeAddress &address, const std::string &field, Value &out) const;
	// Find a record by the symbol another document names it with (a name, an id; a style
	// variable's NAME or its %NAME%): of the symbols the records define (their defining
	// fields, as the kind compares names) that `scope` matches (scope_matches:
	// "GAMETEXT.BIN/WepDes" a key of that section, "MAIN.MNU/STARTUP" a window of that screen;
	// "" any), the first a lookup finds, a row's before a nested record's (a menu's screen before
	// a window of the name). With a scope nothing else: a definition no lookup finds there (a
	// shadowed section's key) is not found. With none, else the first defined where no lookup
	// looks, else the first record of that name (record_name, without case: an animation
	// table's slot, a clip's bone). Defined in graph/reference_resolution.cpp, over the symbols
	// the graph's extraction reads from the document, once per revision.
	bool find(const std::string &symbol, NodeAddress &out, const std::string &scope = std::string()) const;
	// The clipboard seam: the records as this type's payload text, which a Paste of the
	// same type takes back ("" = these records cannot be copied, the default).
	virtual std::string copy(const std::vector<NodeAddress> &records) const {
		(void)records;
		return std::string();
	}

	// --- derived from the declarations above ---------------------------------------
	// The collections of the record (or row) `owner` names.
	std::vector<Collection> collections_of(const NodeAddress &owner) const;
	// A nested record's placement; false for a row or a record the document does not have.
	bool placement(const NodeAddress &address, Placement &out) const;
	// The records that hold `address`, the row first and its direct owner last (none for
	// a row or an unknown record).
	std::vector<NodeAddress> ancestors(const NodeAddress &address) const;
	// Of `records`, those no other of them holds, each once, in their order: a record
	// inside another of them goes with it (a Cut removes a subtree once, a Duplicate copies
	// it once, a move of several windows moves it once).
	std::vector<NodeAddress> outermost(const std::vector<NodeAddress> &records) const;
	// The name a record shows: a row's name, or a nested record's name field, else its
	// kind and its place ("Action 2"); "" when the record is gone.
	std::string record_name(const NodeAddress &address) const;
	// The name the windows show for a record (the outline, the breadcrumb, the Inspector's
	// lists): record_name, unless the type words the token a record is named by (an
	// animation table's slot key as the slot's words: "walk forward" for anim_walk_forward),
	// the windows then putting the token in its tooltip. The graph, the Problems rows and the
	// editor MCP keep record_name.
	virtual std::string record_title(const NodeAddress &address) const { return record_name(address); }
	// The record as the graph's edges name it: every name from the row down, "/"-joined
	// ("STARTUP/MAIN/EXIT").
	std::string record_path(const NodeAddress &address) const;
	// The record's place, stable across a reload of the same file: the row's index, then
	// each collection's kind token and index ("0/window:0/window:2"); "" when unknown.
	std::string locator(const NodeAddress &address) const;
	// The record a locator names now (an empty address when it names none).
	NodeAddress address_at(const std::string &locator) const;
	// The record a source finding's locator names (issues(): the file as last loaded or saved,
	// which is the saved baseline), as it stands now by the identity it keeps: moved since, it is
	// still that record; an empty address when the baseline has none there or it is gone since.
	NodeAddress source_address(const std::string &locator) const;
	// References, answered by the project's asset graph on the view (graph/asset_graph,
	// defined in graph/reference_resolution.cpp): the badge, the picker's names and the
	// file "Go to" opens read the same tables the Problems rows come from.
	ReferenceStatus reference_status(const FieldSchema &field, const Value &value, const SessionView &view,
	                                 std::string *symbol) const;
	// What the picker offers a field (as it applies to its record): the names of its kind in its
	// scope (AssetGraph::choices), then what the value may name instead (a menu's font or texture
	// a stylesheet variable, ADR 0005; an unchecked text a string id), each with what the field
	// would reference, set to it.
	std::vector<ReferenceChoice> reference_choices(const FieldSchema &field, const SessionView &view) const;
	// The finding the graph makes of a record's field whose value resolves to nothing
	// (AssetGraph::missing_finding), for the fixes Problems offers for it: of the variable itself
	// where a %NAME% the stylesheets the game reads do not define stands for a file (the graph
	// reports the variable, never a file of that name), else of the reference, a file named
	// through a variable by the name its value gives. False when the value resolves or names
	// nothing.
	bool missing_finding(const NodeAddress &address, const FieldSchema &field, const Value &value, const SessionView &view,
	                     Diagnostic &out) const;
	std::string reference_target_file(const FieldSchema &field, const Value &value, const SessionView &view) const;
	// Where "Go to" on a reference goes, as the game's lookup reaches it: a symbol's defining
	// record (AssetGraph::resolve_symbol: a string id in its own section, a window on its own
	// screen, a record of this same file included); a file (the file its kind's loader
	// reads), after the style variable that names it where a %NAME% stands (the definition the
	// game reads, then the file its value names). None when nothing resolves.
	std::vector<ReferenceTarget> reference_targets(const FieldSchema &field, const Value &value,
	                                               const SessionView &view) const;
	virtual SerializeResult serialize() const = 0;

	// --- what changed since the last load or save (the saved baseline) -------------------
	// The baseline moves only with a load or a save: an undo back to it makes the document
	// clean again (dirty() is the history's), and these then answer unchanged.
	// A field changed when its value, or an optional field's presence, differs from the
	// baseline's; every field of a record the baseline lacks has changed, and a field `read`
	// answers on neither side (a display field derived from the whole document) never has.
	// Answered from a cache while the revision and the baseline stand, as record_change is.
	bool field_changed(const NodeAddress &address, const std::string &field) const;
	enum class RecordChange { Unchanged, Changed, Added };
	// Added: the baseline has no such record. Changed: a field of its kind changed, or a
	// collection it holds lists other records or another order (so a child removed or moved
	// marks the owner that keeps it), or, for a row, its place among the rows both sides have
	// moved. Answered from a cache while the revision and the baseline stand.
	RecordChange record_change(const NodeAddress &address) const;
	// Whether the file-wide state differs from the baseline's (same_file_state).
	bool file_state_changed() const;
	// A field as the saved file holds it: its value, and whether an optional field is written
	// there (`present`; true for any other). False when the baseline has no such record, or
	// the row alone cannot answer the field (a display field derived from the whole document).
	bool saved_value(const NodeAddress &address, const std::string &field, Value &out, bool *present = nullptr) const;
	// The ordinary edits (Set, Clear, Write) that give a field its saved value and presence
	// back, to apply as one batch (one undo step); none when the field is unchanged or
	// read-only, or its record has no saved counterpart.
	std::vector<Edit> revert_edits(const NodeAddress &address, const std::string &field) const;

	// Hands out fresh session-local identities to the records an edit creates. Public
	// because the document types' free helpers take it (GCC enforces the access).
	using IdAllocator = std::function<NodeId()>;

protected:
	// Parse decoded bytes into rows (their identity stores sized, identities left to
	// the base) and the file-wide state; report the source findings. Everything it makes
	// goes out through its arguments: Save also parses the text it wrote, for its findings.
	virtual bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	                   std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	                   Diagnostic &error) = 0;
	// A field of the record `address` names, read from `row`: the committed row (get) or
	// the saved baseline's (the queries above compare the two), so a nested record is
	// located inside `row` itself. False for a field the record does not have, and for one
	// the row alone cannot say (derived from the whole document: the type's get adds it).
	virtual bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const = 0;
	// Whether an optional field is written in `row` (`field` "" = whether the record itself
	// is). The default: always.
	virtual bool read_present(const Node &row, const NodeAddress &address, const std::string &field) const {
		(void)row; (void)address; (void)field;
		return true;
	}
	// Whether two file-wide states write the same (the baseline's and the current one). The
	// default compares the states themselves: a committed state never changes.
	virtual bool same_file_state(const FileState *a, const FileState *b) const { return a == b; }
	// A new top-level record of `kind` with the type's defaults, already given `id`.
	virtual std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, std::string &error) = 0;
	virtual bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	                       std::string &error) = 0;
	// An optional field left out of the file (Edit Clear, `present` false) or written again
	// (Edit Write, `present` true), its latent value kept either way (the base has checked
	// that the schema calls the field optional). The default refuses.
	virtual bool set_present(Node &row, const NodeAddress &address, const std::string &field, bool present,
	                         std::string &error);
	// Add / Duplicate / Remove / Move inside the row. The base has resolved and checked
	// the addresses: edit.address.row is the row, edit.parent the owner (Add, Duplicate,
	// Remove) or the destination owner (Move), 0 meaning the row itself; the collection is
	// not fixed, a Move's destination holds the kind and is not inside the record, and a
	// Move that would leave the record where it is never arrives.
	virtual bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                             std::string &error) = 0;
	// Paste (Edit Paste): the payload `copy` made (edit.value) into the owner edit.parent
	// (0 = the row) at edit.position; `added` receives the pasted records (not their
	// descendants). The default refuses.
	virtual bool paste_records(Node &row, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
	                           std::string &error);
	virtual bool set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) = 0;
	// The file-wide state after the row at `index` is removed (`remaining` rows stay).
	virtual std::shared_ptr<const FileState> state_after_remove(const std::shared_ptr<const FileState> &state,
	                                                            size_t remaining) const { (void)remaining; return state; }
	// Derived fields to refresh after any edit of a row.
	virtual void after_edit(Node &row) { (void)row; }
	// A row a Duplicate made, before it is committed beside the original (rows() still
	// holds the original): the type may tell the copy apart (a menu names the screen anew,
	// since the game finds the last of two screens of one name). The default keeps it as is.
	virtual void prepare_duplicate(Node &copy) const { (void)copy; }
	// The veto on a change before it commits: the row swap and the file-wide state on both
	// sides, for every edit, the rows added, duplicated, removed and moved included (which
	// never reach the type otherwise). A refusal commits nothing, leaves the history and
	// last_added as they were, and fails the edit with `error` (document.structure). The
	// default accepts.
	virtual bool accept_change(const Change &change, std::string &error) const {
		(void)change; (void)error;
		return true;
	}

	NodeId allocate_id() { return next_id_++; }
	const std::string &absolute_path() const { return absolute_path_; }
	const std::string &game() const { return game_; }

private:
	// Every nested record of one committed row by identity, built by one walk and kept
	// while that row is current (a committed row never changes).
	struct RowIndex {
		std::shared_ptr<const Node> row;
		std::unordered_map<NodeId, Placement> placements;
	};
	const RowIndex &row_index_of(const std::shared_ptr<const Node> &row) const;
	// A nested record's placement inside `row` by walking it (a clone a batch changes).
	bool placement_in(const Node &row, NodeId child, Placement &out) const;
	// Decoded as the game's loader decodes a stored file, then parsed (load, and the text a
	// save wrote).
	bool read_source(std::vector<uint8_t> bytes, std::vector<std::shared_ptr<Node>> &rows,
	                 std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues, Diagnostic &error);
	// The rows and the file-wide state as they stand become the saved baseline.
	void set_baseline();
	// The committed row, or the baseline's, of identity `id` (null when that side has none).
	std::shared_ptr<const Node> current_row(NodeId id) const;
	std::shared_ptr<const Node> saved_row(NodeId id) const;
	// Whether `row` holds the record `address` names, of the kind it says.
	bool holds(const std::shared_ptr<const Node> &row, const NodeAddress &address) const;
	// The record a locator names among `rows` (address_at's walk over the current rows or the
	// baseline's).
	NodeAddress address_in(const std::vector<std::shared_ptr<const Node>> &rows, const std::string &locator) const;
	const FieldSchema *field_schema(NodeKind kind, const std::string &id) const;
	// Whether one field of a record differs between its committed row `now` and the
	// baseline's row `saved` (field_changed).
	bool field_differs(const NodeAddress &address, const FieldSchema &field, const std::shared_ptr<const Node> &now,
	                   const std::shared_ptr<const Node> &saved) const;
	RecordChange compare_record(const NodeAddress &address) const;
	// Whether a row's index among the rows both sides have differs from the baseline's.
	bool row_moved(NodeId id) const;
	bool apply_row_edit(const Edit &edit, Diagnostic &error);
	// The row a batch changes (an Add's or a Paste's owner's row); refused when its edits
	// name two rows.
	bool batch_row(const std::vector<Edit> &edits, NodeId &row, Diagnostic &error) const;
	// The undo step a batch on `row` continues: edits sharing a gesture fold on the row until
	// the group ends (`builds`: each builds on the last); a batch of coalesced Sets folds with
	// the next one of the same fields; "" folds with nothing.
	static std::string step_key(const std::vector<Edit> &edits, NodeId row, bool &builds);
	// One row's batch applied to a clone of the row, nothing committed: the change, the
	// records it made, and whether any edit changed the row.
	struct RowBatch {
		Change change;
		std::vector<NodeId> added;
		bool changed = false;
	};
	bool prepare_row_batch(const std::vector<Edit> &edits, RowBatch &out, Diagnostic &error);
	// The change into the history once the type accepts it (accept_change).
	bool commit(Change change, const std::string &key, Diagnostic &error);
	// A row's index among the rows by its identity (rows().size() when it has none), through an
	// index checked on every hit and made again when stale (a row added, removed or moved since),
	// so a query over every record of a large table stays linear.
	size_t row_index(NodeId id) const;
	void assign_ids(Node &row);

	uint64_t identity_ = 0;
	std::string absolute_path_, relative_path_, game_;
	AssetKind kind_ = AssetKind::Unknown;
	std::vector<std::shared_ptr<const Node>> rows_;
	std::shared_ptr<const FileState> file_state_;
	std::vector<SourceIssue> issues_;
	bool blocked_ = false, wrote_file_ = false;
	uint64_t file_fingerprint_ = 0;
	NodeId next_id_ = 1, last_added_ = 0;
	std::vector<NodeId> added_;
	mutable std::unordered_map<const Node *, RowIndex> indexes_;
	mutable std::unordered_map<NodeId, size_t> row_positions_; // row_index's
	EditHistory history_{rows_, file_state_};
	std::vector<std::shared_ptr<const Node>> saved_rows_;
	std::shared_ptr<const FileState> saved_state_;
	// record_change's and field_changed's answers (by the record's identity, then the field's
	// id) and the rows whose place moved, for one revision of one baseline (set_baseline
	// starts it again).
	struct ChangeCache {
		uint64_t revision = UINT64_MAX;
		std::unordered_map<NodeId, RecordChange> records;
		std::unordered_map<NodeId, std::unordered_map<std::string, bool>> fields;
		bool rows_compared = false;
		std::unordered_set<NodeId> moved_rows;
	};
	// The cache, started again when the revision moved since it was filled.
	ChangeCache &changes() const;
	mutable ChangeCache changes_;
	// The symbols the records define (find's), for one revision of one load (set_baseline
	// forgets them).
	mutable std::shared_ptr<const std::vector<GraphSymbol>> defined_;
	mutable uint64_t defined_revision_ = 0;
};

} // namespace opennova::editor
