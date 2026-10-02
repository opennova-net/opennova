// What a drag of a menu window's handles writes (editor/preview/menu_layout_edit, ADR 0046
// S9k1), against the runtime's own POSITION solve (mnu_layout: the edges, the art's
// extents, the text-size anchor): a move shifts the edges the file writes and keeps the
// ones it leaves out out; a window sized by its art or its text keeps that sizing when a
// shift lands it; an axis the shift cannot land is written whole; each of the eight
// handles moves its own edges; moved edges snap on the screen's grid, under a parent's
// offset; a resize keeps a unit of size; and the edits say only what differs, each
// carrying the drag's gesture.

#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include <editor/model/edit.h>
#include <editor/preview/menu_layout_edit.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>

#include "common/test_expect.h"

using namespace opennova::editor;
namespace mnu = opennova::mnu;

namespace {

// What the runtime reads a window's size from besides its edges.
struct Art {
	int image_w = 0, image_h = 0;          // the largest IMAGE
	bool text = false;                      // a text-sized type with a label
	int text_w = 0, text_h = 0;
	std::string justify, vjustify;
};

// The runtime's three-stage solve, as MenuFrameCompiler::solve_local_rect runs it.
LayoutSolve solver(const Art &art) {
	return [art](const mnu::Position &p) {
		mnu::RectEdges r = mnu::position_rect(p.has_left, p.left, p.has_top, p.top, p.has_right, p.right, p.has_bottom,
		                                      p.bottom, art.image_w, art.image_h);
		if (art.text) r = mnu::adjust_rect_to_text_size(r, art.text_w, art.text_h, art.justify, art.vjustify);
		return r;
	};
}

mnu::Position edges(bool has_left, int left, bool has_top, int top, bool has_right, int right, bool has_bottom,
                    int bottom) {
	mnu::Position p;
	p.has_left = has_left;
	p.left = left;
	p.has_top = has_top;
	p.top = top;
	p.has_right = has_right;
	p.right = right;
	p.has_bottom = has_bottom;
	p.bottom = bottom;
	return p;
}

mnu::Position all(int left, int top, int right, int bottom) { return edges(true, left, true, top, true, right, true, bottom); }

LayoutStart start_of(const mnu::Position &authored, const LayoutSolve &solve, int parent_x = 0, int parent_y = 0) {
	LayoutStart start;
	start.authored = authored;
	start.local = solve(authored);
	start.parent_x = parent_x;
	start.parent_y = parent_y;
	return start;
}

bool same(const mnu::Position &a, const mnu::Position &b) {
	return a.has_left == b.has_left && a.has_top == b.has_top && a.has_right == b.has_right &&
	       a.has_bottom == b.has_bottom && (!a.has_left || a.left == b.left) && (!a.has_top || a.top == b.top) &&
	       (!a.has_right || a.right == b.right) && (!a.has_bottom || a.bottom == b.bottom);
}

bool rect_is(const mnu::RectEdges &r, int left, int top, int right, int bottom) {
	return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}

// The plan, and that the runtime's solve lands it on the drag's target.
bool plan(const LayoutStart &start, LayoutHandle handle, int dx, int dy, int grid, const LayoutSolve &solve,
          mnu::Position &out) {
	const LayoutDrag drag{handle, dx, dy, grid};
	if (!plan_layout_drag(start, drag, solve, out)) return false;
	const mnu::RectEdges target = layout_drag_target(start, drag);
	const mnu::RectEdges landed = solve(out);
	return landed.left == target.left && landed.top == target.top && landed.right == target.right &&
	       landed.bottom == target.bottom;
}

} // namespace

// Both edges written: a move shifts all four, snapped on the grid by the leading edges.
static int test_move_both_edges() {
	const LayoutSolve solve = solver(Art());
	const LayoutStart start = start_of(all(100, 100, 300, 200), solve);
	mnu::Position out;
	TEST_EXPECT(plan(start, LayoutHandle::Move, 13, 5, kLayoutGrid, solve, out));
	TEST_EXPECT(same(out, all(112, 104, 312, 204))); // 113 -> 112, 105 -> 104: the size kept
	TEST_EXPECT(plan(start, LayoutHandle::Move, 13, 5, 0, solve, out));
	TEST_EXPECT(same(out, all(113, 105, 313, 205)));
	TEST_EXPECT(plan(start, LayoutHandle::Move, 0, 0, 0, solve, out) && same(out, start.authored));
	std::printf("test_move_both_edges passed\n");
	return 0;
}

// A window its art sizes (left and top only): the shift lands it, the art still sizes it.
static int test_move_texture_sized() {
	Art art;
	art.image_w = 64;
	art.image_h = 32;
	const LayoutSolve solve = solver(art);
	const LayoutStart start = start_of(edges(true, 10, true, 20, false, 0, false, 0), solve);
	TEST_EXPECT(rect_is(start.local, 10, 20, 74, 52));
	mnu::Position out;
	TEST_EXPECT(plan(start, LayoutHandle::Move, 5, 3, 0, solve, out));
	TEST_EXPECT(same(out, edges(true, 15, true, 23, false, 0, false, 0)));
	std::printf("test_move_texture_sized passed\n");
	return 0;
}

// Right and bottom only: the rect spans from 0, so the move writes the leading edges.
static int test_move_right_only() {
	const LayoutSolve solve = solver(Art());
	const LayoutStart start = start_of(edges(false, 0, false, 0, true, 200, true, 100), solve);
	TEST_EXPECT(rect_is(start.local, 0, 0, 200, 100));
	mnu::Position out;
	TEST_EXPECT(plan(start, LayoutHandle::Move, 10, 20, 0, solve, out));
	TEST_EXPECT(same(out, all(10, 20, 210, 120)));
	// A vertical move alone leaves the left edge out.
	TEST_EXPECT(plan(start, LayoutHandle::Move, 0, 20, 0, solve, out));
	TEST_EXPECT(same(out, edges(false, 0, true, 20, true, 200, true, 120)));
	std::printf("test_move_right_only passed\n");
	return 0;
}

// Past 0 the missing trailing edge reads 0 and no longer leaves the art to size the window:
// that axis is written whole; negative on both axes, all four.
static int test_negative_move() {
	Art art;
	art.image_w = 64;
	art.image_h = 32;
	const LayoutSolve solve = solver(art);
	const LayoutStart start = start_of(edges(true, 10, true, 20, false, 0, false, 0), solve);
	mnu::Position out;
	TEST_EXPECT(plan(start, LayoutHandle::Move, -30, 0, 0, solve, out));
	TEST_EXPECT(same(out, edges(true, -20, true, 20, true, 44, false, 0)));
	TEST_EXPECT(plan(start, LayoutHandle::Move, -30, -30, 0, solve, out));
	TEST_EXPECT(same(out, all(-20, -10, 44, 22)));
	std::printf("test_negative_move passed\n");
	return 0;
}

// Each handle moves its own edges, the rest as they were.
static int test_each_handle() {
	const LayoutSolve solve = solver(Art());
	const LayoutStart start = start_of(all(100, 100, 300, 200), solve);
	struct Case {
		LayoutHandle handle;
		mnu::Position expected;
	};
	const Case cases[] = {
	        {LayoutHandle::Left, all(110, 100, 300, 200)},        {LayoutHandle::Right, all(100, 100, 310, 200)},
	        {LayoutHandle::Top, all(100, 120, 300, 200)},         {LayoutHandle::Bottom, all(100, 100, 300, 220)},
	        {LayoutHandle::TopLeft, all(110, 120, 300, 200)},     {LayoutHandle::TopRight, all(100, 120, 310, 200)},
	        {LayoutHandle::BottomLeft, all(110, 100, 300, 220)},  {LayoutHandle::BottomRight, all(100, 100, 310, 220)},
	};
	for (const Case &c : cases) {
		mnu::Position out;
		TEST_EXPECT(plan(start, c.handle, 10, 20, 0, solve, out));
		TEST_EXPECT(same(out, c.expected));
		LayoutHandle back;
		TEST_EXPECT(layout_handle_from_token(layout_handle_token(c.handle), back) && back == c.handle);
	}
	LayoutHandle none;
	TEST_EXPECT(!layout_handle_from_token("middle", none));
	// A resize of a window its text sizes around a centred anchor writes the whole axis.
	Art art;
	art.text = true;
	art.text_w = 100;
	art.text_h = 16;
	art.justify = "center";
	const LayoutSolve text = solver(art);
	const LayoutStart label = start_of(edges(true, 400, true, 100, false, 0, false, 0), text);
	TEST_EXPECT(rect_is(label.local, 350, 100, 450, 116));
	mnu::Position out;
	TEST_EXPECT(plan(label, LayoutHandle::Move, 10, 0, 0, text, out));
	TEST_EXPECT(same(out, edges(true, 410, true, 100, false, 0, false, 0))); // the anchor moves
	TEST_EXPECT(plan(label, LayoutHandle::Right, 20, 0, 0, text, out));
	TEST_EXPECT(same(out, edges(true, 350, true, 100, true, 470, false, 0)));
	std::printf("test_each_handle passed\n");
	return 0;
}

// The grid is the screen's: under a parent at (5, 3) a moved edge lands where the screen's
// grid is; a resize snaps only the edge it drags.
static int test_absolute_grid() {
	const LayoutSolve solve = solver(Art());
	const LayoutStart start = start_of(all(20, 20, 120, 70), solve, 5, 3);
	mnu::Position out;
	TEST_EXPECT(plan(start, LayoutHandle::Move, 1, 0, kLayoutGrid, solve, out));
	TEST_EXPECT((out.left + 5) % 8 == 0 && (out.top + 3) % 8 == 0);
	TEST_EXPECT(same(out, all(19, 21, 119, 71)));
	TEST_EXPECT(plan(start, LayoutHandle::Right, 0, 0, kLayoutGrid, solve, out));
	TEST_EXPECT(same(out, all(20, 20, 123, 70)) && (out.right + 5) % 8 == 0);
	// Below the screen's origin the grid runs on (-8, not 0).
	const LayoutStart low = start_of(all(-10, 0, 10, 10), solve);
	TEST_EXPECT(plan(low, LayoutHandle::Left, 0, 0, kLayoutGrid, solve, out) && out.left == -8);
	std::printf("test_absolute_grid passed\n");
	return 0;
}

// A resize keeps a unit of size; a window with no area has nothing to move.
static int test_minimum_size() {
	const LayoutSolve solve = solver(Art());
	const LayoutStart start = start_of(all(100, 100, 300, 200), solve);
	mnu::Position out;
	TEST_EXPECT(plan(start, LayoutHandle::Right, -500, 0, kLayoutGrid, solve, out) && same(out, all(100, 100, 101, 200)));
	TEST_EXPECT(plan(start, LayoutHandle::Left, 500, 0, 0, solve, out) && same(out, all(299, 100, 300, 200)));
	TEST_EXPECT(plan(start, LayoutHandle::BottomRight, -500, -500, 0, solve, out) &&
	            same(out, all(100, 100, 101, 101)));
	const LayoutStart empty = start_of(all(100, 100, 100, 100), solve);
	TEST_EXPECT(!plan_layout_drag(empty, LayoutDrag{LayoutHandle::Move, 10, 10, 0}, solve, out));
	std::printf("test_minimum_size passed\n");
	return 0;
}

// The edits: an edge written again then set, an edge set, an edge an earlier step wrote
// left out again with its value back; each carrying the gesture; none when nothing differs.
static int test_layout_edits() {
	const NodeAddress window{3, 1, 9};
	const mnu::Position from = edges(true, 10, true, 20, false, 0, false, 0);
	const mnu::Position to = edges(true, 15, true, 20, true, 79, false, 0);
	const std::vector<Edit> edits = layout_edits(window, from, to, 42);
	TEST_EXPECT(edits.size() == 3);
	TEST_EXPECT(edits[0].operation == EditOperation::Set && edits[0].field == "position.left" &&
	            std::get<int64_t>(edits[0].value) == 15);
	TEST_EXPECT(edits[1].operation == EditOperation::Write && edits[1].field == "position.right");
	TEST_EXPECT(edits[2].operation == EditOperation::Set && edits[2].field == "position.right" &&
	            std::get<int64_t>(edits[2].value) == 79);
	for (const Edit &edit : edits) TEST_EXPECT(edit.gesture == 42 && edit.address == window && !edit.coalesce);
	const std::vector<Edit> back = layout_edits(window, to, from, 42);
	TEST_EXPECT(back.size() == 3);
	TEST_EXPECT(back[0].operation == EditOperation::Set && back[0].field == "position.left");
	TEST_EXPECT(back[1].operation == EditOperation::Set && back[1].field == "position.right" &&
	            std::get<int64_t>(back[1].value) == 0);
	TEST_EXPECT(back[2].operation == EditOperation::Clear && back[2].field == "position.right");
	TEST_EXPECT(layout_edits(window, to, to, 42).empty());
	std::printf("test_layout_edits passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_move_both_edges();
	failures += test_move_texture_sized();
	failures += test_move_right_only();
	failures += test_negative_move();
	failures += test_each_handle();
	failures += test_absolute_grid();
	failures += test_minimum_size();
	failures += test_layout_edits();
	return failures == 0 ? 0 : 1;
}
