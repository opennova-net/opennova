#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/ui/document_views.h>
#include <editor/ui/record_tree.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

class MnuDocument;

// A menu in its Document tab (ADR 0046 S6c, S9h2, S11d, S11e): Reload / Undo / Redo, its
// screens (add, duplicate, remove after a prompt, never the last one, reorder), and the
// selected screen's windows as a tree that scrolls sideways when deep: a click selects one
// (Ctrl+click joins or leaves the selection, Shift+click selects the rows from the last one
// clicked), a drag drops a window before, after or inside another (one Move each), and the
// toolbars (rows that wrap) add a window of a chosen type, duplicate and remove the selected
// windows, reorder, indent and outdent the primary one, and copy, cut, paste and duplicate
// windows (Ctrl+C / X / V / D). Each screen and window is marked when it was added or
// changed since the last save; a window's tooltip names its type and how the file writes
// it. A window's fields and lists are the inspector's; the picture is the preview window's.
// S13 V3: one view per open menu (a DocumentView), what it keeps (the type a new window takes,
// the tree, a range's anchor, the prompt's question) its own; a RevealRecord its document is sent
// opens the selection's owners and scrolls to it again.
class MenuView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	// The prompt Remove screen... asks, drawn by the workspace every frame (a tab not shown
	// draws nothing, and a modal no frame draws would hold the input): by this view alone
	// while it asks.
	void draw_modals(Workspace &workspace) override;
	void rebind(const DocumentBase &document) override;

	// How many times the screen's tree was made.
	size_t trees_made() const { return trees_made_; }

private:
	const Node *draw_screens(Workspace &workspace, const MnuDocument &document);
	void draw_windows(Workspace &workspace, const MnuDocument &document, const Node &screen);
	void draw_window_node(Workspace &workspace, const MnuDocument &document, size_t index);
	void click_window(Workspace &workspace, const MnuDocument &document, size_t index);
	// The tree and each window's line, made again only when the document or the screen changes: an
	// edit, an undo or a redo (the revision alone) whose change set leaves the screen's row as it
	// was (another screen's, S13 V8) keeps it.
	void refresh_tree(const MnuDocument &document, const Node &screen);

	// What the view shows of its own is the workspace's (the MCP gaps lane: workspace.document's
	// new_window_type and remove_screen): the menu's path as last drawn, whose state it reads.
	std::string path_;
	void send(Workspace &workspace, const char *member, io::JsonValue value) const;

	std::string add_type_ = "static"; // the type a new window takes
	ui_kit::Held<std::string> add_type_held_;
	RecordTree tree_;
	std::vector<std::string> lines_; // per tree entry: the name and the type's name, after the change dot's room
	std::vector<std::string> tips_;  // per tree entry: the type's name and how the file writes it
	uint64_t tree_document_ = 0, tree_load_ = 0, tree_revision_ = 0;
	NodeAddress revealed_;       // the selection the tree last opened its owners for
	std::vector<NodeId> reveal_; // the windows to open this frame
	NodeId scroll_to_ = 0;       // the window to scroll into view this frame
	ui_kit::HeldPopup remove_popup_; // Remove screen...'s prompt, open while the workspace names its screen
	NodeId anchor_ = 0;          // the window a Shift+click selects from (the last one clicked)
	size_t trees_made_ = 0;
};

} // namespace opennova::editor
