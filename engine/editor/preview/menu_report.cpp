#include <editor/preview/menu_report.h>

#include <cstddef>
#include <map>
#include <memory>
#include <variant>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_preview_json.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/project/project_files.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

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
	       normalized_logical_name(basename_of(document.path())) == normalized_logical_name(path);
}

// The menu's project-relative path: an open document's, else the scan's entry for it.
std::string menu_path(const SessionView &view, const std::string &path) {
	std::string wanted = path;
	if (wanted.empty()) wanted = !view.documents.previews.menu.path.empty() ? view.documents.previews.menu.path : view.documents.active;
	if (wanted.empty()) return std::string();
	for (const auto &open : view.documents.open)
		if (open && is_menu_kind(open->kind()) && names_menu(*open, wanted)) return open->path();
	for (const AssetEntry &entry : view.project.scan->entries) {
		if (!is_menu_kind(entry.kind)) continue;
		if (entry.relative_path == wanted || normalized_logical_name(entry.logical_name) == normalized_logical_name(wanted))
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
	if (!view.findings.render_check) return nullptr;
	const MenuScreenRender *render = view.findings.render_check->render(document.path(), row);
	current = render && view.findings.render_check->document(document.path()) == &document &&
	          render->status() == MenuPreviewStatus::Ready && render->revision() == document.revision();
	return render;
}

const char *render_status(const MenuScreenRender *render) {
	return render ? menu_preview_status_token(render->status()) : "none";
}

// Where a finding comes from, by its code's family.
std::string finding_source(const std::string &code) {
	if (code.rfind("reference.", 0) == 0 || code.rfind("graph.", 0) == 0) return "graph";
	if (code.rfind("menu.render.", 0) == 0) return "render";
	const size_t dot = code.find('.');
	return dot == std::string::npos ? code : code.substr(0, dot);
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

const MnuDocument *menu_for(const SessionView &view, const std::string &path) {
	const std::string relative = menu_path(view, path);
	if (relative.empty()) return nullptr;
	if (const MnuDocument *open = open_menu(view, relative)) return open;
	return view.findings.render_check ? view.findings.render_check->document(relative) : nullptr;
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
		const std::string source = finding_source(d.code);
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
		const size_t notes = render && render->status() == MenuPreviewStatus::Ready ? render->notes().size() : 0;
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
	MenuPreviewSnapshot none;
	none.status = MenuPreviewStatus::NoScreen;
	JsonValue out;
	if (!view.findings.render_check) {
		none.status = MenuPreviewStatus::NoProject;
		out = menu_preview_to_json(none);
	} else {
		const MenuRenderCheck &check = *view.findings.render_check;
		// The open document when the menu is open (its current state), else the file as the check
		// read it.
		const MnuDocument *document = check.document(path);
		if (const MnuDocument *open = open_menu(view, path)) document = open;
		const Node *screen = document ? document->row(row) : nullptr;
		const MenuScreenRender *render = check.render(path, row);
		if (!document || !screen || !render) {
			none.document = document;
			out = menu_preview_to_json(none);
		} else {
			out = menu_preview_to_json(render_snapshot(*render, *document, *screen));
		}
	}
	// A page of the widgets and, by the same page, of the notes.
	const auto page_of = [&out, &page](const char *key) {
		JsonValue *list = out.get(key);
		if (!list || !list->is_array()) return size_t(0);
		const size_t total = list->array.size();
		std::vector<JsonValue> kept(list->array.begin() + std::ptrdiff_t(page.first(total)),
				list->array.begin() + std::ptrdiff_t(page.last(total)));
		list->array = std::move(kept);
		return total;
	};
	const size_t widgets = page_of("widgets");
	const size_t notes = page_of("notes");
	set_page(out, page, widgets);
	out.set("widget_count", json_number(double(widgets)));
	out.set("note_count", json_number(double(notes)));
	return out;
}

} // namespace opennova::editor
