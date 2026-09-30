#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/ui/catalog_view.h>
#include <editor/ui/workspace.h>
#include <editor/ui/find_cursor.h>
#include <editor/ui/menu_view.h>
#include <editor/ui/record_reveal.h>
#include <editor/ui/strings_view.h>
#include <editor/ui/styles_view.h>
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
// A tab shows its type's view (a catalog, a string table, a stylesheet, a menu; a model, a
// clip or an animation table as its records' outline); each shows the selection a Go to, a
// find or a Problems row moves there (RecordReveal; a menu's window tree its own way: MenuView).
// With no project open it is the
// welcome view; with nothing open it says how to open a file. Ctrl+F (Edit > Find...) opens the
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
	// The views' own confirmations (a menu's Remove screen...), which the workspace draws
	// every frame with its modals, whether a tab shows them or not.
	void draw_modals();
	// Opens the find bar with the keyboard in its text (Edit > Find..., Ctrl+F).
	void open_find();

private:
	void draw_tabs(const SessionView &view);
	void draw_view(const Document &document);
	void draw_find(const Document &document);
	// The find bar's hit `index` shown: its record selected, its field revealed.
	void show_hit(const Document &document, size_t index);

	Workspace &workspace_;
	NewProjectForm &form_;
	CatalogView catalog_;
	StringsView strings_;
	StylesView styles_;
	MenuView menu_;
	RecordReveal outline_; // the outline's reveal of the selection (a model, a clip, an animation table)
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
