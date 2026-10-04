#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/ui/document_views.h>
#include <editor/ui/find_cursor.h>
#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class NewProjectForm;

// The centre of the workspace (ADR 0046 S11d): one tab per open document, in a tab bar
// that scrolls and lists every tab when they do not fit. A tab is the file's name (its
// path when another open file has the name, and in a tooltip), marked while the file has
// unsaved changes; its close button closes the document (the session asks first when it
// has unsaved changes, and a cancelled close leaves the tab: the tabs are the open
// documents). The active document's tab is selected when the active document changes,
// never otherwise, so a click is never fought; a tab a click (or the tab list) shows makes
// its document the active one, a click in the frame the active document changed included.
// A tab shows its document's view (S13 V3, ui/document_views): one per open document by its
// path, made from its type's row the first time the window draws the tab as the active document's
// or sends the document a RevealRecord, and kept with what it holds (its filter and order, what it
// has open, its caches) while the document is open there; rebound when the document is read again
// (the events it held dropped), made anew when the path's document is of another type, gone when
// it closes, so no two open documents share a view's state (a renamed file's document, closed and
// opened at its new path, gets a new one). Each shows the selection a Go to, a find or a Problems
// row moves there, and again for each RevealRecord view event its document is sent, which the view
// holds until it draws.
// With no project open it is the
// welcome page, the workspace's whole (the other windows stand aside for it); with nothing open it
// says how to open a file and, in a new project, how to bring in the game's files. Ctrl+F (Edit > Find...) opens the
// find bar over the active tab's view: every field whose value as the Inspector shows it holds
// the text (find_in_document), how many, the one shown of them, Enter and the arrows next and
// previous (Shift+Enter previous), the hits listed under it; a hit shown (or clicked) selects its
// record and reveals its field in the Inspector; Escape closes the bar. Where the bar is among the
// hits is its model's (FindCursor, S13 V1).
class DocumentWindow : public devtools::Window {
public:
	DocumentWindow(Workspace &workspace, NewProjectForm &form) : workspace_(workspace), form_(form) { open = true; }

	const char *title() const override { return "Document"; }
	bool is_closeable() const override { return false; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Center;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;
	// The views' own modals (a menu's Remove screen...), which the workspace draws every frame
	// with its modals, whether a tab shows them or not; the views of documents no longer open go
	// after (a closed menu's prompt closes first).
	void draw_modals();
	// After every frame's windows (the workspace's frame bracket): each view's (a Main-role view's
	// canvas not drawn this frame ends its gesture).
	void end_frame();
	// Opens the find bar with the keyboard in its text (Edit > Find..., Ctrl+F).
	void open_find();
	// A RevealRecord event, sent to the view of its document (made for it when the document is
	// open and has none yet), which holds it until it draws; dropped for a document not open.
	void receive(const ViewEvent &event);
	// The events the view of the document at `path` holds until it draws (0: none, or no view).
	size_t held_events(const std::string &path) const;
	// The view of the document open at `path`; null until the window meets the document.
	DocumentView *view_of(const std::string &path);
	// The view of the open `document` set by hand, kept and rebound as one its type's row made (a
	// test's: a Main-role view over a type no row gives one yet).
	void set_view(const DocumentBase &document, std::unique_ptr<DocumentView> view);

private:
	// A document's view, its type, and the document it is bound to: the instance and its load.
	struct Slot {
		std::unique_ptr<DocumentView> view;
		DocumentTypeId type = DocumentTypeId::None;
		uint64_t identity = 0;
		uint64_t load = 0;
	};
	// The view of the open `document`: made from its type's row the first time (and again for a
	// document of another type at the path), rebound when the document at its path is another
	// instance or was loaded again. Null for a document no type opens.
	DocumentView *view_for(const DocumentBase &document);
	// The views of documents no longer open go.
	void prune(const SessionView &view);
	void draw_tabs(const SessionView &view);
	// A project open with nothing open in it: how to open a file, and for a project of fewer than kFewFiles
	// files (a new one) the ways to bring in the game's.
	void draw_first_steps(const SessionView &view);
	static constexpr size_t kFewFiles = 40;
	void draw_find(const Document &document);
	// The find bar's hit `index` shown: its record selected, its field revealed.
	void show_hit(const Document &document, size_t index);

	Workspace &workspace_;
	NewProjectForm &form_;
	std::map<std::string, Slot> views_; // by the path of the open document each is for
	// The active document the tab bar last selected the tab of, and the document whose tab
	// the user chose, the OpenDocument raised for it.
	std::string followed_;
	std::string raised_;
	// The find bar: open, the keyboard to go to its text on the next draw, the text and whether
	// case matters, where it is among the hits.
	struct Find {
		bool open = false;
		bool focus = false;
		char text[128] = {};
		bool match_case = false;
		FindCursor cursor;
		bool scroll = false; // the hit shown moved: its line scrolled to
	};
	Find find_;
};

} // namespace opennova::editor
