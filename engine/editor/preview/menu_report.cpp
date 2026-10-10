#include <editor/preview/menu_report.h>

#include <cstddef>
#include <map>
#include <memory>
#include <variant>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_document.h>
#include <editor/model/diagnostic.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/project/project_files.h>
#include <editor/session/finding_codes.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/mnu/mnu_layout.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

JsonValue edges(const mnu::RectEdges &rect) {
	JsonValue out = JsonValue::make_array();
	out.push(json_number(rect.left));
	out.push(json_number(rect.top));
	out.push(json_number(rect.right));
	out.push(json_number(rect.bottom));
	return out;
}

bool names_menu(const DocumentBase &document, const std::string &path) {
	return document.path() == path ||
	       pff::normalized_logical_name(basename_of(document.path())) == pff::normalized_logical_name(path);
}

// The menu's project-relative path: an open document's, else the scan's entry for it. A pathless
// read is of the active document, as a pathless request's edits are (S13 A5): none when it is no
// menu, never the previewed menu instead (the identities a read gives are those a pathless edit
// names).
std::string menu_path(const SessionView &view, const std::string &path) {
	const std::string wanted = path.empty() ? view.documents.active : path;
	if (wanted.empty()) return std::string();
	for (const auto &open : view.documents.open)
		if (open && is_menu_kind(open->kind()) && names_menu(*open, wanted)) return open->path();
	for (const AssetEntry &entry : view.project.scan->entries) {
		if (!is_menu_kind(entry.kind)) continue;
		if (entry.relative_path == wanted || pff::normalized_logical_name(entry.logical_name) == pff::normalized_logical_name(wanted))
			return entry.relative_path;
	}
	return std::string();
}

const MnuDocument *open_menu(const SessionView &view, const std::string &relative) {
	for (const auto &open : view.documents.open)
		if (open && open->path() == relative) return dynamic_cast<const MnuDocument *>(open.get());
	return nullptr;
}

// The render check's render of a screen, and whether it shows the document as it is now.
const MenuScreenRender *render_of(const SessionView &view, const MnuDocument &document, NodeId row, bool &current) {
	current = false;
	const MenuRenderCheck *check = menu_render_check(view.findings.project_checks.get());
	if (!check) return nullptr;
	const MenuScreenRender *render = check->render(document.path(), row);
	current = render && check->document(document.path()) == &document &&
	          render->status() == MenuScreenStatus::Ready && render->revision() == document.revision();
	return render;
}

const char *render_status(const MenuScreenRender *render) {
	return render ? menu_screen_status_token(render->status()) : "none";
}

// Where a finding comes from: its row's source (the asset graph's, the render check's, else its
// group's key); "" for a Diagnostic no finding was made into.
std::string finding_source(const Diagnostic &d) {
	return d.row() ? finding_source_token(*d.row()) : std::string();
}

JsonValue window_to_json(const MnuDocument &document, const NodeAddress &window, const Document::Placement &at,
                         const MenuScreenRender *render, bool current) {
	JsonValue out = JsonValue::make_object();
	out.set("id", json_number(double(window.child)));
	out.set("name", json_string(document.record_name(window)));
	Value type;
	out.set("type", json_string(document.get(window, "type", type) && std::holds_alternative<std::string>(type)
	                            ? std::get<std::string>(type)
	                            : std::string()));
	out.set("parent", json_number(double(at.owner.child)));
	out.set("depth", json_number(double(document.ancestors(window).size() - 1)));
	const int index = document.window_index(window);
	out.set("index", json_number(index));
	Value text;
	if (document.present(window, "string.value") && document.get(window, "string.value", text) &&
	    std::holds_alternative<std::string>(text))
		out.set("text", json_string(std::get<std::string>(text)));
	JsonValue lists = JsonValue::make_object();
	for (const Document::Collection &collection : document.collections_of(window))
		if (!collection.ids.empty() && collection.spec.kind != node_kind(MenuKind::Window))
			lists.set(document.kind_token(collection.spec.kind),
			          json_number(double(collection.ids.size())));
	out.set("lists", std::move(lists));
	if (current && index >= 0 && render) {
		const menu::MenuFrameCompiler &compiler = render->compiler();
		const menu::MenuFrameState &state = render->state();
		mnu::RectEdges rect{};
		if (index < compiler.widget_count() && compiler.widget_rect(index, state, &rect)) {
			out.set("rect", edges(rect));
			mnu::RectEdges local = rect, parent{};
			if (compiler.widget_rect(compiler.widget_parent(index), state, &parent)) {
				local.left -= parent.left;
				local.right -= parent.left;
				local.top -= parent.top;
				local.bottom -= parent.top;
			}
			out.set("local", edges(local));
			out.set("shown", JsonValue::make_bool(compiler.widget_shown(index, state)));
		}
	}
	return out;
}

} // namespace

io::JsonValue menu_widgets_to_json(const MnuDocument &document, const Node &screen,
		const menu::MenuFrameCompiler &compiler, const menu::MenuFrameState &state) {
	JsonValue widgets = JsonValue::make_array();
	for (int index = 0; index < compiler.widget_count(); ++index) {
		JsonValue widget = JsonValue::make_object();
		widget.set("index", json_number(index));
		widget.set("id", json_number(double(document.window_at(screen, size_t(index)))));
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
			widget.set("text_color", json_string(mnu::color_text(colors[menu::kStateDefault])));
		}
		widgets.push(std::move(widget));
	}
	return widgets;
}

io::JsonValue menu_notes_to_json(const MnuDocument &document, const Node &screen,
		const menu::MenuFrameCompiler &compiler, const std::vector<menu::MenuFrameNote> &notes) {
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
		row.set("finding", json_string(finding_code(note.code).token));
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

const MnuDocument *menu_for(const SessionView &view, const std::string &path) {
	const std::string relative = menu_path(view, path);
	if (relative.empty()) return nullptr;
	if (const MnuDocument *open = open_menu(view, relative)) return open;
	const MenuRenderCheck *check = menu_render_check(view.findings.project_checks.get());
	return check ? check->document(relative) : nullptr;
}

io::JsonValue menu_tree_to_json(const SessionView &view, const std::string &path) {
	const MnuDocument *document = menu_for(view, path);
	if (!document) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(document->path()));
	out.set("open", JsonValue::make_bool(open_menu(view, document->path()) == document));
	out.set("dirty", JsonValue::make_bool(document->dirty()));
	out.set("revision", json_number(double(document->revision())));
	JsonValue screens = JsonValue::make_array();
	for (size_t row_index = 0; row_index < document->rows().size(); ++row_index) {
		const Node &row = *document->rows()[row_index];
		bool current = false;
		const MenuScreenRender *render = render_of(view, *document, row.id, current);
		JsonValue screen = JsonValue::make_object();
		screen.set("id", json_number(double(row.id)));
		screen.set("name", json_string(row.name()));
		screen.set("index", json_number(double(row_index)));
		screen.set("status", json_string(render_status(render)));
		screen.set("current", JsonValue::make_bool(current));
		JsonValue windows = JsonValue::make_array();
		document->walk_records(row, [&](const NodeAddress &record, const Document::Placement &at) {
			if (record.kind == node_kind(MenuKind::Window) && document->window_index(record) >= 0)
				windows.push(window_to_json(*document, record, at, render, current));
			return true;
		});
		screen.set("window_count", json_number(double(windows.array.size())));
		screen.set("windows", std::move(windows));
		screens.push(std::move(screen));
	}
	out.set("screens", std::move(screens));
	return out;
}

io::JsonValue menu_findings_to_json(const SessionView &view, const std::string &path) {
	const MnuDocument *document = menu_for(view, path);
	if (!document) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(document->path()));
	std::map<std::string, size_t> counts{{"error", 0}, {"warning", 0}, {"info", 0}}, sources;
	JsonValue problems = JsonValue::make_array();
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.asset != document->path()) continue;
		const std::string source = finding_source(d);
		++counts[diagnostic_severity_label(d.severity)];
		++sources[source];
		JsonValue row = diagnostic_to_json(d);
		row.set("source", json_string(source));
		problems.push(std::move(row));
	}
	out.set("count", json_number(double(problems.array.size())));
	JsonValue severity = JsonValue::make_object();
	for (const auto &entry : counts) severity.set(entry.first, json_number(double(entry.second)));
	out.set("counts", std::move(severity));
	JsonValue by_source = JsonValue::make_object();
	for (const auto &entry : sources) by_source.set(entry.first, json_number(double(entry.second)));
	out.set("sources", std::move(by_source));
	JsonValue screens = JsonValue::make_array();
	for (const auto &row : document->rows()) {
		bool current = false;
		const MenuScreenRender *render = render_of(view, *document, row->id, current);
		JsonValue screen = JsonValue::make_object();
		screen.set("id", json_number(double(row->id)));
		screen.set("name", json_string(row->name()));
		screen.set("status", json_string(render_status(render)));
		screen.set("current", JsonValue::make_bool(current));
		const size_t notes = render && render->status() == MenuScreenStatus::Ready ? render->notes().size() : 0;
		size_t rows = 0;
		for (const Diagnostic &d : view.findings.diagnostics) rows += d.asset == document->path() && d.row_id == row->id ? 1 : 0;
		screen.set("notes", json_number(double(notes)));
		screen.set("problems", json_number(double(rows)));
		screens.push(std::move(screen));
	}
	out.set("screens", std::move(screens));
	out.set("problems", std::move(problems));
	return out;
}

io::JsonValue menu_render_to_json(
		const SessionView &view, const std::string &path, NodeId row, const JsonPage &page) {
	const MenuRenderCheck *render_check = menu_render_check(view.findings.project_checks.get());
	const MnuDocument *document = nullptr;
	const Node *screen = nullptr;
	const MenuScreenRender *render = nullptr;
	if (render_check) {
		const std::string relative = menu_path(view, path);
		if (relative.empty()) return JsonValue::make_null();
		// The open document when the menu is open (its current state), else the file as the check
		// read it.
		document = render_check->document(relative);
		if (const MnuDocument *open = open_menu(view, relative)) document = open;
		screen = document ? document->row(row) : nullptr;
		render = screen ? render_check->render(relative, row) : nullptr;
	}
	const MenuScreenStatus status = !render_check ? MenuScreenStatus::NoProject
			: !render                             ? MenuScreenStatus::NoScreen
												  : render->status();
	const std::string detail = render ? render->detail() : std::string();
	JsonValue out = JsonValue::make_object();
	out.set("status", json_string(menu_screen_status_token(status)));
	out.set("message", json_string(menu_screen_status_message(status, detail)));
	out.set("detail", json_string(detail));
	out.set("path", json_string(document ? document->path() : std::string()));
	if (screen && render) {
		JsonValue named = JsonValue::make_object();
		named.set("id", json_number(double(screen->id)));
		named.set("name", json_string(screen->name()));
		out.set("screen", std::move(named));
	}
	out.set("revision", json_number(double(document ? document->revision() : 0)));
	out.set("shown_revision", json_number(double(render ? render->revision() : 0)));
	const bool current = render && status == MenuScreenStatus::Ready && document &&
			render->revision() == document->revision();
	out.set("current", JsonValue::make_bool(current));
	std::vector<std::string> missing, unreadable;
	if (render) menu::split_unloaded(render->assets(), missing, unreadable);
	JsonValue lacked = JsonValue::make_array();
	for (const std::string &name : missing) lacked.push(json_string(name));
	out.set("missing", std::move(lacked));
	JsonValue failed = JsonValue::make_array();
	for (const std::string &name : unreadable) failed.push(json_string(name));
	out.set("unreadable", std::move(failed));
	JsonValue widgets = JsonValue::make_array(), notes = JsonValue::make_array();
	if (current) {
		widgets = menu_widgets_to_json(*document, *screen, render->compiler(), render->state());
		notes = menu_notes_to_json(*document, *screen, render->compiler(), render->notes());
	}
	// A page of the widgets and, by the same page, of the notes.
	const auto page_of = [&page](JsonValue &list) {
		const size_t total = list.array.size();
		std::vector<JsonValue> kept(list.array.begin() + std::ptrdiff_t(page.first(total)),
				list.array.begin() + std::ptrdiff_t(page.last(total)));
		list.array = std::move(kept);
		return total;
	};
	const size_t widget_total = page_of(widgets);
	const size_t note_total = page_of(notes);
	out.set("widgets", std::move(widgets));
	out.set("notes", std::move(notes));
	// `count` the widgets'; the page runs on while either list has entries past it.
	set_page(out, page, widget_total, note_total);
	out.set("widget_count", json_number(double(widget_total)));
	out.set("note_count", json_number(double(note_total)));
	return out;
}

} // namespace opennova::editor
