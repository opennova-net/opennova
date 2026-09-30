#pragma once

// Thread confinement (ADR 0046 S13 D2). A document belongs to the thread that made it: its
// memos (a record document's: the rows' record indexes, the answers to what changed since the
// save, the records by identity) fill lazily inside its const queries. Another thread reads a
// snapshot() instead (DocumentBase::snapshot), a new instance over the same committed rows and
// file-wide state. That is safe because nothing a committed row holds ever changes: a Node
// carries no memo filled inside a const query (what a row derives, a model's or a menu screen's
// places of its identities, is built when the row is made, at parse and by the edit that makes
// it, before it commits), and the process-wide counters (a document's identity and load
// generation, a gesture's token) are atomic.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/edit.h>
#include <editor/model/edit_history.h>
#include <editor/model/field_use.h>
#include <editor/model/node.h>
#include <editor/model/value.h>

namespace opennova::editor {

class StagedRows;

// What a type's lookup rule makes of one definition (Document::refine_symbol), which the graph
// puts on the symbol the defining field makes: the value it stands for where it stands for one (a
// style variable's, as the game reads it); inert, with why in a few words, where no lookup finds
// it; and the line of the file it is on where the type knows it (0 = none).
struct SymbolFacts {
	std::string value;
	bool inert = false;
	std::string inert_reason;
	size_t line = 0;
};

// One kind of record a document type holds (ADR 0046 S13 D5): a row of the type's kinds(), from
// which the base answers what a kind is called, the token it goes by (in a locator, a collection,
// a batch's add and the editor MCP) and which kinds are the file's own rows.
struct RecordKindRow {
	NodeKind kind = 0;
	const char *token = "";     // "window", "items.item", "carry"
	const char *label = "";     // "Window", "Carry limit"
	const char *add_label = ""; // the outline's tool adding a row of it ("Add screen"); "" = none
	bool top = false;           // a row of the file, not a record nested in one
};

// The record document (ADR 0046 d9, S13 D6): a DocumentBase whose content is rows a document
// type parses from and serializes to its native format, each holding records every edit
// addresses by identity and field. The base keeps the lifecycle (the load and its decode, the
// change fingerprint, the conflict check, the atomic write, the source findings); this keeps the
// rows, their undo/redo and the save checkpoint over them, and everything derived from what a type
// declares. A type supplies the record-level hooks below: eight it must (kinds, collections,
// fields, read, set_field, parse, serialize, snapshot), the others having a default; the
// session, the windows and the shell use only this interface and the base's.
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
class Document : public DocumentBase {
public:
	Document() = default;

	// What apply (the base's) does with a record document's edits (apply_edits, ADR 0046 S13 D7): a
	// batch is one undoable step over any rows of the document, each edit applied to the rows as the
	// edits before it left them (StagedRows: each row an edit touches cloned once), nothing committed
	// when any edit is refused or the type vetoes the step (accept_step). A batch may mix the edits of
	// records (a Set, Clear, Write or Apply of a record's field, and the Adds, Duplicates, Removes,
	// Moves and Pastes inside a row) with the rows' own (a row added, duplicated, removed or moved, the
	// rows a Paste at the top level makes: paste_rows) and the file-wide state's (a SetFileValue, an
	// Apply naming no row). Every address is checked against the rows as the batch has left them: a
	// stale identity, or one naming another kind than the address says, is refused
	// (document.selection), and so is an edit naming a row an earlier edit of the batch removed
	// (document.batch). A later edit may name what an earlier one made, a row or a record
	// (batch_made). A Move that leaves a record where it is, a Clear of a field already left out, a
	// Write of one already written, a Set of the value a field holds (the field as the record reads it,
	// and whether it is written, the same after the Set) and an Apply whose payload changes nothing
	// change nothing; a batch that changes nothing takes no history step (the document stays clean).
	// A batch whose edits are all coalesced Sets (typing) applies to the rows as its open group found
	// them (Edit::coalesce), the group's step undone first, so a value typed on the way (an empty image
	// or hotkey, a name) leaves nothing behind and the whole group stays one step (a refused one puts
	// the group's step back as it was; one typed back to what the group found leaves none). Batches
	// sharing a gesture fold into one step over every row they change, each building on the last
	// (Edit::gesture); a batch that adds, removes or moves a row is a step of its own and ends the
	// group. A record's new name is its own edit: what names it elsewhere, in its file or another, is
	// Rename everywhere's (graph/rename_transaction, plan_symbol_rename_project).
	void end_edit_group() override { history_.end_edit_group(); }
	bool dirty() const override { return history_.dirty(); }
	bool can_undo() const override { return history_.can_undo(); }
	bool can_redo() const override { return history_.can_redo(); }
	uint64_t revision() const override { return history_.revision(); }
	// What the history holds: its steps' rows' footprints (Node::footprint), under the history's budget
	// (HistoryBudget: past it the oldest steps are given up, never the last one).
	size_t history_bytes() const override { return history_.bytes(); }
	// The rows added, removed and changed, whether rows moved among the others and whether the
	// file-wide state changed, from the state (load_generation, revision) to this one (RowChanges, the
	// history's changes_since): false for another load's state, and for one the history no longer
	// holds.
	bool changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const override;
	const Document *as_records() const override { return this; }
	Document *as_records() override { return this; }

	const std::vector<std::shared_ptr<const Node>> &rows() const { return rows_; }
	const FileState *file_state() const { return file_state_.get(); }
	// What the last batch that made records made: its first, and every one in its edits' order (the
	// rows and records added, the copies a Duplicate made, the rows and records a Paste made; not what
	// they hold).
	NodeId last_added() const { return last_added_; }
	const std::vector<NodeId> &last_added_records() const { return added_; }
	// The address of a row or nested record by identity (kind 0 / row 0 when unknown): its row
	// from an index of every record's row (index_records), its place from that row's own index.
	NodeAddress address_of(NodeId id) const;
	const Node *row(NodeId id) const;

	// --- what a document type declares about its records -------------------------
	// A collection an owner holds: its records are of one kind, in order.
	struct CollectionSpec {
		NodeKind kind = 0;             // the records' kind
		const char *label = "";        // "Actions"
		const char *name_field = "";   // the field that names a record ("" = none)
		// No Add, Duplicate, Remove or Move: the core refuses each (a model's LODs and collision
		// records, a clip's bones and frame events).
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
	// Every kind of record the type holds, one row each (RecordKindRow), in storage that outlives
	// the document (a static table of the type): a kind's label, its token, whether it is a row of
	// the file and whether the outline adds one. A collection's records are of a kind listed here.
	virtual const std::vector<RecordKindRow> &kinds() const = 0;
	// From kinds(): a kind's row (null for one the type does not hold), its label and its token
	// ("" for none), and the kind a token names (-1 for none).
	const RecordKindRow *kind_row(NodeKind kind) const;
	const char *kind_label(NodeKind kind) const;
	const char *kind_token(NodeKind kind) const;
	NodeKind kind_from_name(const std::string &token) const;
	// The collections `owner` holds inside `row` (owner.child == 0: the row's own), in
	// the order the file writes them; none for a record that holds nothing. `row` may be
	// an uncommitted clone (the base resolves a batch's later edits against it).
	virtual std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const = 0;
	// Every nested record of a row in pre-order (a record, then what it holds), with its
	// placement. The default recurses through collections(); a type may walk faster.
	virtual void walk_records(const Node &row, const RecordVisitor &visit) const;
	// A kind's fields, in storage that outlives the document (a static table of the type, or
	// one its type object owns; never a temporary): a record's FieldUse points into it.
	virtual const std::vector<FieldSchema> &fields(NodeKind kind) const = 0;
	// A field (an entry of fields(address.kind)) as it applies to one record: whether the game
	// reads it there, the reference it makes given the record's other fields, the scope it
	// resolves in, the symbol it defines and where a lookup finds that (its scope), what its
	// loader picks the file by, its colour's form, and whether the record offers choices of its
	// own. Seeded from the schema, refined by the type (refine_field); read_only never widens.
	FieldUse field_on(const NodeAddress &address, const FieldSchema &field) const;
	// The choices a record offers of its own for a field whose use says so (FieldUse::
	// own_choices: a model's registers by index), made into out; false for none (the default).
	// The widgets, the JSON and the find ask (a value by its choice's name, as shown); the
	// graph's extraction never does.
	virtual bool record_choices(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &out) const;
	// The choices a field offers on its record: the record's own where it has them
	// (record_choices, made into own), else the schema's, not copied.
	const std::vector<FieldChoice> &choices_on(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &own) const;
	// What the symbol a record's defining field makes is to the type's lookup rule (the graph
	// fills its name, file, record, place, field and scope): inert, with why, where no lookup
	// finds this definition (a stylesheet's earlier definition of a name, a menu's screen a later
	// one shadows, a string table's later section of a name, a model's user point past the
	// scan); the value it stands for where it stands for one (a style variable's); its line.
	// The default: every definition is found.
	virtual void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
		(void)address;
		(void)facts;
	}
	// Whether an optional field is written (`field` "" = whether the record itself is):
	// read_present over the record's row.
	bool present(const NodeAddress &address, const std::string &field) const;
	// A field of a record as it stands: read over its row. A type adds here what it derives
	// from the whole document rather than the row (a stylesheet line's number).
	virtual bool get(const NodeAddress &address, const std::string &field, Value &out) const;
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
	// serialize() and snapshot() are the base's (DocumentBase). A record document's snapshot shares
	// its committed rows, file-wide state, history and saved baseline; none of this class's memos
	// come with it (it fills them again as it is read); a type's own, which its copy constructor
	// copies (a menu's saved image and lookups, a stylesheet's evaluated sheet and its values'
	// uses), are each kept for one load generation and revision, which the snapshot shares, so
	// they stay true.

	// --- what changed since the last load or save (the saved baseline) -------------------
	// The baseline moves only with a load or a save: an undo back to it makes the document
	// clean again (dirty() is the history's), and these then answer unchanged.
	// A field changed when its value, or an optional field's presence, differs from the
	// baseline's; every field of a record the baseline lacks has changed, and a field `read`
	// answers on neither side (a display field derived from the whole document) never has. A
	// record whose row is the baseline's own (the same committed row: it never changes) is
	// unchanged without a field read; another row's answers are kept while the two rows compared
	// stand, as record_change's are.
	bool field_changed(const NodeAddress &address, const std::string &field) const;
	enum class RecordChange { Unchanged, Changed, Added };
	// Added: the baseline has no such record. Changed: a field of its kind changed, or a
	// collection it holds lists other records or another order (so a child removed or moved
	// marks the owner that keeps it), or, for a row, its place among the rows both sides have
	// moved. A record of a row the baseline holds as it is reads no field.
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
	// The copy a snapshot is (snapshot()): the same rows, file-wide state, history and baseline
	// over the base's copy; no memo; read only.
	Document(const Document &other);
	Document &operator=(const Document &) = delete;
	// The record half of apply (above), undo and redo, which the base calls only for a document
	// that is neither a snapshot nor blocked.
	bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) override;
	void undo_step() override { history_.undo(); }
	void redo_step() override { history_.redo(); }
	// The record half of a load (DocumentBase::read_source): parse, and when adopting, the rows
	// given their identities, the history started again and the baseline set.
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt,
	                 std::vector<SourceIssue> &issues, Diagnostic &error) override;
	// After a save: the history's checkpoint and the saved baseline move to the rows written.
	void on_saved() override;
	// The type's part of field_on: what the record makes of the field seeded from its schema
	// (whether the game reads it there, the reference and the scope its other fields decide,
	// the symbol it defines, what its loader picks the file by, its own choices). The default
	// keeps the schema's.
	virtual void refine_field(const NodeAddress &address, FieldUse &use) const {
		(void)address;
		(void)use;
	}
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
	// A new top-level record of `kind` with the type's defaults, already given `id`, beside `rows`
	// (the rows as its batch has left them: a name or an id no other row has); null, with why in
	// `error`, for a kind the type adds no row of. The default adds none (a type whose rows the file
	// fixes).
	virtual std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                        const std::vector<std::shared_ptr<const Node>> &rows,
	                                        std::string &error);
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
	// Move that would leave the record where it is never arrives. The default refuses (a type
	// whose records are fixed, or that holds none).
	virtual bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                             std::string &error);
	// Paste (Edit Paste): the payload `copy` made (edit.value) into the owner edit.parent
	// (0 = the row) at edit.position; `added` receives the pasted records (not their
	// descendants). The default refuses.
	virtual bool paste_records(Node &row, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
	                           std::string &error);
	// A Paste at the top level (Edit Paste naming no row and no owner, ADR 0046 S13 D7): the payload
	// `copy` made (edit.value) as rows of the file, in order, their identity stores sized and their
	// identities left to the base (as parse leaves them), each told apart from `rows` (the rows as the
	// batch has left them) where the type tells a copy apart (a name no row has). The base puts them
	// in at edit.position. The default refuses: no type copies rows yet.
	virtual bool paste_rows(const Edit &edit, const std::vector<std::shared_ptr<const Node>> &rows,
	                        std::vector<std::shared_ptr<Node>> &out, std::string &error);
	// A change the type made in C++ (Edit Apply: its EditPayload, whose token says which) to the
	// record `address` names inside `row`, the clone the batch commits once every edit of it is
	// applied: a record type that makes such changes takes the ones it made (S13 D6; a raster's
	// or a text's are its own document's, through its apply_edits). It may reshape the row (fresh
	// identities from `allocate`) and replace `state`, the batch's file-wide state, with a changed
	// copy, committed with the step; `changed` (true when called) false says the payload leaves
	// both as they were, and the batch takes no step for it. The default refuses
	// (document.payload).
	virtual bool apply_payload(Node &row, const NodeAddress &address, const EditPayload &payload,
	                           std::shared_ptr<const FileState> &state, const IdAllocator &allocate,
	                           bool &changed, std::string &error);
	// An Apply naming no row: a change the type made to the file-wide state alone, `state` replaced
	// with a changed copy; one step, a gesture's folding into one; `changed` as above. The default
	// refuses (document.payload).
	virtual bool apply_file_payload(std::shared_ptr<const FileState> &state,
	                                const EditPayload &payload, bool &changed, std::string &error);
	// A file-wide value set (Edit SetFileValue: an item table's vehicle spawn registry). The
	// default refuses: the type has none (document.value).
	virtual bool set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit,
	                            Diagnostic &error);
	// The file-wide state after the row at `index` is removed (`remaining` rows stay).
	virtual std::shared_ptr<const FileState> state_after_remove(const std::shared_ptr<const FileState> &state,
	                                                            size_t remaining) const { (void)remaining; return state; }
	// Derived fields to refresh after any edit of a row, before it commits: what a row derives
	// is built here or when the row is made, never inside a const query (the thread
	// confinement above).
	virtual void after_edit(Node &row) { (void)row; }
	// A row a Duplicate made, before it goes in beside the original (`rows`, the rows as its batch
	// has left them, still hold the original): the type may tell the copy apart (a menu names the
	// screen anew, since the game finds the last of two screens of one name). The default keeps it
	// as is.
	virtual void prepare_duplicate(Node &copy, const std::vector<std::shared_ptr<const Node>> &rows) const {
		(void)copy;
		(void)rows;
	}
	// The veto on a batch's step before it commits (S13 D7, which replaced S9's accept_change of one
	// row): its swaps (a row added, removed, moved or changed in place) and the file-wide state on
	// both sides, with the rows as the step leaves them (StagedRows::rows), for every batch, the rows
	// added, duplicated, removed and moved included (which never reach the type otherwise). A refusal
	// commits nothing, leaves the history and last_added as they were, and fails the batch with
	// `error` (document.structure). The default accepts.
	virtual bool accept_step(const EditStep &step, const StagedRows &rows, std::string &error) const {
		(void)step;
		(void)rows;
		(void)error;
		return true;
	}

	NodeId allocate_id() { return next_id_++; }

private:
	// Every nested record of one committed row by identity, built by one walk and kept
	// while that row is current (a committed row never changes).
	struct RowIndex {
		std::shared_ptr<const Node> row;
		std::unordered_map<NodeId, Placement> placements;
	};
	const RowIndex &row_index_of(const std::shared_ptr<const Node> &row) const;
	// Brings the index of every record's row (address_of's) to the revision one row at a time:
	// a row whose committed version is not the one indexed has that version's records taken out
	// (those still indexed under it) and its own put in; a row no longer among the rows has its
	// records taken out. An edit of one row indexes that row again, not every record.
	void index_records() const;
	// A nested record's placement inside `row` by walking it (a clone a batch changes).
	bool placement_in(const Node &row, NodeId child, Placement &out) const;
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
	// What changed in a record between its committed row and the baseline's, two different
	// rows (record_change): Added, or Changed by a field or by what it holds; a row's place among
	// the rows is not its own two rows' answer (record_change asks row_moved apart).
	RecordChange compare_record(const NodeAddress &address, const std::shared_ptr<const Node> &now,
			const std::shared_ptr<const Node> &saved) const;
	// What changed since the baseline in one row: the record and field answers its two rows give
	// (compare_record, field_differs), made while its committed row and the baseline's are these
	// two (both rows kept, so neither address is reused under the answers), started again when
	// either is another.
	struct RowAnswers {
		std::shared_ptr<const Node> now, saved;
		std::unordered_map<NodeId, RecordChange> records;
		std::unordered_map<NodeId, std::unordered_map<std::string, bool>> fields;
	};
	RowAnswers &row_changes(NodeId row, const std::shared_ptr<const Node> &now,
			const std::shared_ptr<const Node> &saved) const;
	// Whether a row's index among the rows both sides have differs from the baseline's.
	bool row_moved(NodeId id) const;
	// The undo step a batch continues: batches sharing a gesture fold into one step over every row
	// they change until the group ends (`builds`: each builds on the last); a batch of coalesced Sets
	// folds with the next one of the same fields; "" folds with nothing.
	static std::string step_key(const std::vector<Edit> &edits, bool &builds);
	// A batch's edits applied to `staged` in order, nothing committed: false, with `error`, at the
	// first one refused; `added` receives what each edit made, in order (last_added_records).
	bool stage_edits(const std::vector<Edit> &edits, StagedRows &staged, std::vector<NodeId> &added,
	                 Diagnostic &error);
	// A row's index among the rows by its identity (rows().size() when it has none), through an
	// index checked on every hit and made again when stale (a row added, removed or moved since),
	// so a query over every record of a large table stays linear.
	size_t row_index(NodeId id) const;
	void assign_ids(Node &row);

	std::vector<std::shared_ptr<const Node>> rows_;
	std::shared_ptr<const FileState> file_state_;
	NodeId next_id_ = 1, last_added_ = 0;
	std::vector<NodeId> added_;
	mutable std::unordered_map<const Node *, RowIndex> indexes_;
	mutable std::unordered_map<NodeId, size_t> row_positions_; // row_index's
	EditHistory history_{rows_, file_state_};
	std::vector<std::shared_ptr<const Node>> saved_rows_;
	std::shared_ptr<const FileState> saved_state_;
	// A saved row's index by its identity (set_baseline).
	std::unordered_map<NodeId, size_t> saved_positions_;
	// The memos (the thread confinement above), each forgotten by set_baseline: what changed in
	// each row (row_changes); the rows whose place moved, for one revision; every record's row by
	// identity and the committed version of each row it indexed (index_records), brought to a
	// revision a changed row at a time.
	mutable std::unordered_map<NodeId, RowAnswers> row_changes_;
	mutable bool moved_known_ = false;
	mutable uint64_t moved_revision_ = 0;
	mutable std::unordered_set<NodeId> moved_rows_;
	mutable bool records_known_ = false;
	mutable uint64_t records_revision_ = 0;
	mutable std::unordered_map<NodeId, NodeId> record_rows_;
	mutable std::unordered_map<NodeId, std::shared_ptr<const Node>> indexed_rows_;
};

// The record document a document is (DocumentBase::as_records), null for another kind of
// document: what a caller that reads rows, records and fields asks of a document it holds as
// the base; the shared form shares the document's ownership, the owning form takes it (a
// document of another kind is destroyed).
inline const Document *records_of(const DocumentBase &document) { return document.as_records(); }
inline Document *records_of(DocumentBase &document) { return document.as_records(); }
inline std::shared_ptr<const Document>
records_of(const std::shared_ptr<const DocumentBase> &document) {
	const Document *records = document ? document->as_records() : nullptr;
	return records ? std::shared_ptr<const Document>(document, records) : nullptr;
}
inline std::unique_ptr<Document> records_of(std::unique_ptr<DocumentBase> document) {
	Document *records = document ? document->as_records() : nullptr;
	if (!records) return nullptr;
	document.release();
	return std::unique_ptr<Document>(records);
}

} // namespace opennova::editor
