#include "menu_view.h"

#include <editor/documents/mnu_clipboard.h>
#include <editor/documents/mnu_document.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/model/field_text.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstring>
#include <variant>

#include <imgui.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
constexpr NodeKind kWindow = node_kind(MenuKind::Window);
constexpr const char *kRemovePrompt = "Remove screen?";
constexpr const char *kWindowPayload = "OPENNOVA_MENU_WINDOW";

using window_requests::clipboard;
using window_requests::edit;
using window_requests::select;

// The window field a type picker takes its choices from.
const FieldSchema *type_field(const Document &document) {
	for (const FieldSchema &field : document.fields(kWindow))
		if (field.id == "type") return &field;
	return nullptr;
}

// A type as the file writes it, for a tooltip: `type="static"`.
std::string written_type(const std::string &token) { return "The file writes it as type=\"" + token + "\"."; }

} // namespace

void MenuView::send(Workspace &workspace, const char *member, io::JsonValue value) const {
	io::JsonValue members = io::JsonValue::make_object();
	members.set("path", io::JsonValue::make_string(path_));
	members.set(member, std::move(value));
	window_requests::set_workspace(workspace, "document", std::move(members));
}

void MenuView::draw(Workspace &workspace, const DocumentBase &base) {
	// A RevealRecord taken: the tree opens the selection's owners and scrolls to it again.
	if (!take_events().empty()) revealed_ = NodeAddress();
	const auto *document = dynamic_cast<const MnuDocument *>(records_of(base));
	if (!document) return ui_kit::empty_state("This file holds no menu screens to list.");
	path_ = document->path();
	const WorkspaceView::DocumentView &held = workspace.view().workspace.document(path_);
	if (add_type_held_.follow(held.new_window_type)) add_type_ = held.new_window_type;
	draw_document_toolbar(workspace, *document);
	ImGui::BeginDisabled(document->blocked());
	if (const Node *screen = draw_screens(workspace, *document)) draw_windows(workspace, *document, *screen);
	else ui_kit::empty_state("Select a screen to see its windows.");
	ImGui::EndDisabled();
}

void MenuView::rebind(const DocumentBase &) {
	// A range's anchor and the tree name the old document's records; the prompt's question the session
	// drops as it reads the document again (its screens' ids start again), so it closes with nothing removed.
	anchor_ = 0;
	tree_document_ = 0;
	revealed_ = NodeAddress();
}

// The screens in file order with their tools (Add / Duplicate / Remove / Up / Down for the
// selected one), each marked when it was added or changed since the last save.
const Node *MenuView::draw_screens(Workspace &workspace, const MnuDocument &document) {
	const SessionView &view = workspace.view();
	const auto &rows = document.rows();
	const Node *current = document.row(view.documents.selection.primary.row);
	size_t index = 0;
	while (index < rows.size() && rows[index].get() != current) ++index;
	const NodeAddress address{current ? current->id : 0, kScreen, 0};
	ImGui::SeparatorText("Screens");
	ui_kit::WrapRow row;
	ui_kit::RowTools tools;
	tools.count = rows.size();
	tools.selected = current ? index : SIZE_MAX;
	tools.add = "Add screen";
	tools.duplicate = "Duplicate screen";
	tools.remove = "Remove screen...";
	tools.up = "Up##screen";
	tools.down = "Down##screen";
	tools.add_tip = current ? "Adds a screen after the selected one." : "Adds a screen at the end of the menu.";
	tools.duplicate_tip = "A copy right after it, with a name no other screen has.";
	tools.remove_tip = "Removes the screen and every window on it (asks first).";
	tools.pick = "Select a screen first.";
	if (rows.size() < 2) tools.keeps = "A menu keeps at least one screen.";
	switch (ui_kit::row_tools(row, tools)) {
	case ui_kit::RowTool::Add: edit(workspace, document, EditOperation::Add, {0, kScreen, 0}, current ? index + 1 : SIZE_MAX); break;
	case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, index + 1); break;
	case ui_kit::RowTool::Remove:
		// The prompt is the workspace's: open while it names the screen.
		send(workspace, "remove_screen", io::JsonValue::make_number(double(address.row)));
		break;
	case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, index - 1); break;
	case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, index + 1); break;
	case ui_kit::RowTool::None: break;
	}
	for (const auto &screen : rows) {
		ImGui::PushID(static_cast<int>(screen->id));
		const NodeAddress at{screen->id, kScreen, 0};
		const std::string name = ui_kit::kChangeRoom + (screen->name().empty() ? std::string("(no name)") : screen->name());
		const std::string shown = ui_kit::fit(name, ImGui::GetContentRegionAvail().x);
		const float x = ImGui::GetCursorScreenPos().x;
		if (ImGui::Selectable((shown + "###screen").c_str(), current == screen.get() && !view.documents.selection.primary.child)) select(workspace, document, at);
		const Document::RecordChange change = document.record_change(at);
		ui_kit::change_dot(change, x);
		const std::string words = ui_kit::change_words(change);
		ui_kit::tooltip(shown != name ? screen->name() + (words.empty() ? "" : "\n" + words)
		                              : words);
		ImGui::PopID();
	}
	return current;
}

// Removing a screen asks first: it takes every window on it (Undo brings it back). The prompt
// is about the menu it was asked in, that very document (one read again, or closed, closes
// it), and a screen of it while the menu keeps a second one.
void MenuView::draw_modals(Workspace &workspace) {
	// Each open menu has its view and its prompt, known by the menu's path; of the menus holding one, the session's
	// order shows the first (shown_modal), the others waiting.
	if (path_.empty()) return;
	const SessionView &view = workspace.view();
	const NodeId asked = NodeId(view.workspace.document(path_).remove_screen);
	const MnuDocument *document = nullptr;
	for (const auto &open : view.documents.open)
		if (open->path() == path_) document = dynamic_cast<const MnuDocument *>(open.get());
	const Node *screen = document && asked ? document->row(asked) : nullptr;
	// One that names no screen of the menu (gone, the last one) the session closes (workspace_tidies).
	const bool asks = screen && document->rows().size() >= 2 && modal_may_show(view, HeldModal::RemoveScreen, path_);
	if (!asks && !remove_popup_.shown()) return;
	const std::string id = std::string(kRemovePrompt) + "##" + path_;
	if (!remove_popup_.begin(id.c_str(), asks, true, ImGuiWindowFlags_AlwaysAutoResize, false, view.workspace.opened)) {
		if (remove_popup_.dismissed()) send(workspace, "remove_screen", io::JsonValue::make_number(0.0));
		return;
	}
	ImGui::Text("Remove the screen %s and every window on it?", screen->name().c_str());
	ImGui::TextDisabled("Undo brings it back.");
	if (ImGui::Button("Remove")) {
		edit(workspace, *document, EditOperation::Remove, {screen->id, kScreen, 0});
		send(workspace, "remove_screen", io::JsonValue::make_number(0.0));
		remove_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		send(workspace, "remove_screen", io::JsonValue::make_number(0.0));
		remove_popup_.close();
	}
	ImGui::EndPopup();
}

void MenuView::refresh_tree(const MnuDocument &document, const Node &screen) {
	const bool same = tree_document_ == document.identity() && tree_load_ == document.load_generation() &&
	                  tree_.row == screen.id;
	if (same && tree_revision_ == document.revision()) return;
	if (same) {
		ChangeSet set;
		const RowChanges *changes =
		        document.changes_since(tree_load_, tree_revision_, set) ? std::get_if<RowChanges>(&set) : nullptr;
		if (changes && !changes->was_changed(screen.id) && !changes->was_removed(screen.id)) {
			tree_revision_ = document.revision();
			return;
		}
	}
	++trees_made_;
	// Ids start again in every document and every load of it: a range starts from a row of this
	// screen of this load of this document only (an edit keeps it).
	if (tree_document_ != document.identity() || tree_load_ != document.load_generation() || tree_.row != screen.id)
		anchor_ = 0;
	tree_document_ = document.identity();
	tree_load_ = document.load_generation();
	tree_revision_ = document.revision();
	tree_ = build_record_tree(document, screen, kWindow);
	lines_.clear();
	tips_.clear();
	const FieldSchema *type = type_field(document);
	for (const RecordTree::Entry &entry : tree_.entries) {
		Value name, token;
		document.get(entry.address, "name", name);
		document.get(entry.address, "type", token);
		const auto *text = std::get_if<std::string>(&name);
		const auto *as_written = std::get_if<std::string>(&token);
		const FieldChoice *choice = type ? choice_of(*type, token) : nullptr;
		const std::string kind = choice ? choice_title(*choice) : as_written ? *as_written : std::string();
		std::string line = ui_kit::kChangeRoom;
		line += text && !text->empty() ? *text : std::string("(no name)");
		line += "  (" + kind + ")";
		lines_.push_back(std::move(line));
		tips_.push_back(as_written ? kind + ". " + written_type(*as_written) : kind);
	}
	revealed_ = NodeAddress(); // the selection's owners open again in the new tree
}

// The selected screen's windows: the toolbars, then the tree.
void MenuView::draw_windows(Workspace &workspace, const MnuDocument &document, const Node &screen) {
	const SessionView &view = workspace.view();
	refresh_tree(document, screen);
	const NodeAddress &selection = view.documents.selection.primary;
	const bool here = selection.row == screen.id;
	const RecordTree::Entry *selected = here && selection.kind == kWindow ? tree_.find(selection.child) : nullptr;
	// The window a new one goes into: the listed window that is the selected record or holds
	// it (the one a Paste goes after), else the first root window.
	const NodeId holder = here ? listed_window(document, screen.id, selection).child : 0;
	if (selection != revealed_) {
		revealed_ = selection;
		reveal_.clear();
		for (const RecordTree::Entry *entry = tree_.find(holder); entry && entry->owner; entry = tree_.find(entry->owner))
			reveal_.push_back(entry->owner);
		scroll_to_ = holder;
	}

	ImGui::SeparatorText("Windows");
	{
		// The type a new window takes, as wide as its longest name (no wider than the window),
		// then Add window.
		ui_kit::WrapRow row;
		if (const FieldSchema *type = type_field(document)) {
			float widest = 0.0f;
			for (const FieldChoice &choice : type->choices) widest = std::max(widest, ui_kit::text_width(choice_title(choice).c_str()));
			const float width = std::min(widest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f,
			                             ImGui::GetContentRegionAvail().x);
			row.next(width);
			const FieldChoice *current = choice_of(*type, Value(add_type_));
			ImGui::SetNextItemWidth(width);
			if (ImGui::BeginCombo("##new_type", current ? choice_title(*current).c_str() : add_type_.c_str())) {
				for (const FieldChoice &choice : type->choices) {
					if (ImGui::Selectable(choice_title(choice).c_str(), current == &choice) && add_type_ != choice.name) {
						add_type_ = choice.name;
						send(workspace, "new_window_type", io::JsonValue::make_string(add_type_));
					}
					ui_kit::tooltip(written_type(choice.name));
				}
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The type of the window Add window makes." +
			                (current ? " " + written_type(current->name) : std::string()));
		}
		const NodeId parent = holder ? holder : tree_.roots.empty() ? 0 : tree_.entries[tree_.roots.front()].address.child;
		if (ui_kit::tool(row, "Add window", true, "A window of this type inside the selected window (else inside the first root window)."))
			window_requests::add_with(workspace, document, {screen.id, kWindow, 0}, parent, "type", add_type_);
	}

	// The selected windows of this screen (the tree's), those no other of them holds (a
	// Remove takes a window with what it holds), and the roots among those.
	std::vector<NodeAddress> windows;
	if (here)
		for (const NodeAddress &record : view.documents.selection.records)
			if (record.kind == kWindow && tree_.find(record.child)) windows.push_back(record);
	const std::vector<NodeAddress> outer = document.outermost(windows);
	size_t outer_roots = 0;
	for (const NodeAddress &window : outer) outer_roots += tree_.find(window.child)->owner == 0;
	// The clipboard (the canvas asks the same rule): Copy, Cut and Duplicate take the selection
	// while it is windows the tree lists; a Paste goes after the selected window, or after the
	// window holding the selected record (a list row's index is no place among windows), else
	// at the end of the screen's root windows.
	const std::vector<NodeAddress> none;
	const NodeAddress primary = here ? selection : NodeAddress();
	const std::vector<NodeAddress> &records = here ? view.documents.selection.records : none;
	const MenuClipboard board = menu_clipboard(
			document, screen.id, primary, records, !view.documents.clipboard.empty());
	const bool only_windows = board.copy;

	{
		// Duplicate and Remove take every selected window; the rest the primary one among its
		// siblings.
		ui_kit::WrapRow row;
		const size_t at = selected ? selected->index : 0;
		const size_t siblings = selected ? tree_.children_of(selected->owner).size() : 0;
		const NodeAddress target = selected ? selected->address : NodeAddress();
		const char *pick = "Select a window first.";
		if (ui_kit::tool(row, "Duplicate", only_windows,
		                 only_windows ? "A copy of each selected window right after it, every name in it made unique (Ctrl+D)."
		                              : "Select windows only to duplicate them."))
			clipboard(workspace, document, EditorRequestKind::Duplicate);
		const bool last_root = outer_roots > 0 && outer_roots >= tree_.roots.size();
		if (ui_kit::tool(row, "Remove", selected && !last_root && !outer.empty(),
		                 !selected || outer.empty() ? pick
		                 : last_root                ? "A screen keeps at least one root window."
		                                            : "Removes the selected windows with everything they hold.")) {
			std::vector<Edit> removes;
			for (const NodeAddress &window : outer) {
				Edit remove;
				remove.operation = EditOperation::Remove;
				remove.address = window;
				removes.push_back(remove);
			}
			window_requests::edits(workspace, document, std::move(removes));
		}
		if (ui_kit::tool(row, "Up", selected && at > 0,
		                 !selected ? pick : at == 0 ? "It is the first of its siblings." : "Moves it up among its siblings."))
			edit(workspace, document, EditOperation::Move, target, at - 1);
		if (ui_kit::tool(row, "Down", selected && at + 1 < siblings,
		                 !selected ? pick
		                 : at + 1 >= siblings ? "It is the last of its siblings." : "Moves it down among its siblings."))
			edit(workspace, document, EditOperation::Move, target, at + 1);
		Edit indent, outdent;
		const bool can_indent = selected && indent_edit(tree_, target.child, indent);
		const bool can_outdent = selected && outdent_edit(tree_, target.child, outdent);
		if (ui_kit::tool(row, "Indent", can_indent, !selected ? pick : "Into the window above it, at its end."))
			edit(workspace, document, indent);
		if (ui_kit::tool(row, "Outdent", can_outdent, !selected ? pick : "Out of the window that holds it, right after that window."))
			edit(workspace, document, outdent);
	}

	// The clipboard: the selected windows (Ctrl+C / X), pasted (Ctrl+V) where the rule says.
	const bool can_copy = board.copy;
	const bool can_paste = board.paste;
	const auto paste = [&] {
		window_requests::paste(workspace, document, board.paste_row, board.paste_parent,
		                       board.paste_position);
	};
	{
		ui_kit::WrapRow row;
		const char *windows_only = "Select windows only to copy them.";
		if (ui_kit::tool(row, "Copy", can_copy, can_copy ? "Copies the selected windows (Ctrl+C)." : windows_only))
			clipboard(workspace, document, EditorRequestKind::Copy);
		if (ui_kit::tool(row, "Cut", can_copy, can_copy ? "Copies the selected windows and removes them (Ctrl+X)." : windows_only))
			clipboard(workspace, document, EditorRequestKind::Cut);
		if (ui_kit::tool(row, "Paste", can_paste,
		                 can_paste ? "Pastes the copied windows after the selected window, or after the window holding the "
		                             "selected record (Ctrl+V); they work across screens and menus."
		                           : "Copy windows first (Ctrl+C)."))
			paste();
	}
	if (!document.blocked() && !ImGui::GetIO().WantTextInput) {
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && can_copy) clipboard(workspace, document, EditorRequestKind::Copy);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X) && can_copy) clipboard(workspace, document, EditorRequestKind::Cut);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V) && can_paste) paste();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D) && can_copy)
			clipboard(workspace, document, EditorRequestKind::Duplicate);
	}

	// A deep tree scrolls sideways rather than run past the window.
	ImGui::BeginChild("windows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	if (tree_.roots.empty()) ui_kit::empty_state("The screen has no windows yet.");
	for (const size_t root : tree_.roots) draw_window_node(workspace, document, root);
	ImGui::EndChild();
	reveal_.clear();
}

// A click on a window's row: it alone is selected; Ctrl adds or drops it; Shift selects
// every window from the last one clicked to it in the tree's order (Ctrl+Shift adds them),
// the clicked one the primary.
void MenuView::click_window(Workspace &workspace, const MnuDocument &document, size_t index) {
	const ImGuiIO &io = ImGui::GetIO();
	const RecordTree::Entry &entry = tree_.entries[index];
	const RecordTree::Entry *anchor = io.KeyShift ? tree_.find(anchor_) : nullptr;
	if (!anchor) {
		anchor_ = entry.address.child;
		select(workspace, document, entry.address, io.KeyCtrl ? SelectMode::Toggle : SelectMode::Replace);
		return;
	}
	const size_t from = size_t(anchor - tree_.entries.data());
	const size_t low = std::min(from, index), high = std::max(from, index);
	bool first = !io.KeyCtrl;
	for (size_t i = low; i <= high; ++i) {
		if (i == index) continue;
		select(workspace, document, tree_.entries[i].address, first ? SelectMode::Replace : SelectMode::Add);
		first = false;
	}
	select(workspace, document, entry.address, first ? SelectMode::Replace : SelectMode::Add);
}

// One window, marked when it was added or changed since the last save, and when open the
// windows it holds. A drag drops it on another window: the upper quarter before it, the
// lower quarter after it, the middle inside it.
void MenuView::draw_window_node(Workspace &workspace, const MnuDocument &document, size_t index) {
	const SessionView &view = workspace.view();
	const RecordTree::Entry &entry = tree_.entries[index];
	const NodeId id = entry.address.child;
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
	                           ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	if (entry.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
	if (view.documents.selection.holds(entry.address)) flags |= ImGuiTreeNodeFlags_Selected;
	if (std::find(reveal_.begin(), reveal_.end(), id) != reveal_.end()) ImGui::SetNextItemOpen(true);
	ImGui::PushID(static_cast<int>(id));
	const float x = ImGui::GetCursorScreenPos().x;
	const bool open = ImGui::TreeNodeEx("window", flags, "%s", lines_[index].c_str());
	const Document::RecordChange change = document.record_change(entry.address);
	ui_kit::change_dot(change, x + ImGui::GetTreeNodeToLabelSpacing());
	if (id == scroll_to_) {
		if (!ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
		scroll_to_ = 0;
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) click_window(workspace, document, index);
	// Made only while it shows, after a moment: the tree is swept by the mouse.
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
		const char *words = ui_kit::change_words(change);
		ui_kit::tooltip(tips_[index] + (*words ? "\n" : "") + words);
	}
	if (ImGui::BeginDragDropSource()) {
		ImGui::SetDragDropPayload(kWindowPayload, &id, sizeof id);
		ImGui::TextUnformatted(lines_[index].c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
		            kWindowPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
			NodeId dragged = 0;
			std::memcpy(&dragged, payload->Data, sizeof dragged);
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			const float y = ImGui::GetMousePos().y;
			const float quarter = (max.y - min.y) * 0.25f;
			const DropPlace place = y < min.y + quarter ? DropPlace::Before
			                        : y > max.y - quarter ? DropPlace::After
			                                              : DropPlace::Inside;
			Edit move;
			if (drop_edit(tree_, dragged, id, place, move)) {
				ImDrawList *draw = ImGui::GetWindowDrawList();
				const ImU32 colour = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
				if (place == DropPlace::Inside) {
					draw->AddRect(min, max, colour, 0.0f, 0, 2.0f);
				} else {
					const float line = place == DropPlace::Before ? min.y : max.y;
					draw->AddLine(ImVec2(min.x, line), ImVec2(max.x, line), colour, 2.0f);
				}
				if (payload->IsDelivery()) edit(workspace, document, move);
			}
		}
		ImGui::EndDragDropTarget();
	}
	if (open) {
		for (const size_t child : entry.children) draw_window_node(workspace, document, child);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

} // namespace opennova::editor
