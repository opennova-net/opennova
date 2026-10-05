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
#include <algorithm>
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
#include <editor/session/session_json.h>
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
// primary's owner; Cut asks the type, and a menu copies the windows of several screens, a cut it
// refuses leaving the clipboard as it was.
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
	// A primary it does not hold with nothing else held: refused, the selection as it was, no Problems row
	// (the demo round's review).
	menus.session.handle(request::select_record("main.mnu", stale, SelectMode::Add));
	TEST_EXPECT(!menus.session.outcome().done() && v.documents.selection.records == std::vector<NodeAddress>({other}) &&
	            v.documents.selection.primary == other);
	TEST_EXPECT(std::none_of(v.findings.diagnostics.begin(), v.findings.diagnostics.end(),
	                         [](const Diagnostic &d) { return d.code() == "document.selection"; }));
	menus.session.handle(request::select_record("main.mnu", NodeAddress{999999, 0, 0}));
	TEST_EXPECT(!menus.session.outcome().done() && v.documents.selection.primary == other &&
	            v.activity.status == "main.mnu holds no record 999999.");
	// The demo round's bug 11: a primary named by its identities alone (no kind, as the wire leaves it out,
	// or a wrong one) is the document's record of them: selected, as its own address.
	for (const NodeKind kind : {NodeKind(0), NodeKind(title.kind + 7)}) {
		menus.session.handle(request::select_record("main.mnu", NodeAddress{title.row, kind, title.child}));
		TEST_EXPECT(menus.session.outcome().done() && v.documents.selection.primary == title &&
		            v.documents.selection.records == std::vector<NodeAddress>({title}));
	}

	// Cut: the menu copies windows of several screens (the polish), so both go in one step and come
	// back with it.
	menus.session.handle(request::select_record("main.mnu", title, SelectMode::Replace, {other}));
	TEST_EXPECT(menus.act(EditorRequestKind::Cut) && !menus.at("1/window:0/window:1").row && !menus.at(kExit).row &&
	            !v.documents.clipboard.empty());
	menus.session.handle(request::undo(menus.menu().path()));
	TEST_EXPECT(menus.menu().serialize().text == two);
	// A cut the menu refuses (the other screen's MAIN, its only root window, with this screen's TITLE:
	// a screen keeps a root window) leaves the clipboard as the last copy left it.
	menus.select(title);
	TEST_EXPECT(menus.act(EditorRequestKind::Copy));
	const std::string copied = v.documents.clipboard;
	menus.session.handle(request::select_record("main.mnu", title, SelectMode::Replace, {menus.at("1/window:0")}));
	TEST_EXPECT(v.documents.selection.records.size() == 2);
	TEST_EXPECT(!menus.act(EditorRequestKind::Cut) && v.documents.clipboard == copied && menus.menu().serialize().text == two);
	TEST_EXPECT(menus.act(EditorRequestKind::Copy) && v.documents.clipboard != copied); // the two copy: the cut's refusal is the remove's
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

// Undo and Redo say what they did (the UX round's problems lane): each step named with its edit's
// words, Undo saying "Undid: <them>.", Redo "Redid: <them>.", and with nothing left "Nothing to undo in
// main.mnu."; a set of several fields of one record in words too; a duplicate by the record's title; the
// document's wire carries the words. A rename is no step: it says so, the Edit menu's rename back is the
// way back (last_rename), and a preview of a rename names its file by its name alone as every other
// request does (the audit's preview_rename refused "menutxt.bin").
static int test_undo_says_what() {
	Menus menus("opennova_editor_document_set_undo_words");
	const SessionView &v = menus.view();
	const NodeAddress title = menus.at(kTitle);
	Edit left;
	left.address = title;
	left.field = "position.left";
	left.value = int64_t(12);
	menus.session.handle(request::edit_record("main.mnu", left));
	TEST_EXPECT(v.activity.status == "Set Left of TITLE to 12." && menus.menu().undo_words() == "Set Left of TITLE to 12");
	menus.session.handle(request::undo("main.mnu"));
	TEST_EXPECT(v.activity.status == "Undid: Set Left of TITLE to 12." && menus.menu().redo_words() == "Set Left of TITLE to 12");
	menus.session.handle(request::undo("main.mnu"));
	TEST_EXPECT(v.activity.status == "Nothing to undo in main.mnu.");
	menus.session.handle(request::redo("main.mnu"));
	TEST_EXPECT(v.activity.status == "Redid: Set Left of TITLE to 12.");
	menus.session.handle(request::redo("main.mnu"));
	TEST_EXPECT(v.activity.status == "Nothing to redo in main.mnu.");
	// Two fields of one record in one batch (a drag's sides).
	Edit top = left;
	top.field = "position.top";
	top.value = int64_t(30);
	left.value = int64_t(20);
	EditorRequest both = request::edit_record("main.mnu", left);
	both.edits.push_back(top);
	menus.session.handle(both);
	TEST_EXPECT(v.activity.status == "Set Left and Top of TITLE." && menus.menu().undo_words() == "Set Left and Top of TITLE");
	// A duplicate by its record's title; undone, said.
	menus.select(title);
	TEST_EXPECT(menus.act(EditorRequestKind::Duplicate) && v.activity.status == "Duplicated TITLE.");
	const opennova::io::JsonValue json = document_to_json(menus.menu(), nullptr, nullptr);
	TEST_EXPECT(json.get_string("undo_words", "") == "Duplicated TITLE");
	menus.session.handle(request::undo("main.mnu"));
	TEST_EXPECT(v.activity.status == "Undid: Duplicated TITLE.");

	// A rename's preview by the file's name alone; the rename itself, and its way back.
	menus.session.handle(request::preview_rename("main.mnu", kTitle, "name", "HEADER"));
	TEST_EXPECT(v.dialogs.rename_preview.refusals.empty() && v.dialogs.rename_preview.old_name == "TITLE" &&
	            v.dialogs.rename_preview.path == menus.menu().path());
	menus.session.handle(request::save("main.mnu"));
	editor_test::handle_to_end(menus.session, request::rename_symbol("main.mnu", kTitle, "name", "HEADER"));
	const ActivityView::LastRename &last = v.activity.last_rename;
	TEST_EXPECT(last.made && last.symbol && last.from == "TITLE" && last.to == "HEADER" && last.path == menus.menu().path() &&
	            last.locator == kTitle && last.field == "name");
	TEST_EXPECT(menus.menu().record_name(menus.at(kTitle)) == "HEADER");
	bool said = false;
	for (const std::string &line : v.activity.output)
		said = said || line.find("Undo does not take it back: Edit > Rename HEADER back to TITLE does.") != std::string::npos;
	TEST_EXPECT(said);
	// The way back: planned first (the definition, found by its name), then committed.
	editor_test::handle_to_end(menus.session, request::preview_rename_back(true));
	TEST_EXPECT(v.dialogs.rename_preview.back && v.dialogs.rename_preview.refusals.empty() &&
	            v.dialogs.rename_preview.old_name == "HEADER" && v.dialogs.rename_preview.new_name == "TITLE");
	editor_test::handle_to_end(menus.session, request::rename_back());
	TEST_EXPECT(menus.menu().record_name(menus.at(kTitle)) == "TITLE" && last.from == "HEADER" && last.to == "TITLE");
	TEST_EXPECT(v.activity.status == "Renamed HEADER back to TITLE.");
	std::printf("undo words: what Undo and Redo did, a rename's way back\n");
	return 0;
}

// Names are the game's, case-insensitive in its archives and in the project (ADR 0046 S17): a file
// named in another case, its folder's or its own, is the project's file (AssetScan::named through
// SessionCore::project_file), opened as itself (its own path, no document.missing), shown again when
// it is open already, edited and closed by a request that spells it so; the name in a folder it is
// not in, and a name the project has in no case, are still not found.
static int test_names_in_another_case() {
	Menus menus("opennova_editor_document_set_case");
	const SessionView &v = menus.view();
	menus.session.handle(request::create_file("Extra.mnu", asset_kind_token(AssetKind::Menu)));
	const Document *made = menus.session.document_for("Extra.mnu");
	TEST_EXPECT(made != nullptr);
	if (!made) return 1;
	const std::string path = made->path();
	TEST_EXPECT(path.find('/') != std::string::npos && path.substr(path.size() - 9) == "Extra.mnu");
	menus.session.handle(request::close_document(path));
	TEST_EXPECT(!menus.session.document_for(path));

	std::string shouted = path;
	for (char &c : shouted) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
	menus.session.handle(request::open_document(shouted));
	const Document *opened = menus.session.document_for(path);
	TEST_EXPECT(menus.session.outcome().done() && opened && opened->path() == path && v.documents.active == path);
	TEST_EXPECT(menus.session.outcome().findings.empty() && v.activity.status == "Opened " + path + ".");

	std::string lowered = path;
	for (char &c : lowered) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	menus.session.handle(request::open_document("main.mnu"));
	menus.session.handle(request::open_document(lowered));
	TEST_EXPECT(menus.session.document_for(lowered) == opened && v.documents.active == path &&
	            v.activity.status == "Showing " + path + ".");

	Edit rename;
	rename.address = opened->address_at(kMain);
	rename.field = "name";
	rename.value = std::string("SHOUTED");
	menus.session.handle(request::edit_record(shouted, rename));
	TEST_EXPECT(menus.session.last_edit_ok() && opened->dirty() && opened->record_name(opened->address_at(kMain)) == "SHOUTED");

	// Another folder is another file: the name in a folder it is not in opens nothing, edits nothing.
	std::string elsewhere = "strings/" + path.substr(path.find('/') + 1);
	menus.session.handle(request::open_document(elsewhere));
	TEST_EXPECT(!menus.session.outcome().done() && v.documents.active == path);
	menus.session.handle(request::edit_record(elsewhere, rename));
	TEST_EXPECT(!menus.session.last_edit_ok());
	// Closed by any spelling of it, as it opened (its edit undone: clean, no prompt holds the close).
	menus.session.handle(request::undo(path));
	TEST_EXPECT(!opened->dirty());
	menus.session.handle(request::close_document(lowered));
	TEST_EXPECT(!menus.session.document_for(path));
	menus.session.handle(request::open_document("MENUS/NOWHERE.MNU"));
	bool missing = false;
	for (const Diagnostic &finding : menus.session.outcome().findings)
		missing = missing || finding.code() == std::string("document.missing");
	TEST_EXPECT(!menus.session.outcome().done() && missing);
	// The demo round's bug 7: a request naming a path the project lacks (an open, a file's card, Files'
	// show) is refused, its finding the request's outcome, the status line and Output alone: Problems lists
	// the project's problems, and gets no document.missing row.
	const auto in_problems = [&] {
		for (const Diagnostic &row : v.findings.diagnostics)
			if (row.code() == std::string("document.missing")) return true;
		return false;
	};
	for (const EditorRequest &asked : {request::about_file("sounds/nowhere.wav"), request::show_in_files("sounds/nowhere.wav", false)}) {
		menus.session.handle(asked);
		bool refused = false;
		for (const Diagnostic &finding : menus.session.outcome().findings)
			refused = refused || finding.code() == std::string("document.missing");
		TEST_EXPECT(!menus.session.outcome().done() && refused &&
		            v.activity.status == "The project has no file sounds/nowhere.wav.");
	}
	TEST_EXPECT(!in_problems());
	return 0;
}

// The status line of a batch setting one field (ADR 0046 S15, S17): one record's by its title, to
// its value in words; several records' to the value they now share, or "to different values" when
// they do not (never one record's value said of all).
static int test_batch_status_says_each_value() {
	Menus menus("opennova_editor_document_set_status");
	const SessionView &v = menus.view();
	const auto text = [&](const char *locator, const char *value) {
		Edit edit;
		edit.address = menus.at(locator);
		edit.field = "string.value";
		edit.value = std::string(value);
		return edit;
	};
	menus.session.handle(request::edit_record("main.mnu", text(kTitle, "Same")));
	const std::string one = v.activity.status;
	const size_t of = one.find(" of "), to = one.rfind(" to ");
	TEST_EXPECT(menus.session.last_edit_ok() && one.rfind("Set ", 0) == 0 && of != std::string::npos &&
	            to != std::string::npos && one.back() == '.');
	if (of == std::string::npos || to == std::string::npos) return 1;
	const std::string title = one.substr(4, of - 4), shown = one.substr(to + 4, one.size() - to - 5);
	TEST_EXPECT(!title.empty() && shown.find("Same") != std::string::npos);

	menus.session.handle(request::edit_record("main.mnu", std::vector<Edit>{text(kTitle, "Same"), text(kExit, "Other")}));
	TEST_EXPECT(menus.session.last_edit_ok() && v.activity.status == "Set " + title + " of 2 records to different values.");
	menus.session.handle(request::edit_record("main.mnu", std::vector<Edit>{text(kTitle, "Same"), text(kExit, "Same")}));
	TEST_EXPECT(menus.session.last_edit_ok() && v.activity.status == "Set " + title + " of 2 records to " + shown + ".");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_batch_status_says_each_value();
	failures += test_paste_and_duplicate_agree();
	failures += test_remembered_selections();
	failures += test_selection_over_rows();
	failures += test_selection_concern();
	failures += test_undo_says_what();
	failures += test_names_in_another_case();
	if (failures == 0) std::printf("editor_document_set: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
