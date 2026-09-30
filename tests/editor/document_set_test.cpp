// Pins the session's open documents (DocumentSet, ADR 0046 S13 A2) where the split gave them one
// rule each: the one position rule, where a record placed right after another goes, which Paste
// after the selection and Duplicate share (a nested record's copy lands at the index after it in
// its owner's collection, whichever made it, byte for byte the same file; a row's after it among
// the rows; a row selected takes a Paste into itself, at its end); and the selection each open
// document keeps while another is active (the primary and every selected record given back, a
// document read again or closed keeping none).
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/assets/asset_type_registry.h>
#include <editor/model/document.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

// The startup screen's records, by locator (blank_menu's STARTUP: MAIN holding TITLE and EXIT).
constexpr const char *kStartup = "0";
constexpr const char *kMain = "0/window:0";
constexpr const char *kTitle = "0/window:0/window:0";
constexpr const char *kExit = "0/window:0/window:1";

// A new project with its required files, main.mnu open and active.
struct Menus {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	explicit Menus(const char *name) : dir(name), session(platform, preferences) {
		session.handle(request::new_project(dir.file("project"), "Places"));
		editor_test::create_missing_files(session);
		session.handle(request::open_document("main.mnu"));
	}
	const SessionView &view() const { return session.view(); }
	Document &menu() { return *session.document_for("main.mnu"); }
	NodeAddress at(const char *locator) { return menu().address_at(locator); }
	void select(const NodeAddress &address, SelectMode mode = SelectMode::Replace, const std::string &path = "main.mnu") {
		session.handle(request::select_record(path, address, mode));
	}
	bool act(EditorRequestKind kind) {
		EditorRequest request = request::of(kind);
		request.path = menu().path();
		session.handle(request);
		return session.outcome().done() && session.last_edit_ok();
	}
};

} // namespace

// Paste after the selection and Duplicate place a record's copy by one rule: a nested record's at
// the index after it in its owner's collection. A copy of TITLE pasted after it and TITLE
// duplicated make the same file, byte for byte, each undone to the file as it was; likewise after
// EXIT, the last of MAIN's windows. position_after says where, for the row too.
static int test_paste_and_duplicate_agree() {
	Menus menus("opennova_editor_document_set_places");
	const SessionView &v = menus.view();
	TEST_EXPECT(menus.menu().locator(menus.at(kTitle)) == kTitle && menus.menu().record_name(menus.at(kTitle)) == "TITLE");
	TEST_EXPECT(menus.menu().record_name(menus.at(kExit)) == "EXIT" && menus.menu().record_name(menus.at(kMain)) == "MAIN");
	const std::string original = menus.menu().serialize().text;
	for (const char *locator : {kTitle, kExit}) {
		const NodeAddress record = menus.at(locator);
		NodeId parent = 99;
		size_t position = 99;
		TEST_EXPECT(DocumentSet::position_after(menus.menu(), record, parent, position));
		Document::Placement at;
		TEST_EXPECT(menus.menu().placement(record, at) && parent == at.owner.child && position == at.index + 1);

		// Copy, then Paste with no target: after the selected record, by the rule.
		menus.select(record);
		TEST_EXPECT(menus.act(EditorRequestKind::Copy) && !v.documents.clipboard.empty());
		TEST_EXPECT(menus.act(EditorRequestKind::Paste));
		const std::string pasted = menus.menu().serialize().text;
		Document::Placement pasted_at;
		TEST_EXPECT(pasted != original &&
				menus.menu().placement(v.documents.selection, pasted_at) &&
				pasted_at.owner.child == parent && pasted_at.index == position);
		const std::string pasted_name = menus.menu().record_name(v.documents.selection);
		menus.session.handle(request::undo(menus.menu().path()));
		TEST_EXPECT(menus.menu().serialize().text == original);

		// Duplicate: the same place, the same file.
		menus.select(menus.at(locator));
		TEST_EXPECT(menus.act(EditorRequestKind::Duplicate));
		Document::Placement duplicated_at;
		TEST_EXPECT(menus.menu().placement(v.documents.selection, duplicated_at) && duplicated_at.owner.child == parent &&
		            duplicated_at.index == position && menus.menu().record_name(v.documents.selection) == pasted_name);
		TEST_EXPECT(menus.menu().serialize().text == pasted);
		menus.session.handle(request::undo(menus.menu().path()));
		TEST_EXPECT(menus.menu().serialize().text == original);
	}
	TEST_EXPECT(menus.menu().record_name(menus.at(kTitle)) == "TITLE" && menus.menu().record_name(menus.at(kExit)) == "EXIT" &&
	            !menus.at("0/window:0/window:2").row);

	// A row: after itself among the rows, as Duplicate places a screen.
	const NodeAddress screen = menus.at(kStartup);
	NodeId parent = 99;
	size_t position = 99;
	TEST_EXPECT(DocumentSet::position_after(menus.menu(), screen, parent, position) && parent == 0 && position == 1);
	menus.select(screen);
	TEST_EXPECT(menus.act(EditorRequestKind::Duplicate));
	TEST_EXPECT(menus.menu().rows().size() == 2 && v.documents.selection.row == menus.menu().rows()[1]->id && !v.documents.selection.child);
	menus.session.handle(request::undo(menus.menu().path()));
	TEST_EXPECT(menus.menu().serialize().text == original);
	// A row selected takes a Paste into itself, at its end: a copy of TITLE lands after MAIN, a root
	// window of the screen.
	menus.select(menus.at(kTitle));
	TEST_EXPECT(menus.act(EditorRequestKind::Copy));
	menus.select(screen);
	TEST_EXPECT(menus.act(EditorRequestKind::Paste));
	Document::Placement into;
	TEST_EXPECT(menus.menu().placement(v.documents.selection, into) && into.owner.child == 0 && into.index == 1);
	// A record the document does not hold has no place.
	TEST_EXPECT(!DocumentSet::position_after(menus.menu(), {screen.row, screen.kind, 987654}, parent, position));
	TEST_EXPECT(!DocumentSet::position_after(menus.menu(), {987654, screen.kind, 0}, parent, position));
	return 0;
}

// The selection each open document had when another became active comes back with it: the
// primary and every selected record, repaired against the records as they are. A document read
// again (its file changed outside the editor) keeps none, nor does one closed and opened again: a
// menu then shows its first screen.
static int test_remembered_selections() {
	Menus menus("opennova_editor_document_set_selections");
	const SessionView &v = menus.view();
	const std::string menu = menus.menu().path();
	menus.select(menus.at(kTitle));
	menus.select(menus.at(kExit), SelectMode::Add);
	TEST_EXPECT(v.documents.selection == menus.at(kExit) && v.documents.selected.size() == 2);

	// Another document active: its own selection; then back, the menu's as it was.
	menus.session.handle(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	const Document *extra = menus.session.document_for("extra.mnu");
	TEST_EXPECT(extra && v.documents.active == extra->path());
	TEST_EXPECT(v.documents.selection.row == extra->rows().front()->id && !v.documents.selection.child); // its first screen
	const NodeAddress extra_main = extra->address_at(kMain);
	menus.select(extra_main, SelectMode::Replace, extra->path());
	menus.session.handle(request::open_document(menu));
	TEST_EXPECT(v.documents.active == menu && v.documents.selection == menus.at(kExit) &&
			v.documents.selected ==
					std::vector<NodeAddress>({ menus.at(kTitle), menus.at(kExit) }));
	menus.session.handle(request::open_document("extra.mnu"));
	TEST_EXPECT(v.documents.active == extra->path() && v.documents.selection == extra_main && v.documents.selected.size() == 1);

	// The menu's file changed outside the editor: a Rescan reads it again, and it keeps no
	// selection (its records are new ones).
	{
		const std::string path = menus.view().project.root + "/" + menu;
		std::string bytes;
		std::string error;
		TEST_EXPECT(read_file_text(path, bytes, error));
		TEST_EXPECT(editor_test::write_text(path, bytes + "\r\n"));
	}
	menus.session.handle(request::rescan());
	menus.session.handle(request::open_document(menu));
	TEST_EXPECT(v.documents.active == menu && v.documents.selected.size() == 1 && v.documents.selection.row == menus.menu().rows().front()->id &&
	            !v.documents.selection.child);

	// Selected again, then closed while another is active, and opened again: none kept.
	menus.select(menus.at(kTitle));
	menus.session.handle(request::open_document("extra.mnu"));
	menus.session.handle(request::close_document(menu));
	TEST_EXPECT(!menus.session.document_for(menu) && v.documents.active == extra->path());
	menus.session.handle(request::open_document(menu));
	TEST_EXPECT(v.documents.active == menu && v.documents.selected.size() == 1 &&
			!v.documents.selection.child);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_paste_and_duplicate_agree();
	failures += test_remembered_selections();
	if (failures == 0) std::printf("editor_document_set: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
