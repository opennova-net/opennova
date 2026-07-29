#ifndef OPENNOVA_ONED_EDIT_EDIT_HISTORY_H
#define OPENNOVA_ONED_EDIT_EDIT_HISTORY_H

#include <cstddef>
#include <utility>
#include <vector>

namespace opennova {
namespace edit {

// Shared undo/redo core for the OpenNova editor (ONED) workspaces.
//
// This is the snapshot/memento mechanic that the mission, music, terrain and
// (future) MNU editors otherwise each hand-roll: a pair of stacks holding whole
// or partial document SNAPSHOTS, a begin/commit bracket that coalesces a gesture
// into one step and suppresses no-ops, and an exact dirty flag measured against a
// "clean" baseline.
//
// It is deliberately Godot-agnostic (lives in libs/, depends on nothing) so it
// can be instantiated two ways:
//   * typed on a C++ value document (e.g. EditHistory<bms::File, BmsEqual>) for
//     in-process editors that want move + a domain equality with no boxing;
//   * typed on a boxed payload (e.g. EditHistory<godot::Variant, VariantEqual>)
//     behind a thin GDExtension RefCounted for the GDScript editors.
//
// Snapshot must be default-constructible, copyable and movable. Equal is a binary
// predicate deciding whether two snapshots are "the same document"; it is a TYPE
// PARAMETER (not Snapshot::operator==) on purpose, because domain equality is
// often a free function (mission uses opennova::bms::equal) rather than an
// operator, and a synthesised default could be wrong or expensive.
//
// Two usage modes share one class:
//   * Whole-document (mission/music/MNU): begin(live) captures the pre-edit
//     snapshot; commit(live) records it as one step iff the live document changed
//     (equal-gated, so a click / same-value edit records nothing). undo()/redo()
//     swap the live document with a stack top via swap_undo()/swap_redo() in O(1).
//     Dirty is exact: is_dirty(live) means the document differs from the baseline.
//   * Delta (terrain): the caller skips the begin/commit bracket and calls
//     push(snapshot) with its own {before, after} payload it applies itself.
template <class Snapshot, class Equal = std::equal_to<Snapshot>>
class EditHistory {
public:
	explicit EditHistory(std::size_t limit = 100, Equal equal = Equal())
		: limit_(limit ? limit : 1), equal_(std::move(equal)) {}

	// --- Whole-document begin/commit bracket -----------------------------

	// Open an edit session, snapshotting the current document. Inert if a session
	// is already open (so a run of edits inside one gesture coalesces into a
	// single step). The caller guards its own preconditions (e.g. "is a document
	// loaded?") before calling.
	void begin(const Snapshot &live) {
		if (editing_) {
			return;
		}
		pending_ = live;
		editing_ = true;
	}

	// Close the session opened by begin(). Records the held pre-edit snapshot as
	// one undo step ONLY if the document actually changed (equal-gated), clearing
	// the redo stack on a real change and dropping the oldest step past the cap.
	// Returns whether a step was recorded. Inert (returns false) with no open
	// session.
	bool commit(const Snapshot &live) {
		if (!editing_) {
			return false;
		}
		editing_ = false;
		bool recorded = false;
		if (!equal_(live, pending_)) {
			push_undo(std::move(pending_));
			redo_.clear();
			recorded = true;
		}
		pending_ = Snapshot();
		return recorded;
	}

	// --- Delta push (caller-applied snapshots) ---------------------------

	// Record a snapshot directly, skipping the equal-gate. For editors whose
	// documents are too large to copy whole each step and which instead store a
	// {before, after} delta they apply themselves (terrain). Clears redo + caps.
	void push(Snapshot snapshot) {
		push_undo(std::move(snapshot));
		redo_.clear();
	}

	// --- Undo / redo -----------------------------------------------------

	bool can_undo() const { return !undo_.empty(); }
	bool can_redo() const { return !redo_.empty(); }
	std::size_t undo_depth() const { return undo_.size(); }

	// Swap the live document with the top undo step in O(1) (no copy): the live
	// document moves onto the redo stack and the popped snapshot becomes live.
	// For typed whole-document editors that adopt the snapshot AS the document.
	// Returns false (and leaves live untouched) when there is nothing to undo.
	bool swap_undo(Snapshot &live) {
		if (undo_.empty()) {
			return false;
		}
		discard_open_session();
		redo_.push_back(std::move(live));
		live = std::move(undo_.back());
		undo_.pop_back();
		return true;
	}

	bool swap_redo(Snapshot &live) {
		if (redo_.empty()) {
			return false;
		}
		discard_open_session();
		undo_.push_back(std::move(live));
		live = std::move(redo_.back());
		redo_.pop_back();
		return true;
	}

	// Pop the top undo step onto the redo stack and return it via `out`,
	// WITHOUT touching any live document: the caller applies the snapshot
	// itself (terrain's {before, after} deltas, the fnt/mnu op records).
	// Discards any open begin() session first (same defence as swap_*).
	// Returns false (and leaves `out` untouched) when there is nothing to undo.
	bool pop_undo(Snapshot &out) {
		if (undo_.empty()) {
			return false;
		}
		discard_open_session();
		out = undo_.back();
		redo_.push_back(std::move(undo_.back()));
		undo_.pop_back();
		return true;
	}

	bool pop_redo(Snapshot &out) {
		if (redo_.empty()) {
			return false;
		}
		discard_open_session();
		out = redo_.back();
		undo_.push_back(std::move(redo_.back()));
		redo_.pop_back();
		return true;
	}

	// --- Dirty vs. a clean baseline --------------------------------------

	// Adopt the current document as the clean baseline (call at open / save /
	// new). After this, is_dirty(live) is exact and survives undo-stack trimming.
	void mark_clean(const Snapshot &live) {
		clean_baseline_ = live;
		has_clean_baseline_ = true;
	}

	// True iff the live document differs from the clean baseline. Before any
	// baseline is set there is nothing exact to compare against, so the caller's
	// `fallback` (e.g. a coarse "modified since load" flag) is returned instead.
	bool is_dirty(const Snapshot &live, bool fallback = false) const {
		if (!has_clean_baseline_) {
			return fallback;
		}
		return !equal_(live, clean_baseline_);
	}

	// --- Reset -----------------------------------------------------------

	// Drop the whole history + any open session. Does NOT touch the clean
	// baseline (clearing history at e.g. a save must not make the document look
	// dirty); call mark_clean() separately when the baseline should move.
	void clear() {
		undo_.clear();
		redo_.clear();
		pending_ = Snapshot();
		editing_ = false;
	}

private:
	// Drop any in-flight begin() session. An undo/redo that fires mid-gesture would otherwise leave
	// editing_/pending_ stale: the next commit() would compare against the pre-undo snapshot, push it
	// as a spurious step, and wipe the redo entry the swap just produced. Whole-document drivers are
	// expected to flush (commit) before undo/redo, but defend the invariant so a consumer that forgets
	// cannot corrupt the stacks.
	void discard_open_session() {
		editing_ = false;
		pending_ = Snapshot();
	}

	void push_undo(Snapshot snapshot) {
		undo_.push_back(std::move(snapshot));
		if (undo_.size() > limit_) {
			undo_.erase(undo_.begin());
		}
	}

	std::vector<Snapshot> undo_;
	std::vector<Snapshot> redo_;
	Snapshot pending_ = Snapshot();
	bool editing_ = false;
	Snapshot clean_baseline_ = Snapshot();
	bool has_clean_baseline_ = false;
	std::size_t limit_;
	Equal equal_;
};

} // namespace edit
} // namespace opennova

#endif // OPENNOVA_ONED_EDIT_EDIT_HISTORY_H
