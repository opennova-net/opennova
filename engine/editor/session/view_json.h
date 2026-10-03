#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <base/io/json.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

// The view's wire form, by section (ADR 0046 S13 A5): what the state query answers of the view,
// each section what one part of it shows, moving with the concerns it names (view_revisions.h), so
// a client asks for the sections it reads and, with `since`, is answered only those that moved.
// The lists the view holds are paged where they are served: the files (the files query), the
// output lines and the events (by their absolute index and their sequence), the import plan's
// rows (the import_preview query).
enum class ViewSection : uint8_t {
	Status,
	Project,
	Requirements,
	Documents,
	Selection,
	Operation,
	Run,
	Import,
	Dialogs,
	ProblemCounts,
	GraphCounts,
	Preferences,
	Output,
	Events,
	kCount,
};

inline constexpr size_t kViewSectionCount = static_cast<size_t>(ViewSection::kCount);

// One row per section (view_json.cpp, static_asserted into place): its token (its key in the state
// answer), the concerns whose moves it follows, what writes it, and what it shows.
struct ViewSectionRow {
	ViewSection section = ViewSection::kCount;
	const char *token = "";
	ConcernSet concerns = 0;
	io::JsonValue (*write)(const SessionView &view) = nullptr;
	const char *doc = "";
};

// A section's row; the Status row for a value past the last section.
const ViewSectionRow &view_section_row(ViewSection section);
// The section a token names ("documents"); false for none.
bool view_section_from_token(const std::string &token, ViewSection &out);
// A section of the view, as its row writes it.
io::JsonValue view_section_to_json(const SessionView &view, ViewSection section);
// Whether a concern of `section` moved after the view's revision `since` (ViewRevisions::stamp).
bool view_section_moved(const SessionView &view, ViewSection section, uint64_t since);

// The operation that runs: {running}, and while one does its id, kind token, label, done, total,
// unit token (bytes, files, steps), cancellable, and reads and writes (tokens: files, documents,
// project, slot).
io::JsonValue operation_status_to_json(const OperationStatus &status);
// What an operation came to: {id, kind, end (done, failed, cancelled), findings}; null before the
// first ends.
io::JsonValue operation_outcome_to_json(const OperationOutcome &outcome);
// The operation, what the last one came to and the last build: {operation, last_operation,
// build: {has_build, and with one ok, id, dir, reused_existing, archives_written,
// archives_reused, loose_written, diagnostics}} (the Operation section, the operation query).
io::JsonValue activity_operation_to_json(const SessionView &view);
// A view event (view_events.h): {seq, kind (its token: reveal_record, reveal_file, ask_rename,
// settings_applied, import_planned)} and, as the kind sets them, `path`, `address` (the record's
// {row, kind, child}), `field`, `flag` (only when true) and `tag` (only when not 0).
io::JsonValue view_event_to_json(const ViewEvent &event);
// A page of the output lines by absolute index (OutputLog): {first (the oldest line held), next
// (one past the newest), count (the lines held), cursor (the page's first, a cursor below `first`
// moved up to it), next_cursor, lines}: a client paging by next_cursor neither skips nor repeats
// a line while the log drops its oldest past 2000.
io::JsonValue output_page_to_json(const OutputLog &output, uint64_t cursor, size_t limit);
// A page of the view's events by seq (ViewEvents): {first, next, count, cursor, next_cursor, items
// (view_event_to_json's)}, paged as the output lines are.
io::JsonValue events_page_to_json(const ViewEvents &events, uint64_t cursor, size_t limit);
// The import dialog's preview (DialogsView::ImportPreview): {open, with_dependencies, all?, changed?,
// then a page of its plan's importable rows (`rows`, `count` the whole plan's; of `kind` alone when
// one is named, `count` theirs and `kind` echoed), `total_bytes` and `summary` (the whole plan's
// files by kind: {kind, files, bytes}, the largest first), and by the same offset and limit the
// lists it offers and chose (`choices`, `roots`) and the files not found (`not_found`), each with
// its whole length (`choice_count`, `root_count`, `not_found_count`), then `not_followed`,
// `undefined`, `truncated` and the plan's `diagnostics`}. Each row: its state (selected or found),
// name, kind, source (as a request's imports take it), destination, size, made_from, needed_by,
// found_in, selected, problem and rivals.
io::JsonValue import_preview_to_json(const SessionView &view, const JsonPage &page, AssetKind kind = AssetKind::kCount);

} // namespace opennova::editor
