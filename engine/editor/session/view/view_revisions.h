#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>

namespace opennova::editor {

// What in the session's view a change is about (ADR 0046 S13 D1). Each concern has its own
// counter (ViewRevisions), so a window keys what it derives on the concerns it reads and keeps
// it while they stand: a line of the game's log moves Output alone, and the files, the
// findings and the graph a window drew from stay as they were. Since S13 V4 each concern is
// about one sub-view of the view (session_view.h), named first below; SessionView's one
// `revisions` counts them all, so a cache's key names concerns, never sub-views.
enum class ViewConcern : uint8_t {
	// ProjectView: whether a project is open, its folder and its project document; and
	// DialogsView's quit_requested.
	Project,
	// ProjectView: the scan, the requirements, the imports and the game install's file names.
	Files,
	// FindingsView: the Problems rows; moves only when the rows the session composed differ.
	Findings,
	// FindingsView: what the asset graph holds; moves only when its update changed it (its
	// generation).
	Graph,
	// DocumentsView: the open documents, what they hold and their history.
	Documents,
	// DocumentsView: the active document, the selected records, what the previews follow and
	// the clipboard; a record's field or a file asked to be shown (the RevealRecord and
	// RevealFile view events).
	Selection,
	// ActivityView: the Output lines and the status line, what the editor said.
	Output,
	// ActivityView: the running operation (a build: its steps), the last one, the last build.
	Operation,
	// ActivityView: the game Play started: its state, process, port, exit and boot report.
	Run,
	// DialogsView: the unsaved-changes prompt, the import preview, the rename preview; and
	// ProjectView's settings result (the AskRename, SettingsApplied and ImportPlanned events).
	Dialogs,
	// ProjectView: the editor's settings: the recent projects, the game install, the runtime,
	// the import setting.
	Preferences,
	// DocumentsView: which documents are open and whether each has unsaved edits; moves when a
	// document opens, closes, is read again, or goes from saved to unsaved or back; never for
	// an edit that leaves it as it was there (Documents moves with every edit).
	DocumentSet,
	// DocumentsView: which document is the active one; moves only when another becomes it
	// (Selection moves with every record picked in it).
	ActiveDocument,
	kCount,
};

inline constexpr size_t kViewConcernCount = static_cast<size_t>(ViewConcern::kCount);

// A concern's token: its key in the view JSON's "revisions".
struct ViewConcernRow {
	ViewConcern concern;
	const char *token;
};

inline constexpr ViewConcernRow kViewConcernRows[] = {
	{ViewConcern::Project, "project"},     {ViewConcern::Files, "files"},
	{ViewConcern::Findings, "findings"},   {ViewConcern::Graph, "graph"},
	{ViewConcern::Documents, "documents"}, {ViewConcern::Selection, "selection"},
	{ViewConcern::Output, "output"},       {ViewConcern::Operation, "operation"},
	{ViewConcern::Run, "run"},             {ViewConcern::Dialogs, "dialogs"},
	{ViewConcern::Preferences, "preferences"},
	{ViewConcern::DocumentSet, "document_set"},
	{ViewConcern::ActiveDocument, "active_document"},
};

static_assert(std::size(kViewConcernRows) == kViewConcernCount,
		"every view concern has exactly one row");

constexpr bool view_concern_rows_in_order() {
	for (size_t i = 0; i < kViewConcernCount; ++i)
		if (kViewConcernRows[i].concern != static_cast<ViewConcern>(i)) return false;
	return true;
}
static_assert(view_concern_rows_in_order(), "the view concern rows follow the enum's order");

constexpr bool view_concern_tokens_unique() {
	for (size_t i = 0; i < kViewConcernCount; ++i)
		for (size_t j = i + 1; j < kViewConcernCount; ++j) {
			const char *a = kViewConcernRows[i].token;
			const char *b = kViewConcernRows[j].token;
			size_t k = 0;
			while (a[k] != '\0' && a[k] == b[k]) ++k;
			if (a[k] == b[k]) return false;
		}
	return true;
}
static_assert(view_concern_tokens_unique(), "each view concern has a token of its own");

inline const char *view_concern_token(ViewConcern concern) {
	return kViewConcernRows[static_cast<size_t>(concern)].token;
}

// The view's counters: one per concern, and `any`, which moves with each of them. `any` feeds
// only the view JSON's "revision" (what a client polls to see that anything moved); no window
// keys a cache on it. The counters move only through touch: the session's (SessionCore::
// touch), a test's hand-made view's; a window holds the view const and never can.
struct ViewRevisions {
	uint64_t of(ViewConcern concern) const { return counters_[static_cast<size_t>(concern)]; }
	uint64_t any() const { return any_; }
	// A change of `concern`: its counter and `any` move together.
	void touch(ViewConcern concern) {
		++counters_[static_cast<size_t>(concern)];
		++any_;
	}

private:
	std::array<uint64_t, kViewConcernCount> counters_{};
	uint64_t any_ = 0;
};

// A cache's key: the counters of the concerns it reads, every other one 0. It keeps what it
// made while its key stands. A window names its concerns once, in its cache_key(view).
struct RevisionKey {
	std::array<uint64_t, kViewConcernCount> counters{};
	bool operator==(const RevisionKey &other) const { return counters == other.counters; }
	bool operator!=(const RevisionKey &other) const { return counters != other.counters; }
};

inline RevisionKey revision_key(const ViewRevisions &revisions,
		std::initializer_list<ViewConcern> concerns) {
	RevisionKey key;
	for (const ViewConcern concern : concerns)
		key.counters[static_cast<size_t>(concern)] = revisions.of(concern);
	return key;
}

// The key of what either of two keys of one view reads.
inline RevisionKey operator|(RevisionKey a, const RevisionKey &b) {
	for (size_t i = 0; i < kViewConcernCount; ++i)
		if (b.counters[i] != 0) a.counters[i] = b.counters[i];
	return a;
}

} // namespace opennova::editor
