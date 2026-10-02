#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <string>

#include <editor/model/value.h>

namespace opennova::editor {

// What a request asked one window to do once (ADR 0046 S13 V4): show a record's field, show a
// file in Files, ask a name's new name, say how the settings' Apply came out, take a new import
// plan. The view keeps state; an event is an ask, which a second ask of the same thing repeats
// (a second click on one Problems row shows its field again).
enum class ViewEventKind : uint8_t {
	// An OpenDocument that named a record and a field (a Problems row, a Go to): `path` the
	// document, `address` the record, `field` its field to show. The document's view shows the
	// record and the Inspector lights the field.
	RevealRecord,
	// An OpenDocument of a text document that named a place in it (a Go to's span, a Problems
	// row's line; ADR 0046 S13 D9): `path` the document, `locator` the place ("line:column"). The
	// document's view scrolls to the line and marks it (S13 V10's script device to the span).
	RevealText,
	// A ShowInFiles: `path` the project file (project-relative), `flag` asks its new name (Files'
	// Rename...). Files comes forward and selects it.
	RevealFile,
	// A PreviewRename that asks the new name: `path` the file defining the name, `field` its
	// field, `tag` the rename preview's serial (the view's rename_preview it asks from). The Rename
	// everywhere dialog opens.
	AskRename,
	// An ApplyProjectSettings: `tag` the request's settings serial, `flag` a setting could not be
	// written (the view's settings_result lists them). The settings dialog waiting on it closes,
	// or says what failed.
	SettingsApplied,
	// A plan of the import dialog made (a preview, its files chosen again, the dependencies'
	// switch, an Import planning again first): `flag` an Import found the files changed since the
	// plan it was shown and wrote nothing. The dialog takes the plan's checks again.
	ImportPlanned,
	kCount,
};

inline constexpr size_t kViewEventKindCount = static_cast<size_t>(ViewEventKind::kCount);

// A kind's token: its `kind` in the view JSON's events.
struct ViewEventKindRow {
	ViewEventKind kind;
	const char *token;
};

inline constexpr ViewEventKindRow kViewEventKindRows[] = {
	{ViewEventKind::RevealRecord, "reveal_record"},
	{ViewEventKind::RevealText, "reveal_text"},
	{ViewEventKind::RevealFile, "reveal_file"},
	{ViewEventKind::AskRename, "ask_rename"},
	{ViewEventKind::SettingsApplied, "settings_applied"},
	{ViewEventKind::ImportPlanned, "import_planned"},
};

static_assert(std::size(kViewEventKindRows) == kViewEventKindCount,
		"every view event kind has exactly one row");

constexpr bool view_event_kind_rows_in_order() {
	for (size_t i = 0; i < kViewEventKindCount; ++i)
		if (kViewEventKindRows[i].kind != static_cast<ViewEventKind>(i)) return false;
	return true;
}
static_assert(view_event_kind_rows_in_order(), "the view event kind rows follow the enum's order");

constexpr bool view_event_kind_tokens_unique() {
	for (size_t i = 0; i < kViewEventKindCount; ++i)
		for (size_t j = i + 1; j < kViewEventKindCount; ++j) {
			const char *a = kViewEventKindRows[i].token;
			const char *b = kViewEventKindRows[j].token;
			size_t k = 0;
			while (a[k] != '\0' && a[k] == b[k]) ++k;
			if (a[k] == b[k]) return false;
		}
	return true;
}
static_assert(view_event_kind_tokens_unique(), "each view event kind has a token of its own");

inline const char *view_event_kind_token(ViewEventKind kind) {
	return kViewEventKindRows[static_cast<size_t>(kind)].token;
}

// One event: its place in the view's sequence (`seq`, from 1, one more for each event posted) and
// what it asks, by kind (ViewEventKind); a field its kind does not use stays empty.
struct ViewEvent {
	uint64_t seq = 0;
	ViewEventKind kind = ViewEventKind::RevealRecord;
	std::string path;
	NodeAddress address;
	std::string field;
	std::string locator; // a text's place (RevealText)
	bool flag = false;
	uint64_t tag = 0;
};

// The view's events (SessionView::events): the last kKept posted, oldest first, each by its seq.
// The session posts them; the workspace hands each new one to the window it is for
// (EditorWindows::begin_frame, which holds it until that window draws), and the view JSON pages
// them by seq, so a client reading from its last `next_cursor` neither skips nor repeats one.
class ViewEvents {
public:
	static constexpr size_t kKept = 64;

	// `event` posted as the next in the sequence (its seq set); the oldest held goes past kKept.
	// Its seq.
	uint64_t post(ViewEvent event);
	// The oldest seq held (next_seq() when none is), and one past the newest.
	uint64_t first_seq() const { return events_.empty() ? next_ : events_.front().seq; }
	uint64_t next_seq() const { return next_; }
	// The events held, oldest first.
	const std::deque<ViewEvent> &held() const { return events_; }

private:
	std::deque<ViewEvent> events_;
	uint64_t next_ = 1;
};

} // namespace opennova::editor
