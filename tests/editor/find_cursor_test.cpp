// S13 V1 (ADR 0046 S13): the Document window's find bar model (ui/find_cursor), with no ImGui,
// over an item table whose three items turn at 90 degrees a second. The hits are found again
// only when the document (its identity and revision), the text or the case moves; before a hit
// is shown the next is the first and the previous the last, and on a hit they wrap at either
// end; the hit shown that stops matching leaves the cursor between hits, the next going to the
// first hit after it that still does (and, the edit undone, to that same hit among the three
// again); a new text starts over; case matters only when asked; another document is searched
// anew; an index past the hits shows nothing.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/ui/find_cursor.h>

#include "common/test_expect.h"

using namespace opennova::editor;

namespace {

const char *const kItems = "begin \"Found Thing\"\nid 100300\ntype vehicle\nturn_rate 90\nend\n"
                           "begin \"Second Thing\"\nid 100301\ntype vehicle\nturn_rate 90\nend\n"
                           "begin \"Third Thing\"\nid 100302\ntype vehicle\nturn_rate 90\nend\n";

bool load(DefCatalogDocument &document) {
	const std::string text = kItems;
	Diagnostic error;
	return document.load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "items.def",
	                           AssetKind::ItemDefs, "jo", error);
}

int test_steps() {
	DefCatalogDocument items;
	TEST_EXPECT(load(items));
	FindCursor cursor;
	cursor.refresh(items, "90", false);
	TEST_EXPECT(cursor.hits().size() == 3 && !cursor.on_hit());
	TEST_EXPECT(cursor.step(true) == 0 && cursor.step(false) == 2);
	const std::vector<DocumentHit> first = cursor.hits();
	TEST_EXPECT(first[0].field == "turn_rate" && first[1].field == "turn_rate" &&
	            first[2].field == "turn_rate");

	// On a hit: the next and the previous wrap at either end.
	const DocumentHit *shown = cursor.show(0);
	TEST_EXPECT(shown && shown->address == first[0].address && cursor.on_hit() &&
	            cursor.current() == 0);
	TEST_EXPECT(cursor.step(true) == 1 && cursor.step(false) == 2);
	cursor.show(2);
	TEST_EXPECT(cursor.step(true) == 0 && cursor.step(false) == 1);
	TEST_EXPECT(!cursor.show(3) && cursor.current() == 2);

	// The second item shown, then its turn rate changed: the cursor sits between hits, the next
	// going to the third item (the first after it that still matches), the previous to the first.
	cursor.show(1);
	Edit change;
	change.address = first[1].address;
	change.field = "turn_rate";
	change.value = int64_t(45);
	Diagnostic error;
	TEST_EXPECT(items.apply(change, error));
	cursor.refresh(items, "90", false);
	TEST_EXPECT(cursor.hits().size() == 2 && !cursor.on_hit());
	TEST_EXPECT(cursor.step(true) == 1 && cursor.hits()[1].address == first[2].address);
	TEST_EXPECT(cursor.step(false) == 0);
	// The edit undone: three hits again, the next still the third item.
	items.undo();
	cursor.refresh(items, "90", false);
	TEST_EXPECT(cursor.hits().size() == 3 && !cursor.on_hit() && cursor.step(true) == 2);
	// Shown and found again unchanged: still on it.
	cursor.show(2);
	cursor.refresh(items, "90", false);
	TEST_EXPECT(cursor.on_hit() && cursor.current() == 2);

	// A new text starts over; an empty one finds nothing.
	cursor.refresh(items, "Thing", false);
	TEST_EXPECT(cursor.hits().size() == 3 && !cursor.on_hit() && cursor.step(true) == 0);
	cursor.refresh(items, "", false);
	TEST_EXPECT(cursor.hits().empty() && cursor.step(true) == SIZE_MAX &&
	            cursor.step(false) == SIZE_MAX);
	return 0;
}

int test_case_and_documents() {
	DefCatalogDocument items;
	TEST_EXPECT(load(items));
	FindCursor cursor;
	cursor.refresh(items, "found thing", false);
	TEST_EXPECT(cursor.hits().size() == 1);
	cursor.refresh(items, "found thing", true);
	TEST_EXPECT(cursor.hits().empty());
	cursor.refresh(items, "Found Thing", true);
	TEST_EXPECT(cursor.hits().size() == 1);
	cursor.show(0);
	// Another document (a reload is a new one): searched anew, the cursor on no hit.
	DefCatalogDocument reloaded;
	TEST_EXPECT(load(reloaded));
	cursor.refresh(reloaded, "Found Thing", true);
	TEST_EXPECT(cursor.hits().size() == 1 && !cursor.on_hit() && cursor.step(true) == 0);
	return 0;
}

} // namespace

int main() {
	if (test_steps() != 0) return 1;
	if (test_case_and_documents() != 0) return 1;
	std::printf("editor_find_cursor: all tests passed\n");
	return 0;
}
