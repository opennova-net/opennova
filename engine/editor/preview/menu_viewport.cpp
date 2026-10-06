#include <editor/preview/menu_viewport.h>

#include <algorithm>
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
// A menu zoom's token: fit, scale, device; and back (false for another).
const char *menu_zoom_token(MenuZoom zoom) {
	return zoom == MenuZoom::Scale ? "scale" : zoom == MenuZoom::Device ? "device" : "fit";
}
bool menu_zoom_from_token(const std::string &token, MenuZoom &out) {
	if (token == "fit") out = MenuZoom::Fit;
	else if (token == "scale") out = MenuZoom::Scale;
	else if (token == "device") out = MenuZoom::Device;
	else return false;
	return true;
}

// The least and the most of the design a Scale zoom shows it at.
constexpr float kMenuScaleMin = 0.1f, kMenuScaleMax = 8.0f;

bool read_options(const JsonValue &json, MenuViewportOptions &held, MenuCanvasShow &show, MenuPointerShow &pointer,
		std::string &error) {
	if (!json.is_object()) {
		error = "\"options\" is an object.";
		return false;
	}
	MenuViewportOptions out = held;
	MenuCanvasShow shown = show;
	MenuPointerShow pointed = pointer;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		int64_t number = 0;
		if (key == "pointer") {
			if (!value.is_bool()) {
				error = "options.pointer is true or false.";
				return false;
			}
			pointed.shown = value.boolean;
		} else if (key == "pointer_at") {
			// A point of the picture in design units, where the mouse can be on the game's screen; null lets
			// go of it.
			if (value.is_null()) {
				pointed.held = false;
				continue;
			}
			const bool point = value.is_array() && value.array.size() == 2 && value.array[0].is_number() &&
					value.array[1].is_number() && value.array[0].number >= 0.0 &&
					value.array[0].number < double(menu::kMenuDesignWidth) && value.array[1].number >= 0.0 &&
					value.array[1].number < double(menu::kMenuDesignHeight);
			if (!point) {
				error = "options.pointer_at is a point of the picture [x, y] in design units (x from 0 to under 800, "
				        "y from 0 to under 600), or null to let it go.";
				return false;
			}
			pointed.held = true;
			pointed.x = float(value.array[0].number);
			pointed.y = float(value.array[1].number);
		} else if (key == "zoom") {
			if (!value.is_string() || !menu_zoom_from_token(value.string, shown.zoom)) {
				error = "options.zoom is fit, scale (with scale) or device.";
				return false;
			}
		} else if (key == "scale") {
			if (!value.is_number() || !(value.number >= kMenuScaleMin && value.number <= kMenuScaleMax)) {
				error = "options.scale is the design's scale, 0.1 to 8 (1 its own size).";
				return false;
			}
			shown.scale = float(value.number);
		} else if (key == "snap") {
			if (!value.is_bool()) {
				error = "options.snap is true or false.";
				return false;
			}
			shown.snap = value.boolean;
		} else if (key == "force_id") {
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
					"\" (it takes show_hidden, force_id, force_state, checked, popup_open, focus, zoom, scale, snap, "
					"pointer, pointer_at).";
			return false;
		}
	}
	held = out;
	show = shown;
	pointer = pointed;
	return true;
}

// The window `id` of the screen the frame shows (none when it is not one of its windows).
NodeAddress shown_window(const MenuCanvasFrame &frame, NodeId id) {
	const NodeAddress window = frame.document->address_of(id);
	const bool shown = window.row == frame.screen->id && window.kind == kWindowKind && window.child;
	return shown ? window : NodeAddress();
}

// Whether what changed in the menu (S13 V8) reaches the picture of the screen `row`. The compile reads
// more than the screen: the frame notes the first load of every texture across the whole menu before
// it compiles one screen, and that first load fixes the band every later user of the texture draws
// (an IMAGE row's HEIGHT, the cache keyed by the name and FLAGS alone: menu-re.md "The IMAGE pass",
// MenuFrameCompiler::note_texture_loads), so a screen before the shown one in the file feeds its
// picture. It reaches it: the file-wide state (the encoding the game reads the text in); the screens
// reordered or one removed (where it stood is gone with it); the shown screen's own row changed or a
// row before it added or changed. A screen added or changed after it does not: no first load there is
// one the shown screen's textures take.
bool feeds_screen(const ChangeSet &changes, const MnuDocument &document, NodeId row) {
	const auto *rows = std::get_if<RowChanges>(&changes);
	if (!rows || rows->file_state || rows->reordered || !rows->removed.empty()) return true;
	for (const auto &screen : document.rows()) {
		if (rows->was_changed(screen->id) || rows->was_added(screen->id)) return true;
		if (screen->id == row) return false;
	}
	return true; // the screen is not the menu's: its picture goes
}

// The text of the screens up to `last` (the one a picture shows) as the menu's writer writes them:
// every screen whose first loads the picture's compile took.
std::string screens_text(const mnu::Document &image, const mnu::Screen *last) {
	mnu::Document upto;
	for (const mnu::Screen &screen : image.screens) {
		upto.screens.push_back(screen);
		if (&screen == last) break;
	}
	return mnu::serialize(upto);
}

// Why the frame's picture is not the menu as it is now: its viewport shows none (`shown`'s reason).
std::string not_current(const MenuViewport &shown) {
	const std::string why = shown.message();
	return "The viewport shows no picture of the menu as it is now" + (why.empty() ? std::string(".") : ": " + why);
}

// What a drag of the window `id` by its handle `token` holds on the screen the frame shows: the
// handle, the window and its rect as the compile placed it. False, with why: a handle no window has,
// a picture that is not the menu as it is now, a record that is no window of the screen, one with no
// rect.
bool drag_window(const MenuViewport &shown, const MenuCanvasFrame &frame, NodeId id, const std::string &token,
		LayoutHandle &handle, NodeAddress &window, mnu::RectEdges &rect, std::string &error) {
	if (!layout_handle_from_token(token, handle)) {
		error = "Unknown handle \"" + token +
				"\" (move, left, right, top, bottom, top_left, top_right, bottom_left, bottom_right).";
		return false;
	}
	if (!frame.current) {
		error = not_current(shown);
		return false;
	}
	window = shown_window(frame, id);
	if (!window.child) {
		error = "Record " + std::to_string(id) + " is no window of the screen the viewport shows.";
		return false;
	}
	const int index = frame.document->window_index(window);
	if (index < 0 || !frame.compiler->widget_rect(index, *frame.state, &rect)) {
		error = "Record " + std::to_string(id) + " has no rect on the screen the viewport shows.";
		return false;
	}
	return true;
}

// The edges a handle moves: a move's every one (its point the top left corner), an edge's own, a
// corner's two.
struct HandleEdges {
	bool left = false, right = false, top = false, bottom = false;
};
HandleEdges edges_moved(LayoutHandle handle) {
	HandleEdges out;
	out.left = handle == LayoutHandle::Move || handle == LayoutHandle::Left || handle == LayoutHandle::TopLeft ||
			handle == LayoutHandle::BottomLeft;
	out.right = handle == LayoutHandle::Right || handle == LayoutHandle::TopRight || handle == LayoutHandle::BottomRight;
	out.top = handle == LayoutHandle::Move || handle == LayoutHandle::Top || handle == LayoutHandle::TopLeft ||
			handle == LayoutHandle::TopRight;
	out.bottom = handle == LayoutHandle::Bottom || handle == LayoutHandle::BottomLeft ||
			handle == LayoutHandle::BottomRight;
	return out;
}

} // namespace

io::JsonValue menu_options_to_json(
		const MenuViewportOptions &held, const MenuCanvasShow &show, const MenuPointerShow &pointer) {
	JsonValue options = JsonValue::make_object();
	options.set("show_hidden", JsonValue::make_bool(held.show_hidden));
	options.set("force_id", json_number(double(held.force_window)));
	options.set("force_state", json_string(menu_force_state_token(held.force_state)));
	options.set("checked", JsonValue::make_bool(held.checked));
	options.set("popup_open", JsonValue::make_bool(held.popup_open));
	options.set("focus", JsonValue::make_bool(held.focused));
	options.set("zoom", json_string(menu_zoom_token(show.zoom)));
	options.set("scale", json_number(show.scale));
	options.set("snap", JsonValue::make_bool(show.snap));
	options.set("pointer", JsonValue::make_bool(pointer.shown));
	if (pointer.held) {
		JsonValue at = JsonValue::make_array();
		at.push(json_number(pointer.x));
		at.push(json_number(pointer.y));
		options.set("pointer_at", std::move(at));
	} else {
		options.set("pointer_at", JsonValue::make_null());
	}
	return options;
}

namespace {

// The pointer the cursor pass draws for the claim `claim` (-1 none) over the state the picture holds (its
// windows held as the options say, never hovered by the mouse: a picture never pumps).
MenuPointer pointer_of_claim(const MenuCanvasFrame &frame, int claim, int spin_part) {
	MenuPointer out;
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	menu::MenuFrameState state = *frame.state;
	state.cursor_claim = claim;
	state.cursor_spin_part = spin_part;
	const menu::MenuFrameCompiler::FrameCursor cursor = compiler.frame_cursor(state);
	if (cursor.owner < 0) return out;
	out.drawn = true;
	out.owner = cursor.owner;
	out.width = cursor.width;
	out.height = cursor.height;
	const std::vector<std::string> &names = compiler.texture_names();
	if (cursor.texture >= 0 && size_t(cursor.texture) < names.size()) out.file = names[size_t(cursor.texture)];
	if (cursor.owner < compiler.widget_count()) {
		out.name = compiler.widget_name(cursor.owner);
		const NodeId id = frame.document->window_at(*frame.screen, size_t(cursor.owner));
		if (id) out.window = frame.document->address_of(id);
	}
	return out;
}

} // namespace

MenuPointer menu_pointer_at(const MenuCanvasFrame &frame, float x, float y) {
	if (!frame.current) return MenuPointer();
	// The claim the pump makes there (MenuFrameCompiler::claim_at), and the cursor it stamps.
	const menu::MenuFrameCompiler::MouseClaim claim = frame.compiler->claim_at(*frame.state, x, y, 1.0f, 1.0f);
	return pointer_of_claim(frame, claim.hovered, claim.spin_part);
}

MenuPointer menu_screen_pointer(const MenuCanvasFrame &frame) {
	if (!frame.current) return MenuPointer();
	return pointer_of_claim(frame, -1, 0);
}

io::JsonValue menu_pointer_to_json(const MenuPointer &pointer) {
	JsonValue out = JsonValue::make_object();
	out.set("drawn", JsonValue::make_bool(pointer.drawn));
	if (!pointer.drawn) return out;
	out.set("file", json_string(pointer.file));
	out.set("width", json_number(pointer.width));
	out.set("height", json_number(pointer.height));
	out.set("window", json_number(double(pointer.window.child)));
	out.set("name", json_string(pointer.name));
	return out;
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
	// Focused, its caret blinks on the preview clock (menu_frame_time).
	if (options.focused) row.focused = true;
}

uint32_t menu_frame_time(const PreviewClock &clock) {
	return clock.ms();
}

bool menu_frame_clock(const MenuViewport &menu, uint32_t frame_ms, const PreviewClock &clock,
		uint32_t &time) {
	const int index = menu.forced_index();
	const menu::MenuFrameCompiler &compiler = menu.render().compiler();
	if (!menu.options().focused || index < 0 || index >= compiler.widget_count()) return false;
	const int type = compiler.widget_kind(index);
	if (type != int(mnu::WindowType::Edit) && type != int(mnu::WindowType::MultilineEdit)) return false;
	time = menu_frame_time(clock);
	return menu::menu_caret_shown(time) != menu::menu_caret_shown(frame_ms);
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
	switch (picture_.follow(key, moves_(input, *document, row->id), files, generation)) {
	case PreviewFollow::Found::Same: return kept_(*document);
	case PreviewFollow::Found::Files:
		if (styles_alone_(files)) {
			picture_.restamp(files);
			return kept_(*document);
		}
		break;
	case PreviewFollow::Found::Anew: break;
	}
	picture_.show(key, generation);
	// The screen compiled headless from the menu the game would read were it saved now.
	style_vars_ = style_.vars(files);
	++configures_;
	screen_variables_made_ = false;
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

bool MenuViewport::moves_(const ViewportInput &input, const MnuDocument &document, NodeId row) const {
	switch (input.change) {
	case ChangeClass::None: return false;
	case ChangeClass::Unknown:
	case ChangeClass::Loaded: return true;
	case ChangeClass::Changed: break;
	}
	if (reason_ != MenuScreenStatus::Ready || !input.changes || feeds_screen(*input.changes, document, row))
		return true;
	// A change after it that leaves the menu without an image the game would read (unwritable, or not
	// read back) reaches every screen: the image a configure compiles, made once per state.
	return !document.saved_image();
}

bool MenuViewport::styles_alone_(const FileSource &files) {
	std::vector<menu::MenuDependency> sheets;
	style_.dependencies(sheets);
	std::vector<std::string> names;
	for (const menu::MenuDependency &sheet : sheets) names.push_back(sheet.name);
	if (picture_.files().moved_but(files, names) || !render_.image() || !render_.screen()) return false;
	const std::map<std::string, std::string> vars = style_.vars(files);
	if (!screen_variables_made_) {
		screen_variables_ = menu_variables_named(screens_text(*render_.image(), render_.screen()));
		screen_variables_made_ = true;
	}
	for (const std::string &name : changed_menu_variables(style_vars_, vars))
		if (std::binary_search(screen_variables_.begin(), screen_variables_.end(), name)) return false;
	style_vars_ = vars;
	return true;
}

ViewportAction MenuViewport::kept_(const MnuDocument &document) {
	if (reason_ == MenuScreenStatus::Ready) shown(document);
	return ViewportAction::Keep;
}

bool MenuViewport::takes_(const std::string &member) const {
	return member == "options";
}

bool MenuViewport::check_(const io::JsonValue &json, std::string &error) const {
	const JsonValue *options = json.get("options");
	MenuViewportOptions held = options_;
	MenuCanvasShow show = show_;
	MenuPointerShow pointer = pointer_;
	return !options || read_options(*options, held, show, pointer, error);
}

void MenuViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	const JsonValue *options = json.get("options");
	MenuViewportOptions held = options_;
	MenuCanvasShow show = show_;
	MenuPointerShow pointer = pointer_;
	std::string error;
	if (!options || !read_options(*options, held, show, pointer, error)) return;
	// The zoom and the snap change no picture, and the pointer only the cursor pass the device draws again
	// (DI-08): the screen is configured again for the held window alone.
	show_ = show;
	pointer_ = pointer;
	if (held == options_) return;
	options_ = held;
	++options_serial_;
}

bool MenuViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	device_rects_ = report.rects;
	return false;
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
	// The pointer the game draws with the mouse there (DI-08), over a window or not.
	out.pointer = menu_pointer_to_json(menu_pointer_at(frame, x, y));
	out.index = render_.compiler().hit_widget(render_.state(), x, y, 1.0f, 1.0f);
	if (out.index < 0) return out;
	out.id = frame.document->window_at(*frame.screen, size_t(out.index));
	out.name = render_.compiler().widget_name(out.index);
	const int type = render_.compiler().widget_kind(out.index);
	out.kind = type >= 0 ? mnu::window_type_name(static_cast<mnu::WindowType>(type)) : "";
	return out;
}

bool MenuViewport::click_frame(const ViewportContext &context, SelectMode, int &width, int &height,
		std::string &error) const {
	if (!canvas_frame(context).current) {
		const std::string why = message();
		error = "The viewport shows no picture of the menu as it is now" + (why.empty() ? std::string(".") : ": " + why);
		return false;
	}
	// The click's point in design pixels: the canvas reads the picture at the design size (its scale 1).
	width = menu::kMenuDesignWidth;
	height = menu::kMenuDesignHeight;
	return true;
}

std::vector<ViewportHit> MenuViewport::box(const ViewportContext &context, float x0, float y0, float x1,
		float y1) const {
	std::vector<ViewportHit> out;
	const MenuCanvasFrame frame = canvas_frame(context);
	if (!frame.current) return out;
	const std::vector<NodeAddress> touched =
			menu_marquee_windows(frame, CanvasPoint{ x0, y0 }, CanvasPoint{ x1, y1 });
	const menu::MenuFrameCompiler &compiler = render_.compiler();
	// Each as hit names one: its place in the compiled screen, its window, its name and type.
	for (int index = 0; index < compiler.widget_count() && out.size() < touched.size(); ++index) {
		const NodeId id = frame.document->window_at(*frame.screen, size_t(index));
		bool taken = false;
		for (const NodeAddress &window : touched) taken = taken || (id && window.child == id);
		if (!taken) continue;
		ViewportHit hit;
		hit.current = true;
		hit.index = index;
		hit.id = id;
		hit.name = compiler.widget_name(index);
		const int type = compiler.widget_kind(index);
		hit.kind = type >= 0 ? mnu::window_type_name(static_cast<mnu::WindowType>(type)) : "";
		out.push_back(std::move(hit));
	}
	return out;
}

bool MenuViewport::handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x,
		float &y, std::string &error) const {
	const MenuCanvasFrame frame = canvas_frame(context);
	LayoutHandle held = LayoutHandle::Move;
	NodeAddress window;
	mnu::RectEdges rect{};
	if (!drag_window(*this, frame, id, handle, held, window, rect, error)) return false;
	const HandleEdges moved = edges_moved(held);
	x = float(moved.left ? rect.left : moved.right ? rect.right : (rect.left + rect.right) / 2);
	y = float(moved.top ? rect.top : moved.bottom ? rect.bottom : (rect.top + rect.bottom) / 2);
	return true;
}

bool MenuViewport::drag(const ViewportContext &context, const ViewportDrag &drag,
		CanvasRequests &out, std::string &error) const {
	const MenuCanvasFrame frame = canvas_frame(context);
	LayoutHandle handle = LayoutHandle::Move;
	NodeAddress held;
	mnu::RectEdges rect{};
	if (!drag_window(*this, frame, drag.id, drag.handle, handle, held, rect, error)) return false;
	if (!frame.editable) {
		error = context.not_editable();
		return false;
	}
	const std::string &path = frame.document->path();
	// A step that moves nothing plans no batch; the gesture its sample names ends with it all the same.
	if (drag.by && drag.x == 0.0f && drag.y == 0.0f) {
		if (drag.end && drag.gesture) out.request(request::end_edit(path));
		return true;
	}
	// By design units from where the picture shows the handle, rounded to the nearest; to a design
	// point, as far as the handle is from it there (a move's handle the window's top left corner, an
	// edge's its edge, a corner's its corner).
	int dx = int(std::lround(drag.x)), dy = int(std::lround(drag.y));
	if (!drag.by) {
		const HandleEdges moved = edges_moved(handle);
		const int x = int(std::lround(drag.x)), y = int(std::lround(drag.y));
		dx = moved.left ? x - rect.left : moved.right ? x - rect.right : 0;
		dy = moved.top ? y - rect.top : moved.bottom ? y - rect.bottom : 0;
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
		error = not_current(*this);
		return false;
	}
	if (!frame.editable) {
		error = context.not_editable();
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
	return menu_options_to_json(options_, show_, pointer_);
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
