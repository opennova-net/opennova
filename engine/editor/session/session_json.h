#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/document_search.h>
#include <editor/session/editor_request.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

// The session's wire form (ADR 0046 d10, the editor MCP): a request from JSON, and the
// view, a document, a record and the findings as JSON. Portable on purpose: the
// editor's MCP transport marshals nothing itself, a command line can print the same
// records, and one ctest pins every token both ways.

// The stable snake_case token of every request kind ("new_project", its row's in the request
// table, request_kinds.h), edit operation ("set"), pick purpose, unsaved choice, selection mode,
// finding severity ("error", diagnostic_severity_label's), and Problems scope and grouping. Every
// enumerator has one; parsing is exact.
const char *editor_request_kind_token(EditorRequestKind kind);
bool editor_request_kind_from_token(const std::string &token, EditorRequestKind &out);
const char *edit_operation_token(EditOperation operation);
bool edit_operation_from_token(const std::string &token, EditOperation &out);
const char *pick_purpose_token(PickPurpose purpose);
bool pick_purpose_from_token(const std::string &token, PickPurpose &out);
const char *unsaved_choice_token(UnsavedChoice choice);
bool unsaved_choice_from_token(const std::string &token, UnsavedChoice &out);
const char *select_mode_token(SelectMode mode);
bool select_mode_from_token(const std::string &token, SelectMode &out);
bool diagnostic_severity_from_token(const std::string &token, DiagnosticSeverity &out);
const char *problem_scope_token(ProblemScope scope);
bool problem_scope_from_token(const std::string &token, ProblemScope &out);
const char *problem_grouping_token(ProblemGrouping grouping);
bool problem_grouping_from_token(const std::string &token, ProblemGrouping &out);
// Every kind token in enumerator order (a client lists what it may ask for).
std::vector<std::string> editor_request_kind_tokens();

// Where a request's edits are named (S13 A5: the batch form, record_batch.h): the record document
// the request acts on, and, read, the label each edit gave by the edit's index
// (RecordBatch::labels), which the request's outcome pairs with what each edit made (`made`).
struct RequestNames {
	const Document *document = nullptr;
	std::vector<std::string> labels;
	// A first read, before the document the request asks to open first opens (open_first): its
	// edits' records are read as identities and not looked for, what only the document knows (a
	// replaced list's records) waits for the read after it opens, and their
	// kinds are named in a blank of the path's type. A request refused here opens nothing.
	bool unresolved = false;
};

// A request from its wire form (S13 A4): an object with "kind", the kind's token, and the fields
// its row takes (request_kinds.h: params), each by its token and of its JSON type
// (request_fields.h): "dir", "path", "edits" (the batch form, record_batch.h: [{op, id, parent,
// kind, field, value, position, as, coalesce, gesture, list, records}], revert_to_saved's [{id,
// field}]), "address" ({row, kind, child}), "paste_at" ({row, parent, position}), "imports"
// ([{path, entry, install, native}]) and the rest. An edit's value that is a whole JSON number
// reads as an integer, any other number as a real, a bool as 0 / 1; a "new_name" that is a whole
// number (an item id) reads as its digits. An edit names its records and kinds in
// `names->document`, the record document the request acts on (S13 A5); with none, it names kinds
// in a blank of the type that opens the request's "path" and no record. False with `error` on an
// unknown kind, member or token, a field the kind does not take, a field it must carry left out, a
// wrongly typed member or a malformed edit (named by its place, "edits[1]"); an unknown member and
// a field taken or left out wrongly name the fields the kind takes. `out` is untouched on failure.
// editor_request_to_json writes the fields the kind takes that a request carries (those it must
// carry always), its edits named in `names` (else a blank of the type its path opens), which the
// reader takes back as they were.
bool editor_request_from_json(const io::JsonValue &json, EditorRequest &out, std::string &error,
		RequestNames *names = nullptr);
io::JsonValue editor_request_to_json(const EditorRequest &request, const Document *names = nullptr);

// A page of a list (S13 A5: every list a query serves is paged here, in C++): the entries from
// `offset`, `limit` of them at most.
struct JsonPage {
	size_t offset = 0;
	size_t limit = SIZE_MAX;
	// The page over a list of `total` entries: [first, last).
	size_t first(size_t total) const { return offset < total ? offset : total; }
	size_t last(size_t total) const {
		const size_t from = first(total);
		return from + (limit < total - from ? limit : total - from);
	}
};
// A page's place in its list: `count` the list's whole length, `offset` the page's first, and
// `next_offset` the next page's while entries remain (null at the end). `beside` is the longest
// other list the same page covers (an import plan's choices beside its rows, a screen's notes
// beside its widgets; 0 for none), each written with its own count: the page runs on to the end
// of the longer, so paging by next_offset reads every list whole. Offset pages are gapless while
// the list stays as it was; one that changes between pages can shift an entry across them.
void set_page(io::JsonValue &out, const JsonPage &page, size_t total, size_t beside = 0);

// A record's address, {row, kind, child}; an import source as a request's imports take it,
// {path, entry?, install?, native?}.
io::JsonValue address_to_json(const NodeAddress &address);
io::JsonValue import_source_to_json(const ImportSource &source);

// A Value both ways: integers and reals are JSON numbers, text a JSON string.
io::JsonValue value_to_json(const Value &value);
bool value_from_json(const io::JsonValue &json, Value &out);

// A finding: severity, code, message, and as it has them asset, field, record, line, the
// record's identities, and what it is about (role, target, reference as a kind token, scope).
io::JsonValue diagnostic_to_json(const Diagnostic &diagnostic);
io::JsonValue diagnostics_to_json(const std::vector<Diagnostic> &diagnostics);
// A fix: {label, detail, bulk, request} with the request's wire form.
io::JsonValue problem_fix_to_json(const ProblemFix &fix);
// What a query shows: {total, shown, counts {errors, warnings, infos} over every finding,
// and the page of problems (set_page's, `count` the rows shown), each a finding with its `fixes`
// (from `fixes`, which keeps them while what they read stands)}. Grouped, each problem
// also names its `group`, `group_count` is how many groups the query shows, and `groups`
// holds those of the page's problems, in order, each whole: {key, title, first (the index of
// its first row among the shown), count, errors, warnings, infos}.
io::JsonValue problems_to_json(const SessionView &view, const ProblemAnswer &answer,
		const JsonPage &page, ProblemFixCache &fixes);
// What one request came to: {done, unsaved_prompt, operation, findings, added}; `done` is false
// when it was refused or failed (an error among the findings) or waits on the unsaved prompt;
// `operation` names the operation it started or joined (0: none); `added` the records its edits
// made, in order (an edit_record's adds and duplicates, a paste's, a duplicate's copies).
io::JsonValue action_outcome_to_json(const ActionOutcome &outcome);
// A document: its lifecycle state and the source issues; for a record document (as_records) also
// file_state_changed (its file-wide state differs from the saved baseline's), its row count, the
// last record added and the kinds its outline adds, and, with a page of rows (`rows`: `count` the
// rows, set_page's), each row of the page with its collections (kind, kind token, label, fixed)
// and their records (identities, names), each row and record with its change since the saved
// baseline (Document::record_change: unchanged, changed, added) and the collections it holds in
// turn.
io::JsonValue document_to_json(const DocumentBase &document, const JsonPage *rows = nullptr);
// A record: its address, name, path and locator, its change since the saved baseline
// (unchanged, changed, added), its owner and index there, every field of its kind as it
// applies to this record (Document::field_on) with the value, the choices, whether an
// optional field is present, a reference's status against the view's tables, and for a
// field changed since the saved baseline `changed` with what the saved file holds (`saved`,
// null when the file lacks the record; `saved_present` for an optional field), then the
// collections it holds. Null for a stale or wrong-kind address.
io::JsonValue record_to_json(const Document &document, const NodeAddress &address, const SessionView &view);
// The asset graph's edges (each with its resolution) and symbols.
io::JsonValue graph_edge_to_json(const AssetGraph &graph, const GraphEdge &edge);
io::JsonValue graph_edges_to_json(const AssetGraph &graph, const std::vector<const GraphEdge *> &edges);
io::JsonValue graph_symbol_to_json(const GraphSymbol &symbol);
// A reference field of a record, as it applies to it (Document::field_on): the names its
// picker offers (reference_choices): {field, reference (its kind token), scope,
// count, choices: [{name, kind, file, record, status, inert, reason}]}; and where its Go to
// leads with the value it holds (reference_targets): {field, reference, value,
// count, targets: [{label, file, locator, field, editable}]}. A field that is no reference
// has none. Null for a record the document does not hold or a field it does not have.
// Each a page of its list (set_page's).
io::JsonValue reference_choices_to_json(const Document &document, const NodeAddress &address,
		const std::string &field, const SessionView &view, const JsonPage &page = {});
io::JsonValue reference_targets_to_json(const Document &document, const NodeAddress &address,
		const std::string &field, const SessionView &view, const JsonPage &page = {});
// Find in a document (the document_search query): {count, hits}, each hit its record's id (the
// nested record's identity, else the row's), address, record path and locator, the field's id
// and name, the value as shown and where the text is found in it.
io::JsonValue document_hits_to_json(const std::vector<DocumentHit> &hits, const JsonPage &page = {});
// Find in the project (the project_search query): {count, hits}, each a file ({kind "file", name,
// file, usages}) or a symbol ({kind as its token, name, file, and as graph_symbol_to_json the
// record, locator, field, scope and inert, then usages}).
io::JsonValue graph_search_to_json(const std::vector<GraphSearchHit> &hits, const JsonPage &page = {});

} // namespace opennova::editor
