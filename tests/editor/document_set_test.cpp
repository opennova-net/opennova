// Pins the session's open documents (DocumentSet, ADR 0046 S13 A2) where the split gave them one
// rule each: the one position rule, where a record placed right after another goes, which Paste
// after the selection and Duplicate share (a nested record's copy lands at the index after it in
// its owner's collection, whichever made it, byte for byte the same file; a row's after it among
// the rows; a row selected takes a Paste into itself, at its end); the selection each open
// document keeps while another is active (the primary and every selected record given back, a
// document read again or closed keeping none); and a selection over several rows (S13 D7), whose
// Duplicate and whose removal are one step each, the primary's copy the primary; records the
// document does not hold never selected; and the Selection concern moving with the selection
// alone.
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
		session.run_operations();
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
				menus.menu().placement(v.documents.selection.primary, pasted_at) &&
				pasted_at.owner.child == parent && pasted_at.index == position);
		const std::string pasted_name = menus.menu().record_name(v.documents.selection.primary);
		menus.session.handle(request::undo(menus.menu().path()));
		TEST_EXPECT(menus.menu().serialize().text == original);

		// Duplicate: the same place, the same file.
		menus.select(menus.at(locator));
		TEST_EXPECT(menus.act(EditorRequestKind::Duplicate));
		Document::Placement duplicated_at;
		TEST_EXPECT(menus.menu().placement(v.documents.selection.primary, duplicated_at) && duplicated_at.owner.child == parent &&
		            duplicated_at.index == position && menus.menu().record_name(v.documents.selection.primary) == pasted_name);
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
	TEST_EXPECT(menus.menu().rows().size() == 2 && v.documents.selection.primary.row == menus.menu().rows()[1]->id && !v.documents.selection.primary.child);
	menus.session.handle(request::undo(menus.menu().path()));
	TEST_EXPECT(menus.menu().serialize().text == original);
	// A row selected takes a Paste into itself, at its end: a copy of TITLE lands after MAIN, a root
	// window of the screen.
	menus.select(menus.at(kTitle));
	TEST_EXPECT(menus.act(EditorRequestKind::Copy));
	menus.select(screen);
	TEST_EXPECT(menus.act(EditorRequestKind::Paste));
	Document::Placement into;
	TEST_EXPECT(menus.menu().placement(v.documents.selection.primary, into) && into.owner.child == 0 && into.index == 1);
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
	TEST_EXPECT(v.documents.selection.primary == menus.at(kExit) && v.documents.selection.records.size() == 2);

	// Another document active: its own selection; then back, the menu's as it was.
	menus.session.handle(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	const Document *extra = menus.session.document_for("extra.mnu");
	TEST_EXPECT(extra && v.documents.active == extra->path());
	TEST_EXPECT(v.documents.selection.primary.row == extra->rows().front()->id && !v.documents.selection.primary.child); // its first screen
	const NodeAddress extra_main = extra->address_at(kMain);
	menus.select(extra_main, SelectMode::Replace, extra->path());
	menus.session.handle(request::open_document(menu));
	TEST_EXPECT(v.documents.active == menu && v.documents.selection.primary == menus.at(kExit) &&
			v.documents.selection.records ==
					std::vector<NodeAddress>({ menus.at(kTitle), menus.at(kExit) }));
	menus.session.handle(request::open_document("extra.mnu"));
	TEST_EXPECT(v.documents.active == extra->path() && v.documents.selection.primary == extra_main && v.documents.selection.records.size() == 1);

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
	menus.session.run_operations();
	menus.session.handle(request::open_document(menu));
	TEST_EXPECT(v.documents.active == menu && v.documents.selection.records.size() == 1 && v.documents.selection.primary.row == menus.menu().rows().front()->id &&
	            !v.documents.selection.primary.child);

	// Selected again, then closed while another is active, and opened again: none kept.
	menus.select(menus.at(kTitle));
	menus.session.handle(request::open_document("extra.mnu"));
	menus.session.handle(request::close_document(menu));
	TEST_EXPECT(!menus.session.document_for(menu) && v.documents.active == extra->path());
	menus.session.handle(request::open_document(menu));
	TEST_EXPECT(v.documents.active == menu && v.documents.selection.records.size() == 1 &&
			!v.documents.selection.primary.child);
	return 0;
}

// A selection over several rows (S13 D7): the TITLE of two screens, a marquee's (SelectRecord with
// records) or one joined to the other (Add, another row); Duplicate copies each after itself in one
// step, the copies selected; a batch removing both is one step, the selection repaired to the
// primary's owner; Cut asks the type, and a menu copies the windows of one screen only.
static int test_selection_over_rows() {
	Menus menus("opennova_editor_document_set_rows");
	const SessionView &v = menus.view();
	menus.select(menus.at(kStartup));
	TEST_EXPECT(menus.act(EditorRequestKind::Duplicate) && menus.menu().rows().size() == 2);
	const std::string two = menus.menu().serialize().text;
	const NodeAddress title = menus.at(kTitle), other = menus.at("1/window:0/window:0");
	TEST_EXPECT(other.row && other.row != title.row && menus.menu().record_name(other) == "TITLE");
	menus.select(title);
	menus.select(other, SelectMode::Add);
	TEST_EXPECT(v.documents.selection.records == std::vector<NodeAddress>({title, other}) && v.documents.selection.primary == other);
	menus.session.handle(request::select_record("main.mnu", title, SelectMode::Replace, {other}));
	TEST_EXPECT(v.documents.selection.records == std::vector<NodeAddress>({other, title}) && v.documents.selection.primary == title);

	// Duplicate: the copies selected, the copy of the primary the primary (the preview stays on its
	// screen), whichever screen it is on.
	TEST_EXPECT(menus.act(EditorRequestKind::Duplicate));
	TEST_EXPECT(v.documents.selection.records == std::vector<NodeAddress>({menus.at("1/window:0/window:1"), menus.at("0/window:0/window:1")}) &&
	            v.documents.selection.primary == menus.at("0/window:0/window:1"));
	menus.session.handle(request::undo(menus.menu().path()));
	TEST_EXPECT(menus.menu().serialize().text == two);
	menus.session.handle(request::select_record("main.mnu", other, SelectMode::Replace, {title}));
	TEST_EXPECT(menus.act(EditorRequestKind::Duplicate));
	TEST_EXPECT(v.documents.selection.primary == menus.at("1/window:0/window:1") && v.documents.selection.records.size() == 2);
	menus.session.handle(request::undo(menus.menu().path()));
	TEST_EXPECT(menus.menu().serialize().text == two);

	// Records the document does not hold are left out as they are named (S13 D7's second review:
	// the repair after an edit asks only about the rows it changed): a stale identity, one named as
	// another kind; a primary it does not hold gives way to the first known.
	const NodeAddress stale{title.row, title.kind, 999999}, as_screen{other.row, 0, other.child};
	menus.session.handle(request::select_record("main.mnu", title, SelectMode::Replace, {stale, as_screen}));
	TEST_EXPECT(v.documents.selection.records == std::vector<NodeAddress>({title}) && v.documents.selection.primary == title);
	menus.session.handle(request::select_record("main.mnu", stale, SelectMode::Replace, {other}));
	TEST_EXPECT(v.documents.selection.records == std::vector<NodeAddress>({other}) && v.documents.selection.primary == other);
	menus.session.handle(request::select_record("main.mnu", stale, SelectMode::Add));
	TEST_EXPECT(v.documents.selection.records.empty() && !v.documents.selection.primary.row);

	// Cut: the menu copies windows of one screen only, so nothing is cut.
	menus.session.handle(request::select_record("main.mnu", title, SelectMode::Replace, {other}));
	TEST_EXPECT(!menus.act(EditorRequestKind::Cut) && menus.menu().serialize().text == two);
	// Both removed in one batch: one step; the primary's owner selected.
	Edit remove_title, remove_other;
	remove_title.operation = remove_other.operation = EditOperation::Remove;
	remove_title.address = title;
	remove_other.address = other;
	menus.session.handle(request::edit_record("main.mnu", std::vector<Edit>{remove_title, remove_other}));
	TEST_EXPECT(menus.session.last_edit_ok() && !menus.at("1/window:0/window:1").row && !menus.at(kExit).row);
	TEST_EXPECT(v.documents.selection.primary == menus.at(kMain) && v.documents.selection.records.size() == 1);
	menus.session.handle(request::undo(menus.menu().path()));
	TEST_EXPECT(menus.menu().serialize().text == two);
	return 0;
}

// The Selection concern moves when the selection does, and only then (S13 D7's second review: after
// an edit, an undo and a redo, the touch waits on the selection's serial): a change of a selected
// window's text, its undo and its redo leave the selection and its concern as they were; the
// window's removal moves both, the window's owner selected.
static int test_selection_concern() {
	Menus menus("opennova_editor_document_set_concern");
	const SessionView &v = menus.view();
	const NodeAddress title = menus.at(kTitle);
	menus.select(title);
	const uint64_t before = v.revisions.of(ViewConcern::Selection);
	Edit text;
	text.address = title;
	text.field = "string.value";
	text.value = std::string("Renamed");
	menus.session.handle(request::edit_record("main.mnu", text));
	TEST_EXPECT(menus.session.last_edit_ok() && v.revisions.of(ViewConcern::Selection) == before);
	menus.session.handle(request::undo(menus.menu().path()));
	menus.session.handle(request::redo(menus.menu().path()));
	TEST_EXPECT(v.revisions.of(ViewConcern::Selection) == before && v.documents.selection.primary == title &&
	            v.documents.selection.records == std::vector<NodeAddress>({title}));
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = title;
	menus.session.handle(request::edit_record("main.mnu", remove));
	TEST_EXPECT(menus.session.last_edit_ok() && v.revisions.of(ViewConcern::Selection) != before &&
	            v.documents.selection.primary == menus.at(kMain));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_paste_and_duplicate_agree();
	failures += test_remembered_selections();
	failures += test_selection_over_rows();
	failures += test_selection_concern();
	if (failures == 0) std::printf("editor_document_set: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
