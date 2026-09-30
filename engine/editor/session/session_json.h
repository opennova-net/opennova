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

// The stable snake_case token of every request kind ("new_project"), edit operation
// ("set"), pick purpose, unsaved choice, selection mode, finding severity ("error",
// diagnostic_severity_label's), and Problems scope and grouping. Every enumerator has one;
// parsing is exact.
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

// A request from its wire form: an object with "kind" and, as the kind needs them, "path",
// "text", "flag", "purpose", "paths" and "names" (strings), "imports" ([{path, entry,
// retail, native}]), "edit" ({operation, row, kind, child, parent, field, value, position,
// coalesce, gesture}), "edits" (an array of edits: a batch on one row), "mode"
// (select_record: replace, add or toggle) and "unsaved_choice". A JSON number that is whole
// reads as an integer value, any other as a real; a bool reads as 0 / 1. False with `error`
// on an unknown kind or token, a wrongly typed member or a malformed edit; `out` is
// untouched on failure. editor_request_to_json writes what the reader takes back.
bool editor_request_from_json(const io::JsonValue &json, EditorRequest &out, std::string &error);
io::JsonValue editor_request_to_json(const EditorRequest &request);

// A Value both ways: integers and reals are JSON numbers, text a JSON string.
io::JsonValue value_to_json(const Value &value);
bool value_from_json(const io::JsonValue &json, Value &out);

struct SessionJsonOptions {
	// The first output line to include, by its absolute index (OutputLog): a cursor kept from the
	// last page's next_cursor neither skips nor repeats a line.
	uint64_t output_cursor = 0;
	size_t output_limit = 200;
	// The page of the import block's lists (its choices, roots, rows and not_found): each
	// from `import_offset`, `import_limit` entries at most; their counts say how many there are.
	size_t import_offset = 0;
	size_t import_limit = 200;
	// The first event to include, by its seq (ViewEvents; 0: from the oldest held), and how many:
	// a cursor kept from the last page's next_cursor neither skips nor repeats one.
	uint64_t event_cursor = 0;
	size_t event_limit = ViewEvents::kKept;
};

// The view: revision (moves with any change: ViewRevisions::any), revisions (each concern's
// counter by its token, view_revisions.h: a client waits on the concern it reads), status,
// the unsaved-changes prompt ({open}, and while it is open the
// waiting request's kind token as `action`, its `target`, the `files` it lists and
// `can_discard`), project (its `files`: every file the scan lists, with its kind and whether
// the editor opens it), requirements (rows included), documents (their paths, kinds,
// dirtiness), the selection (the primary and every selected record), the clipboard's size, the
// operation (operation_status_to_json) and the last one's outcome (`last_operation`), the build
// and play blocks, what the last settings' Apply could not write (`settings_result`),
// the import state (the dialog: its `choices` and `roots` as a request's imports take them,
// the plan's importable `rows` with their source, destination, needed_by, found_in,
// selected, problem and rivals, the `not_found` rows, each of the four lists a page from
// `offset` with its count, then `not_followed`, `truncated` and the plan's findings; the
// editor's `import_dependencies` setting; the project's imported sources), the problem
// counts, the recent projects, a page of the output lines by absolute index ({first, next,
// cursor, next_cursor, lines}) and a page of the events by seq ({first, next, cursor,
// next_cursor, items}, each view_event_to_json's).
io::JsonValue session_view_to_json(const SessionView &view, const SessionJsonOptions &options = {});
// A view event (view_events.h): {seq, kind (its token: reveal_record, reveal_file, ask_rename,
// settings_applied, import_planned)} and, as the kind sets them, `path`, `address` (the record's
// {row, kind, child}), `field`, `flag` (only when true) and `tag` (only when not 0).
io::JsonValue view_event_to_json(const ViewEvent &event);
// The operation that runs: {running}, and while one does its id, kind token, label, done, total,
// unit token (bytes, files, steps), cancellable and holds (tokens: files, documents, project).
io::JsonValue operation_status_to_json(const OperationStatus &status);
// What an operation came to: {id, kind, end (done, failed, cancelled), findings}; null before the
// first ends.
io::JsonValue operation_outcome_to_json(const OperationOutcome &outcome);
// A finding: severity, code, message, and as it has them asset, field, record, line, the
// record's identities, and what it is about (role, target, reference as a kind token, scope).
io::JsonValue diagnostic_to_json(const Diagnostic &diagnostic);
io::JsonValue diagnostics_to_json(const std::vector<Diagnostic> &diagnostics);
// A fix: {label, detail, bulk, request} with the request's wire form.
io::JsonValue problem_fix_to_json(const ProblemFix &fix);
// A Problems query from its wire form: an object with, each optional, "severities" (an
// array of "error", "warning", "info"; all when left out), "text", "scope" (project,
// active_file, open_files), "fixable", "group" (none, file, kind), and the page's "offset"
// and "limit" (whole numbers; every row when there is no limit). False with `error` on an
// unknown member or token or a wrongly typed one; the outputs are untouched on failure.
bool problem_query_from_json(const io::JsonValue &json, ProblemQuery &query, size_t &offset, size_t &limit,
                             std::string &error);
// What a query shows: {total, shown, counts {errors, warnings, infos} over every finding,
// and the page of problems from `offset`, `limit` of them, each a finding with its `fixes`
// (from `fixes`, which keeps them while what they read stands)}. Grouped, each problem
// also names its `group`, `group_count` is how many groups the query shows, and `groups`
// holds those of the page's problems, in order, each whole: {key, title, first (the index of
// its first row among the shown), count, errors, warnings, infos}.
io::JsonValue problems_to_json(const SessionView &view, const ProblemAnswer &answer, size_t offset, size_t limit,
                               ProblemFixCache &fixes);
// What one request came to: {done, unsaved_prompt, operation, findings}; `done` is false when
// it was refused or failed (an error among the findings) or waits on the unsaved prompt;
// `operation` names the operation it started or joined (0: none).
io::JsonValue action_outcome_to_json(const ActionOutcome &outcome);
// A document: its lifecycle state (with file_state_changed: its file-wide state differs from
// the saved baseline's), the source issues and, with rows, every row with its collections
// (kind, kind token, label, fixed) and their records (identities, names), each row and record
// with its change since the saved baseline (Document::record_change: unchanged, changed,
// added) and the collections it holds in turn.
io::JsonValue document_to_json(const Document &document, bool with_rows);
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
// picker offers (Document::reference_choices): {field, reference (its kind token), scope,
// count, choices: [{name, kind, file, record, status, inert, reason}]}; and where its Go to
// leads with the value it holds (Document::reference_targets): {field, reference, value,
// count, targets: [{label, file, locator, field, editable}]}. A field that is no reference
// has none. Null for a record the document does not hold or a field it does not have.
io::JsonValue reference_choices_to_json(const Document &document, const NodeAddress &address, const std::string &field,
                                        const SessionView &view);
io::JsonValue reference_targets_to_json(const Document &document, const NodeAddress &address, const std::string &field,
                                        const SessionView &view);
// Find in a document (editor_document op=search): {count, hits}, each hit its record's id (the
// nested record's identity, else the row's), address, record path and locator, the field's id
// and name, the value as shown and where the text is found in it.
io::JsonValue document_hits_to_json(const std::vector<DocumentHit> &hits);
// Find in the project (editor_graph op=search): {count, hits}, each a file ({kind "file", name,
// file, usages}) or a symbol ({kind as its token, name, file, and as graph_symbol_to_json the
// record, locator, field, scope and inert, then usages}).
io::JsonValue graph_search_to_json(const std::vector<GraphSearchHit> &hits);

} // namespace opennova::editor
