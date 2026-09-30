// S13 V1 (ADR 0046 S13): the menu clipboard's one rule (documents/mnu_clipboard), which the menu
// view's tree and the Preview's canvas both ask. Over a screen whose MAIN holds BACK (with an
// ACTION), PANEL (holding the list CHOICES, whose scrollbar part holds INPART) and TITLE:
// Copy, Cut and Duplicate take the selection only while every selected record is a window the
// tree lists, so INPART, a window a part holds, is copyable from neither side (the canvas used
// to take it), nor with BACK beside it, nor a list row or the screen; a Paste goes after the
// primary's window among its siblings (INPART's: CHOICES, the listed window holding it; a list
// row's: its window), at the end of the screen's root windows for the screen, nothing, or a
// selection of another screen; and only while the clipboard holds records. The listed window a
// record is or that holds it (listed_window: what a Paste goes after, and where the menu view's
// Add window puts a new one) is one walk for both.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/mnu_clipboard.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/reference_queries.h>

#include "common/test_expect.h"
#include "editor/menu_test_support.h"

using namespace opennova::editor;

namespace {

constexpr const char *kMenu =
        "<SCREEN>\r\n"
        "\t<NAME>OPTIONS</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM>"
        "</POSITION>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"BACK\">\r\n"
        "\t\t\t<ACTION type=\"POP_SCREEN\"></ACTION>\r\n"
        "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"PANEL\">\r\n"
        "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t<WINDOW type=\"list\" name=\"CHOICES\">\r\n"
        "\t\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t\t<SCROLLBAR><APPEARANCE state=\"default\"></APPEARANCE>"
        "<WINDOW type=\"static\" name=\"INPART\"><APPEARANCE state=\"default\"></APPEARANCE>"
        "</WINDOW></SCROLLBAR>\r\n"
        "\t\t\t</WINDOW>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"static\" name=\"TITLE\"><APPEARANCE state=\"default\"></APPEARANCE>"
        "</WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "\t<WINDOW type=\"window\" name=\"OVERLAY\"><APPEARANCE state=\"default\"></APPEARANCE>"
        "</WINDOW>\r\n"
        "</SCREEN>\r\n"
        "<SCREEN>\r\n"
        "\t<NAME>OTHER</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"ELSEWHERE\"><APPEARANCE state=\"default\"></APPEARANCE>"
        "</WINDOW>\r\n"
        "</SCREEN>\r\n";

constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
constexpr NodeKind kWindow = node_kind(MenuKind::Window);

NodeAddress named(const Document &document, const char *name) {
	NodeAddress address;
	find_definition(AssetGraph(), document, name, address);
	return address;
}

// A window by name wherever it sits, a part's too (find() reaches the windows a lookup finds).
NodeAddress walked(const Document &document, const Node &row, const char *name) {
	NodeAddress found;
	document.walk_records(row, [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind == kWindow && document.record_name(record) == name) found = record;
		return !found.child;
	});
	return found;
}

bool pastes_at(const MenuClipboard &board, NodeId screen, NodeId parent, size_t position) {
	return board.paste && board.paste_row == NodeAddress{screen, kWindow, 0} &&
	       board.paste_parent == parent && board.paste_position == position;
}

int test_rule() {
	MnuDocument document;
	Diagnostic error;
	const std::string text = kMenu;
	TEST_EXPECT(document.load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "options.mnu",
	                                AssetKind::Menu, "jo", error));
	TEST_EXPECT(document.rows().size() == 2);
	if (document.rows().size() != 2) return 1;
	const Node &screen = *document.rows()[0];
	const NodeId id = screen.id;
	const NodeAddress main = named(document, "MAIN"), back = named(document, "BACK"),
	                  panel = named(document, "PANEL"), choices = named(document, "CHOICES"),
	                  title = named(document, "TITLE"), overlay = named(document, "OVERLAY");
	const NodeAddress in_part = walked(document, screen, "INPART");
	TEST_EXPECT(main.child && back.child && panel.child && choices.child && title.child &&
	            overlay.child && in_part.child);
	TEST_EXPECT(document.ancestors(in_part).back().kind != kWindow); // held by the scrollbar part

	// Windows the tree lists: copied, and a Paste after the primary among its siblings.
	MenuClipboard board = menu_clipboard(document, id, back, {back}, true);
	TEST_EXPECT(board.copy && pastes_at(board, id, main.child, 1));
	board = menu_clipboard(document, id, title, {back, title}, true);
	TEST_EXPECT(board.copy && pastes_at(board, id, main.child, 3));
	board = menu_clipboard(document, id, main, {main}, true);
	TEST_EXPECT(board.copy && pastes_at(board, id, 0, 1)); // a root: among the screen's roots
	board = menu_clipboard(document, id, choices, {choices}, true);
	TEST_EXPECT(board.copy && pastes_at(board, id, panel.child, 1));

	// The disagreement: a window a part holds is not copyable, alone or with a listed one; its
	// Paste goes after CHOICES, the listed window holding it, never into the part.
	board = menu_clipboard(document, id, in_part, {in_part}, true);
	TEST_EXPECT(!board.copy && pastes_at(board, id, panel.child, 1));
	board = menu_clipboard(document, id, in_part, {back, in_part}, true);
	TEST_EXPECT(!board.copy);

	// A list row (BACK's ACTION): not copyable, a Paste after BACK; the screen, or nothing of
	// it: at the end of its root windows; a selection of another screen: the same.
	const NodeAddress action = menu_test::child_of(document, back, "action");
	TEST_EXPECT(action.child != 0);
	board = menu_clipboard(document, id, action, {action}, true);
	TEST_EXPECT(!board.copy && pastes_at(board, id, main.child, 1));
	const NodeAddress row{id, kScreen, 0};
	board = menu_clipboard(document, id, row, {row}, true);
	TEST_EXPECT(!board.copy && pastes_at(board, id, 0, SIZE_MAX));
	board = menu_clipboard(document, id, row, {row, back}, true);
	TEST_EXPECT(!board.copy);
	board = menu_clipboard(document, id, NodeAddress(), {}, true);
	TEST_EXPECT(!board.copy && pastes_at(board, id, 0, SIZE_MAX));
	const NodeAddress elsewhere = named(document, "ELSEWHERE");
	board = menu_clipboard(document, id, elsewhere, {elsewhere}, true);
	TEST_EXPECT(!board.copy && pastes_at(board, id, 0, SIZE_MAX));

	// The listed window a record is or that holds it: a listed window itself; INPART's, CHOICES;
	// the ACTION's, BACK; none for the screen, nothing, or another screen's window.
	TEST_EXPECT(listed_window(document, id, back) == back &&
	            listed_window(document, id, in_part) == choices &&
	            listed_window(document, id, action) == back);
	TEST_EXPECT(!listed_window(document, id, row).child &&
	            !listed_window(document, id, NodeAddress()).child &&
	            !listed_window(document, id, elsewhere).child);

	// Nothing on the clipboard: no Paste.
	board = menu_clipboard(document, id, back, {back}, false);
	TEST_EXPECT(board.copy && !board.paste);
	return 0;
}

} // namespace

int main() {
	if (test_rule() != 0) return 1;
	std::printf("editor_mnu_clipboard: all tests passed\n");
	return 0;
}
