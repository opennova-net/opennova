// The raster core's planes and history (ADR 0046 S25, lane A1): a plane in copy-on-write tiles (a copy shares
// them, a write copies the tiles it touches, from_bytes and bytes() round-trip, edge tiles cut); the history's
// steps of tile swaps (undo and redo put the tiles back by pointer), a gesture's batches folded into one step
// keeping each tile's first before and latest after (never over the saved checkpoint), a whole change a step
// of its own, a bake's landing joining its stroke's step (amend_last, refused with a redo pending, over the
// saved checkpoint or after a whole change), a cancelled gesture dropped, the step marks (a fresh serial for
// each batch made or folded, the gesture as the link), changes_since over any kept state (the tiles and side
// parts between; cannot say across a whole change or for a state discarded), the budget, frozen().
#include <editor/model/raster_history.h>
#include <editor/model/raster_plane.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "common/test_expect.h"
#include "editor/raster_blob_document.h"

using namespace opennova::editor;
using editor_test::RasterNote;

namespace {

RasterState make_state(int w = 130, int h = 70, RasterFormat format = RasterFormat::U16) {
	RasterState state;
	state.planes = {RasterPlane(format, w, h)};
	state.side = std::make_shared<RasterNote>("first");
	return state;
}

// A change writing `value` at each texel of `points` on plane 0 of `state` (the history records it after).
RasterChange write(RasterState &state, const std::vector<std::pair<int, int>> &points, uint16_t value) {
	RasterWriter writer(state.planes[0], 0);
	for (const auto &[x, y] : points) writer.set_u16(x, y, value);
	RasterChange change;
	change.swaps = writer.finish();
	return change;
}

int test_plane() {
	RasterPlane zeros(RasterFormat::U16, 130, 70);
	TEST_EXPECT(zeros.tiles_across() == 3 && zeros.tiles_down() == 2 && zeros.tile_count() == 6);
	TEST_EXPECT(zeros.tile(0) == zeros.tile(5)); // one zero tile shared
	std::vector<uint8_t> texels(130 * 70 * 2);
	for (size_t i = 0; i < texels.size(); ++i) texels[i] = uint8_t(i * 7);
	const RasterPlane plane = RasterPlane::from_bytes(RasterFormat::U16, 130, 70, texels.data());
	TEST_EXPECT(plane.bytes() == texels);
	TEST_EXPECT(plane.u16(129, 69) == uint16_t(texels[(69 * 130 + 129) * 2] | (texels[(69 * 130 + 129) * 2 + 1] << 8)));
	std::vector<uint8_t> rect(5 * 3 * 2);
	plane.copy_rect(62, 30, 5, 3, rect.data()); // across a tile edge
	TEST_EXPECT(rect[0] == texels[(30 * 130 + 62) * 2] && rect[4 * 2] == texels[(30 * 130 + 66) * 2]);

	RasterPlane copy = plane;
	TEST_EXPECT(copy.shares_tiles_with(plane));
	RasterWriter writer(copy, 3);
	writer.set_u16(64, 0, 0xBEEF);
	writer.set_u16(65, 1, 0xBEEF);
	writer.set_u16(0, 64, 1);
	const std::vector<TileSwap> swaps = writer.finish();
	TEST_EXPECT(swaps.size() == 2 && swaps[0].plane == 3 && swaps[0].tile == 1 && swaps[1].tile == 3);
	TEST_EXPECT(swaps[0].before == plane.tile(1) && swaps[0].after == copy.tile(1) && swaps[0].after != swaps[0].before);
	TEST_EXPECT(copy.u16(64, 0) == 0xBEEF && plane.u16(64, 0) != 0xBEEF); // the original untouched
	TEST_EXPECT(copy.tile(0) == plane.tile(0) && copy.tile(2) == plane.tile(2)); // untouched tiles still shared
	TEST_EXPECT(!copy.same_texels(plane) && plane.same_texels(RasterPlane::from_bytes(RasterFormat::U16, 130, 70, texels.data())));
	TEST_EXPECT(raster_texel_bytes(RasterFormat::Rgb8) == 3 && raster_texel_bytes(RasterFormat::Rgba8) == 4);
	return 0;
}

int test_steps_and_folding() {
	RasterState state = make_state();
	RasterHistory history;
	const auto zero = state.planes[0].tile(0);
	history.commit(write(state, {{1, 1}}, 10), 0);
	const StepMark first = history.undo_mark();
	TEST_EXPECT(history.revision() == 1 && history.dirty() && history.steps() == 1 && first.serial && first.link == 0);
	// A gesture's batches fold into one step: the tile's first before kept, its latest after.
	history.commit(write(state, {{1, 1}, {100, 1}}, 20), 77);
	history.commit(write(state, {{1, 1}}, 30), 77);
	history.commit(write(state, {{2, 66}}, 40), 77);
	TEST_EXPECT(history.steps() == 2 && history.revision() == 4);
	const StepMark folded = history.undo_mark();
	TEST_EXPECT(folded.link == 77 && folded.serial > first.serial);
	history.undo(state);
	TEST_EXPECT(history.revision() == 1 && state.planes[0].u16(1, 1) == 10 && state.planes[0].u16(100, 1) == 0 &&
	            state.planes[0].u16(2, 66) == 0);
	history.redo(state);
	TEST_EXPECT(history.revision() == 4 && state.planes[0].u16(1, 1) == 30 && state.planes[0].u16(2, 66) == 40);
	history.undo(state);
	history.undo(state);
	TEST_EXPECT(history.revision() == 0 && !history.dirty() && state.planes[0].tile(0) == zero);
	history.redo(state);
	// A new batch discards the redo branch.
	history.commit(write(state, {{3, 3}}, 5), 0);
	TEST_EXPECT(!history.can_redo() && history.steps() == 2 && history.revision() == 5);

	// The saved checkpoint: a gesture never folds over it.
	history.mark_saved();
	history.commit(write(state, {{4, 4}}, 6), 88);
	history.commit(write(state, {{4, 4}}, 7), 88);
	TEST_EXPECT(history.steps() == 3); // folded together, but not into the saved step
	history.mark_saved();
	history.commit(write(state, {{4, 4}}, 8), 88);
	TEST_EXPECT(history.steps() == 4);
	history.end_edit_group();
	history.commit(write(state, {{4, 4}}, 9), 88);
	TEST_EXPECT(history.steps() == 5); // the group ended
	return 0;
}

int test_side_whole_amend_cancel() {
	RasterState state = make_state();
	RasterHistory history;
	// The side state: its first before and latest after, the parts between.
	RasterChange note;
	note.side_before = state.side;
	note.side_after = std::make_shared<RasterNote>("second");
	state.side = note.side_after;
	history.commit(std::move(note), 0);
	std::vector<std::pair<int, int>> tiles;
	uint32_t parts = 0;
	TEST_EXPECT(history.changes_since(0, tiles, parts) && tiles.empty() && parts == 1u);
	history.undo(state);
	TEST_EXPECT(static_cast<const RasterNote *>(state.side.get())->text == "first");
	history.redo(state);

	// A stroke, then its bake landing inside its step.
	history.commit(write(state, {{10, 10}}, 1), 5);
	history.end_edit_group(); // the stroke ended
	TEST_EXPECT(history.can_amend());
	const uint64_t stroke = history.revision();
	RasterChange bake = write(state, {{10, 10}, {70, 10}}, 2);
	bake.side_before = state.side;
	bake.side_after = std::make_shared<RasterNote>("baked");
	state.side = bake.side_after;
	TEST_EXPECT(history.amend_last(std::move(bake)));
	TEST_EXPECT(history.revision() > stroke && history.steps() == 2 && history.undo_mark().link == 5);
	TEST_EXPECT(history.changes_since(stroke, tiles, parts) && tiles.size() == 2 && parts == 1u);
	history.undo(state); // one undo takes the stroke and its bake
	TEST_EXPECT(state.planes[0].u16(10, 10) == 0 && state.planes[0].u16(70, 10) == 0 &&
	            static_cast<const RasterNote *>(state.side.get())->text == "second");
	TEST_EXPECT(!history.can_amend()); // something to redo
	history.redo(state);
	TEST_EXPECT(state.planes[0].u16(70, 10) == 2);
	history.mark_saved();
	const RasterChange stray = write(state, {{1, 1}}, 3);
	TEST_EXPECT(!history.can_amend() && !history.amend_last(stray)); // over the saved checkpoint
	for (const TileSwap &swap : stray.swaps) state.planes[0].set_tile(swap.tile, swap.before); // the caller's to put back

	// A whole change: a step of its own, which changes_since cannot say regions of.
	const uint64_t before_whole = history.revision();
	RasterChange whole;
	whole.whole = true;
	whole.whole_before = state;
	RasterState grown = state;
	grown.planes = {RasterPlane(RasterFormat::U16, 200, 70)};
	whole.whole_after = grown;
	state = grown;
	history.commit(std::move(whole), 5);
	TEST_EXPECT(state.planes[0].width() == 200 && !history.can_amend());
	TEST_EXPECT(!history.changes_since(before_whole, tiles, parts));
	history.commit(write(state, {{199, 1}}, 4), 5); // never folds into the whole step
	TEST_EXPECT(history.undo_mark().link == 5 && history.steps() == 4);
	history.undo(state);
	history.undo(state);
	TEST_EXPECT(state.planes[0].width() == 130);
	history.redo(state);
	TEST_EXPECT(state.planes[0].width() == 200);

	// A cancelled gesture: its open step dropped, no redo of it.
	RasterHistory cancel;
	RasterState fresh = make_state();
	cancel.commit(write(fresh, {{1, 1}}, 1), 0);
	cancel.commit(write(fresh, {{2, 2}}, 2), 9);
	cancel.commit(write(fresh, {{3, 3}}, 3), 9);
	TEST_EXPECT(!cancel.drop_open(8, fresh) && cancel.drop_open(9, fresh));
	TEST_EXPECT(cancel.revision() == 1 && !cancel.can_redo() && fresh.planes[0].u16(2, 2) == 0 &&
	            fresh.planes[0].u16(1, 1) == 1);
	TEST_EXPECT(!cancel.drop_open(9, fresh));
	return 0;
}

int test_changes_since_budget_frozen() {
	RasterState state = make_state();
	RasterHistory history;
	history.commit(write(state, {{1, 1}}, 1), 0);        // r1, tile (0,0)
	history.commit(write(state, {{70, 1}}, 2), 3);       // r2, tile 1
	history.commit(write(state, {{1, 65}}, 3), 3);       // r3, tile 3 (folded)
	history.commit(write(state, {{129, 69}}, 4), 0);     // r4, tile 5
	std::vector<std::pair<int, int>> tiles;
	uint32_t parts = 0;
	TEST_EXPECT(history.changes_since(4, tiles, parts) && tiles.empty());
	TEST_EXPECT(history.changes_since(2, tiles, parts) && tiles == (std::vector<std::pair<int, int>>{{0, 3}, {0, 5}}));
	TEST_EXPECT(history.changes_since(0, tiles, parts) && tiles.size() == 4);
	history.undo(state);
	history.undo(state); // at r1
	TEST_EXPECT(history.changes_since(3, tiles, parts) && tiles == (std::vector<std::pair<int, int>>{{0, 1}, {0, 3}}));
	TEST_EXPECT(history.changes_since(4, tiles, parts) && tiles.size() == 3);
	history.commit(write(state, {{5, 5}}, 9), 0); // the redo branch discarded
	TEST_EXPECT(!history.changes_since(3, tiles, parts) && !history.changes_since(999, tiles, parts));

	// The budget gives up the oldest steps, never the last.
	HistoryBudget small;
	small.bytes = 20000;
	RasterHistory budgeted(small);
	RasterState s = make_state();
	for (int i = 0; i < 6; ++i) budgeted.commit(write(s, {{i * 20, 1}}, uint16_t(i + 1)), 0);
	TEST_EXPECT(budgeted.steps() < 6 && budgeted.steps() >= 1 && budgeted.bytes() <= small.bytes + 20000);
	TEST_EXPECT(!budgeted.changes_since(0, tiles, parts));

	const RasterHistory frozen = history.frozen();
	TEST_EXPECT(frozen.revision() == history.revision() && !frozen.can_undo() && !frozen.can_redo() &&
	            frozen.dirty() == history.dirty() && frozen.changes_since(history.revision(), tiles, parts));
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_plane();
	failures += test_steps_and_folding();
	failures += test_side_whole_amend_cancel();
	failures += test_changes_since_budget_frozen();
	if (failures == 0) std::printf("editor_raster_history: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
