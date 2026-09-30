// Arranging several menu windows (editor/preview/menu_arrange, ADR 0046 S9k2) against the
// runtime's own POSITION solve: each alignment lands every window on the primary's edge or
// centre on the screen, under parents at other origins and for a window its art sizes (its
// trailing edges stay out of the file); distributing keeps the first and the last and makes
// the gaps equal; a window another of them holds is written relative to where that one
// lands (the primary included), and a window with no area that would have to move stops
// the plan; the four order changes and the Moves that make them; and
// on a real menu through its compiled screen: a later sibling is the one the runtime's hit
// walk finds (its draw walk paints it last), Bring to front changes that in one undo step,
// an alignment is one batch, a move of several windows takes each once (a window the
// moved one holds rides along), a drag as the preview starts one (a selected window moves
// the selection, another window moves alone, a resize holds its window alone), and the
// refusals.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/edit.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_screen_render.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
namespace mnu = opennova::mnu;

namespace {

// What the runtime reads a window's size from besides its edges: its largest IMAGE.
LayoutSolve solver(int image_w = 0, int image_h = 0) {
	return [image_w, image_h](const mnu::Position &p) {
		return mnu::position_rect(p.has_left, p.left, p.has_top, p.top, p.has_right, p.right, p.has_bottom, p.bottom,
		                          image_w, image_h);
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

ArrangeWindow window(const mnu::Position &authored, const LayoutSolve &solve, int parent_x = 0, int parent_y = 0,
                     int holder = -1) {
	ArrangeWindow out;
	out.start.authored = authored;
	out.start.local = solve(authored);
	out.start.parent_x = parent_x;
	out.start.parent_y = parent_y;
	out.solve = solve;
	out.holder = holder;
	return out;
}

bool same(const mnu::Position &a, const mnu::Position &b) {
	return a.has_left == b.has_left && a.has_top == b.has_top && a.has_right == b.has_right &&
	       a.has_bottom == b.has_bottom && (!a.has_left || a.left == b.left) && (!a.has_top || a.top == b.top) &&
	       (!a.has_right || a.right == b.right) && (!a.has_bottom || a.bottom == b.bottom);
}

// Where a planned window lands on the screen, its parent at (parent_x, parent_y).
mnu::RectEdges landed(const ArrangeWindow &window, const mnu::Position &position, int parent_x, int parent_y) {
	const mnu::RectEdges local = window.solve(position);
	return {local.left + parent_x, local.top + parent_y, local.right + parent_x, local.bottom + parent_y};
}

bool rect_is(const mnu::RectEdges &r, int left, int top, int right, int bottom) {
	return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}

// The order `moves` leave `from` in, applied as Document::apply applies Moves.
std::vector<NodeId> applied(std::vector<NodeId> from, const std::vector<std::pair<NodeId, size_t>> &moves) {
	for (const auto &move : moves) {
		from.erase(std::find(from.begin(), from.end(), move.first));
		from.insert(from.begin() + std::ptrdiff_t(std::min(move.second, from.size())), move.first);
	}
	return from;
}

} // namespace

static int test_tokens() {
	for (const ArrangeOp op : kArrangeOps) {
		ArrangeOp back = ArrangeOp::SendToBack;
		TEST_EXPECT(arrange_op_from_token(arrange_op_token(op), back) && back == op);
		TEST_EXPECT(*arrange_op_label(op) != 0);
	}
	ArrangeOp none;
	TEST_EXPECT(!arrange_op_from_token("align_middle", none));
	TEST_EXPECT(arrange_minimum(ArrangeOp::AlignLeft) == 2 && arrange_minimum(ArrangeOp::DistributeVertically) == 3 &&
	            arrange_minimum(ArrangeOp::BringForward) == 1);
	TEST_EXPECT(arrange_moves(ArrangeOp::AlignVerticalCenters) && !arrange_moves(ArrangeOp::SendToBack));
	std::printf("test_tokens passed\n");
	return 0;
}

// The primary A at (100, 100)-(200, 150); B under a parent at (50, 20); C its art sizes
// (64 x 32, left and top written): each alignment lands them on A's edge or centre on the
// screen, and C keeps its right and bottom out of the file.
static int test_align() {
	const std::vector<ArrangeWindow> windows = {
	        window(all(100, 100, 200, 150), solver()),
	        window(all(10, 10, 60, 40), solver(), 50, 20),
	        window(edges(true, 300, true, 200, false, 0, false, 0), solver(64, 32)),
	};
	TEST_EXPECT(rect_is(landed(windows[2], windows[2].start.authored, 0, 0), 300, 200, 364, 232));
	struct Case {
		ArrangeOp op;
		mnu::Position b, c;
	};
	const Case cases[] = {
	        {ArrangeOp::AlignLeft, all(50, 10, 100, 40), edges(true, 100, true, 200, false, 0, false, 0)},
	        {ArrangeOp::AlignRight, all(100, 10, 150, 40), edges(true, 136, true, 200, false, 0, false, 0)},
	        {ArrangeOp::AlignTop, all(10, 80, 60, 110), edges(true, 300, true, 100, false, 0, false, 0)},
	        {ArrangeOp::AlignBottom, all(10, 100, 60, 130), edges(true, 300, true, 118, false, 0, false, 0)},
	        {ArrangeOp::AlignHorizontalCenters, all(75, 10, 125, 40), edges(true, 118, true, 200, false, 0, false, 0)},
	        {ArrangeOp::AlignVerticalCenters, all(10, 90, 60, 120), edges(true, 300, true, 109, false, 0, false, 0)},
	};
	for (const Case &c : cases) {
		std::vector<mnu::Position> out;
		TEST_EXPECT(plan_arrange(c.op, windows, 0, out) && out.size() == 3);
		TEST_EXPECT(same(out[0], windows[0].start.authored)); // the primary stays
		TEST_EXPECT(same(out[1], c.b));
		TEST_EXPECT(same(out[2], c.c));
	}
	// Where it lands, checked through the solve: B's left edge on A's.
	std::vector<mnu::Position> out;
	TEST_EXPECT(plan_arrange(ArrangeOp::AlignLeft, windows, 0, out));
	TEST_EXPECT(landed(windows[1], out[1], 50, 20).left == 100 && landed(windows[2], out[2], 0, 0).left == 100);
	// Aligned to another primary: A and B to C's bottom edge (232).
	TEST_EXPECT(plan_arrange(ArrangeOp::AlignBottom, windows, 2, out));
	TEST_EXPECT(same(out[0], all(100, 182, 200, 232)) && same(out[1], all(10, 182, 60, 212)) && same(out[2], windows[2].start.authored));
	// An odd difference of centres rounds toward the top.
	const std::vector<ArrangeWindow> odd = {window(all(0, 0, 100, 11), solver()), window(all(200, 0, 300, 10), solver())};
	TEST_EXPECT(plan_arrange(ArrangeOp::AlignVerticalCenters, odd, 0, out) && same(out[1], all(200, 0, 300, 10)));
	TEST_EXPECT(!plan_arrange(ArrangeOp::AlignLeft, {windows[0]}, 0, out));
	TEST_EXPECT(!plan_arrange(ArrangeOp::AlignLeft, windows, 3, out));
	TEST_EXPECT(!plan_arrange(ArrangeOp::BringToFront, windows, 0, out));
	std::printf("test_align passed\n");
	return 0;
}

// Four windows spread left to right (given out of order): the first and the last stay,
// the gaps between neighbours equal (76 2/3 units, rounded per window); three spread top
// to bottom under a parent's offset, one sized by its art.
static int test_distribute() {
	const std::vector<ArrangeWindow> row = {
	        window(all(200, 0, 210, 10), solver()), // third
	        window(all(0, 0, 20, 10), solver()),    // first
	        window(all(300, 0, 330, 10), solver()), // last
	        window(all(50, 0, 90, 10), solver()),   // second
	};
	std::vector<mnu::Position> out;
	TEST_EXPECT(plan_arrange(ArrangeOp::DistributeHorizontally, row, 0, out) && out.size() == 4);
	TEST_EXPECT(same(out[1], all(0, 0, 20, 10)) && same(out[2], all(300, 0, 330, 10)));
	TEST_EXPECT(same(out[3], all(97, 0, 137, 10)) && same(out[0], all(213, 0, 223, 10)));
	const std::vector<ArrangeWindow> column = {
	        window(all(0, 0, 50, 20), solver(), 100, 100),                                  // 100..120
	        window(edges(true, 0, true, 60, false, 0, false, 0), solver(40, 10), 100, 100), // 160..170
	        window(all(0, 300, 50, 340), solver(), 100, 100),                               // 400..440
	};
	TEST_EXPECT(plan_arrange(ArrangeOp::DistributeVertically, column, 2, out));
	// The gaps: (440 - 100 - 70) / 2 = 135: the middle one's top at 120 + 135 = 255 (local 155).
	TEST_EXPECT(same(out[0], column[0].start.authored) && same(out[2], column[2].start.authored));
	TEST_EXPECT(same(out[1], edges(true, 0, true, 155, false, 0, false, 0)));
	TEST_EXPECT(!plan_arrange(ArrangeOp::DistributeVertically, {column[0], column[1]}, 0, out));
	std::printf("test_distribute passed\n");
	return 0;
}

// A window another of the list holds lands where the op puts it, written relative to where
// its holder lands; a holder of the primary moves while the primary stays on the screen.
static int test_holders() {
	// H at (100, 100)-(300, 300) holds K at (120, 120)-(160, 160); the primary Q at x 50.
	std::vector<ArrangeWindow> windows = {
	        window(all(100, 100, 300, 300), solver()),
	        window(all(20, 20, 60, 60), solver(), 100, 100, 0),
	        window(all(50, 400, 90, 450), solver()),
	};
	std::vector<mnu::Position> out;
	TEST_EXPECT(plan_arrange(ArrangeOp::AlignLeft, windows, 2, out));
	TEST_EXPECT(same(out[0], all(50, 100, 250, 300)));
	TEST_EXPECT(same(out[1], all(0, 20, 40, 60))); // H moved by -50: K's own -70 less that
	TEST_EXPECT(landed(windows[1], out[1], 50, 100).left == 50);
	// K the primary: H aligns its left edge to K's (120), K stays where it shows.
	TEST_EXPECT(plan_arrange(ArrangeOp::AlignLeft, windows, 1, out));
	TEST_EXPECT(same(out[0], all(120, 100, 320, 300)) && same(out[1], all(0, 20, 40, 60)));
	TEST_EXPECT(landed(windows[1], out[1], 120, 100).left == 120);
	// H with no height holds K: H cannot be written moved, so nothing is planned (K would
	// land off by H's move); H the primary stays, so K aligns to it.
	windows[0] = window(all(100, 100, 300, 100), solver());
	size_t stuck = SIZE_MAX;
	TEST_EXPECT(!plan_arrange(ArrangeOp::AlignLeft, windows, 2, out, &stuck) && stuck == 0 && out.empty());
	TEST_EXPECT(plan_arrange(ArrangeOp::AlignLeft, windows, 0, out, &stuck) && stuck == SIZE_MAX);
	TEST_EXPECT(same(out[0], windows[0].start.authored) && same(out[2], all(100, 400, 140, 450)));
	std::printf("test_holders passed\n");
	return 0;
}

// The four order changes over siblings 1..5 with 2 and 4 chosen, and the Moves that make
// each order (applied one after another as Document::apply applies them).
static int test_order() {
	const std::vector<NodeId> siblings = {1, 2, 3, 4, 5};
	const std::vector<NodeId> chosen = {2, 4};
	struct Case {
		ArrangeOp op;
		std::vector<NodeId> expected;
	};
	const Case cases[] = {
	        {ArrangeOp::BringToFront, {1, 3, 5, 2, 4}},
	        {ArrangeOp::SendToBack, {2, 4, 1, 3, 5}},
	        {ArrangeOp::BringForward, {1, 3, 2, 5, 4}},
	        {ArrangeOp::SendBackward, {2, 1, 4, 3, 5}},
	};
	for (const Case &c : cases) {
		const std::vector<NodeId> order = arrange_order(c.op, siblings, chosen);
		TEST_EXPECT(order == c.expected);
		TEST_EXPECT(applied(siblings, reorder_moves(siblings, order)) == order);
	}
	TEST_EXPECT(reorder_moves(siblings, arrange_order(ArrangeOp::BringToFront, siblings, chosen)).size() == 2);
	// A run of chosen ones moves as a block; one already at the end goes nowhere.
	TEST_EXPECT(arrange_order(ArrangeOp::BringForward, siblings, {2, 3}) == std::vector<NodeId>({1, 4, 2, 3, 5}));
	TEST_EXPECT(arrange_order(ArrangeOp::BringForward, siblings, {4, 5}) == siblings);
	TEST_EXPECT(arrange_order(ArrangeOp::SendBackward, siblings, {1, 2}) == siblings);
	TEST_EXPECT(reorder_moves(siblings, siblings).empty());
	std::printf("test_order passed\n");
	return 0;
}

// --- a real menu through its compiled screen ---------------------------------------------

const char *const kMenu =
        "<SCREEN>\r\n"
        "\t<NAME>ARRANGE</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"A\">\r\n"
        "\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"B\">\r\n"
        "\t\t\t<POSITION><LEFT>200</LEFT><TOP>150</TOP><RIGHT>400</RIGHT><BOTTOM>250</BOTTOM></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"C\">\r\n"
        "\t\t\t<POSITION><LEFT>500</LEFT><TOP>100</TOP><RIGHT>600</RIGHT><BOTTOM>150</BOTTOM></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"PANEL\">\r\n"
        "\t\t\t<POSITION><LEFT>50</LEFT><TOP>300</TOP><RIGHT>350</RIGHT><BOTTOM>500</BOTTOM></POSITION>\r\n"
        "\t\t\t<WINDOW type=\"window\" name=\"D\">\r\n"
        "\t\t\t\t<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>60</RIGHT><BOTTOM>40</BOTTOM></POSITION>\r\n"
        "\t\t\t</WINDOW>\r\n"
        "\t\t\t<WINDOW type=\"window\" name=\"FLAT\">\r\n"
        "\t\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>200</RIGHT><BOTTOM>100</BOTTOM></POSITION>\r\n"
        "\t\t\t</WINDOW>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n";

struct NoFiles : opennova::FileSource {
	bool read(const std::string &, std::vector<uint8_t> &) const override { return false; }
	uint64_t stamp(const std::string &) const override { return 0; }
};

int64_t edge(const Document &document, const NodeAddress &window, const char *field) {
	Value value;
	return document.get(window, field, value) ? std::get<int64_t>(value) : -1;
}

static int test_on_a_menu() {
	editor_test::TempProjectDir dir("opennova_menu_arrange_test");
	TEST_EXPECT(editor_test::write_text(dir.file("arrange.mnu"), kMenu));
	MnuDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load(dir.file("arrange.mnu"), "arrange.mnu", AssetKind::Menu, "jo", error));
	const std::string original = document.serialize().text;
	const NodeId screen = document.rows()[0]->id;
	NodeAddress main, a, b, c, panel, d, flat;
	TEST_EXPECT(find_definition(AssetGraph(), document, "MAIN", main) &&
			find_definition(AssetGraph(), document, "A", a) &&
			find_definition(AssetGraph(), document, "B", b) &&
			find_definition(AssetGraph(), document, "C", c) &&
			find_definition(AssetGraph(), document, "PANEL", panel) &&
			find_definition(AssetGraph(), document, "D", d) &&
			find_definition(AssetGraph(), document, "FLAT", flat));
	NoFiles files;
	MenuScreenRender render;
	auto compile = [&] { return render.configure(document, screen, files, {}) == MenuPreviewStatus::Ready; };
	TEST_EXPECT(compile());

	// The draw order: where A and B overlap, the later sibling B is the one the hit walk
	// finds (the runtime draws it last); Bring to front of A is one Move, one undo step.
	TEST_EXPECT(render.compiler().hit_widget(render.state(), 250.0f, 175.0f, 1.0f, 1.0f) == document.window_index(b));
	std::vector<Edit> edits;
	std::string why;
	TEST_EXPECT(arrange_edits(document, {a}, a, ArrangeOp::BringToFront, render.compiler(), render.state(), edits, &why));
	TEST_EXPECT(edits.size() == 1 && edits[0].operation == EditOperation::Move && edits[0].address == a &&
	            edits[0].position == 3);
	TEST_EXPECT(document.apply(edits, error) && compile());
	TEST_EXPECT(render.compiler().hit_widget(render.state(), 250.0f, 175.0f, 1.0f, 1.0f) == document.window_index(a));
	document.undo();
	TEST_EXPECT(document.serialize().text == original && compile());
	// Send to back of B and C together: both before A, in their order.
	TEST_EXPECT(arrange_edits(document, {c, b}, c, ArrangeOp::SendToBack, render.compiler(), render.state(), edits, &why));
	TEST_EXPECT(document.apply(edits, error));
	const std::vector<Document::Collection> children = document.collections_of(main);
	std::vector<NodeId> order;
	for (const Document::Collection &collection : children)
		if (collection.spec.kind == node_kind(MenuKind::Window)) order = collection.ids;
	TEST_EXPECT(order == std::vector<NodeId>({b.child, c.child, a.child, panel.child}));
	document.undo();
	TEST_EXPECT(document.serialize().text == original && compile());

	// C's bottom edge aligned to A's (200): one batch, one undo step.
	TEST_EXPECT(arrange_edits(document, {a, c}, a, ArrangeOp::AlignBottom, render.compiler(), render.state(), edits, &why));
	TEST_EXPECT(!edits.empty());
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(edge(document, c, "position.top") == 150 && edge(document, c, "position.bottom") == 200 &&
	            edge(document, a, "position.bottom") == 200);
	document.undo();
	TEST_EXPECT(document.serialize().text == original && compile());
	// D (inside PANEL) aligned to C's left: its rect in PANEL moves, PANEL does not.
	TEST_EXPECT(arrange_edits(document, {c, d}, c, ArrangeOp::AlignLeft, render.compiler(), render.state(), edits, &why));
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(edge(document, d, "position.left") == 450 && edge(document, d, "position.right") == 500 &&
	            edge(document, panel, "position.left") == 50);
	document.undo();
	TEST_EXPECT(compile());
	// Already aligned: no edits.
	TEST_EXPECT(arrange_edits(document, {a, c}, a, ArrangeOp::AlignTop, render.compiler(), render.state(), edits, &why) &&
	            edits.empty());
	// Refusals.
	TEST_EXPECT(!arrange_edits(document, {a}, a, ArrangeOp::AlignLeft, render.compiler(), render.state(), edits, &why) &&
	            !why.empty());
	TEST_EXPECT(!arrange_edits(document, {a, c}, a, ArrangeOp::DistributeHorizontally, render.compiler(), render.state(),
	                           edits, &why));
	TEST_EXPECT(!arrange_edits(document, {a, c}, b, ArrangeOp::AlignLeft, render.compiler(), render.state(), edits, &why));
	// FLAT has no height: aligning it is refused by its name, nothing written.
	why.clear();
	TEST_EXPECT(!arrange_edits(document, {a, flat}, a, ArrangeOp::AlignLeft, render.compiler(), render.state(), edits,
	                           &why) &&
	            edits.empty() && why.find("FLAT") != std::string::npos);

	// A move of several windows: A held by the drag, C with it; snapped by A's leading edges
	// (100 + 13 -> 112, 100 + 5 -> 104), C moved as far, unsnapped.
	LayoutGroup group;
	TEST_EXPECT(layout_group_start(document, a, {a, c}, render.compiler(), render.state(), group));
	TEST_EXPECT(group.members.size() == 2);
	TEST_EXPECT(layout_group_drag_edits(document, group, render.compiler(), LayoutDrag{LayoutHandle::Move, 13, 5, kLayoutGrid},
	                                    7, edits));
	for (const Edit &one : edits) TEST_EXPECT(one.gesture == 7);
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(edge(document, a, "position.left") == 112 && edge(document, a, "position.top") == 104 &&
	            edge(document, c, "position.left") == 512 && edge(document, c, "position.right") == 612 &&
	            edge(document, c, "position.top") == 104);
	document.undo();
	TEST_EXPECT(document.serialize().text == original && compile());
	// PANEL and the D it holds, D held by the drag: PANEL moves once, D rides along.
	TEST_EXPECT(layout_group_start(document, d, {panel, d}, render.compiler(), render.state(), group));
	TEST_EXPECT(group.members.size() == 1 && group.members[0].window == panel && group.lead.window == d);
	TEST_EXPECT(layout_group_drag_edits(document, group, render.compiler(), LayoutDrag{LayoutHandle::Move, 8, 8, 0}, 9,
	                                    edits));
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(edge(document, panel, "position.left") == 58 && edge(document, d, "position.left") == 10);
	document.undo();
	TEST_EXPECT(document.serialize().text == original);
	// The selection's windows: a record's window, each once, only on its screen; the
	// primary's among them.
	const NodeAddress screen_address{screen, node_kind(MenuKind::Screen), 0};
	TEST_EXPECT(windows_holding(document, {a, d, a, screen_address}, screen) == std::vector<NodeAddress>({a, d}));
	const std::vector<NodeAddress> chosen = selected_windows(document, c, {a, d}, screen);
	TEST_EXPECT(chosen == std::vector<NodeAddress>({a, d, c}));
	TEST_EXPECT(selected_windows(document, a, {a, d}, screen) == std::vector<NodeAddress>({a, d}));
	// A selection over rows (S13 D7): a record of another screen stays out of the screen's drag.
	const NodeAddress elsewhere{screen + 1000, node_kind(MenuKind::Window), 5};
	TEST_EXPECT(selected_windows(document, c, {a, elsewhere, d}, screen) == std::vector<NodeAddress>({a, d, c}));

	// A drag as the preview pane and the editor MCP start one (layout_press): a move of a
	// selected window takes the selection, A's snap stepping every one (D inside PANEL too);
	// a move of a window not selected takes it alone; a resize holds its window alone.
	LayoutPress press;
	TEST_EXPECT(layout_press(document, a, LayoutHandle::Move, chosen, render.compiler(), render.state(), press));
	TEST_EXPECT(press.windows == chosen && press.group.lead.window == a && press.group.members.size() == 3);
	TEST_EXPECT(layout_press_edits(document, press, render.compiler(), 13, 5, kLayoutGrid, 11, edits));
	for (const Edit &one : edits) TEST_EXPECT(one.gesture == 11);
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(edge(document, a, "position.left") == 112 && edge(document, a, "position.top") == 104 &&
	            edge(document, c, "position.left") == 512 && edge(document, d, "position.left") == 22 &&
	            edge(document, d, "position.top") == 14 && edge(document, b, "position.left") == 200);
	document.undo();
	TEST_EXPECT(document.serialize().text == original && compile());
	TEST_EXPECT(layout_press(document, b, LayoutHandle::Move, chosen, render.compiler(), render.state(), press));
	TEST_EXPECT(press.windows == std::vector<NodeAddress>({b}) && press.group.members.size() == 1);
	TEST_EXPECT(layout_press(document, a, LayoutHandle::Right, chosen, render.compiler(), render.state(), press));
	TEST_EXPECT(press.windows == std::vector<NodeAddress>({a}) && press.group.members.empty());
	TEST_EXPECT(layout_press_edits(document, press, render.compiler(), 20, 0, 0, 12, edits));
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(edge(document, a, "position.right") == 320 && edge(document, a, "position.left") == 100 &&
	            edge(document, c, "position.left") == 500);
	document.undo();
	TEST_EXPECT(document.serialize().text == original);
	TEST_EXPECT(!layout_press(document, screen_address, LayoutHandle::Move, chosen, render.compiler(), render.state(),
	                          press));
	TEST_EXPECT(document.outermost({d, panel, a}) == std::vector<NodeAddress>({panel, a}));
	std::printf("test_on_a_menu passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_tokens();
	failures += test_align();
	failures += test_distribute();
	failures += test_holders();
	failures += test_order();
	failures += test_on_a_menu();
	return failures == 0 ? 0 : 1;
}
