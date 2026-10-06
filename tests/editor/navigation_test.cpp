// The navigation history (CONTEXT.md "Navigation history"; session/navigation_history.h, the session's
// NavigationController): the history alone, clocked by the test (a move keeps where it left, a run of
// quick moves is one step, Back and Forward move places between the sides and keep none, a new move drops
// Forward, the farthest place past kMost goes, a place of a file gone drops, a renamed file's places
// follow it); and over a session on a fake platform whose clock the test steps: what is a step (a
// document switched to, a Go to, another screen of a menu, a page, Show in Files) and what is not (a
// record picked where the person is, a selection's refinement), Back and Forward showing a place again
// (a closed document opened again at its record by its locator, a text at its line, a page, Files), a
// place whose file is gone passed over, a rename's places following it, the history going with its
// project; and the wire: navigate_back and navigate_forward with steps, the navigation section, the
// catalog, the show_document event. DI-17, a Go to always lands: a file the editor has no editor for on its
// page with the record marked, a place with its mark; a native text at the record's line; a missing name
// where it belongs.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/document.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <editor/session/file_page.h>
#include <editor/session/navigation_history.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
using Pane = NavigationPlace::Pane;

namespace {

// The startup screen's records in the blank main menu, by locator (STARTUP: MAIN holding TITLE and EXIT).
constexpr const char *kStartup = "0";
constexpr const char *kTitle = "0/window:0/window:0";
constexpr const char *kExit = "0/window:0/window:1";

NavigationPlace at(const char *path, const char *locator = "", Pane pane = Pane::Document) {
	NavigationPlace place;
	place.pane = pane;
	place.path = path;
	place.locator = locator;
	place.label = path;
	return place;
}

bool paths_are(const std::deque<NavigationPlace> &side, std::vector<std::string> paths) {
	if (side.size() != paths.size()) return false;
	for (size_t i = 0; i < paths.size(); ++i)
		if (side[i].path != paths[i]) return false;
	return true;
}

// --- the history alone -----------------------------------------------------------------------------

// A move keeps where it left as Back's nearest; Back takes it and puts where the person was on Forward;
// Forward retraces; several at once step over places, which the other way retraces in order; a side with
// too few changes nothing; a new move from anywhere drops what Forward held.
int test_history_steps() {
	NavigationHistory history;
	const NavigationPlace a = at("a.mnu"), b = at("b.mnu"), c = at("c.mnu"), d = at("d.mnu");
	// From nowhere, or to the place it left: nothing kept.
	TEST_EXPECT(!history.moved(NavigationPlace(), a, 0) && history.back().empty());
	TEST_EXPECT(!history.moved(a, at("a.mnu"), 0) && history.back().empty());
	TEST_EXPECT(history.moved(a, b, 1000) && paths_are(history.back(), {"a.mnu"}) && history.forward().empty());
	TEST_EXPECT(history.moved(b, c, 5000) && paths_are(history.back(), {"b.mnu", "a.mnu"}));
	NavigationPlace to;
	TEST_EXPECT(history.step(true, 1, c, to) && to.path == "b.mnu");
	TEST_EXPECT(paths_are(history.back(), {"a.mnu"}) && paths_are(history.forward(), {"c.mnu"}));
	TEST_EXPECT(history.step(false, 1, b, to) && to.path == "c.mnu");
	TEST_EXPECT(paths_are(history.back(), {"b.mnu", "a.mnu"}) && history.forward().empty());
	// Two back at once: the one stepped over is Forward's nearest, where the person was behind it.
	TEST_EXPECT(history.step(true, 2, c, to) && to.path == "a.mnu");
	TEST_EXPECT(history.back().empty() && paths_are(history.forward(), {"b.mnu", "c.mnu"}));
	TEST_EXPECT(!history.step(true, 1, a, to) && paths_are(history.forward(), {"b.mnu", "c.mnu"}));
	TEST_EXPECT(!history.step(false, 3, a, to) && paths_are(history.forward(), {"b.mnu", "c.mnu"}));
	// A move from here drops Forward, as a browser's does.
	TEST_EXPECT(history.moved(a, d, 9000) && paths_are(history.back(), {"a.mnu"}) && history.forward().empty());
	// Back from nowhere (every document closed) keeps nothing on Forward.
	TEST_EXPECT(history.step(true, 1, NavigationPlace(), to) && to.path == "a.mnu" && history.forward().empty());
	return 0;
}

// A run of moves each within kCoalesceMs of the last is one step, Back going to where the run began, as
// long as the run goes on; Back or Forward ends a run, so the move after it is a step of its own; a place
// already Back's nearest is not kept twice.
int test_history_runs() {
	NavigationHistory history;
	const int64_t quick = NavigationHistory::kCoalesceMs / 2;
	TEST_EXPECT(history.moved(at("a.mnu"), at("b.mnu"), 0));
	TEST_EXPECT(!history.moved(at("b.mnu"), at("c.mnu"), quick) && paths_are(history.back(), {"a.mnu"}));
	TEST_EXPECT(!history.moved(at("c.mnu"), at("d.mnu"), 2 * quick) && paths_are(history.back(), {"a.mnu"}));
	const int64_t later = 2 * quick + NavigationHistory::kCoalesceMs;
	TEST_EXPECT(history.moved(at("d.mnu"), at("e.mnu"), later) && paths_are(history.back(), {"d.mnu", "a.mnu"}));
	NavigationPlace to;
	TEST_EXPECT(history.step(true, 1, at("e.mnu"), to) && to.path == "d.mnu");
	TEST_EXPECT(history.moved(at("d.mnu"), at("f.mnu"), later + 1) && paths_are(history.back(), {"d.mnu", "a.mnu"}) &&
	            history.forward().empty());
	// A move back from Back's nearest place keeps it once.
	TEST_EXPECT(!history.moved(at("d.mnu"), at("g.mnu"), later + 10 * NavigationHistory::kCoalesceMs) &&
	            paths_are(history.back(), {"d.mnu", "a.mnu"}));
	return 0;
}

// Each side keeps kMost places, the farthest going; a place of a file gone drops from both sides; a
// rename's places follow the file, their records found again by locator in the document read there.
int test_history_cap_drop_and_moves() {
	NavigationHistory history;
	const size_t moves = NavigationHistory::kMost + 10;
	for (size_t i = 0; i < moves; ++i)
		history.moved(at(("f" + std::to_string(i) + ".mnu").c_str()), at(("f" + std::to_string(i + 1) + ".mnu").c_str()),
		              int64_t(i) * 10 * NavigationHistory::kCoalesceMs);
	TEST_EXPECT(history.back().size() == NavigationHistory::kMost);
	TEST_EXPECT(history.back().front().path == "f" + std::to_string(moves - 1) + ".mnu" &&
	            history.back().back().path == "f" + std::to_string(moves - NavigationHistory::kMost) + ".mnu");
	history.clear();
	TEST_EXPECT(history.back().empty() && history.forward().empty());

	NavigationPlace kept = at("menus/a.mnu", kExit);
	kept.document = 7;
	kept.record = {3, 1, 9};
	TEST_EXPECT(history.moved(kept, at("b.mnu"), 0) && history.moved(at("b.mnu"), at("c.mnu"), 10000));
	NavigationPlace to;
	TEST_EXPECT(history.step(true, 1, at("c.mnu"), to)); // b.mnu taken; c.mnu Forward's, a.mnu Back's
	TEST_EXPECT(history.follow_moves({{"menus/a.mnu", "menus/z.mnu"}}) && history.back().front().path == "menus/z.mnu" &&
	            history.back().front().document == 0 && history.back().front().locator == kExit);
	TEST_EXPECT(!history.follow_moves({{"nothing.mnu", "else.mnu"}}));
	TEST_EXPECT(history.drop([](const NavigationPlace &place) { return place.path == "c.mnu"; }) && history.forward().empty() &&
	            history.back().size() == 1);
	TEST_EXPECT(!history.drop([](const NavigationPlace &) { return false; }));
	return 0;
}

// Two places are one by pane and file, and in a document by the record: the same address of the same
// read, or the same locator in any read of the file; none named in either is the document itself.
int test_same_place() {
	NavigationPlace a = at("main.mnu", kExit), b = at("main.mnu", kExit);
	a.document = b.document = 5;
	a.record = b.record = {4, 1, 12};
	TEST_EXPECT(same_place(a, b));
	b.record = {4, 1, 13};
	b.locator = kTitle;
	TEST_EXPECT(!same_place(a, b));
	b.document = 6; // read again: by its locator
	b.locator = kExit;
	TEST_EXPECT(same_place(a, b));
	TEST_EXPECT(same_place(at("main.mnu"), at("main.mnu")) && !same_place(at("main.mnu"), at("main.mnu", kExit)));
	TEST_EXPECT(!same_place(at("hit.wav", "", Pane::Page), at("hit.wav", "", Pane::Files)));
	TEST_EXPECT(same_place(at("hit.wav", "", Pane::Files), at("hit.wav", "", Pane::Files)));
	// A page by the record and field a Go to marked there (DI-17), or none.
	NavigationPlace foliage = at("isle.trn", "foliage 1", Pane::Page), again = at("isle.trn", "foliage 1", Pane::Page);
	foliage.field = again.field = "graphic";
	TEST_EXPECT(same_place(foliage, again) && !same_place(foliage, at("isle.trn", "", Pane::Page)));
	again.field = "texture";
	TEST_EXPECT(!same_place(foliage, again));
	again = at("isle.trn", "foliage 2", Pane::Page);
	again.field = "graphic";
	TEST_EXPECT(!same_place(foliage, again) && same_place(at("isle.trn", "", Pane::Page), at("isle.trn", "", Pane::Page)));
	return 0;
}

// --- the session -----------------------------------------------------------------------------------

// A new project with its required files and a second menu, on a platform whose clock the test steps:
// main.mnu opened first, then extra.mnu made and opened.
struct Navigated {
	editor_test::TempProjectDir dir;
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	std::string main, extra;
	explicit Navigated(const char *name) : dir(name), session(platform, preferences) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Places"));
		editor_test::create_missing_files(session);
		go(request::open_document("main.mnu"));
		go(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
		if (const Document *menu = session.document_for("main.mnu")) main = menu->path();
		if (const Document *menu = session.document_for("extra.mnu")) extra = menu->path();
	}
	const SessionView &view() const { return session.view(); }
	// A request a while after the last, so it is never part of the last move's run.
	bool go(const EditorRequest &request) {
		platform.clock += 10 * NavigationHistory::kCoalesceMs;
		return session.handle(request) && session.outcome().done();
	}
	bool back(uint32_t steps = 1) { return go(request::navigate_back(steps)); }
	bool forward(uint32_t steps = 1) { return go(request::navigate_forward(steps)); }
	NodeAddress record(const std::string &path, const char *locator) {
		const Document *document = session.document_for(path);
		return document ? document->address_at(locator) : NodeAddress();
	}
	bool showing(const std::string &path, const char *locator) {
		const Document *document = session.document_for(path);
		return document && view().documents.active == path && view().documents.selection.primary.row &&
		       document->locator(view().documents.selection.primary) == locator;
	}
	std::vector<std::string> back_paths() const {
		std::vector<std::string> out;
		for (const NavigationPlace &place : view().navigation.back) out.push_back(place.path);
		return out;
	}
};

bool refused_with(const ProjectSession &session, const char *code) {
	if (session.outcome().done()) return false;
	for (const Diagnostic &d : session.outcome().findings)
		if (d.code() == code) return true;
	return false;
}

// What moves the person: the first document opened is no step (from nowhere), a new file opened is,
// a record picked within the screen that shows is not (where the person is moves with it), a Go to is
// (another document's record, or another record of the same one); Back shows the place again, its
// record selected, the status line saying where; Forward retraces; nothing that way is refused quietly
// (navigation.none, nothing changed); a move after Back drops Forward; a run of quick moves is one step.
int test_session_steps() {
	Navigated n("opennova_editor_navigation_steps");
	const SessionView &v = n.view();
	TEST_EXPECT(!n.main.empty() && !n.extra.empty() && v.documents.active == n.extra);
	TEST_EXPECT(n.back_paths() == std::vector<std::string>({n.main}) && v.navigation.forward.empty());
	// A record picked in the screen that shows: no step, where the person is moves with it.
	// A record of another document selected: a step (the place left, extra.mnu's screen).
	TEST_EXPECT(n.go(request::select_record(n.main, n.record(n.main, kTitle))) && v.documents.active == n.main);
	TEST_EXPECT(n.back_paths() == std::vector<std::string>({n.extra, n.main}));
	const uint64_t moved = v.revisions.of(ViewConcern::Navigation);
	TEST_EXPECT(n.go(request::select_record(n.main, n.record(n.main, kExit))));
	TEST_EXPECT(n.back_paths() == std::vector<std::string>({n.extra, n.main}) && v.revisions.of(ViewConcern::Navigation) == moved);
	// A Go to within the document: a step, the place left the record picked last (EXIT).
	TEST_EXPECT(n.go(request::open_document(n.main, kTitle, "name")) && n.showing(n.main, kTitle));
	TEST_EXPECT(n.back_paths() == std::vector<std::string>({n.main, n.extra, n.main}));
	TEST_EXPECT(v.navigation.back.front().locator == kExit && v.navigation.back.front().label.find("EXIT") != std::string::npos);
	TEST_EXPECT(v.revisions.of(ViewConcern::Navigation) != moved);

	// Back: EXIT selected again, the title's place Forward's; the status line says where.
	TEST_EXPECT(n.back() && n.showing(n.main, kExit) && v.navigation.forward.size() == 1 &&
	            v.navigation.forward.front().locator == kTitle);
	TEST_EXPECT(v.activity.status.find("Back to ") == 0 && v.activity.status.find("EXIT") != std::string::npos);
	TEST_EXPECT(n.back() && v.documents.active == n.extra && n.back_paths() == std::vector<std::string>({n.main}));
	TEST_EXPECT(n.back() && v.documents.active == n.main && v.navigation.back.empty() && v.navigation.forward.size() == 3);
	const uint64_t before = v.revisions.of(ViewConcern::Navigation);
	TEST_EXPECT(!n.back() && refused_with(n.session, "navigation.none") && v.documents.active == n.main &&
	            v.revisions.of(ViewConcern::Navigation) == before && v.navigation.forward.size() == 3);
	// Forward two at once, then the last: the Go to's record.
	TEST_EXPECT(n.forward(2) && n.showing(n.main, kExit) && v.navigation.forward.size() == 1);
	TEST_EXPECT(n.forward() && n.showing(n.main, kTitle) && v.navigation.forward.empty());
	TEST_EXPECT(!n.forward() && refused_with(n.session, "navigation.none"));
	// A move after Back drops Forward.
	TEST_EXPECT(n.back() && v.navigation.forward.size() == 1);
	TEST_EXPECT(n.go(request::open_document(n.extra)) && v.navigation.forward.empty() && n.back_paths().front() == n.main);
	// A run of quick moves (rows clicked through): one step, Back going to where it began.
	const size_t kept = v.navigation.back.size();
	n.platform.clock += 10 * NavigationHistory::kCoalesceMs;
	n.session.handle(request::open_document(n.main, kTitle, "name"));
	n.platform.clock += NavigationHistory::kCoalesceMs / 4;
	n.session.handle(request::open_document(n.main, kExit, "name"));
	n.platform.clock += NavigationHistory::kCoalesceMs / 4;
	n.session.handle(request::open_document(n.extra));
	TEST_EXPECT(v.navigation.back.size() == kept + 1 && v.navigation.back.front().path == n.extra);
	return 0;
}

// A menu's screens are its pages: a record picked on another screen is a step, one within the screen is
// none; Back shows the screen it left.
int test_session_screens() {
	Navigated n("opennova_editor_navigation_screens");
	const SessionView &v = n.view();
	TEST_EXPECT(n.go(request::open_document(n.main)) && n.go(request::select_record(n.main, n.record(n.main, kStartup))));
	TEST_EXPECT(n.go(request::duplicate(n.main))); // a second screen, selected
	const Document *menu = n.session.document_for(n.main);
	TEST_EXPECT(menu && menu->rows().size() == 2 && v.documents.selection.primary.row == menu->rows()[1]->id);
	if (!menu || menu->rows().size() != 2) return 1;
	const size_t kept = v.navigation.back.size();
	TEST_EXPECT(n.go(request::select_record(n.main, n.record(n.main, kTitle)))); // the first screen's TITLE
	TEST_EXPECT(v.navigation.back.size() == kept + 1 && v.navigation.back.front().locator == "1");
	TEST_EXPECT(n.go(request::select_record(n.main, n.record(n.main, kExit)))); // the same screen: no step
	TEST_EXPECT(v.navigation.back.size() == kept + 1);
	TEST_EXPECT(n.back() && v.documents.selection.primary.row == menu->rows()[1]->id && !v.documents.selection.primary.child);
	return 0;
}

// A place whose document closed opens it again, its record found by its locator; one whose file is gone
// (deleted, the project read again) is dropped from both sides, Back passing over it; a renamed file's
// places follow it; the history goes with its project, and a project opens with none.
int test_session_closed_gone_renamed() {
	Navigated n("opennova_editor_navigation_closed");
	const SessionView &v = n.view();
	TEST_EXPECT(n.go(request::open_document(n.main, kExit, "name")) && n.go(request::open_document(n.extra)));
	TEST_EXPECT(v.navigation.back.front().path == n.main && v.navigation.back.front().locator == kExit);
	TEST_EXPECT(n.go(request::close_document(n.main)) && !n.session.document_for(n.main));
	TEST_EXPECT(n.back() && n.session.document_for(n.main) && n.showing(n.main, kExit));
	TEST_EXPECT(v.navigation.forward.size() == 1 && v.navigation.forward.front().path == n.extra);

	// Renamed: its places follow it (read again at the new path, the record by its locator).
	TEST_EXPECT(editor_test::handle_to_end(n.session, request::rename_asset(n.extra, "renamed.mnu")).done());
	const Document *renamed = n.session.document_for("renamed.mnu");
	TEST_EXPECT(renamed && v.navigation.forward.size() == 1 && v.navigation.forward.front().path == renamed->path());
	if (!renamed) return 1;
	const std::string renamed_path = renamed->path();
	TEST_EXPECT(n.forward() && v.documents.active == renamed_path);

	// Gone: closed, deleted on disk, the project read again: Back passes over its place.
	TEST_EXPECT(n.go(request::open_document(n.main)) && n.back_paths().front() == renamed_path);
	TEST_EXPECT(n.go(request::close_document(renamed_path)));
	std::error_code ec;
	std::filesystem::remove(system_path(join_path(v.project.root, renamed_path)), ec);
	TEST_EXPECT(!ec);
	editor_test::handle_to_end(n.session, request::rescan());
	for (const NavigationPlace &place : v.navigation.back) TEST_EXPECT(place.path != renamed_path);
	for (const NavigationPlace &place : v.navigation.forward) TEST_EXPECT(place.path != renamed_path);

	// The project closes: its history goes; opened again, it starts with none.
	TEST_EXPECT(!v.navigation.back.empty());
	TEST_EXPECT(n.go(request::close_project()) && v.navigation.back.empty() && v.navigation.forward.empty());
	TEST_EXPECT(!n.back() && refused_with(n.session, "navigation.none"));
	editor_test::handle_to_end(n.session, request::open_project(n.dir.file("project")));
	TEST_EXPECT(v.project.open && v.navigation.back.empty() && v.navigation.forward.empty());
	return 0;
}

// The panes: a file the editor has no editor for shows its page, a place of its own (Back from the
// document to it posts show_document flagged for the page, the Document window's About tab over the
// document's); Show in Files is a place in Files (Forward to it reveals the file there again); a Go to a
// text's line is a place at that line (Back to it reveals the line again).
int test_session_panes() {
	Navigated n("opennova_editor_navigation_panes");
	const SessionView &v = n.view();
	std::string font, gametext;
	for (const AssetEntry &entry : v.project.scan->entries) {
		if (entry.kind == AssetKind::Font && font.empty()) font = entry.relative_path;
		if (basename_of(entry.relative_path) == "gametext.bin") gametext = entry.relative_path;
	}
	TEST_EXPECT(!font.empty() && !gametext.empty());
	// A page, then the active document's tab again: each a step.
	TEST_EXPECT(n.go(request::open_document(font)) && v.documents.page == font);
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Document && v.navigation.back.front().path == n.extra);
	TEST_EXPECT(n.go(request::open_document(n.extra)));
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Page && v.navigation.back.front().path == font &&
	            v.navigation.back.front().label == "About " + basename_of(font));
	uint64_t seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.back() && v.documents.page == font);
	std::vector<ViewEvent> shown = editor_test::events_after(v, seq, ViewEventKind::ShowDocument);
	TEST_EXPECT(shown.size() == 1 && shown[0].path == font && shown[0].flag);
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.forward() && v.documents.active == n.extra);
	shown = editor_test::events_after(v, seq, ViewEventKind::ShowDocument);
	TEST_EXPECT(shown.size() == 1 && shown[0].path == n.extra && !shown[0].flag);

	// Show in Files: a place in Files.
	TEST_EXPECT(n.go(request::show_in_files(gametext)));
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Document && v.navigation.back.front().path == n.extra);
	TEST_EXPECT(n.back() && v.documents.active == n.extra && v.navigation.forward.front().pane == Pane::Files &&
	            v.navigation.forward.front().path == gametext);
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.forward() && v.documents.file_selected.path == gametext);
	const std::vector<ViewEvent> revealed = editor_test::events_after(v, seq, ViewEventKind::RevealFile);
	TEST_EXPECT(revealed.size() == 1 && revealed[0].path == gametext);
	// A record picked in the document from there: the person is back at the document, a place there.
	TEST_EXPECT(n.go(request::select_record(n.main, n.record(n.main, kTitle))));
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Files && v.navigation.back.front().path == gametext);

	// A text's line a Go to showed.
	TEST_EXPECT(n.go(request::create_file("intro.wac")));
	const DocumentBase *script = n.session.document_base_for("intro.wac");
	TEST_EXPECT(script && v.documents.active == script->path());
	if (!script) return 1;
	const std::string script_path = script->path();
	TEST_EXPECT(n.go(request::open_document(script_path, "1:1")));
	TEST_EXPECT(n.go(request::open_document(n.main)));
	TEST_EXPECT(v.navigation.back.front().path == script_path && v.navigation.back.front().locator == "1:1" &&
	            v.navigation.back.front().label == "intro.wac, line 1");
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.back() && v.documents.active == script_path);
	const std::vector<ViewEvent> lines = editor_test::events_after(v, seq, ViewEventKind::RevealText);
	TEST_EXPECT(lines.size() == 1 && lines[0].locator == "1:1");
	return 0;
}

JsonValue parsed(const std::string &text) {
	JsonValue out;
	std::string error;
	opennova::io::json_parse(text, out, error);
	return out;
}

// The lines of a page a Go to marked.
std::vector<std::string> marked_lines(const FilePage &page) {
	std::vector<std::string> out;
	for (const FilePageDefinition &defined : page.defines)
		if (defined.at) out.push_back(defined.text);
	for (const FilePageLine &line : page.names)
		if (line.at) out.push_back(line.text);
	return out;
}

// DI-17, a Go to always lands: a file the editor has no editor for lands on its page, the record it names
// marked (a terrain's colour map, from the texture it names), a place of its own with its mark (another line
// of the same page a step, Back and Forward marking each again); a wave's page, whose user is a sound bank,
// a document of its own (S22: the specific document wins), there opened at the single; a native text held as
// a text (DI-06: an avatar table, whose parser keeps no places) at the line that writes the record's name (of
// two records naming one model each its own line, a record named alone its first, the text already open),
// again on Back; a name nothing resolves where it belongs (a string id in its table, a style variable in a
// stylesheet), a file the project lacks nowhere; the page's wire: what it names with its mark, a wave's Play,
// where its lines go.
int test_go_to_lands() {
	Navigated n("opennova_editor_navigation_lands");
	const SessionView &v = n.view();
	const std::string fixtures = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/";
	std::string avatars = "Avatars.def";
	for (const AssetEntry &entry : v.project.scan->entries)
		if (entry.kind == AssetKind::AvatarDefs) avatars = entry.relative_path;
	const std::string root = v.project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/sounds/menu.lwf", test_io::read_file(fixtures + "lwf/menu.lwf")) &&
	            editor_test::write_bytes(root + "/sounds/tone.wav", test_io::read_file(fixtures + "lwf/tone.wav")) &&
	            editor_test::write_bytes(root + "/" + avatars, test_io::read_file(fixtures + "avatars/synth_avatars.def")) &&
	            editor_test::write_text(root + "/textures/map.tga", "not a picture") &&
	            editor_test::write_text(root + "/textures/grain.tga", "not a picture") &&
	            editor_test::write_text(root + "/terrains/isle.trn",
	                                    "polytrn_colormap map.tga\npolytrn_detailmap grain.tga\npolytrn_polydata isle.cpt\n"));
	editor_test::handle_to_end(n.session, request::rescan());
	const std::string bank = "sounds/menu.lwf", wave = "sounds/tone.wav", terrain = "terrains/isle.trn";

	// The terrain's page, from the texture its colour map names: that line marked, the page's tab forward.
	ReferenceTarget colormap;
	for (const FilePageLine &line : file_page(v, "textures/map.tga").used_by)
		if (line.target.file == terrain) colormap = line.target;
	TEST_EXPECT(colormap.locator.empty() && colormap.field == "polytrn_colormap" && !colormap.editable);
	uint64_t seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.go(request::open_document(colormap.file, colormap.locator, colormap.field)) && v.documents.page == terrain &&
	            v.documents.page_locator.empty() && v.documents.page_field == "polytrn_colormap");
	std::vector<ViewEvent> shown = editor_test::events_after(v, seq, ViewEventKind::ShowDocument);
	TEST_EXPECT(shown.size() == 1 && shown[0].path == terrain && shown[0].flag);
	TEST_EXPECT(v.activity.status.find("at polytrn_colormap") != std::string::npos);
	const FilePage terrain_page = shown_file_page(v, terrain);
	const std::vector<std::string> marked = marked_lines(terrain_page);
	TEST_EXPECT(marked.size() == 1 && marked[0].rfind("polytrn_colormap: ", 0) == 0 &&
	            marked[0].find("map.tga") != std::string::npos);
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Document && v.navigation.back.front().path == n.extra);
	// Its missing height data: no file to go to (its finding in Problems holds the fixes); the page alone marks
	// nothing.
	size_t missing = 0;
	for (const FilePageLine &line : terrain_page.names)
		if (line.missing) missing += line.target.file.empty() && line.name == "isle.cpt" ? 1 : 100;
	TEST_EXPECT(missing == 1);
	TEST_EXPECT(marked_lines(file_page(v, terrain)).empty());

	// Another line of the same page: a step of its own, the place left the page at the colour map.
	TEST_EXPECT(n.go(request::open_document(terrain, "", "polytrn_detailmap")) && v.documents.page_field == "polytrn_detailmap");
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Page && v.navigation.back.front().path == terrain &&
	            v.navigation.back.front().locator.empty() && v.navigation.back.front().field == "polytrn_colormap" &&
	            v.navigation.back.front().label == "About isle.trn: polytrn_colormap");
	TEST_EXPECT(n.back() && v.documents.page == terrain && v.documents.page_field == "polytrn_colormap" &&
	            marked_lines(shown_file_page(v, terrain)) == marked);
	TEST_EXPECT(n.back() && v.documents.active == n.extra);
	TEST_EXPECT(n.forward(2) && v.documents.page == terrain && v.documents.page_field == "polytrn_detailmap");

	// A wave's page, its Play; its user, the bank, is a document of its own: opened at the single.
	TEST_EXPECT(n.go(request::open_document(wave)) && v.documents.page == wave && v.documents.page_locator.empty());
	const FilePage wave_page = shown_file_page(v, wave);
	ReferenceTarget single;
	for (const FilePageLine &line : wave_page.used_by)
		if (line.target.file == bank) single = line.target;
	TEST_EXPECT(wave_page.found && wave_page.wave && single.editable && !single.locator.empty());
	TEST_EXPECT(n.go(request::open_document(single.file, single.locator, single.field)) && v.documents.active == bank);
	const DocumentBase *bank_document = n.session.document_base_for(bank);
	const Document *bank_records = bank_document ? records_of(*bank_document) : nullptr;
	TEST_EXPECT(bank_records && v.documents.selection.primary.row &&
	            bank_records->locator(v.documents.selection.primary) == single.locator);
	TEST_EXPECT(v.navigation.back.front().pane == Pane::Page && v.navigation.back.front().path == wave);

	// A native text at the record's line: two heads name synth_boonie.3di, each Go to its own line.
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.go(request::open_document(avatars, "SYN_HEAD_BOONIE_CAMO_1", "graphic")));
	const DocumentBase *text = n.session.document_base_for(avatars);
	std::vector<ViewEvent> lines = editor_test::events_after(v, seq, ViewEventKind::RevealText);
	TEST_EXPECT(text && text_of(*text) && lines.size() == 1 && lines[0].locator == "15:11");
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.go(request::open_document(avatars, "SYN_HEAD_BOONIE", "graphic")));
	lines = editor_test::events_after(v, seq, ViewEventKind::RevealText);
	TEST_EXPECT(lines.size() == 1 && lines[0].locator == "6:11");
	TEST_EXPECT(v.navigation.back.front().path == avatars && v.navigation.back.front().locator == "15:11" &&
	            v.navigation.back.front().label == basename_of(avatars) + ", line 15");
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.back());
	lines = editor_test::events_after(v, seq, ViewEventKind::RevealText);
	TEST_EXPECT(lines.size() == 1 && lines[0].locator == "15:11");
	// A record named alone, the text open: the first name the parser reads of it (SYN_HEAD_2's model).
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.go(request::open_document(avatars, "SYN_HEAD_2")));
	lines = editor_test::events_after(v, seq, ViewEventKind::RevealText);
	TEST_EXPECT(lines.size() == 1 && lines[0].locator == "24:11");
	// A record the text does not hold: the text, at no line.
	seq = v.events.next_seq() - 1;
	TEST_EXPECT(n.go(request::open_document(avatars, "NO_SUCH_HEAD", "graphic")) &&
	            editor_test::events_after(v, seq, ViewEventKind::RevealText).empty());

	// A name nothing resolves: where it belongs; a file the project lacks, nowhere.
	std::string gametext, style;
	for (const AssetEntry &entry : v.project.scan->entries) {
		if (basename_of(entry.relative_path) == "gametext.bin") gametext = entry.relative_path;
		if (entry.kind == AssetKind::MenuStyle && style.empty()) style = entry.relative_path;
	}
	ReferenceTarget home;
	TEST_EXPECT(!gametext.empty() &&
	            missing_target(ReferenceSubject{ReferenceKind::TextId, "NO_SUCH_KEY", "GAMETEXT.BIN/WepDes", -1}, v, home) &&
	            home.file == gametext && home.missing && home.editable && home.locator.empty());
	TEST_EXPECT(!style.empty() && missing_target(ReferenceSubject{ReferenceKind::StyleVar, "NO_SUCH_VAR", "", -1}, v, home) &&
	            home.missing && asset_kind_row(v.project.scan->at_path(home.file)->kind).document != DocumentTypeId::None);
	TEST_EXPECT(!missing_target(ReferenceSubject{ReferenceKind::Model, "nothing.3di", "", -1}, v, home));
	TEST_EXPECT(!missing_target(ReferenceSubject{ReferenceKind::TextId, "NO_SUCH_KEY", "", -1}, v, home));
	TEST_EXPECT(n.go(request::open_document(gametext)) && v.documents.active == gametext &&
	            v.navigation.back.front().path == avatars);

	// The wire: the page showing with its mark, what it names going where, the wave's Play.
	TEST_EXPECT(n.go(request::open_document(terrain, "", "polytrn_colormap")));
	std::string error;
	JsonValue page = n.session.query("file_page", JsonValue(), error);
	TEST_EXPECT(page.get_string("path", "") == terrain && !page.get("at_locator") &&
	            page.get_string("at_field", "") == "polytrn_colormap" && page.get("defines") && !page.get("wave"));
	bool at_map = false;
	if (const JsonValue *names = page.get("names"))
		for (const JsonValue &line : names->array)
			at_map |= line.get_bool("at", false) && line.get_string("file", "") == "textures/map.tga";
	TEST_EXPECT(at_map);
	page = n.session.query("file_page", parsed(R"({"path": "sounds/tone.wav"})"), error);
	bool to_bank = false;
	if (const JsonValue *users = page.get("used_by"))
		for (const JsonValue &line : users->array)
			to_bank |= line.get_string("file", "") == bank && line.get_string("locator", "") == single.locator;
	TEST_EXPECT(page.get_bool("wave", false) && to_bank && !page.get("at_field"));
	const JsonValue state = n.session.query("state", parsed(R"({"sections": ["documents", "navigation"]})"), error);
	const JsonValue *documents = state.get("documents");
	TEST_EXPECT(documents && documents->get_string("page", "") == terrain && !documents->get("page_locator") &&
	            documents->get_string("page_field", "") == "polytrn_colormap");
	return 0;
}

JsonValue navigation_section(ProjectSession &session) {
	std::string error;
	const JsonValue state = session.query("state", parsed(R"({"sections": ["navigation"]})"), error);
	const JsonValue *section = state.get("navigation");
	return section ? *section : JsonValue();
}

// The wire: navigate_back and navigate_forward with steps (1 or more; 0 is not read), the navigation
// section (can_back, can_forward, each side's places with their pane, path, label and record), the
// catalog listing both kinds and the field, and the show_document event's token.
int test_wire() {
	Navigated n("opennova_editor_navigation_wire");
	const SessionView &v = n.view();
	TEST_EXPECT(n.go(request::open_document(n.main, kExit, "name")) && n.go(request::open_document(n.extra)));
	JsonValue section = navigation_section(n.session);
	TEST_EXPECT(section.get_bool("can_back", false) && !section.get_bool("can_forward", true));
	const JsonValue *back = section.get("back");
	TEST_EXPECT(back && back->array.size() == 3);
	if (!back || back->array.size() != 3) return 1;
	const JsonValue &nearest = back->array[0];
	TEST_EXPECT(nearest.get_string("pane", "") == "document" && nearest.get_string("path", "") == n.main &&
	            nearest.get_string("locator", "") == kExit && nearest.get("record") &&
	            nearest.get_string("label", "").find("EXIT") != std::string::npos);

	n.platform.clock += 10 * NavigationHistory::kCoalesceMs;
	JsonValue answer = n.session.handle_json(parsed(R"({"kind": "navigate_back", "steps": 2})"));
	TEST_EXPECT(answer.get_bool("ok", false) && answer.get("outcome") && answer.get("outcome")->get_bool("done", false));
	TEST_EXPECT(v.documents.active == n.extra && v.navigation.back.size() == 1 && v.navigation.forward.size() == 2);
	section = navigation_section(n.session);
	TEST_EXPECT(section.get_bool("can_back", false) && section.get_bool("can_forward", false));
	answer = n.session.handle_json(parsed(R"({"kind": "navigate_forward", "steps": 0})"));
	TEST_EXPECT(!answer.get_bool("ok", true) && answer.get_string("error", "").find("1 or more") != std::string::npos);
	answer = n.session.handle_json(parsed(R"({"kind": "navigate_forward"})"));
	TEST_EXPECT(answer.get_bool("ok", false) && v.navigation.forward.size() == 1);
	answer = n.session.handle_json(parsed(R"({"kind": "navigate_forward", "steps": 5})"));
	TEST_EXPECT(answer.get_bool("ok", false) && !answer.get("outcome")->get_bool("done", true) &&
	            refused_with(n.session, "navigation.none"));

	std::string error;
	const JsonValue catalog = n.session.query("catalog", JsonValue(), error);
	bool back_listed = false, forward_listed = false, steps_listed = false;
	if (const JsonValue *requests = catalog.get("requests"))
		for (const JsonValue &row : requests->array) {
			back_listed |= row.get_string("kind", "") == "navigate_back";
			forward_listed |= row.get_string("kind", "") == "navigate_forward";
		}
	if (const JsonValue *fields = catalog.get("fields"))
		for (const JsonValue &row : fields->array) steps_listed |= row.get_string("field", "") == "steps";
	TEST_EXPECT(back_listed && forward_listed && steps_listed);
	TEST_EXPECT(std::string(view_event_kind_token(ViewEventKind::ShowDocument)) == "show_document");
	TEST_EXPECT(request_kind_row(EditorRequestKind::OpenDocument).navigates &&
	            request_kind_row(EditorRequestKind::SelectRecord).navigates &&
	            !request_kind_row(EditorRequestKind::NavigateBack).navigates &&
	            !request_kind_row(EditorRequestKind::EditRecord).navigates);
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_history_steps();
	failed += test_history_runs();
	failed += test_history_cap_drop_and_moves();
	failed += test_same_place();
	failed += test_session_steps();
	failed += test_session_screens();
	failed += test_session_closed_gone_renamed();
	failed += test_session_panes();
	failed += test_wire();
	failed += test_go_to_lands();
	if (failed == 0) std::printf("editor_navigation: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
