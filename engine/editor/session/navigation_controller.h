#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <editor/session/editor_request.h>
#include <editor/session/navigation_history.h>
#include <editor/session/view/navigation_view.h>

namespace opennova::editor {

class SessionCore;
struct SessionView;

// The navigation history's part of the session (CONTEXT.md "Navigation history"): where the person is,
// the places a move took them from, and Back and Forward (navigate_back, navigate_forward), which take
// them there again. A place is the pane that shows it and the file (session/view/navigation_view.h): a
// document's tab with the record selected in it (a text's line a Go to showed), a file's page (the record a
// Go to marked there, DI-17), or Files on a file.
//
// What moves the person (the request table's `navigates` column, request_kinds.h) is a step: a document
// opened or switched to (Files, a tab, the unsaved files' list), a Go to (a reference, a use, a referrer,
// the find bar's and Find in project's hits, a Problems row, the mission view's Go to in outline and
// events using a record), a record selected in another document, another screen of a menu shown (a kind
// whose viewport shows one row of its document at a time), a new file opened, a texture's use shown,
// Show in Files and a file's card. Each is a request whether a window, the Shell or the editor MCP raises
// it, so the wire's moves are steps as a person's are. What is no step: a record picked within what shows
// (an outline's row, a click in a viewport, a marquee), an edit, a caret's move, a viewport's camera, a
// close, a reload, the documents a project reopens with: the place the person is at refines as they go,
// and the step a later move keeps is the place they left, as they left it. A run of quick moves is one
// step (NavigationHistory::kCoalesceMs); Back and Forward move places and keep none; a place whose file is
// gone is dropped, one whose file a rename moved follows it, one whose document closed opens again. The
// history is the open project's alone: it goes as the project closes and starts empty as one opens.
class NavigationController {
public:
	explicit NavigationController(SessionCore &core);
	NavigationController(const NavigationController &) = delete;
	NavigationController &operator=(const NavigationController &) = delete;

	// What a request may move, taken as it is served (serve_request) before its handler runs: where the
	// person was (only for a request whose row navigates: the step it may keep), the active document, the
	// selection's serial, the page shown and the view's next event.
	struct Mark {
		NavigationPlace place;
		std::string active;
		uint64_t selection = 0;
		std::string page;
		uint64_t events = 0;
	};
	Mark mark(const EditorRequest &request) const;
	// After its handler: the pane it showed (Files for a file it revealed there, a page for a file's page,
	// else the document); for a row that navigates, the place it left kept as a step where it took the
	// person somewhere else (a record picked in the document that shows is none: another document, or
	// another screen of a menu, is); for any other, the document's place moved by it (a record picked, a
	// close) is where the person is. A text's place a Go to showed is kept for the text.
	void follow(const EditorRequest &request, const Mark &before);
	// NavigateBack (`back`) and NavigateForward: the place `steps` that way shown again, where the person is
	// put on the other side with the places stepped over, the status line saying where. Refused quietly
	// (navigation.none: the outcome alone), nothing changed, with fewer places that way.
	void go(bool back, size_t steps);
	// Where the person is now: the pane shown last, its file, and in a document the record selected (a
	// text's line a Go to showed); nowhere with no document open.
	NavigationPlace here() const;
	// The project closes, or one opens: every place goes.
	void clear();
	// The files a rename moved (each from, to): their places follow them.
	void follow_moves(const std::vector<std::pair<std::string, std::string>> &moved);
	// The places of files the project no longer has dropped, once its files moved since the last look:
	// as each request is served and each operation ends (a refresh that found a file gone).
	void tidy();

private:
	// The active document's place.
	NavigationPlace document_place() const;
	// A file's page, or Files on a file.
	NavigationPlace file_place(NavigationPlace::Pane pane, const std::string &path) const;
	// A move of a navigating request's that is a step: one to another pane or file, and within one
	// document a Go to (an open naming a record), or a selection that shows another of a menu's screens.
	bool is_step(const EditorRequest &request, const NavigationPlace &from, const NavigationPlace &to) const;
	// `place` shown again: its document opened or made active, its record selected (by its address in the
	// read it was kept of, else by its locator), a text at its line, a page, or Files on its file; the
	// Document window comes forward with its tab (a ShowDocument view event).
	void show(const NavigationPlace &place);
	// The view's places as the history holds them, the Navigation concern moved.
	void publish();

	SessionCore &core_;
	SessionView &view_;
	NavigationHistory history_;
	// The pane the last move showed and, for a page or Files, its file: the person is there until the
	// document's place moves again.
	NavigationPlace::Pane pane_ = NavigationPlace::Pane::Document;
	std::string pane_path_;
	// Each text document's place a Go to last showed ("line:column"), by its path.
	std::map<std::string, std::string> text_places_;
	uint64_t files_seen_ = 0; // the Files concern's counter at the last tidy
};

// Whether Back (`back`) or Forward goes anywhere now, as the windows' buttons and shortcuts and the
// Shell's mouse buttons ask before they raise one: a project open, a place that way, the busy gate taking
// the request, and no dialog that takes the whole editor showing (a Back behind it would move what it is
// about).
bool navigation_offered(const SessionView &view, bool back);

} // namespace opennova::editor
