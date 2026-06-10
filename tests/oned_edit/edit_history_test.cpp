// Unit test for the shared undo/redo core (opennova::edit::EditHistory).
//
// Exercises the two usage modes and the contract the mission editor depends on:
//   * begin/commit no-op suppression + gesture coalescing,
//   * swap_undo/swap_redo round-trip (the O(1) whole-document path),
//   * redo-tail truncation on a fresh edit,
//   * the step cap dropping the oldest,
//   * raw delta push() (the terrain/follow-up path),
//   * exact dirty-vs-baseline that survives cap-trimming,
//   * Equal being a real policy (not Snapshot::operator==).

#include <string>

#include "common/test_expect.h"
#include "oned_edit/edit_history.h"

using opennova::edit::EditHistory;

namespace {

// A snapshot whose equality deliberately ignores `note`, to prove Equal is a
// real policy rather than a defaulted member-wise compare.
struct Doc {
	int value = 0;
	std::string note;
};

struct DocEqualByValue {
	bool operator()(const Doc &a, const Doc &b) const { return a.value == b.value; }
};

} // namespace

int main() {
	// --- begin/commit: no-op suppression + coalescing -------------------
	{
		EditHistory<int> h;
		int doc = 5;
		h.begin(doc);
		TEST_EXPECT(!h.commit(doc)); // unchanged document records nothing
		TEST_EXPECT(h.undo_depth() == 0);
		TEST_EXPECT(!h.can_undo());

		h.begin(doc);
		doc = 6;
		TEST_EXPECT(h.commit(doc)); // a real change records one step
		TEST_EXPECT(h.undo_depth() == 1);
		TEST_EXPECT(h.can_undo());

		TEST_EXPECT(!h.commit(doc)); // commit with no open session is inert

		// A second begin() before commit() coalesces: pending stays the first.
		h.begin(doc); // pending = 6
		doc = 7;
		h.begin(doc); // inert, pending still 6
		doc = 8;
		TEST_EXPECT(h.commit(doc)); // one step spanning 6 -> 8
		TEST_EXPECT(h.undo_depth() == 2);
	}

	// --- swap_undo / swap_redo round-trip restores the live document ----
	{
		EditHistory<int> h;
		int doc = 1;
		h.begin(doc); doc = 2; h.commit(doc);
		h.begin(doc); doc = 3; h.commit(doc);
		TEST_EXPECT(doc == 3 && h.undo_depth() == 2);

		TEST_EXPECT(h.swap_undo(doc)); TEST_EXPECT(doc == 2);
		TEST_EXPECT(h.swap_undo(doc)); TEST_EXPECT(doc == 1);
		TEST_EXPECT(!h.can_undo());
		TEST_EXPECT(!h.swap_undo(doc)); // nothing left; live untouched
		TEST_EXPECT(doc == 1);

		TEST_EXPECT(h.swap_redo(doc)); TEST_EXPECT(doc == 2);
		TEST_EXPECT(h.swap_redo(doc)); TEST_EXPECT(doc == 3);
		TEST_EXPECT(!h.can_redo());
	}

	// --- a fresh commit after undo clears the redo tail -----------------
	{
		EditHistory<int> h;
		int doc = 0;
		h.begin(doc); doc = 1; h.commit(doc);
		h.begin(doc); doc = 2; h.commit(doc);
		TEST_EXPECT(h.swap_undo(doc)); // doc = 1, redo holds [2]
		TEST_EXPECT(h.can_redo());
		h.begin(doc); doc = 9; h.commit(doc); // branches off: redo tail dropped
		TEST_EXPECT(!h.can_redo());
		TEST_EXPECT(h.undo_depth() == 2);
	}

	// --- the step cap drops the oldest ----------------------------------
	{
		EditHistory<int> h(3); // keep at most 3 steps
		int doc = 0;
		for (int i = 1; i <= 5; ++i) {
			h.begin(doc);
			doc = i;
			h.commit(doc);
		}
		TEST_EXPECT(h.undo_depth() == 3); // capped, two oldest dropped
		TEST_EXPECT(h.swap_undo(doc)); TEST_EXPECT(doc == 4);
		TEST_EXPECT(h.swap_undo(doc)); TEST_EXPECT(doc == 3);
		TEST_EXPECT(h.swap_undo(doc)); TEST_EXPECT(doc == 2);
		TEST_EXPECT(!h.can_undo()); // only 3 retained
	}

	// --- raw delta push (skips the equal-gate) + redo-truncate ----------
	{
		EditHistory<int> h(2);
		h.push(10);
		h.push(20);
		h.push(30); // cap 2 -> keeps [20, 30]
		TEST_EXPECT(h.undo_depth() == 2);
		int doc = 99;
		TEST_EXPECT(h.swap_undo(doc)); TEST_EXPECT(doc == 30);
		TEST_EXPECT(h.can_redo());
		h.push(40); // a push clears redo
		TEST_EXPECT(!h.can_redo());
	}

	// --- pop_undo/pop_redo: caller-applied steps move between stacks ----
	{
		EditHistory<int> h;
		int step = -1;
		TEST_EXPECT(!h.pop_undo(step)); // empty history: nothing to pop
		TEST_EXPECT(!h.pop_redo(step));
		TEST_EXPECT(step == -1);        // out untouched on failure

		h.push(10);
		h.push(20);
		TEST_EXPECT(h.pop_undo(step)); TEST_EXPECT(step == 20);
		TEST_EXPECT(h.pop_undo(step)); TEST_EXPECT(step == 10);
		TEST_EXPECT(!h.can_undo());
		TEST_EXPECT(!h.pop_undo(step)); // drained; out keeps last value
		TEST_EXPECT(step == 10);

		// The same snapshots come back in order from the redo side.
		TEST_EXPECT(h.pop_redo(step)); TEST_EXPECT(step == 10);
		TEST_EXPECT(h.pop_redo(step)); TEST_EXPECT(step == 20);
		TEST_EXPECT(!h.can_redo());
		TEST_EXPECT(h.undo_depth() == 2); // both steps live on the undo stack again

		// A fresh push after popping clears the redo tail (branching).
		TEST_EXPECT(h.pop_undo(step));
		TEST_EXPECT(h.can_redo());
		h.push(30);
		TEST_EXPECT(!h.can_redo());
	}

	// --- pop_undo discards an open begin() session -----------------------
	{
		EditHistory<int> h;
		int doc = 1;
		h.begin(doc); doc = 2; h.commit(doc); // one recorded step
		h.begin(doc);                          // open session, pending = 2
		int step = 0;
		TEST_EXPECT(h.pop_undo(step));         // pops AND drops the session
		TEST_EXPECT(step == 1);
		doc = 9;
		TEST_EXPECT(!h.commit(doc));           // session was discarded: inert
		TEST_EXPECT(h.can_redo());             // and the redo entry survived
	}

	// --- pop_undo respects the cap (oldest dropped by push) -------------
	{
		EditHistory<int> h(2);
		h.push(1);
		h.push(2);
		h.push(3); // cap 2: keeps [2, 3]
		int step = 0;
		TEST_EXPECT(h.pop_undo(step)); TEST_EXPECT(step == 3);
		TEST_EXPECT(h.pop_undo(step)); TEST_EXPECT(step == 2);
		TEST_EXPECT(!h.pop_undo(step));
	}

	// --- exact dirty vs. a clean baseline, surviving cap-trim -----------
	{
		EditHistory<int> h(2);
		int doc = 100;
		h.mark_clean(doc);
		TEST_EXPECT(!h.is_dirty(doc));            // matches baseline
		TEST_EXPECT(!h.is_dirty(doc, true));      // baseline set -> fallback ignored

		h.begin(doc); doc = 101; h.commit(doc);
		TEST_EXPECT(h.is_dirty(doc));             // differs from baseline

		// Pile on edits past the cap; dirty is by VALUE vs the baseline, not by
		// stack depth, so it stays exact even though the stack was trimmed.
		h.begin(doc); doc = 102; h.commit(doc);
		h.begin(doc); doc = 103; h.commit(doc);
		TEST_EXPECT(h.undo_depth() == 2);
		doc = 100; // hand the value back to the baseline
		TEST_EXPECT(!h.is_dirty(doc));            // clears even though the stack can't reach it

		// No baseline -> is_dirty returns the caller-supplied fallback.
		EditHistory<int> fresh;
		TEST_EXPECT(!fresh.is_dirty(7));          // default fallback false
		TEST_EXPECT(fresh.is_dirty(7, true));     // caller fallback honoured
	}

	// --- Equal is a real policy: equality ignores `note` ----------------
	{
		EditHistory<Doc, DocEqualByValue> h;
		Doc doc{5, "a"};
		h.begin(doc);
		doc.note = "b"; // value unchanged
		TEST_EXPECT(!h.commit(doc)); // ignored field: nothing recorded
		TEST_EXPECT(h.undo_depth() == 0);

		h.begin(doc);
		doc.value = 6; // value changed
		TEST_EXPECT(h.commit(doc));
		TEST_EXPECT(h.undo_depth() == 1);
	}

	return 0;
}
