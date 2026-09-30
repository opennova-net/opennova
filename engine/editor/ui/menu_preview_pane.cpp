#include "menu_preview_pane.h"

#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_render_check.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <variant>
#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr int kMaxDevice = 8192;

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

} // namespace

MenuPreviewPane::MenuPreviewPane(EditorHost &host) :
		host_(host), canvas_(menu::kMenuDesignWidth, menu::kMenuDesignHeight), requests_(host) {}

void MenuPreviewPane::end_frame() {
	menu_canvas_.end_frame(requests_);
	canvas_.end_frame();
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
	const MnuDocument &document = *frame.canvas.document;
	const NodeAddress &selected = frame.canvas.primary;
	MenuPreviewOptions options = viewport_->options();
	const MenuPreviewOptions before = options;
	const float unit = ImGui::GetFontSize();
	ui_kit::WrapRow row;

	using Zoom = ViewportCanvas::Zoom;
	const Zoom zoom = canvas_.zoom();
	char zoom_label[32];
	if (zoom == Zoom::Fit)
		std::snprintf(zoom_label, sizeof(zoom_label), "Fit");
	else if (zoom == Zoom::Device)
		std::snprintf(zoom_label, sizeof(zoom_label), "Device size");
	else
		std::snprintf(
				zoom_label, sizeof(zoom_label), "%d%%", int(std::lround(canvas_.scale() * 100.0f)));
	row.next(ui_kit::field_width(unit * 7.0f, "Zoom"));
	ImGui::SetNextItemWidth(unit * 7.0f);
	if (ImGui::BeginCombo("Zoom", zoom_label)) {
		if (ImGui::Selectable("Fit", zoom == Zoom::Fit))
			canvas_.set_zoom(Zoom::Fit);
		for (const float level : ViewportCanvas::kZoomLevels) {
			char text[16];
			std::snprintf(text, sizeof(text), "%d%%", int(std::lround(level * 100.0f)));
			if (ImGui::Selectable(
						text, zoom == Zoom::Scale && std::fabs(canvas_.scale() - level) < 0.001f))
				canvas_.set_zoom(Zoom::Scale, level);
		}
		if (ImGui::Selectable("Device size", zoom == Zoom::Device))
			canvas_.set_zoom(Zoom::Device);
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Ctrl+wheel over the picture zooms about the mouse.");
	if (zoom == Zoom::Device) {
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
	ImGui::BeginDisabled(frame.canvas.windows.empty() || document.blocked());
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
		ui_kit::empty_state(menu_preview_status_message(MenuPreviewStatus::NoDevice, std::string()).c_str());
		return;
	}
	std::string detail;
	const MenuPreviewStatus status = viewport_->status(&detail);
	// The screen the view keeps for the preview: the last menu screen selected, kept while
	// another document (the stylesheet it draws with) is active.
	Frame frame;
	MenuCanvasFrame &canvas = frame.canvas;
	for (const auto &open : view.documents)
		if (open->path() == view.menu_preview.path)
			canvas.document = dynamic_cast<const MnuDocument *>(open.get());
	canvas.screen = canvas.document ? canvas.document->row(view.menu_preview.screen) : nullptr;
	if (status != MenuPreviewStatus::Ready || !canvas.document || !canvas.screen) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", menu_preview_status_message(status, detail).c_str());
		ImGui::PopTextWrapPos();
		return;
	}
	const MnuDocument &document = *canvas.document;
	const Node &screen = *canvas.screen;
	if (!viewport_->missing().empty() || !viewport_->unreadable().empty()) {
		ImGui::PushTextWrapPos(0.0f);
		if (!viewport_->missing().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", missing_banner(viewport_->missing()).c_str());
		if (!viewport_->unreadable().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
			                   unreadable_banner(viewport_->unreadable()).c_str());
		ImGui::PopTextWrapPos();
	}
	canvas.compiler = viewport_->compiler();
	canvas.state = viewport_->frame_state();
	// A picture of another revision (the edit lands on the next pump) maps no index.
	canvas.current =
			canvas.compiler && canvas.state && viewport_->shown_revision() == document.revision();
	// The selection on this screen while the menu is the active document: the primary record,
	// the window holding it, every selected window, and what the clipboard takes of it.
	const bool active = view.active_document == document.path();
	const std::vector<NodeAddress> none;
	menu_canvas_select(
			canvas, active ? view.selection : NodeAddress(), active ? view.selected : none);
	frame.clipboard = menu_canvas_clipboard(canvas, !view.clipboard.empty());
	follow_selection_(document, canvas.primary);
	toolbar_(frame);
	canvas.snap = snap_;
	// The compiler's notes on the screen as it stands (ADR 0046 S9j2): what configure
	// noted, then what laying the frame state out comes to.
	if (canvas.current) {
		canvas.notes = canvas.compiler->build_notes();
		for (menu::MenuFrameNote &note : canvas.compiler->layout_notes(*canvas.state))
			canvas.notes.push_back(std::move(note));
	}
	menu_canvas_.follow(canvas, requests_);
	keys_(frame);
	// Room below the picture for the notes list: its heading and up to six notes.
	const size_t note_lines =
			canvas.notes.empty() ? 0 : 1 + std::min<size_t>(canvas.notes.size(), 6);
	const float notes_height = ImGui::GetFrameHeightWithSpacing() * float(note_lines);
	draw_canvas_(frame, std::max(64.0f, ImGui::GetContentRegionAvail().y - notes_height));

	// The notes as a list, each cut to the window (whole in its tooltip); a click selects the
	// record a note is on.
	if (canvas.notes.empty())
		return;
	ImGui::TextDisabled("Notes (%d)", int(canvas.notes.size()));
	if (ImGui::BeginChild("notes", ImVec2(0.0f, 0.0f), false)) {
		for (size_t i = 0; i < canvas.notes.size(); ++i) {
			const menu::MenuFrameNote &note = canvas.notes[i];
			const std::string name =
					note.widget >= 0 ? canvas.compiler->widget_name(note.widget) : screen.name();
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
	if (menu_canvas_.gesture().nudging() && (!held || !focused))
		menu_canvas_.end(requests_);
	if (!focused || menu_canvas_.gesture().pressed())
		return;
	const MnuDocument &document = *frame.canvas.document;
	if (!document.blocked()) {
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) return clipboard_(frame, EditorRequestKind::Copy);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) return clipboard_(frame, EditorRequestKind::Cut);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) return clipboard_(frame, EditorRequestKind::Paste);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) return clipboard_(frame, EditorRequestKind::Duplicate);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && menu_canvas_escape(frame.canvas, requests_))
		return;
	const int step = io.KeyShift ? kLayoutGrid : 1;
	int dx = 0, dy = 0;
	if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx -= step;
	if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx += step;
	if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy -= step;
	if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy += step;
	menu_canvas_.nudge(frame.canvas, dx, dy, requests_);
}

// Copy, Cut, Duplicate and Paste as the menu clipboard's rule says (menu_clipboard).
void MenuPreviewPane::clipboard_(const Frame &frame, EditorRequestKind kind) {
	const MnuDocument &document = *frame.canvas.document;
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
	const MenuCanvasFrame &canvas = frame.canvas;
	const size_t count = canvas.windows.size();
	for (const ArrangeOp op : kArrangeOps) {
		if (op == ArrangeOp::DistributeHorizontally || op == ArrangeOp::BringToFront) ImGui::Separator();
		const bool enabled =
				canvas.current && count >= arrange_minimum(op) && !canvas.document->blocked();
		if (ImGui::MenuItem(arrange_op_label(op), nullptr, false, enabled))
			menu_canvas_arrange(canvas, op, requests_);
		if (!enabled)
			ui_kit::tooltip("Select " + std::to_string(arrange_minimum(op)) + " windows or more.");
	}
}

// The canvas: the picture, the pointer's gestures on it, the right-click menu, and what the
// picture maps drawn over it.
void MenuPreviewPane::draw_canvas_(const Frame &frame, float height) {
	const MenuCanvasFrame &canvas = frame.canvas;
	const MnuDocument &document = *canvas.document;
	const MenuPreviewOptions &options = viewport_->options();
	const bool shows = canvas_.begin(
			height, options.width, options.height,
			[this](int width, int tall) { viewport_->draw(width, tall); },
			[&](const CanvasInput &in) { return menu_canvas_.hover_tip(canvas, in); });
	if (shows) {
		const CanvasInput &in = canvas_.input();
		// Where the mouse is on the picture, in design units (the toolbar's readout).
		const float sx = float(in.width) / float(menu::kMenuDesignWidth);
		const float sy = float(in.height) / float(menu::kMenuDesignHeight);
		mouse_on_picture_ = in.hovered && in.mouse.x >= 0.0f && in.mouse.y >= 0.0f &&
				in.mouse.x < float(in.width) && in.mouse.y < float(in.height);
		mouse_design_x_ = int(std::floor(in.mouse.x / sx));
		mouse_design_y_ = int(std::floor(in.mouse.y / sy));
		menu_canvas_.input(canvas, in, requests_);
		// The right button: the window under it selected unless it already is, and the menu of
		// what the selection can do.
		if (canvas_.right_clicked() && !menu_canvas_.gesture().pressed()) {
			const NodeAddress window = menu_window_at(canvas, in);
			if (window.child && !holds(host_.view().selected, window))
				window_requests::select(host_, document, window);
			ImGui::OpenPopup("canvas_menu");
		}
		if (ImGui::BeginPopup("canvas_menu")) {
			const bool editable_now = !document.blocked();
			const bool copyable = frame.clipboard.copy && editable_now;
			if (ImGui::MenuItem("Cut", "Ctrl+X", false, copyable))
				clipboard_(frame, EditorRequestKind::Cut);
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, copyable))
				clipboard_(frame, EditorRequestKind::Copy);
			if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable_now && frame.clipboard.paste))
				clipboard_(frame, EditorRequestKind::Paste);
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, copyable))
				clipboard_(frame, EditorRequestKind::Duplicate);
			ImGui::Separator();
			if (ImGui::BeginMenu("Arrange", !canvas.windows.empty() && editable_now)) {
				arrange_items_(frame);
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
		canvas_.draw(menu_canvas_.shapes(canvas, in), menu_canvas_.cursor(canvas, in));
	}
	canvas_.end();
}

} // namespace opennova::editor
