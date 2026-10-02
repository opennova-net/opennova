#include <editor/ui/menu_viewport_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <editor/documents/mnu_clipboard.h>
#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_canvas.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_viewport.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>
#include <runtime/menu/menu_frame.h>

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

// The viewport's options set (a SetViewport of them).
void set_options(Workspace &workspace, const ViewportModel &model, const MenuViewportOptions &options) {
	workspace.request(request::set_viewport(
			model.path(), viewport_change(ViewportKind::Menu, "options", menu_options_to_json(options))));
}

} // namespace

// What the view keeps of its own: the snap, and where the mouse was on the picture in design units
// at the last canvas pass (the toolbar's readout).
struct MenuViewportView::Tools {
	bool snap = true;
	bool mouse_on_picture = false;
	int mouse_x = 0;
	int mouse_y = 0;

	void toolbar(Workspace &workspace, ViewportView &view, ViewportCanvas &canvas, const MenuViewport &menu,
			const MenuCanvasFrame &frame);
	void arrange_items(Workspace &workspace, const MenuCanvasFrame &frame);
	void keys(Workspace &workspace, const MenuCanvasFrame &frame, const MenuClipboard &board, bool pressed);
	void clipboard(Workspace &workspace, const MenuCanvasFrame &frame, const MenuClipboard &board,
			EditorRequestKind kind);
};

MenuViewportView::MenuViewportView() : ViewportView(ViewportKind::Menu), tools_(std::make_unique<Tools>()) {}

MenuViewportView::~MenuViewportView() = default;

// The toolbar, on a row that wraps whole controls in a narrow window.
void MenuViewportView::Tools::toolbar(Workspace &workspace, ViewportView &, ViewportCanvas &canvas,
		const MenuViewport &menu, const MenuCanvasFrame &frame) {
	const MnuDocument &document = *frame.document;
	const NodeAddress &selected = frame.primary;
	MenuViewportOptions options = menu.options();
	const MenuViewportOptions before = options;
	const float unit = ImGui::GetFontSize();
	ui_kit::WrapRow row;

	using Zoom = ViewportCanvas::Zoom;
	const Zoom zoom = canvas.zoom();
	char zoom_label[32];
	if (zoom == Zoom::Fit)
		std::snprintf(zoom_label, sizeof(zoom_label), "Fit");
	else if (zoom == Zoom::Device)
		std::snprintf(zoom_label, sizeof(zoom_label), "Device size");
	else
		std::snprintf(zoom_label, sizeof(zoom_label), "%d%%", int(std::lround(canvas.scale() * 100.0f)));
	row.next(ui_kit::field_width(unit * 7.0f, "Zoom"));
	ImGui::SetNextItemWidth(unit * 7.0f);
	if (ImGui::BeginCombo("Zoom", zoom_label)) {
		if (ImGui::Selectable("Fit", zoom == Zoom::Fit)) canvas.set_zoom(Zoom::Fit);
		for (const float level : ViewportCanvas::kZoomLevels) {
			char text[16];
			std::snprintf(text, sizeof(text), "%d%%", int(std::lround(level * 100.0f)));
			if (ImGui::Selectable(text, zoom == Zoom::Scale && std::fabs(canvas.scale() - level) < 0.001f))
				canvas.set_zoom(Zoom::Scale, level);
		}
		if (ImGui::Selectable("Device size", zoom == Zoom::Device)) canvas.set_zoom(Zoom::Device);
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Ctrl+wheel over the picture zooms about the mouse.");
	if (zoom == Zoom::Device) {
		int size[2] = {menu.state().width, menu.state().height};
		row.next(unit * 8.0f);
		ImGui::SetNextItemWidth(unit * 8.0f);
		if (ImGui::InputInt2("##device", size)) {
			io::JsonValue device = io::JsonValue::make_object();
			device.set("width", io::json_number(std::clamp(size[0], 1, kMaxDevice)));
			device.set("height", io::json_number(std::clamp(size[1], 1, kMaxDevice)));
			workspace.request(request::set_viewport(
					menu.path(), viewport_change(ViewportKind::Menu, "device", std::move(device))));
		}
		ui_kit::tooltip("The screen size in pixels to draw at; each axis scales on its own, as in "
		                "the game.");
	}
	row.next(ui_kit::checkbox_width("Snap"));
	ImGui::Checkbox("Snap", &snap);
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
	const mnu::WindowType type = menu_window_type(document, selected);
	auto hold = [&](const char *label, bool &flag, const char *tip) {
		row.next(ui_kit::checkbox_width(label));
		if (ImGui::Checkbox(label, &flag)) options.force_window = selected.child;
		ui_kit::tooltip(tip);
	};
	if (menu_type_checkable(type)) hold("Checked", options.checked, "Show the selected window checked.");
	if (menu_type_has_list(type)) hold("Open list", options.popup_open, "Show the selected combo box with its list open.");
	if (menu_type_editable(type)) hold("Focus", options.focused, "Show the selected edit box focused, with its caret.");
	ImGui::EndDisabled();

	// Arrange: the selected windows aligned to the primary, spread, or reordered.
	row.next(ui_kit::button_width("Arrange"));
	ImGui::BeginDisabled(frame.windows.empty() || !frame.editable);
	if (ImGui::Button("Arrange")) ImGui::OpenPopup("arrange");
	ImGui::EndDisabled();
	ui_kit::tooltip("Align the selected windows to the primary one (the last selected), spread "
	                "them evenly, or change their drawing order. Shift+click or drag a box to "
	                "select several.");
	if (ImGui::BeginPopup("arrange")) {
		arrange_items(workspace, frame);
		ImGui::EndPopup();
	}

	// Where the mouse is on the picture, in design units: as wide as the widest it reads.
	row.next(ui_kit::text_width("x 0000, y 0000"));
	ImGui::AlignTextToFramePadding();
	if (mouse_on_picture) ImGui::Text("x %d, y %d", mouse_x, mouse_y);
	else ImGui::TextDisabled("x -, y -");
	ui_kit::tooltip("Where the mouse is on the picture, in the menu's 800 x 600 units.");
	if (options != before) set_options(workspace, menu, options);
}

void MenuViewportView::Tools::arrange_items(Workspace &workspace, const MenuCanvasFrame &frame) {
	CanvasWindowRequests requests(workspace);
	const size_t count = frame.windows.size();
	for (const ArrangeOp op : kArrangeOps) {
		if (op == ArrangeOp::DistributeHorizontally || op == ArrangeOp::BringToFront) ImGui::Separator();
		const bool enabled = frame.current && count >= arrange_minimum(op) && frame.editable;
		if (ImGui::MenuItem(arrange_op_label(op), nullptr, false, enabled))
			menu_canvas_arrange(frame, op, requests);
		if (!enabled)
			ui_kit::tooltip("Select " + std::to_string(arrange_minimum(op)) + " windows or more.");
	}
}

// Ctrl+C / X / V / D copy, cut, paste and duplicate windows while the viewport has the focus, no
// text box takes the keys and no press is down, as the menu clipboard's rule says (the arrows and
// Esc are the canvas's, preview/menu_canvas); a Copy edits nothing, so only the others wait while an
// operation holds the documents (S13 A3).
void MenuViewportView::Tools::keys(
		Workspace &workspace, const MenuCanvasFrame &frame, const MenuClipboard &board, bool pressed) {
	const ImGuiIO &io = ImGui::GetIO();
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
	if (!focused || pressed || frame.document->blocked()) return;
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) return clipboard(workspace, frame, board, EditorRequestKind::Copy);
	if (!frame.editable) return;
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) return clipboard(workspace, frame, board, EditorRequestKind::Cut);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) return clipboard(workspace, frame, board, EditorRequestKind::Paste);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D))
		return clipboard(workspace, frame, board, EditorRequestKind::Duplicate);
}

// Copy, Cut, Duplicate and Paste as the menu clipboard's rule says (menu_clipboard).
void MenuViewportView::Tools::clipboard(Workspace &workspace, const MenuCanvasFrame &frame,
		const MenuClipboard &board, EditorRequestKind kind) {
	const MnuDocument &document = *frame.document;
	if (kind != EditorRequestKind::Paste) {
		if (board.copy) window_requests::clipboard(workspace, document, kind);
		return;
	}
	if (board.paste)
		window_requests::paste(workspace, document, board.paste_row, board.paste_parent, board.paste_position);
}

void MenuViewportView::draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) {
	const auto &menu = static_cast<const MenuViewport &>(model);
	const SessionView &view = workspace.view();
	// What the toolbar, the keys and the right-click menu read: the canvas's frame of the viewport.
	const MenuCanvasFrame frame = menu.canvas_frame(context);
	if (!frame.document || !frame.screen) {
		draw_empty(workspace, &model, model.path());
		return;
	}
	if (!menu.missing().empty() || !menu.unreadable().empty()) {
		ImGui::PushTextWrapPos(0.0f);
		if (!menu.missing().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", missing_banner(menu.missing()).c_str());
		if (!menu.unreadable().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", unreadable_banner(menu.unreadable()).c_str());
		ImGui::PopTextWrapPos();
	}
	const MenuClipboard board = menu_canvas_clipboard(frame, !view.documents.clipboard.empty());
	tools_->toolbar(workspace, *this, canvas_ui(), menu, frame);
	snap = tools_->snap ? 1.0f : 0.0f;
	context.snap = snap;
	const bool pressed = half() && half()->gesture().pressed();
	tools_->keys(workspace, frame, board, pressed);
	// Room below the picture for the notes list: its heading and up to six notes.
	const std::vector<menu::MenuFrameNote> &notes = menu.notes();
	const size_t note_lines = notes.empty() ? 0 : 1 + std::min<size_t>(notes.size(), 6);
	const float notes_height = ImGui::GetFrameHeightWithSpacing() * float(note_lines);
	const MnuDocument &document = *frame.document;
	canvas(workspace, model, context, std::max(64.0f, ImGui::GetContentRegionAvail().y - notes_height),
			[&](const CanvasInput &in) {
				// Where the mouse is on the picture, in design units (the toolbar's readout).
				const float sx = float(in.width) / float(menu::kMenuDesignWidth);
				const float sy = float(in.height) / float(menu::kMenuDesignHeight);
				tools_->mouse_on_picture = in.hovered && in.mouse.x >= 0.0f && in.mouse.y >= 0.0f &&
						in.mouse.x < float(in.width) && in.mouse.y < float(in.height);
				tools_->mouse_x = int(std::floor(in.mouse.x / sx));
				tools_->mouse_y = int(std::floor(in.mouse.y / sy));
				// The right button: the window under it selected unless it already is, and the menu
				// of what the selection can do.
				if (canvas_ui().right_clicked() && !(half() && half()->gesture().pressed())) {
					const NodeAddress window = menu_window_at(frame, in);
					if (window.child && !view.documents.selection.holds(window))
						window_requests::select(workspace, document, window);
					ImGui::OpenPopup("canvas_menu");
				}
				if (ImGui::BeginPopup("canvas_menu")) {
					// The edits held back while an operation holds the documents (S13 A3); a Copy edits
					// nothing.
					const bool readable = !document.blocked();
					const bool editable_now = frame.editable;
					const bool copyable = board.copy && editable_now;
					if (ImGui::MenuItem("Cut", "Ctrl+X", false, copyable))
						tools_->clipboard(workspace, frame, board, EditorRequestKind::Cut);
					if (ImGui::MenuItem("Copy", "Ctrl+C", false, board.copy && readable))
						tools_->clipboard(workspace, frame, board, EditorRequestKind::Copy);
					if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable_now && board.paste))
						tools_->clipboard(workspace, frame, board, EditorRequestKind::Paste);
					if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, copyable))
						tools_->clipboard(workspace, frame, board, EditorRequestKind::Duplicate);
					ImGui::Separator();
					if (ImGui::BeginMenu("Arrange", !frame.windows.empty() && editable_now)) {
						tools_->arrange_items(workspace, frame);
						ImGui::EndMenu();
					}
					ImGui::EndPopup();
				}
			});

	// The notes as a list, each cut to the window (whole in its tooltip); a click selects the
	// record a note is on.
	if (notes.empty()) return;
	const Node &screen = *frame.screen;
	ImGui::TextDisabled("Notes (%d)", int(notes.size()));
	if (ImGui::BeginChild("notes", ImVec2(0.0f, 0.0f), false)) {
		for (size_t i = 0; i < notes.size(); ++i) {
			const menu::MenuFrameNote &note = notes[i];
			const std::string name =
					note.widget >= 0 ? menu.render().compiler().widget_name(note.widget) : screen.name();
			const std::string line = name + ": " + menu_note_message(note);
			const std::string shown = ui_kit::fit(line, ImGui::GetContentRegionAvail().x);
			ImGui::PushID(int(i));
			if (ImGui::Selectable((shown + "###note").c_str())) {
				const NodeAddress address = menu_note_address(note, document, screen, nullptr);
				window_requests::select(workspace, document, address);
			}
			if (shown != line) ui_kit::tooltip(line);
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
}

} // namespace opennova::editor
