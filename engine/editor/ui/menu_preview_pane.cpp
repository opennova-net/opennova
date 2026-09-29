#include "menu_preview_pane.h"

#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_render_check.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <set>
#include <variant>
#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr float kMargin = 12.0f;       // canvas around the picture: edge handles stay on it
constexpr float kHandleSize = 6.0f;    // a handle's square
constexpr float kHandleSlop = 4.0f;    // how far past it a press still takes it
constexpr float kDragThreshold = 3.0f; // pixels before a press becomes a drag
constexpr float kZoomLevels[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
constexpr int kMaxDevice = 8192;
constexpr ImU32 kSelectedColor = IM_COL32(255, 200, 60, 255);
constexpr ImU32 kHoverColor = IM_COL32(120, 190, 255, 220);

// "logo.tga, gunpl22b.fnt": the names as a banner lists them.
std::string name_list(const std::vector<std::string> &names) {
	std::string out;
	for (const std::string &name : names) out += (out.empty() ? "" : ", ") + name;
	return out;
}

std::string missing_banner(const std::vector<std::string> &missing) {
	return name_list(missing) + (missing.size() == 1 ? " is" : " are") +
	       " not in the project (Refresh in Files after adding files outside the editor).";
}

std::string unreadable_banner(const std::vector<std::string> &unreadable) {
	const bool one = unreadable.size() == 1;
	return name_list(unreadable) + " did not load: the project has " + (one ? "the file" : "the files") +
	       ", but the game could not read " + (one ? "it." : "them.");
}

constexpr NodeKind kWindowKind = node_kind(MenuKind::Window);
constexpr ImU32 kMarqueeColor = IM_COL32(140, 210, 255, 230);

bool has(const std::vector<NodeAddress> &windows, const NodeAddress &window) {
	return std::find(windows.begin(), windows.end(), window) != windows.end();
}

// A window's TYPE as the factory matches it (the generic window for none).
mnu::WindowType window_type(const MnuDocument &document, const NodeAddress &window) {
	Value type;
	if (!window.child || !document.get(window, "type", type) || !std::holds_alternative<std::string>(type))
		return mnu::WindowType::Window;
	return mnu::parse_window_type(std::get<std::string>(type));
}

// What the toolbar's Checked, Open list and Focus hold, by the window's type.
bool checkable(mnu::WindowType type) {
	return type == mnu::WindowType::CheckBox || type == mnu::WindowType::Radio || type == mnu::WindowType::RadioEdit;
}
bool has_list(mnu::WindowType type) { return type == mnu::WindowType::Combo; }
bool editable(mnu::WindowType type) { return type == mnu::WindowType::Edit || type == mnu::WindowType::MultilineEdit; }

// A design-space rect on the device, on the pixels the game draws it on.
void device_rect(const mnu::RectEdges &rect, float ox, float oy, float sx, float sy, ImVec2 &a, ImVec2 &b) {
	a = ImVec2(ox + menu::menu_scaled_edge(rect.left, sx), oy + menu::menu_scaled_edge(rect.top, sy));
	b = ImVec2(ox + menu::menu_scaled_edge(rect.right, sx), oy + menu::menu_scaled_edge(rect.bottom, sy));
}

ImVec2 handle_point(LayoutHandle handle, const ImVec2 &a, const ImVec2 &b) {
	const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
	switch (handle) {
	case LayoutHandle::Left: return ImVec2(a.x, my);
	case LayoutHandle::Right: return ImVec2(b.x, my);
	case LayoutHandle::Top: return ImVec2(mx, a.y);
	case LayoutHandle::Bottom: return ImVec2(mx, b.y);
	case LayoutHandle::TopLeft: return a;
	case LayoutHandle::TopRight: return ImVec2(b.x, a.y);
	case LayoutHandle::BottomLeft: return ImVec2(a.x, b.y);
	case LayoutHandle::BottomRight: return b;
	case LayoutHandle::Move: break;
	}
	return ImVec2(mx, my);
}

// Corners first: where handles overlap (a small window), a corner takes the press.
constexpr LayoutHandle kHandles[] = {LayoutHandle::TopLeft,     LayoutHandle::TopRight, LayoutHandle::BottomLeft,
                                     LayoutHandle::BottomRight, LayoutHandle::Left,     LayoutHandle::Right,
                                     LayoutHandle::Top,         LayoutHandle::Bottom};

// Which way a handle's side of the rect faces the inside on each axis: +1 the left or top
// edge, -1 the right or bottom edge, 0 a handle in the middle of that axis.
int inward_x(LayoutHandle handle) {
	switch (handle) {
	case LayoutHandle::Left:
	case LayoutHandle::TopLeft:
	case LayoutHandle::BottomLeft: return 1;
	case LayoutHandle::Right:
	case LayoutHandle::TopRight:
	case LayoutHandle::BottomRight: return -1;
	default: return 0;
	}
}
int inward_y(LayoutHandle handle) {
	switch (handle) {
	case LayoutHandle::Top:
	case LayoutHandle::TopLeft:
	case LayoutHandle::TopRight: return 1;
	case LayoutHandle::Bottom:
	case LayoutHandle::BottomLeft:
	case LayoutHandle::BottomRight: return -1;
	default: return 0;
	}
}

// Whether `v` is on a handle at `p` along one axis: `reach` either way along its edge
// (`inward` 0), else `reach` outside the rect and at most `in` inside it.
bool within_handle(float v, float p, int inward, float reach, float in) {
	if (inward == 0) return std::fabs(v - p) <= reach;
	const float t = (v - p) * float(inward);
	return t >= -reach && t <= in;
}

// The handle a press takes. Outside the rect a handle reaches its square and slop; inside
// it reaches no further than a quarter of the rect across on the axis it resizes, so the
// middle of a window however small on the screen is a move, never a resize.
bool handle_at(float x, float y, const ImVec2 &a, const ImVec2 &b, LayoutHandle &out) {
	const float reach = kHandleSize * 0.5f + kHandleSlop;
	const float in_x = std::min(reach, (b.x - a.x) * 0.25f);
	const float in_y = std::min(reach, (b.y - a.y) * 0.25f);
	for (const LayoutHandle handle : kHandles) {
		const ImVec2 p = handle_point(handle, a, b);
		if (within_handle(x, p.x, inward_x(handle), reach, in_x) &&
		    within_handle(y, p.y, inward_y(handle), reach, in_y)) {
			out = handle;
			return true;
		}
	}
	return false;
}

bool inside(float x, float y, const ImVec2 &a, const ImVec2 &b) { return x >= a.x && x < b.x && y >= a.y && y < b.y; }

ImGuiMouseCursor handle_cursor(LayoutHandle handle) {
	switch (handle) {
	case LayoutHandle::Left:
	case LayoutHandle::Right: return ImGuiMouseCursor_ResizeEW;
	case LayoutHandle::Top:
	case LayoutHandle::Bottom: return ImGuiMouseCursor_ResizeNS;
	case LayoutHandle::TopLeft:
	case LayoutHandle::BottomRight: return ImGuiMouseCursor_ResizeNWSE;
	case LayoutHandle::TopRight:
	case LayoutHandle::BottomLeft: return ImGuiMouseCursor_ResizeNESW;
	case LayoutHandle::Move: break;
	}
	return ImGuiMouseCursor_ResizeAll;
}

std::string rect_text(const mnu::RectEdges &r) {
	char text[112];
	std::snprintf(text, sizeof(text), "left %d, top %d, right %d, bottom %d (%d x %d)", r.left, r.top, r.right,
	              r.bottom, r.right - r.left, r.bottom - r.top);
	return text;
}

// The next zoom level past `scale` in the wheel's direction (the last one at either end).
float next_zoom(float scale, bool in) {
	if (in) {
		for (const float level : kZoomLevels)
			if (level > scale + 0.001f) return level;
		return kZoomLevels[std::size(kZoomLevels) - 1];
	}
	for (size_t i = std::size(kZoomLevels); i-- > 0;)
		if (kZoomLevels[i] < scale - 0.001f) return kZoomLevels[i];
	return kZoomLevels[0];
}

} // namespace

void MenuPreviewPane::end_press_() {
	if (press_.dragging && press_.sent) window_requests::end_edit(host_, press_.path);
	press_ = Press();
}

void MenuPreviewPane::end_nudge_() {
	if (nudge_.gesture && nudge_.sent) window_requests::end_edit(host_, nudge_.path);
	nudge_ = Nudge();
}

void MenuPreviewPane::end_gestures() {
	end_press_();
	end_nudge_();
	panning_ = false;
}

// The held state follows the selection: a newly selected window is the one held, and what
// its type does not have (a check, a list, a caret) is let go.
void MenuPreviewPane::follow_selection_(const MnuDocument &document, const NodeAddress &selected) {
	if (selected.child == followed_) return;
	followed_ = selected.child;
	MenuPreviewOptions options = viewport_->options();
	if (!options.forcing() || !selected.child) return;
	const mnu::WindowType type = window_type(document, selected);
	options.force_window = selected.child;
	options.checked = options.checked && checkable(type);
	options.popup_open = options.popup_open && has_list(type);
	options.focused = options.focused && editable(type);
	viewport_->set_options(options);
}

// The toolbar, on a row that wraps whole controls in a narrow window.
void MenuPreviewPane::toolbar_(const Frame &frame) {
	const MnuDocument &document = *frame.document;
	const NodeAddress &selected = frame.selected;
	MenuPreviewOptions options = viewport_->options();
	const MenuPreviewOptions before = options;
	const float unit = ImGui::GetFontSize();
	ui_kit::WrapRow row;

	char zoom_label[32];
	if (zoom_ == Zoom::Fit) std::snprintf(zoom_label, sizeof(zoom_label), "Fit");
	else if (zoom_ == Zoom::Device) std::snprintf(zoom_label, sizeof(zoom_label), "Device size");
	else std::snprintf(zoom_label, sizeof(zoom_label), "%d%%", int(std::lround(scale_ * 100.0f)));
	row.next(ui_kit::field_width(unit * 7.0f, "Zoom"));
	ImGui::SetNextItemWidth(unit * 7.0f);
	if (ImGui::BeginCombo("Zoom", zoom_label)) {
		if (ImGui::Selectable("Fit", zoom_ == Zoom::Fit)) zoom_ = Zoom::Fit;
		for (const float level : kZoomLevels) {
			char text[16];
			std::snprintf(text, sizeof(text), "%d%%", int(std::lround(level * 100.0f)));
			if (ImGui::Selectable(text, zoom_ == Zoom::Scale && std::fabs(scale_ - level) < 0.001f)) {
				zoom_ = Zoom::Scale;
				scale_ = level;
			}
		}
		if (ImGui::Selectable("Device size", zoom_ == Zoom::Device)) zoom_ = Zoom::Device;
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Ctrl+wheel over the picture zooms about the mouse.");
	if (zoom_ == Zoom::Device) {
		int size[2] = {options.width, options.height};
		row.next(unit * 8.0f);
		ImGui::SetNextItemWidth(unit * 8.0f);
		if (ImGui::InputInt2("##device", size)) {
			options.width = std::clamp(size[0], 1, kMaxDevice);
			options.height = std::clamp(size[1], 1, kMaxDevice);
		}
		ui_kit::tooltip("The screen size in pixels to draw at; each axis scales on its own, as in "
		                "the game.");
	}
	row.next(ui_kit::checkbox_width("Snap"));
	ImGui::Checkbox("Snap", &snap_);
	ui_kit::tooltip("Drags snap to a grid of 8 units. Hold Alt to place freely.");
	row.next(ui_kit::checkbox_width("Show hidden"));
	ImGui::Checkbox("Show hidden", &options.show_hidden);
	ui_kit::tooltip("Draw every window, including those the screen starts hidden.");

	// The selected window held in a state, as the game's mouse and keyboard would leave it.
	ImGui::BeginDisabled(!selected.child);
	static const char *const kStateNames[] = {"Normal", "Mouseover", "Selected", "Disabled"};
	static const int kStates[] = {-1, menu::kStateMouseover, menu::kStateSelected, menu::kStateDisabled};
	int shown = 0;
	for (int i = 0; i < 4; ++i)
		if (kStates[i] == options.force_state) shown = i;
	row.next(ui_kit::field_width(unit * 7.0f, "State"));
	ImGui::SetNextItemWidth(unit * 7.0f);
	if (ImGui::BeginCombo("State", kStateNames[shown])) {
		for (int i = 0; i < 4; ++i)
			if (ImGui::Selectable(kStateNames[i], i == shown)) {
				options.force_state = kStates[i];
				options.force_window = selected.child;
			}
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Show the selected window under the mouse, pressed or disabled.");
	const mnu::WindowType type = window_type(document, selected);
	auto hold = [&](const char *label, bool &flag, const char *tip) {
		row.next(ui_kit::checkbox_width(label));
		if (ImGui::Checkbox(label, &flag)) options.force_window = selected.child;
		ui_kit::tooltip(tip);
	};
	if (checkable(type)) hold("Checked", options.checked, "Show the selected window checked.");
	if (has_list(type)) hold("Open list", options.popup_open, "Show the selected combo box with its list open.");
	if (editable(type)) hold("Focus", options.focused, "Show the selected edit box focused, with its caret.");
	ImGui::EndDisabled();

	// Arrange: the selected windows aligned to the primary, spread, or reordered.
	row.next(ui_kit::button_width("Arrange"));
	ImGui::BeginDisabled(frame.windows.empty() || document.blocked());
	if (ImGui::Button("Arrange")) ImGui::OpenPopup("arrange");
	ImGui::EndDisabled();
	ui_kit::tooltip("Align the selected windows to the primary one (the last selected), spread "
	                "them evenly, or change their drawing order. Shift+click or drag a box to "
	                "select several.");
	if (ImGui::BeginPopup("arrange")) {
		arrange_items_(frame);
		ImGui::EndPopup();
	}

	// Where the mouse is on the picture, in design units: as wide as the widest it reads.
	row.next(ui_kit::text_width("x 0000, y 0000"));
	ImGui::AlignTextToFramePadding();
	if (mouse_on_picture_) ImGui::Text("x %d, y %d", mouse_design_x_, mouse_design_y_);
	else ImGui::TextDisabled("x -, y -");
	ui_kit::tooltip("Where the mouse is on the picture, in the menu's 800 x 600 units.");
	if (options != before) viewport_->set_options(options);
}

void MenuPreviewPane::draw() {
	const SessionView &view = host_.view();
	if (!viewport_) {
		end_gestures();
		ui_kit::empty_state(menu_preview_status_message(MenuPreviewStatus::NoDevice, std::string()).c_str());
		return;
	}
	std::string detail;
	const MenuPreviewStatus status = viewport_->status(&detail);
	// The screen the view keeps for the preview: the last menu screen selected, kept while
	// another document (the stylesheet it draws with) is active.
	Frame frame;
	for (const auto &open : view.documents)
		if (open->path() == view.menu_preview.path) frame.document = dynamic_cast<const MnuDocument *>(open.get());
	frame.screen = frame.document ? frame.document->row(view.menu_preview.screen) : nullptr;
	if (status != MenuPreviewStatus::Ready || !frame.document || !frame.screen) {
		end_gestures();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", menu_preview_status_message(status, detail).c_str());
		ImGui::PopTextWrapPos();
		return;
	}
	const MnuDocument &document = *frame.document;
	const Node &screen = *frame.screen;
	if (!viewport_->missing().empty() || !viewport_->unreadable().empty()) {
		ImGui::PushTextWrapPos(0.0f);
		if (!viewport_->missing().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", missing_banner(viewport_->missing()).c_str());
		if (!viewport_->unreadable().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
			                   unreadable_banner(viewport_->unreadable()).c_str());
		ImGui::PopTextWrapPos();
	}
	frame.compiler = viewport_->compiler();
	frame.state = viewport_->frame_state();
	// A picture of another revision (the edit lands on the next pump) maps no index.
	frame.current = frame.compiler && frame.state && viewport_->shown_revision() == document.revision();
	// The primary window of this screen (or the window that holds the primary record), and
	// every selected one.
	const bool active = view.active_document == document.path();
	if (active) {
		frame.selected = window_holding(document, view.selection, screen.id);
		frame.windows = selected_windows(document, view.selection, view.selected, screen.id);
	}
	// The menu view's clipboard rule: Copy, Cut and Duplicate only while the selection is
	// windows the tree lists (never a list row, the screen or a window a part holds), a Paste
	// after the primary's window.
	const std::vector<NodeAddress> none;
	frame.clipboard = menu_clipboard(document, screen.id, active ? view.selection : NodeAddress(),
	                                 active ? view.selected : none, !view.clipboard.empty());
	follow_selection_(document, frame.selected);
	toolbar_(frame);
	// The compiler's notes on the screen as it stands (ADR 0046 S9j2): what configure
	// noted, then what laying the frame state out comes to.
	if (frame.current) {
		frame.notes = frame.compiler->build_notes();
		for (menu::MenuFrameNote &note : frame.compiler->layout_notes(*frame.state)) frame.notes.push_back(std::move(note));
	}
	keys_(frame);
	// Room below the picture for the notes list.
	const float notes_height = frame.notes.empty()
	                                   ? 0.0f
	                                   : ImGui::GetFrameHeightWithSpacing() * float(1 + std::min<size_t>(frame.notes.size(), 6));
	canvas_(frame, std::max(64.0f, ImGui::GetContentRegionAvail().y - notes_height));

	// The notes as a list, each cut to the window (whole in its tooltip); a click selects the
	// record a note is on.
	if (frame.notes.empty()) return;
	ImGui::TextDisabled("Notes (%d)", int(frame.notes.size()));
	if (ImGui::BeginChild("notes", ImVec2(0.0f, 0.0f), false)) {
		for (size_t i = 0; i < frame.notes.size(); ++i) {
			const menu::MenuFrameNote &note = frame.notes[i];
			const std::string name = note.widget >= 0 ? frame.compiler->widget_name(note.widget) : screen.name();
			const std::string line = name + ": " + menu_note_message(note);
			const std::string shown = ui_kit::fit(line, ImGui::GetContentRegionAvail().x);
			ImGui::PushID(int(i));
			if (ImGui::Selectable((shown + "###note").c_str())) {
				const NodeAddress address = menu_note_address(note, document, screen, nullptr);
				window_requests::select(host_, document, address);
			}
			if (shown != line) ui_kit::tooltip(line);
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
}

// The keys while the preview has the focus and no text box takes them: the arrows move
// the selected windows 1 unit (8 with Shift), one undo step while any is held; Esc selects
// what holds the primary; Ctrl+C / X / V / D copy, cut, paste and duplicate windows.
void MenuPreviewPane::keys_(const Frame &frame) {
	const ImGuiIO &io = ImGui::GetIO();
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
	const bool held = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
	                  ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow);
	if (nudge_.gesture && (!held || !focused || nudge_.windows != frame.windows)) end_nudge_();
	if (!focused || press_.active) return;
	const MnuDocument &document = *frame.document;
	const SessionView &view = host_.view();
	if (!document.blocked()) {
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) return clipboard_(frame, EditorRequestKind::Copy);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) return clipboard_(frame, EditorRequestKind::Cut);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) return clipboard_(frame, EditorRequestKind::Paste);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) return clipboard_(frame, EditorRequestKind::Duplicate);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && view.active_document == document.path() &&
	    view.selection.row == frame.screen->id && view.selection.child) {
		const std::vector<NodeAddress> owners = document.ancestors(view.selection);
		if (!owners.empty()) window_requests::select(host_, document, owners.back());
		return;
	}
	const int step = io.KeyShift ? kLayoutGrid : 1;
	int dx = 0, dy = 0;
	if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx -= step;
	if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx += step;
	if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy -= step;
	if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy += step;
	if ((!dx && !dy) || !frame.selected.child || !frame.compiler) return;
	if (!nudge_.gesture) {
		// The nudge starts where the picture shows the windows: it must be the document's own.
		if (!frame.current || !layout_press(document, frame.selected, LayoutHandle::Move, frame.windows, *frame.compiler,
		                                     *frame.state, nudge_.press))
			return;
		nudge_.gesture = next_edit_gesture();
		nudge_.path = document.path();
		nudge_.windows = frame.windows;
	}
	nudge_.dx += dx;
	nudge_.dy += dy;
	std::vector<Edit> edits;
	if (layout_press_edits(document, nudge_.press, *frame.compiler, nudge_.dx, nudge_.dy, 0, nudge_.gesture, edits) &&
	    !edits.empty()) {
		window_requests::edits(host_, document, std::move(edits));
		nudge_.sent = true;
	}
}

// Copy, Cut, Duplicate and Paste as the menu clipboard's rule says (menu_clipboard).
void MenuPreviewPane::clipboard_(const Frame &frame, EditorRequestKind kind) {
	const MnuDocument &document = *frame.document;
	const MenuClipboard &board = frame.clipboard;
	if (kind != EditorRequestKind::Paste) {
		if (board.copy) window_requests::clipboard(host_, document, kind);
		return;
	}
	if (board.paste)
		window_requests::paste(host_, document, board.paste_row, board.paste_parent,
		                       board.paste_position);
}

void MenuPreviewPane::arrange_items_(const Frame &frame) {
	const size_t count = frame.windows.size();
	for (const ArrangeOp op : kArrangeOps) {
		if (op == ArrangeOp::DistributeHorizontally || op == ArrangeOp::BringToFront) ImGui::Separator();
		const bool enabled = frame.current && count >= arrange_minimum(op) && !frame.document->blocked();
		if (ImGui::MenuItem(arrange_op_label(op), nullptr, false, enabled)) arrange_(frame, op);
		if (!enabled)
			ui_kit::tooltip("Select " + std::to_string(arrange_minimum(op)) + " windows or more.");
	}
}

// One arrange of the selected windows, one undo step.
void MenuPreviewPane::arrange_(const Frame &frame, ArrangeOp op) {
	if (!frame.current) return;
	std::vector<Edit> edits;
	if (arrange_edits(*frame.document, frame.windows, frame.selected, op, *frame.compiler, *frame.state, edits,
	                  nullptr) &&
	    !edits.empty())
		window_requests::edits(host_, *frame.document, std::move(edits));
}

// The selected window whose outline holds the point (screen pixels): the primary when it
// does, else the front-most such window (the last in the compiled screen's pre-order, the
// order the runtime draws in); none when no selected window's rect holds it.
NodeAddress MenuPreviewPane::selected_window_at_(const Frame &frame, float x, float y, float origin_x, float origin_y,
                                                   float sx, float sy) const {
	NodeAddress found;
	int front = -1;
	for (const NodeAddress &window : frame.windows) {
		const int index = frame.document->window_index(window);
		mnu::RectEdges rect{};
		if (index < 0 || !frame.compiler->widget_rect(index, *frame.state, &rect)) continue;
		ImVec2 a, b;
		device_rect(rect, origin_x, origin_y, sx, sy, a, b);
		if (!inside(x, y, a, b)) continue;
		if (window == frame.selected) return window;
		if (index > front) {
			front = index;
			found = window;
		}
	}
	return found;
}

// A left press on the canvas. With Shift or Ctrl the selection changes on release and
// nothing moves. Otherwise: on a handle of the primary window a resize; inside the primary
// or another selected window (its rect, whatever window the hit test finds there) a move
// of every selected one (the handles keep out of a small window's middle); on the
// screen's background (nothing, or a root window not selected) a marquee; elsewhere a pick
// (a drag that follows moves the window picked).
void MenuPreviewPane::press_on_canvas_(const Frame &frame, float mouse_x, float mouse_y, float origin_x, float origin_y,
                                         float sx, float sy) {
	end_press_();
	const ImGuiIO &io = ImGui::GetIO();
	press_.active = true;
	press_.x = mouse_x;
	press_.y = mouse_y;
	press_.sx = sx;
	press_.sy = sy;
	press_.from_x = press_.to_x = (mouse_x - origin_x) / sx;
	press_.from_y = press_.to_y = (mouse_y - origin_y) / sy;
	press_.join = io.KeyCtrl ? SelectMode::Toggle : io.KeyShift ? SelectMode::Add : SelectMode::Replace;
	press_.path = frame.document->path();
	if (!frame.current) return; // an old picture maps no index
	const MnuDocument &document = *frame.document;
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	press_.pick = compiler.hit_widget(*frame.state, mouse_x - origin_x, mouse_y - origin_y, sx, sy);
	const NodeId picked = press_.pick >= 0 ? document.window_at(*frame.screen, size_t(press_.pick)) : 0;
	const NodeAddress picked_window = picked ? NodeAddress{frame.screen->id, kWindowKind, picked} : NodeAddress();
	// The screen's background: no window, or a root window that is not selected.
	const bool background = !picked || (compiler.widget_parent(press_.pick) < 0 && !has(frame.windows, picked_window));
	if (press_.join != SelectMode::Replace) {
		press_.marquee = background;
		return;
	}
	const int primary = frame.selected.child ? document.window_index(frame.selected) : -1;
	mnu::RectEdges rect{};
	if (primary >= 0 && compiler.widget_rect(primary, *frame.state, &rect)) {
		ImVec2 a, b;
		device_rect(rect, origin_x, origin_y, sx, sy, a, b);
		if (handle_at(mouse_x, mouse_y, a, b, press_.handle)) {
			press_.resize = true;
			press_.window = frame.selected;
			if (!layout_press(document, press_.window, press_.handle, frame.windows, compiler, *frame.state, press_.layout))
				press_.window = NodeAddress();
			return;
		}
	}
	// A selected window moves every selected one; the window picked, not selected, moves alone.
	press_.window = selected_window_at_(frame, mouse_x, mouse_y, origin_x, origin_y, sx, sy);
	if (!press_.window.child && background) {
		press_.marquee = true;
		return;
	}
	if (!press_.window.child) press_.window = picked_window;
	if (press_.window.child &&
	    !layout_press(document, press_.window, LayoutHandle::Move, frame.windows, compiler, *frame.state, press_.layout))
		press_.window = NodeAddress();
}

// One sample of a drag: the marquee's box, or the step from where it began, planned and
// sent when it moved (the edits of one drag share its gesture, so the drag is one undo
// step; a move writes every window it takes in one batch).
void MenuPreviewPane::drag_step_(const Frame &frame, float mouse_x, float mouse_y, float origin_x, float origin_y,
                                   bool free) {
	const MnuDocument &document = *frame.document;
	if (!press_.dragging) {
		if ((!press_.window.child && !press_.marquee) || std::hypot(mouse_x - press_.x, mouse_y - press_.y) < kDragThreshold)
			return;
		press_.dragging = true;
		if (!press_.marquee) {
			press_.gesture = next_edit_gesture();
			// The window a drag holds is the primary one (its handles follow it); the others the
			// move takes stay selected.
			if (press_.window != frame.selected)
				window_requests::select(host_, document, press_.window,
				                        press_.layout.windows.size() > 1 ? SelectMode::Add : SelectMode::Replace);
		}
	}
	if (press_.marquee) {
		press_.to_x = (mouse_x - origin_x) / press_.sx;
		press_.to_y = (mouse_y - origin_y) / press_.sy;
		return;
	}
	const int dx = int(std::lround((mouse_x - press_.x) / press_.sx));
	const int dy = int(std::lround((mouse_y - press_.y) / press_.sy));
	const int grid = snap_ && !free ? kLayoutGrid : 0;
	if (dx == press_.dx && dy == press_.dy && grid == press_.grid) return;
	if (!frame.compiler) return; // no solve until the picture is back
	press_.dx = dx;
	press_.dy = dy;
	press_.grid = grid;
	std::vector<Edit> edits;
	if (layout_press_edits(document, press_.layout, *frame.compiler, dx, dy, grid, press_.gesture, edits) &&
	    !edits.empty()) {
		window_requests::edits(host_, document, std::move(edits));
		press_.sent = true;
	}
}

// The button let go: a drag ends its gesture, a marquee selects what its box touches; a
// click (no drag, not on a handle) selects the window the game's hit test found, with
// Shift adding it and Ctrl adding or dropping it.
void MenuPreviewPane::release_(const Frame &frame) {
	if (press_.marquee && press_.dragging) {
		if (frame.current) marquee_select_(frame);
	} else if (!press_.dragging && !press_.resize && press_.pick >= 0 && frame.current) {
		const NodeId id = frame.document->window_at(*frame.screen, size_t(press_.pick));
		if (id) window_requests::select(host_, *frame.document, {frame.screen->id, kWindowKind, id}, press_.join);
	}
	end_press_();
}

// Every shown window of the screen but its root windows (the background a marquee starts
// on) whose rect the box touches, in the screen's order: the selection (the last one the
// primary), or added to it with Shift or Ctrl. A box that touches none selects the screen.
void MenuPreviewPane::marquee_select_(const Frame &frame) {
	const MnuDocument &document = *frame.document;
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	const float left = std::min(press_.from_x, press_.to_x), right = std::max(press_.from_x, press_.to_x);
	const float top = std::min(press_.from_y, press_.to_y), bottom = std::max(press_.from_y, press_.to_y);
	std::vector<NodeAddress> touched;
	for (int index = 0; index < compiler.widget_count(); ++index) {
		mnu::RectEdges rect{};
		if (compiler.widget_parent(index) < 0 || !compiler.widget_shown(index, *frame.state) ||
		    !compiler.widget_rect(index, *frame.state, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
			continue;
		const NodeId id = document.window_at(*frame.screen, size_t(index));
		if (id && float(rect.left) < right && float(rect.right) > left && float(rect.top) < bottom &&
		    float(rect.bottom) > top)
			touched.push_back({frame.screen->id, kWindowKind, id});
	}
	if (press_.join == SelectMode::Replace && touched.empty()) {
		window_requests::select(host_, document, {frame.screen->id, frame.screen->kind, 0});
		return;
	}
	for (size_t i = 0; i < touched.size(); ++i)
		window_requests::select(host_, document, touched[i],
		                        i == 0 && press_.join == SelectMode::Replace ? SelectMode::Replace : SelectMode::Add);
}

void MenuPreviewPane::canvas_(const Frame &frame, float height) {
	const ImGuiIO &io = ImGui::GetIO();
	const MenuPreviewOptions &options = viewport_->options();
	const MnuDocument &document = *frame.document;
	const ImVec2 region(ImGui::GetContentRegionAvail().x, height);
	// The picture's device size, and where it sits on the canvas.
	int width = menu::kMenuDesignWidth, tall = menu::kMenuDesignHeight;
	ImVec2 pad(kMargin, kMargin);
	switch (zoom_) {
	case Zoom::Fit: {
		// The 800x600 design aspect, as the game letterboxes it: whole steps of 4 by 3, so
		// the picture keeps the aspect exactly whatever room the window has.
		const int step = std::max(16, std::min(int(region.x - 2.0f * kMargin) / 4, int(region.y - 2.0f * kMargin) / 3));
		width = step * 4;
		tall = step * 3;
		pad.x = std::max(kMargin, std::floor((region.x - float(width)) * 0.5f));
		break;
	}
	case Zoom::Scale:
		width = int(std::lround(float(menu::kMenuDesignWidth) * scale_));
		tall = int(std::lround(float(menu::kMenuDesignHeight) * scale_));
		break;
	case Zoom::Device:
		width = options.width;
		tall = options.height;
		break;
	}
	const float sx = float(width) / float(menu::kMenuDesignWidth);
	const float sy = float(tall) / float(menu::kMenuDesignHeight);
	const ImVec2 content(float(width) + 2.0f * pad.x, float(tall) + 2.0f * pad.y);
	if (scroll_pending_) {
		ImGui::SetNextWindowScroll(ImVec2(scroll_x_, scroll_y_));
		scroll_pending_ = false;
	}
	ImGui::SetNextWindowContentSize(content);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool visible = ImGui::BeginChild("##canvas", region, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::PopStyleVar();
	if (!visible) {
		ImGui::EndChild();
		return;
	}
	// Every press on the canvas, the picture and its margin, is the window's: the surface is
	// the first item, so the device's own item under it never takes the mouse.
	const ImVec2 base = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##surface", content, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const bool activated = ImGui::IsItemActivated();
	const float ox = base.x + pad.x, oy = base.y + pad.y;
	ImGui::SetCursorScreenPos(ImVec2(ox, oy));
	viewport_->draw(width, tall);
	ImDrawList *paint = ImGui::GetWindowDrawList();
	paint->AddRect(ImVec2(ox - 1.0f, oy - 1.0f), ImVec2(ox + float(width) + 1.0f, oy + float(tall) + 1.0f),
	              IM_COL32(90, 90, 90, 255));

	const ImVec2 mouse = io.MousePos;
	mouse_on_picture_ = hovered && mouse.x >= ox && mouse.y >= oy && mouse.x < ox + float(width) &&
	                    mouse.y < oy + float(tall);
	mouse_design_x_ = int(std::floor((mouse.x - ox) / sx));
	mouse_design_y_ = int(std::floor((mouse.y - oy) / sy));

	// Ctrl+wheel zooms about the mouse: the design point under it stays under it.
	if (hovered && io.KeyCtrl && io.MouseWheel != 0.0f) {
		const float px = (mouse.x - ox) / sx, py = (mouse.y - oy) / sy;
		scale_ = next_zoom(zoom_ == Zoom::Scale ? scale_ : (sx + sy) * 0.5f, io.MouseWheel > 0.0f);
		zoom_ = Zoom::Scale;
		const ImVec2 at = ImGui::GetWindowPos();
		scroll_x_ = std::max(0.0f, at.x + kMargin + px * scale_ - mouse.x);
		scroll_y_ = std::max(0.0f, at.y + kMargin + py * scale_ - mouse.y);
		scroll_pending_ = true;
	}
	// The middle button, or Space with the left, pans.
	const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
	if (activated && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || space)) panning_ = true;
	if (panning_) {
		if (!active) {
			panning_ = false;
		} else {
			ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
			ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		}
	} else if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		press_on_canvas_(frame, mouse.x, mouse.y, ox, oy, sx, sy);
	}
	if (press_.active) {
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) release_(frame);
		else drag_step_(frame, mouse.x, mouse.y, ox, oy, io.KeyAlt);
	}
	// The right button: the window under it selected unless it already is, and the menu of
	// what the selection can do.
	if (hovered && !press_.active && !panning_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
		if (frame.current) {
			const int under = frame.compiler->hit_widget(*frame.state, mouse.x - ox, mouse.y - oy, sx, sy);
			const NodeId id = under >= 0 ? document.window_at(*frame.screen, size_t(under)) : 0;
			const NodeAddress window{frame.screen->id, kWindowKind, id};
			if (id && !has(host_.view().selected, window)) window_requests::select(host_, document, window);
		}
		ImGui::OpenPopup("canvas_menu");
	}
	if (ImGui::BeginPopup("canvas_menu")) {
		const bool editable_now = !document.blocked();
		const bool copyable = frame.clipboard.copy && editable_now;
		if (ImGui::MenuItem("Cut", "Ctrl+X", false, copyable)) clipboard_(frame, EditorRequestKind::Cut);
		if (ImGui::MenuItem("Copy", "Ctrl+C", false, copyable)) clipboard_(frame, EditorRequestKind::Copy);
		if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable_now && frame.clipboard.paste))
			clipboard_(frame, EditorRequestKind::Paste);
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, copyable)) clipboard_(frame, EditorRequestKind::Duplicate);
		ImGui::Separator();
		if (ImGui::BeginMenu("Arrange", !frame.windows.empty() && editable_now)) {
			arrange_items_(frame);
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}

	// What the picture maps to records, only while it is the document's own.
	if (!frame.current) {
		ImGui::EndChild();
		return;
	}
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	const menu::MenuFrameState &state = *frame.state;
	// A marker on every window that has a note: a small triangle in its top-left corner.
	std::set<int> marked;
	for (const menu::MenuFrameNote &note : frame.notes) {
		mnu::RectEdges rect{};
		if (note.widget < 0 || !marked.insert(note.widget).second || !compiler.widget_rect(note.widget, state, &rect))
			continue;
		const ImVec2 corner(ox + menu::menu_scaled_edge(rect.left, sx), oy + menu::menu_scaled_edge(rect.top, sy));
		paint->AddTriangleFilled(corner, ImVec2(corner.x + 7.0f, corner.y), ImVec2(corner.x, corner.y + 7.0f),
		                        IM_COL32(255, 170, 40, 230));
	}
	// Hover: the front-most shown widget, the last drawn (the game's own hit test), outlined
	// thin, with its name, type, rects and notes.
	const bool idle = hovered && !press_.active && !panning_;
	const int under = idle ? compiler.hit_widget(state, mouse.x - ox, mouse.y - oy, sx, sy) : -1;
	mnu::RectEdges rect{};
	if (under >= 0 && compiler.widget_rect(under, state, &rect)) {
		ImVec2 a, b;
		device_rect(rect, ox, oy, sx, sy, a, b);
		paint->AddRect(a, b, kHoverColor, 0.0f, 0, 1.0f);
		ImGui::BeginTooltip();
		const int kind = compiler.widget_kind(under);
		ImGui::TextUnformatted(compiler.widget_name(under).c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", kind >= 0 ? mnu::window_type_name(static_cast<mnu::WindowType>(kind)) : "");
		ImGui::Text("On the screen: %s", rect_text(rect).c_str());
		mnu::RectEdges parent{};
		const int owner = compiler.widget_parent(under);
		if (compiler.widget_rect(owner, state, &parent)) {
			const mnu::RectEdges local{rect.left - parent.left, rect.top - parent.top, rect.right - parent.left,
			                           rect.bottom - parent.top};
			ImGui::Text("In %s: %s", compiler.widget_name(owner).c_str(), rect_text(local).c_str());
		}
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
		for (const menu::MenuFrameNote &note : frame.notes)
			if (note.widget == under) ImGui::BulletText("%s", menu_note_message(note).c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
	// The other selected windows: outlined thin; a press inside one moves them all.
	for (const NodeAddress &window : frame.windows) {
		const int index = window == frame.selected ? -1 : document.window_index(window);
		if (index < 0 || !compiler.widget_rect(index, state, &rect)) continue;
		ImVec2 a, b;
		device_rect(rect, ox, oy, sx, sy, a, b);
		paint->AddRect(a, b, kSelectedColor, 0.0f, 0, 1.0f);
	}
	if (idle && selected_window_at_(frame, mouse.x, mouse.y, ox, oy, sx, sy).child)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	// The marquee's box.
	if (press_.marquee && press_.dragging) {
		const ImVec2 a(ox + std::min(press_.from_x, press_.to_x) * sx, oy + std::min(press_.from_y, press_.to_y) * sy);
		const ImVec2 b(ox + std::max(press_.from_x, press_.to_x) * sx, oy + std::max(press_.from_y, press_.to_y) * sy);
		paint->AddRectFilled(a, b, IM_COL32(140, 210, 255, 30));
		paint->AddRect(a, b, kMarqueeColor, 0.0f, 0, 1.0f);
	}
	// The primary window: outlined on the pixels the game draws its rect on, with its eight
	// handles; the cursor says what a press there does.
	const int selected = frame.selected.child ? document.window_index(frame.selected) : -1;
	if (selected >= 0 && compiler.widget_rect(selected, state, &rect)) {
		ImVec2 a, b;
		device_rect(rect, ox, oy, sx, sy, a, b);
		paint->AddRect(a, b, kSelectedColor, 0.0f, 0, 2.0f);
		const float half = kHandleSize * 0.5f;
		for (const LayoutHandle handle : kHandles) {
			const ImVec2 p = handle_point(handle, a, b);
			paint->AddRectFilled(ImVec2(p.x - half, p.y - half), ImVec2(p.x + half, p.y + half), IM_COL32_WHITE);
			paint->AddRect(ImVec2(p.x - half, p.y - half), ImVec2(p.x + half, p.y + half), IM_COL32(40, 40, 40, 255));
		}
		LayoutHandle handle = LayoutHandle::Move;
		if (press_.active && press_.window == frame.selected) {
			ImGui::SetMouseCursor(handle_cursor(press_.handle));
		} else if (idle && handle_at(mouse.x, mouse.y, a, b, handle)) {
			ImGui::SetMouseCursor(handle_cursor(handle));
		} else if (idle && inside(mouse.x, mouse.y, a, b)) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		}
	}
	ImGui::EndChild();
}

} // namespace opennova::editor
