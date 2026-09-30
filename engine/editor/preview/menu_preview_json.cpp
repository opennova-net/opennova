#include <editor/preview/menu_preview_json.h>

#include <cmath>
#include <cstdio>

#include <editor/documents/mnu_document.h>
#include <editor/model/edit.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using io::json_whole_in;

JsonValue edges(const mnu::RectEdges &rect) {
	JsonValue out = JsonValue::make_array();
	out.push(json_number(rect.left));
	out.push(json_number(rect.top));
	out.push(json_number(rect.right));
	out.push(json_number(rect.bottom));
	return out;
}

std::string argb(uint32_t color) {
	char text[9];
	std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(color));
	return text;
}

// True when the device shows the document as it is now.
bool current(const MenuPreviewSnapshot &snapshot) {
	return snapshot.status == MenuPreviewStatus::Ready && snapshot.document && snapshot.screen && snapshot.compiler &&
	       snapshot.state && snapshot.shown_revision == snapshot.document->revision();
}

// The window `id` of the screen the snapshot shows (none when it is not one of its windows).
NodeAddress shown_window(const MenuPreviewSnapshot &snapshot, NodeId id) {
	const NodeAddress window = snapshot.document->address_of(id);
	const bool shown = window.row == snapshot.screen->id && window.kind == node_kind(MenuKind::Window) && window.child;
	return shown ? window : NodeAddress();
}

} // namespace

MenuPreviewSnapshot menu_preview_snapshot(const SessionView &view, const MenuPreviewModel &model,
                                          const menu::MenuFrameCompiler *compiler, const menu::MenuFrameState *state) {
	MenuPreviewSnapshot snapshot;
	snapshot.status = compiler ? model.status() : MenuPreviewStatus::NoDevice;
	snapshot.detail = model.detail();
	snapshot.options = model.options();
	snapshot.missing = model.missing();
	snapshot.unreadable = model.unreadable();
	snapshot.shown_revision = model.shown_revision();
	for (const auto &open : view.documents)
		if (open && open->path() == view.menu_preview.path) snapshot.document = dynamic_cast<const MnuDocument *>(open.get());
	if (snapshot.document) snapshot.screen = snapshot.document->row(view.menu_preview.screen);
	if (snapshot.status == MenuPreviewStatus::Ready) {
		snapshot.compiler = compiler;
		snapshot.state = state;
	}
	return snapshot;
}

MenuPreviewSnapshot render_snapshot(const MenuScreenRender &render, const MnuDocument &document, const Node &screen) {
	MenuPreviewSnapshot snapshot;
	snapshot.status = render.status();
	snapshot.detail = render.detail();
	snapshot.document = &document;
	snapshot.screen = &screen;
	snapshot.shown_revision = render.revision();
	split_unloaded(render.assets(), snapshot.missing, snapshot.unreadable);
	if (snapshot.status == MenuPreviewStatus::Ready) {
		snapshot.compiler = &render.compiler();
		snapshot.state = &render.state();
	}
	return snapshot;
}

io::JsonValue menu_notes_to_json(const MnuDocument &document, const Node &screen,
                                 const menu::MenuFrameCompiler &compiler,
                                 const std::vector<menu::MenuFrameNote> &notes) {
	JsonValue out = JsonValue::make_array();
	for (const menu::MenuFrameNote &note : notes) {
		std::string field;
		const NodeAddress address = menu_note_address(note, document, screen, &field);
		DiagnosticSeverity severity = DiagnosticSeverity::Warning;
		const bool problem = menu_note_problem(note.code, &severity);
		JsonValue row = JsonValue::make_object();
		row.set("index", json_number(note.widget));
		row.set("window_id", json_number(note.widget >= 0 ? double(document.window_at(screen, size_t(note.widget))) : 0.0));
		row.set("id", json_number(double(address.child)));
		row.set("name", json_string(note.widget >= 0 ? compiler.widget_name(note.widget) : ""));
		row.set("code", json_string(menu::menu_frame_note_token(note.code)));
		row.set("finding", json_string(menu_note_code(note.code)));
		row.set("basis", json_string(menu::menu_frame_note_basis_token(menu::menu_frame_note_basis(note.code))));
		row.set("severity", json_string(problem ? diagnostic_severity_label(severity) : "preview"));
		row.set("subject", json_string(note.subject));
		row.set("list", json_string(note.list));
		row.set("record", json_number(note.record));
		row.set("field", json_string(field));
		row.set("message", json_string(menu_note_message(note)));
		out.push(row);
	}
	return out;
}

io::JsonValue menu_preview_to_json(const MenuPreviewSnapshot &snapshot) {
	JsonValue out = JsonValue::make_object();
	out.set("status", json_string(menu_preview_status_token(snapshot.status)));
	out.set("message", json_string(menu_preview_status_message(snapshot.status, snapshot.detail)));
	out.set("detail", json_string(snapshot.detail));
	out.set("path", json_string(snapshot.document ? snapshot.document->path() : std::string()));
	if (snapshot.screen) {
		JsonValue screen = JsonValue::make_object();
		screen.set("id", json_number(double(snapshot.screen->id)));
		screen.set("name", json_string(snapshot.screen->name()));
		out.set("screen", screen);
	}
	out.set("revision", json_number(double(snapshot.document ? snapshot.document->revision() : 0)));
	out.set("shown_revision", json_number(double(snapshot.shown_revision)));
	out.set("current", JsonValue::make_bool(current(snapshot)));
	JsonValue device = JsonValue::make_object();
	device.set("width", json_number(snapshot.options.width));
	device.set("height", json_number(snapshot.options.height));
	out.set("device", device);
	JsonValue options = JsonValue::make_object();
	options.set("show_hidden", JsonValue::make_bool(snapshot.options.show_hidden));
	options.set("force_id", json_number(double(snapshot.options.force_window)));
	options.set("force_state", json_string(menu_preview_state_token(snapshot.options.force_state)));
	options.set("checked", JsonValue::make_bool(snapshot.options.checked));
	options.set("popup_open", JsonValue::make_bool(snapshot.options.popup_open));
	options.set("focus", JsonValue::make_bool(snapshot.options.focused));
	out.set("options", options);
	JsonValue missing = JsonValue::make_array();
	for (const std::string &name : snapshot.missing) missing.push(json_string(name));
	out.set("missing", missing);
	JsonValue unreadable = JsonValue::make_array();
	for (const std::string &name : snapshot.unreadable) unreadable.push(json_string(name));
	out.set("unreadable", unreadable);
	JsonValue widgets = JsonValue::make_array();
	if (current(snapshot)) {
		const menu::MenuFrameCompiler &compiler = *snapshot.compiler;
		const menu::MenuFrameState &state = *snapshot.state;
		for (int index = 0; index < compiler.widget_count(); ++index) {
			JsonValue widget = JsonValue::make_object();
			widget.set("index", json_number(index));
			widget.set("id", json_number(double(snapshot.document->window_at(*snapshot.screen, size_t(index)))));
			widget.set("name", json_string(compiler.widget_name(index)));
			const int kind = compiler.widget_kind(index);
			widget.set("type", json_string(kind >= 0 ? mnu::window_type_name(static_cast<mnu::WindowType>(kind)) : ""));
			widget.set("shown", JsonValue::make_bool(compiler.widget_shown(index, state)));
			widget.set("disabled", JsonValue::make_bool(compiler.widget_disabled(index, state)));
			mnu::RectEdges rect{};
			if (compiler.widget_rect(index, state, &rect)) {
				widget.set("rect", edges(rect));
				mnu::RectEdges local = rect;
				mnu::RectEdges parent{};
				if (compiler.widget_rect(compiler.widget_parent(index), state, &parent)) {
					local.left -= parent.left;
					local.right -= parent.left;
					local.top -= parent.top;
					local.bottom -= parent.top;
				}
				widget.set("local", edges(local));
			}
			widget.set("text", json_string(compiler.widget_authored_text(index)));
			std::string font;
			uint32_t colors[4] = {};
			if (compiler.widget_font(index, &font, colors)) {
				widget.set("font", json_string(font));
				widget.set("text_color", json_string(argb(colors[menu::kStateDefault])));
			}
			widgets.push(widget);
		}
	}
	out.set("widgets", widgets);
	JsonValue notes = JsonValue::make_array();
	if (current(snapshot)) {
		std::vector<menu::MenuFrameNote> all = snapshot.compiler->build_notes();
		for (menu::MenuFrameNote &note : snapshot.compiler->layout_notes(*snapshot.state)) all.push_back(std::move(note));
		notes = menu_notes_to_json(*snapshot.document, *snapshot.screen, *snapshot.compiler, all);
	}
	out.set("notes", notes);
	return out;
}

io::JsonValue menu_preview_hit_to_json(const MenuPreviewSnapshot &snapshot, float x, float y) {
	JsonValue out = JsonValue::make_object();
	int index = -1;
	if (current(snapshot)) index = snapshot.compiler->hit_widget(*snapshot.state, x, y, 1.0f, 1.0f);
	out.set("index", json_number(index));
	out.set("id", json_number(index >= 0 ? double(snapshot.document->window_at(*snapshot.screen, size_t(index))) : 0.0));
	out.set("name", json_string(index >= 0 ? snapshot.compiler->widget_name(index) : ""));
	out.set("current", JsonValue::make_bool(current(snapshot)));
	return out;
}

bool menu_preview_options_from_json(const io::JsonValue &json, MenuPreviewOptions &options) {
	if (!json.is_object()) return false;
	MenuPreviewOptions out = options;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		int64_t number = 0;
		if (key == "width" || key == "height") {
			if (!json_whole_in(value, 1.0, 8192.0, number)) return false;
			(key == "width" ? out.width : out.height) = int(number);
		} else if (key == "force_id") {
			if (!json_whole_in(value, 0.0, 9007199254740992.0, number)) return false;
			out.force_window = NodeId(number);
		} else if (key == "force_state") {
			if (!value.is_string() || !menu_preview_state_from_token(value.string, out.force_state)) return false;
		} else if (key == "show_hidden" || key == "checked" || key == "popup_open" || key == "focus") {
			if (!value.is_bool()) return false;
			bool &flag = key == "show_hidden" ? out.show_hidden
			             : key == "checked"   ? out.checked
			             : key == "popup_open" ? out.popup_open
			                                   : out.focused;
			flag = value.boolean;
		} else {
			return false;
		}
	}
	options = out;
	return true;
}

bool menu_preview_drag(ProjectSession &session, const MenuPreviewSnapshot &snapshot, NodeId window, LayoutHandle handle,
                       int dx, int dy, bool snap) {
	if (!current(snapshot)) return false;
	const MnuDocument &document = *snapshot.document;
	const NodeAddress held = shown_window(snapshot, window);
	if (!held.child) return false;
	const SessionView &view = session.view();
	const std::vector<NodeAddress> selected =
	        view.active_document == document.path()
	                ? selected_windows(document, view.selection, view.selected, snapshot.screen->id)
	                : std::vector<NodeAddress>();
	LayoutPress press;
	std::vector<Edit> edits;
	if (!layout_press(document, held, handle, selected, *snapshot.compiler, *snapshot.state, press) ||
	    !layout_press_edits(document, press, *snapshot.compiler, dx, dy, snap ? kLayoutGrid : 0, next_edit_gesture(),
	                        edits))
		return false;
	if (edits.empty()) return true;
	const std::string path = document.path();
	session.handle(request::edit_record(path, std::move(edits)));
	const bool ok = session.outcome().done();
	session.handle(request::end_edit(path));
	return ok;
}

bool menu_preview_arrange(ProjectSession &session, const MenuPreviewSnapshot &snapshot,
                          const std::vector<NodeId> &windows, ArrangeOp op) {
	if (!current(snapshot) || windows.empty()) return false;
	const MnuDocument &document = *snapshot.document;
	std::vector<NodeAddress> shown;
	for (const NodeId id : windows) {
		const NodeAddress window = shown_window(snapshot, id);
		if (!window.child) return false;
		shown.push_back(window);
	}
	std::vector<Edit> edits;
	if (!arrange_edits(document, shown, shown.front(), op, *snapshot.compiler, *snapshot.state, edits, nullptr))
		return false;
	if (edits.empty()) return true;
	session.handle(request::edit_record(document.path(), std::move(edits)));
	return session.outcome().done();
}

} // namespace opennova::editor
