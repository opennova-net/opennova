#include <editor/preview/menu_viewport.h>

#include <cmath>
#include <cstdio>
#include <variant>

#include <editor/documents/mnu_document.h>
#include <editor/model/diagnostic.h>
#include <editor/model/edit.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_canvas.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_report.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using io::json_whole_in;

constexpr NodeKind kWindowKind = node_kind(MenuKind::Window);

// The frame state's row for a widget, made on first use (the runtime's per-widget state).
menu::MenuWidgetState &widget_state(menu::MenuFrameState &state, int index) {
	for (menu::MenuWidgetState &row : state.widgets)
		if (row.index == index) return row;
	menu::MenuWidgetState row;
	row.index = index;
	state.widgets.push_back(row);
	return state.widgets.back();
}

// The options an `options` member sets over `held`: {show_hidden, force_id (a window record, 0
// none), force_state (menu_force_state_from_token), checked, popup_open, focus}, each optional, a
// number's fraction dropped. False, with why, for another member or a value out of range or of
// another type.
bool read_options(const JsonValue &json, MenuViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "\"options\" is an object.";
		return false;
	}
	MenuViewportOptions out = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		int64_t number = 0;
		if (key == "force_id") {
			if (!json_whole_in(value, 0.0, 9007199254740992.0, number)) {
				error = "options.force_id is a window's record id (0: none).";
				return false;
			}
			out.force_window = NodeId(number);
		} else if (key == "force_state") {
			if (!value.is_string() || !menu_force_state_from_token(value.string, out.force_state)) {
				error = "options.force_state is normal, mouseover, selected or disabled.";
				return false;
			}
		} else if (key == "show_hidden" || key == "checked" || key == "popup_open" || key == "focus") {
			if (!value.is_bool()) {
				error = "options." + key + " is true or false.";
				return false;
			}
			bool &flag = key == "show_hidden" ? out.show_hidden
					: key == "checked"        ? out.checked
					: key == "popup_open"     ? out.popup_open
											  : out.focused;
			flag = value.boolean;
		} else {
			error = "Unknown menu option \"" + key +
					"\" (it takes show_hidden, force_id, force_state, checked, popup_open, focus).";
			return false;
		}
	}
	held = out;
	return true;
}

// The window `id` of the screen the frame shows (none when it is not one of its windows).
NodeAddress shown_window(const MenuCanvasFrame &frame, NodeId id) {
	const NodeAddress window = frame.document->address_of(id);
	const bool shown = window.row == frame.screen->id && window.kind == kWindowKind && window.child;
	return shown ? window : NodeAddress();
}

} // namespace

io::JsonValue menu_options_to_json(const MenuViewportOptions &held) {
	JsonValue options = JsonValue::make_object();
	options.set("show_hidden", JsonValue::make_bool(held.show_hidden));
	options.set("force_id", json_number(double(held.force_window)));
	options.set("force_state", json_string(menu_force_state_token(held.force_state)));
	options.set("checked", JsonValue::make_bool(held.checked));
	options.set("popup_open", JsonValue::make_bool(held.popup_open));
	options.set("focus", JsonValue::make_bool(held.focused));
	return options;
}

const char *menu_force_state_token(int state) {
	switch (state) {
	case menu::kStateMouseover: return "mouseover";
	case menu::kStateSelected: return "selected";
	case menu::kStateDisabled: return "disabled";
	default: return "normal";
	}
}

bool menu_force_state_from_token(const std::string &token, int &out) {
	if (token == "normal") out = -1;
	else if (token == "mouseover") out = menu::kStateMouseover;
	else if (token == "selected") out = menu::kStateSelected;
	else if (token == "disabled") out = menu::kStateDisabled;
	else return false;
	return true;
}

void apply_menu_options(const MenuViewportOptions &options, int forced_index,
		const menu::MenuFrameCompiler &compiler, menu::MenuFrameState &state) {
	if (options.show_hidden) {
		for (int index = 0; index < compiler.widget_count(); ++index) {
			menu::MenuWidgetState &row = widget_state(state, index);
			row.hide = false;
			row.show = true;
		}
	}
	if (forced_index < 0 || forced_index >= compiler.widget_count()) return;
	menu::MenuWidgetState &row = widget_state(state, forced_index);
	// The pump's verdicts: hovered with the button up is state 2, held down state 3; the
	// runtime's disable replaces the authored one.
	switch (options.force_state) {
	case menu::kStateMouseover: row.hovered = true; break;
	case menu::kStateSelected: row.hovered = row.pressed = true; break;
	case menu::kStateDisabled: row.has_disabled = row.disabled = true; break;
	default: break;
	}
	if (options.checked) row.has_checked = row.checked = true;
	if (options.popup_open) row.popup_open = true;
	if (options.focused) {
		row.focused = true;
		// A clock in the caret's shown half-second (MenuFrameState::time_ms).
		state.time_ms = 0x300;
	}
}

mnu::WindowType menu_window_type(const MnuDocument &document, const NodeAddress &window) {
	Value type;
	if (!window.child || !document.get(window, "type", type) || !std::holds_alternative<std::string>(type))
		return mnu::WindowType::Window;
	return mnu::parse_window_type(std::get<std::string>(type));
}

bool menu_type_checkable(mnu::WindowType type) {
	return type == mnu::WindowType::CheckBox || type == mnu::WindowType::Radio ||
			type == mnu::WindowType::RadioEdit;
}
bool menu_type_has_list(mnu::WindowType type) { return type == mnu::WindowType::Combo; }
bool menu_type_editable(mnu::WindowType type) {
	return type == mnu::WindowType::Edit || type == mnu::WindowType::MultilineEdit;
}

// --- MenuViewport --------------------------------------------------------------------------------

MenuViewport::MenuViewport(std::string path) :
		ViewportModel(ViewportKind::Menu, std::move(path),
				ViewportState{ menu::kMenuDesignWidth, menu::kMenuDesignHeight }) {}

std::unique_ptr<ViewportModel> MenuViewport::make(const std::string &path) {
	return std::make_unique<MenuViewport>(path);
}

ViewportStatus MenuViewport::status() const {
	switch (reason_) {
	case MenuScreenStatus::Ready: return ViewportStatus::Ready;
	case MenuScreenStatus::Unserializable:
	case MenuScreenStatus::ScreenMissing: return ViewportStatus::Failed;
	default: return ViewportStatus::Empty;
	}
}

std::string MenuViewport::caption() const {
	return screen_name_.empty() ? std::string() : " - " + screen_name_;
}

ViewportLayout MenuViewport::layout() const {
	return ViewportLayout{ menu::kMenuDesignWidth, menu::kMenuDesignHeight };
}

std::unique_ptr<CanvasHalf> MenuViewport::make_canvas() const {
	return std::make_unique<MenuCanvas>();
}

ViewportAction MenuViewport::stop_(MenuScreenStatus reason, const std::string &detail) {
	reason_ = reason;
	detail_ = detail;
	notes_.clear();
	missing_.clear();
	unreadable_.clear();
	forced_index_ = -1;
	device_rects_.clear();
	shown_none();
	return reason == MenuScreenStatus::Unserializable || reason == MenuScreenStatus::ScreenMissing
			? picture_.failed()
			: picture_.stop();
}

void MenuViewport::follow_selection_(const ViewportInput &input, const MnuDocument &document) {
	const SessionView &view = input.view;
	const NodeAddress selected = view.documents.active == document.path()
			? window_holding(document, view.documents.selection.primary, part_)
			: NodeAddress();
	if (selected.child == followed_window_) return;
	followed_window_ = selected.child;
	if (!options_.forcing() || !selected.child) return;
	// A window newly selected is the one held; what its type cannot hold is let go.
	const mnu::WindowType type = menu_window_type(document, selected);
	MenuViewportOptions options = options_;
	options.force_window = selected.child;
	options.checked = options.checked && menu_type_checkable(type);
	options.popup_open = options.popup_open && menu_type_has_list(type);
	options.focused = options.focused && menu_type_editable(type);
	if (options == options_) return;
	options_ = options;
	++options_serial_;
	state_moved();
}

ViewportAction MenuViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) {
		part_ = 0;
		screen_name_.clear();
		return stop_(MenuScreenStatus::NoProject, std::string());
	}
	const auto *document = dynamic_cast<const MnuDocument *>(input.document);
	if (!document) {
		// A viewport of no menu (the Preview window's while none is followed): a menu open with no
		// screen selected asks for one.
		bool menu_open = false;
		for (const auto &open : view.documents.open)
			menu_open = menu_open || dynamic_cast<const MnuDocument *>(open.get());
		part_ = 0;
		screen_name_.clear();
		return stop_(menu_open ? MenuScreenStatus::NoScreen : MenuScreenStatus::NoMenu, std::string());
	}
	// The screen: the Preview window's while it follows this menu, else the one it showed.
	const PreviewTarget &target = view.documents.previews[ViewportKind::Menu];
	if (target.path == path() && target.part) part_ = target.part;
	const Node *row = document->row(part_);
	if (!row) {
		screen_name_.clear();
		return stop_(MenuScreenStatus::NoScreen, std::string());
	}
	screen_name_ = row->name();
	follow_selection_(input, *document);
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	const PreviewFollow::Key key{ row->id, options_serial_ };
	switch (picture_.follow(key, input.change != ChangeClass::None, files, generation)) {
	case PreviewFollow::Found::Same: return ViewportAction::Keep;
	case PreviewFollow::Found::Files:
	case PreviewFollow::Found::Anew: break;
	}
	picture_.show(key, generation);
	// The screen compiled headless from the menu the game would read were it saved now.
	style_vars_ = style_.vars(files);
	++configures_;
	const MenuScreenStatus compiled = render_.configure(*document, row->id, files, style_vars_);
	if (compiled != MenuScreenStatus::Ready) return stop_(compiled, render_.detail());
	forced_index_ = -1;
	if (options_.force_window) {
		const NodeAddress forced = document->address_of(options_.force_window);
		if (forced.row == row->id) forced_index_ = document->window_index(forced);
	}
	menu::MenuFrameState state = render_.state();
	apply_menu_options(options_, forced_index_, render_.compiler(), state);
	render_.set_state(state);
	notes_ = render_.notes();
	split_unloaded(render_.assets(), missing_, unreadable_);
	device_rects_.clear();
	reason_ = MenuScreenStatus::Ready;
	detail_.clear();
	shown(*document);
	// What the picture read: the two stylesheets and every file the screen loaded or looked for.
	std::vector<menu::MenuDependency> dependencies;
	style_.dependencies(dependencies);
	dependencies.insert(dependencies.end(), render_.assets().dependencies().begin(),
			render_.assets().dependencies().end());
	FileStamps read;
	for (const menu::MenuDependency &dependency : dependencies) read.note(dependency.name, dependency.stamp);
	return picture_.built(std::move(read));
}

bool MenuViewport::takes_(const std::string &member) const {
	return member == "options";
}

bool MenuViewport::check_(const io::JsonValue &json, std::string &error) const {
	const JsonValue *options = json.get("options");
	MenuViewportOptions held = options_;
	return !options || read_options(*options, held, error);
}

void MenuViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	const JsonValue *options = json.get("options");
	MenuViewportOptions held = options_;
	std::string error;
	if (!options || !read_options(*options, held, error) || held == options_) return;
	options_ = held;
	++options_serial_;
}

void MenuViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	device_rects_ = report.rects;
}

MenuCanvasFrame MenuViewport::canvas_frame(const ViewportContext &context) const {
	MenuCanvasFrame frame;
	const auto *document = dynamic_cast<const MnuDocument *>(context.input.document);
	frame.document = document;
	frame.screen = document ? document->row(part_) : nullptr;
	if (!document || !frame.screen) return frame;
	if (status() == ViewportStatus::Ready) {
		frame.compiler = &render_.compiler();
		frame.state = &render_.state();
		frame.notes = &notes_;
	}
	// A picture of another revision (an edit lands on the next pump) maps no index.
	frame.current = frame.compiler && current(context.input);
	frame.snap = context.snap != 0.0f;
	frame.editable = context.editable();
	// The selection on this screen while the menu is the active document.
	static const std::vector<NodeAddress> kNone;
	const SessionView &view = context.input.view;
	const bool active = view.documents.active == document->path();
	menu_canvas_select(frame, active ? view.documents.selection.primary : NodeAddress(),
			active ? view.documents.selection.records : kNone);
	return frame;
}

ViewportHit MenuViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	const MenuCanvasFrame frame = canvas_frame(context);
	out.current = frame.current;
	if (!frame.current) return out;
	out.index = render_.compiler().hit_widget(render_.state(), x, y, 1.0f, 1.0f);
	if (out.index < 0) return out;
	out.id = frame.document->window_at(*frame.screen, size_t(out.index));
	out.name = render_.compiler().widget_name(out.index);
	const int type = render_.compiler().widget_kind(out.index);
	out.kind = type >= 0 ? mnu::window_type_name(static_cast<mnu::WindowType>(type)) : "";
	return out;
}

bool MenuViewport::drag(const ViewportContext &context, const ViewportDrag &drag,
		CanvasRequests &out, std::string &error) const {
	const MenuCanvasFrame frame = canvas_frame(context);
	LayoutHandle handle = LayoutHandle::Move;
	if (!layout_handle_from_token(drag.handle, handle)) {
		error = "Unknown handle \"" + drag.handle +
				"\" (move, left, right, top, bottom, top_left, top_right, bottom_left, bottom_right).";
		return false;
	}
	if (!frame.current) {
		error = "The viewport does not show the menu as it is now (select its screen and let it catch up).";
		return false;
	}
	if (!frame.editable) {
		error = "The session takes no edit now (an operation holds the documents).";
		return false;
	}
	const NodeAddress held = shown_window(frame, drag.id);
	if (!held.child) {
		error = "Record " + std::to_string(drag.id) + " is no window of the screen the viewport shows.";
		return false;
	}
	// To a design point: as far as the handle is from it where the picture shows it now (a move's
	// handle the window's top left corner, an edge's its edge, a corner's its corner).
	int dx = int(drag.x), dy = int(drag.y);
	if (!drag.by) {
		mnu::RectEdges rect{};
		const int index = frame.document->window_index(held);
		if (index < 0 || !frame.compiler->widget_rect(index, *frame.state, &rect)) {
			error = "Record " + std::to_string(drag.id) + " has no rect on the screen the viewport shows.";
			return false;
		}
		const bool left = handle == LayoutHandle::Move || handle == LayoutHandle::Left ||
				handle == LayoutHandle::TopLeft || handle == LayoutHandle::BottomLeft;
		const bool right = handle == LayoutHandle::Right || handle == LayoutHandle::TopRight ||
				handle == LayoutHandle::BottomRight;
		const bool top = handle == LayoutHandle::Move || handle == LayoutHandle::Top ||
				handle == LayoutHandle::TopLeft || handle == LayoutHandle::TopRight;
		const bool bottom = handle == LayoutHandle::Bottom || handle == LayoutHandle::BottomLeft ||
				handle == LayoutHandle::BottomRight;
		const int x = int(std::lround(drag.x)), y = int(std::lround(drag.y));
		dx = left ? x - rect.left : right ? x - rect.right : 0;
		dy = top ? y - rect.top : bottom ? y - rect.bottom : 0;
	}
	LayoutPress press;
	std::vector<Edit> edits;
	if (!layout_press(*frame.document, held, handle, frame.windows, *frame.compiler, *frame.state, press) ||
			!layout_press_edits(*frame.document, press, *frame.compiler, dx, dy,
					drag.snap != 0.0f ? kLayoutGrid : 0, drag.gesture ? drag.gesture : next_edit_gesture(), edits)) {
		error = "The drag leaves the window no area.";
		return false;
	}
	// The batch, then the gesture's end where there is one to end: this batch's, or the gesture the
	// drag went on with (its last drag may move nothing).
	const std::string &path = frame.document->path();
	const bool planned = !edits.empty();
	if (planned) out.request(request::edit_record(path, std::move(edits)));
	if (drag.end && (planned || drag.gesture)) out.request(request::end_edit(path));
	return true;
}

bool MenuViewport::command(const ViewportContext &context, const std::string &name,
		const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const {
	ArrangeOp op = ArrangeOp::AlignLeft;
	if (!arrange_op_from_token(name, op)) {
		error = "Unknown menu command \"" + name + "\" (the arrange ops: align_left, ... send_to_back).";
		return false;
	}
	const MenuCanvasFrame frame = canvas_frame(context);
	if (!frame.current) {
		error = "The viewport does not show the menu as it is now.";
		return false;
	}
	if (!frame.editable) {
		error = "The session takes no edit now (an operation holds the documents).";
		return false;
	}
	std::vector<NodeAddress> windows;
	for (const NodeId id : ids) {
		const NodeAddress window = shown_window(frame, id);
		if (!window.child) {
			error = "Record " + std::to_string(id) + " is no window of the screen the viewport shows.";
			return false;
		}
		windows.push_back(window);
	}
	if (windows.empty() || windows.size() < arrange_minimum(op)) {
		error = std::string(arrange_op_label(op)) + " takes " + std::to_string(arrange_minimum(op)) +
				" windows or more.";
		return false;
	}
	std::vector<Edit> edits;
	if (!arrange_edits(*frame.document, windows, windows.front(), op, *frame.compiler, *frame.state,
				edits, nullptr)) {
		error = std::string(arrange_op_label(op)) + " cannot arrange these windows.";
		return false;
	}
	if (!edits.empty()) out.request(request::edit_record(frame.document->path(), std::move(edits)));
	return true;
}

io::JsonValue MenuViewport::options_json() const {
	return menu_options_to_json(options_);
}

io::JsonValue MenuViewport::body_json(const ViewportInput &input) const {
	JsonValue body = JsonValue::make_object();
	const auto *document = dynamic_cast<const MnuDocument *>(input.document);
	if (const Node *row = document ? document->row(part_) : nullptr) {
		JsonValue screen = JsonValue::make_object();
		screen.set("id", json_number(double(row->id)));
		screen.set("name", json_string(row->name()));
		body.set("screen", std::move(screen));
	} else {
		body.set("screen", JsonValue::make_null());
	}
	JsonValue missing = JsonValue::make_array();
	for (const std::string &name : missing_) missing.push(json_string(name));
	body.set("missing", std::move(missing));
	JsonValue unreadable = JsonValue::make_array();
	for (const std::string &name : unreadable_) unreadable.push(json_string(name));
	body.set("unreadable", std::move(unreadable));
	return body;
}

io::JsonValue MenuViewport::items_json(const ViewportInput &input) const {
	const auto *document = dynamic_cast<const MnuDocument *>(input.document);
	const Node *screen = document ? document->row(part_) : nullptr;
	if (!screen || !current(input)) return JsonValue::make_array();
	JsonValue widgets = menu_widgets_to_json(*document, *screen, render_.compiler(), render_.state());
	// Where the device's own compile placed each (its report), beside the viewport's.
	for (size_t index = 0; index < widgets.array.size() && index < device_rects_.size(); ++index) {
		const ViewportDeviceReport::Rect &placed = device_rects_[index];
		if (!placed.placed) continue;
		JsonValue rect = JsonValue::make_array();
		for (const int edge : { placed.left, placed.top, placed.right, placed.bottom })
			rect.push(json_number(edge));
		widgets.array[index].set("device_rect", std::move(rect));
	}
	return widgets;
}

io::JsonValue MenuViewport::notes_json(const ViewportInput &input) const {
	const auto *document = dynamic_cast<const MnuDocument *>(input.document);
	const Node *screen = document ? document->row(part_) : nullptr;
	if (!screen || !current(input)) return JsonValue::make_array();
	return menu_notes_to_json(*document, *screen, render_.compiler(), notes_);
}

io::JsonValue MenuViewport::render_json(
		const ViewportInput &input, NodeId row, const JsonPage &page, std::string &error) const {
	JsonValue render = menu_render_to_json(input.view, path(), row, page);
	if (render.is_null()) error = "no menu '" + path() + "' in the project.";
	return render;
}

} // namespace opennova::editor
