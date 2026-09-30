#include <editor/session/editor_queries.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <string>
#include <utility>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document_search.h>
#include <editor/preview/menu_report.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/document_set.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/view_json.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

using K = EditorQueryKind;
using J = QueryJson;

// --- the params ----------------------------------------------------------------------------------

constexpr const char *kOffsetDoc =
		"The page's first entry, by its place in the list (0 the first).";
constexpr const char *kLimitDoc = "How many entries the page holds at most, 1 to 200.";
constexpr const char *kCursorDoc =
		"The page's first entry by its absolute index (the last page's next_cursor; 0 the oldest "
		"held). One the list dropped before it was read starts the page at the oldest held: the "
		"answer's cursor comes back larger than asked.";
constexpr const char *kDocumentDoc =
		"An open document by its project-relative path or its logical name; left out, the active "
		"one.";
constexpr const char *kMenuDoc =
		"A menu by its project-relative path or its logical name; left out, the active document, "
		"which must be a menu (as a pathless request's edits name records of the active "
		"document). A closed menu answers as the last validation read it.";

constexpr QueryParam kStateParams[] = {
	{ "sections", J::Strings, false, nullptr,
			"The sections to answer, by name (the catalog's sections); every one when left out." },
	{ "since", J::Integer, false, nullptr,
			"A view_revision an earlier answer carried: the sections none of whose concerns moved "
			"since are left out; 0 or left out, every section. One past the view's clock is "
			"refused." },
};

constexpr QueryParam kPageParams[] = {
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kDocumentParams[] = {
	{ "path", J::String, false, nullptr, kDocumentDoc },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kRecordParams[] = {
	{ "path", J::String, false, nullptr, kDocumentDoc },
	{ "id", J::Integer, false, nullptr,
			"The record by its identity (a row's or a nested record's), as the document query "
			"lists it." },
	{ "symbol", J::String, false, nullptr,
			"Or the record defining a name other records use (a screen, a window, a string key, a "
			"weapon), the one the game's lookup reaches." },
	{ "scope", J::String, false, nullptr,
			"Where the symbol is looked up: \"GAMETEXT.BIN/WepDes\" a key of that section, "
			"\"MAIN.MNU/STARTUP\" a window of that screen; left out, anywhere." },
};

constexpr QueryParam kFieldParams[] = {
	{ "path", J::String, false, nullptr, kDocumentDoc },
	{ "id", J::Integer, true, nullptr, "The record by its identity." },
	{ "field", J::String, true, nullptr, "The reference field, by its id." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kDocumentSearchParams[] = {
	{ "path", J::String, false, nullptr, kDocumentDoc },
	{ "text", J::String, true, nullptr,
			"What to find in every field's value as the Inspector shows it." },
	{ "match_case", J::Boolean, false, "false",
			"ASCII letters compared with their case (without it, when left out)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kProblemsParams[] = {
	{ "severities", J::Strings, false, nullptr,
			"The levels shown, of error, warning and info; every level when left out." },
	{ "text", J::String, false, nullptr,
			"Matched without case against the message, file, record, field and code." },
	{ "scope", J::String, false, "project", "Whose findings: project, active_file or open_files." },
	{ "fixable", J::Boolean, false, "false", "Only the findings with a fix." },
	{ "group", J::String, false, "none",
			"How the rows are grouped: none, file or kind (the code's family)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kReferencesParams[] = {
	{ "path", J::String, true, nullptr,
			"A project file: the file at that project-relative path, else, for a name with no "
			"folder, the file the name resolves to." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kReferrersParams[] = {
	{ "path", J::String, false, nullptr, "A project file, as references takes it." },
	{ "kind", J::String, false, nullptr,
			"Or a symbol's reference kind token (weapon, text_id, style_var, menu_screen, "
			"menu_window, user_point, ...) with its name." },
	{ "name", J::String, false, nullptr,
			"The symbol's name, as the symbols query gives it (a style variable's NAME, not "
			"%NAME%)." },
	{ "scope", J::String, false, nullptr, "The scope the symbol is defined in; left out, any." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kSymbolsParams[] = {
	{ "kind", J::String, false, nullptr,
			"Only the symbols of this reference kind token; every kind when left out." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kProjectSearchParams[] = {
	{ "text", J::String, true, nullptr,
			"What to find, without case, in the files' names and the symbols' names." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kMenuTreeParams[] = {
	{ "path", J::String, false, nullptr, kMenuDoc },
	{ "screen", J::Integer, false, nullptr, "One screen, by its row identity." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kMenuFindingsParams[] = {
	{ "path", J::String, false, nullptr, kMenuDoc },
	{ "severity", J::String, false, nullptr,
			"Only the rows of this level: error, warning or info." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kMenuRenderParams[] = {
	{ "path", J::String, false, nullptr, kMenuDoc },
	{ "screen", J::Integer, true, nullptr, "The screen, by its row identity." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kCursorParams[] = {
	{ "cursor", J::Integer, false, "0", kCursorDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

// --- helpers -------------------------------------------------------------------------------------

JsonPage page_of(const QueryArgs &args) {
	return JsonPage{ args.offset(), args.limit() };
}

std::string no_document(const std::string &path) {
	return path.empty() ? std::string("no document is open.") : "no open document " + path + ".";
}

// The open document `args` names ("" the active one), of any kind (S13 D6), or null with `error`.
const DocumentBase *open_document_of(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string path = args.text("path");
	const DocumentBase *document = context.core.documents().document_for(path);
	if (!document)
		error = no_document(path);
	return document;
}

// The open record document `args` names, or null with `error`: none is open there, or the one
// open is of another kind, which holds no records (S13 D6).
const Document *document_of(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	const DocumentBase *open = open_document_of(context, args, error);
	const Document *document = open ? records_of(*open) : nullptr;
	if (open && !document)
		error = open->path() + " holds no records (document.no_records).";
	return document;
}

// A record of `document` by the identity `args` names, or false with `error`.
bool record_of(
		const Document &document, const QueryArgs &args, NodeAddress &out, std::string &error) {
	const int64_t id = args.integer("id");
	out = document.address_of(NodeId(id));
	if (out.row)
		return true;
	error = "no record " + std::to_string(id) + " in " + document.path() + ".";
	return false;
}

NodeId identity_of(const NodeAddress &address) {
	return address.child ? address.child : address.row;
}

// A page of edges.
JsonValue edges_page(const AssetGraph *graph, const std::vector<const GraphEdge *> &edges,
		const JsonPage &page, size_t total) {
	JsonValue out = JsonValue::make_object();
	set_page(out, page, total);
	JsonValue list = JsonValue::make_array();
	if (graph)
		for (size_t i = page.first(edges.size()); i < page.last(edges.size()); ++i)
			list.push(graph_edge_to_json(*graph, *edges[i]));
	out.set("edges", std::move(list));
	return out;
}

// A page of a list the menu report wrote whole, under `key` (the report's own count kept apart).
void page_list(JsonValue &answer, const char *key, const JsonPage &page) {
	JsonValue *list = answer.get(key);
	if (!list || !list->is_array())
		return;
	const size_t total = list->array.size();
	std::vector<JsonValue> kept(list->array.begin() + std::ptrdiff_t(page.first(total)),
			list->array.begin() + std::ptrdiff_t(page.last(total)));
	list->array = std::move(kept);
	set_page(answer, page, total);
}

// --- the handlers --------------------------------------------------------------------------------

JsonValue answer_state(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	std::vector<ViewSection> sections;
	if (args.has("sections")) {
		for (const std::string &name : args.strings("sections")) {
			ViewSection section = ViewSection::Status;
			if (!view_section_from_token(name, section)) {
				std::string names;
				for (size_t i = 0; i < kViewSectionCount; ++i)
					names += (i ? ", " : "") +
							std::string(view_section_row(static_cast<ViewSection>(i)).token);
				error = "no section \"" + name + "\" (" + names + ").";
				return JsonValue::make_null();
			}
			sections.push_back(section);
		}
	} else {
		for (size_t i = 0; i < kViewSectionCount; ++i)
			sections.push_back(static_cast<ViewSection>(i));
	}
	// `since` a view_revision an earlier answer carried (0: every section); the view's clock never
	// moves back, so one past it names no answer the session gave.
	const uint64_t since = uint64_t(args.integer("since"));
	const uint64_t clock = view.revisions.any();
	if (since > clock) {
		error = "\"since\" is " + std::to_string(since) + ", past the view's clock, " +
				std::to_string(clock) + ".";
		return JsonValue::make_null();
	}
	JsonValue out = JsonValue::make_object();
	// Each concern's stamp: the clock value at which it last moved (0: never).
	JsonValue revisions = JsonValue::make_object();
	for (size_t i = 0; i < kViewConcernCount; ++i) {
		const ViewConcern concern = static_cast<ViewConcern>(i);
		revisions.set(
				view_concern_token(concern), json_number(double(view.revisions.stamp(concern))));
	}
	out.set("revisions", std::move(revisions));
	for (const ViewSection section : sections)
		if (since == 0 || view_section_moved(view, section, since))
			out.set(view_section_row(section).token, view_section_to_json(view, section));
	return out;
}

JsonValue answer_files(const QueryContext &context, const QueryArgs &args, std::string &) {
	const SessionView &view = context.core.view();
	const std::vector<AssetEntry> &entries = view.project.scan->entries;
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	set_page(out, page, entries.size());
	JsonValue files = JsonValue::make_array();
	for (size_t i = page.first(entries.size()); i < page.last(entries.size()); ++i) {
		const AssetEntry &entry = entries[i];
		JsonValue file = JsonValue::make_object();
		file.set("path", json_string(entry.relative_path));
		file.set("name", json_string(entry.logical_name));
		file.set("kind", json_string(asset_kind_token(entry.kind)));
		file.set("editable", JsonValue::make_bool(is_editable_kind(entry.kind)));
		files.push(std::move(file));
	}
	out.set("files", std::move(files));
	return out;
}

JsonValue answer_documents(const QueryContext &context, const QueryArgs &args, std::string &) {
	const SessionView &view = context.core.view();
	std::vector<const DocumentBase *> open;
	for (const auto &document : view.documents.open)
		if (document)
			open.push_back(document.get());
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("active", json_string(view.documents.active));
	set_page(out, page, open.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(open.size()); i < page.last(open.size()); ++i)
		list.push(document_to_json(*open[i]));
	out.set("documents", std::move(list));
	return out;
}

JsonValue answer_document(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const DocumentBase *document = open_document_of(context, args, error);
	if (!document)
		return JsonValue::make_null();
	const JsonPage page = page_of(args);
	return document_to_json(*document, &page);
}

JsonValue answer_record(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const Document *document = document_of(context, args, error);
	if (!document)
		return JsonValue::make_null();
	if (args.has("id") == args.has("symbol")) {
		error = "it names its record by \"id\" or by \"symbol\", one of them.";
		return JsonValue::make_null();
	}
	NodeAddress address;
	if (args.has("id")) {
		if (!record_of(*document, args, address, error))
			return JsonValue::make_null();
	} else {
		const std::string symbol = args.text("symbol"), scope = args.text("scope");
		const AssetGraph *graph = view.findings.graph.get();
		if (!graph || !find_definition(*graph, *document, symbol, address, scope)) {
			error = "no record named '" + symbol + "'" + (scope.empty() ? "" : " in " + scope) +
					" in " + document->path() + ".";
			return JsonValue::make_null();
		}
	}
	JsonValue out = record_to_json(*document, address, view);
	if (out.is_null()) {
		error = "no record " + std::to_string(identity_of(address)) + " in " + document->path() +
				".";
		return out;
	}
	out.set("id", json_number(double(identity_of(address))));
	out.set("document", json_string(document->path()));
	return out;
}

// reference_choices and reference_targets.
JsonValue answer_reference(
		const QueryContext &context, const QueryArgs &args, std::string &error, bool choices) {
	const SessionView &view = context.core.view();
	const Document *document = document_of(context, args, error);
	NodeAddress address;
	if (!document || !record_of(*document, args, address, error))
		return JsonValue::make_null();
	const std::string field = args.text("field");
	JsonValue out = choices
			? reference_choices_to_json(*document, address, field, view, page_of(args))
			: reference_targets_to_json(*document, address, field, view, page_of(args));
	if (out.is_null())
		error = "record " + std::to_string(identity_of(address)) + " of " + document->path() +
				" has no field '" + field + "'.";
	return out;
}

JsonValue answer_reference_choices(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	return answer_reference(context, args, error, true);
}

JsonValue answer_reference_targets(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	return answer_reference(context, args, error, false);
}

JsonValue answer_document_search(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	const Document *document = document_of(context, args, error);
	if (!document)
		return JsonValue::make_null();
	const std::string text = args.text("text");
	if (text.empty()) {
		error = "the text to find is empty.";
		return JsonValue::make_null();
	}
	SearchOptions options;
	options.match_case = args.boolean("match_case");
	JsonValue out =
			document_hits_to_json(find_in_document(*document, text, options), page_of(args));
	out.set("document", json_string(document->path()));
	return out;
}

JsonValue answer_problems(const QueryContext &context, const QueryArgs &args, std::string &error) {
	ProblemQuery query;
	if (args.has("severities")) {
		query.errors = query.warnings = query.infos = false;
		for (const std::string &token : args.strings("severities")) {
			DiagnosticSeverity severity = DiagnosticSeverity::Error;
			if (!diagnostic_severity_from_token(token, severity)) {
				error = "\"severities\" takes error, warning and info, not \"" + token + "\".";
				return JsonValue::make_null();
			}
			(severity == DiagnosticSeverity::Error					  ? query.errors
							: severity == DiagnosticSeverity::Warning ? query.warnings
																	  : query.infos) = true;
		}
	}
	query.text = args.text("text");
	query.fixable = args.boolean("fixable");
	const std::string scope = args.text("scope"), group = args.text("group");
	if (!problem_scope_from_token(scope, query.scope)) {
		error = "no scope \"" + scope + "\" (project, active_file, open_files).";
		return JsonValue::make_null();
	}
	if (!problem_grouping_from_token(group, query.grouping)) {
		error = "no grouping \"" + group + "\" (none, file, kind).";
		return JsonValue::make_null();
	}
	ProblemsService &problems = context.core.problems();
	return problems_to_json(
			context.core.view(), problems.answer(query), page_of(args), problems.fixes());
}

JsonValue answer_references(const QueryContext &context, const QueryArgs &args, std::string &) {
	const AssetGraph *graph = context.core.view().findings.graph.get();
	const std::string path = args.text("path");
	const std::vector<const GraphEdge *> edges =
			graph ? graph->references_of(path) : std::vector<const GraphEdge *>();
	JsonValue out = edges_page(graph, edges, page_of(args), edges.size());
	out.set("path", json_string(path));
	return out;
}

// referrers and usages: of a file, or of a symbol by its kind and name.
JsonValue answer_users(
		const QueryContext &context, const QueryArgs &args, std::string &error, bool usages) {
	const AssetGraph *graph = context.core.view().findings.graph.get();
	const std::string path = args.text("path"), kind_token = args.text("kind");
	std::vector<const GraphEdge *> edges;
	if (!path.empty()) {
		if (graph)
			edges = usages ? graph->usages_of(path) : graph->referrers_of_file(path);
	} else if (!kind_token.empty() && args.has("name")) {
		ReferenceKind kind = ReferenceKind::None;
		if (!reference_kind_from_token(kind_token, kind)) {
			error = "no reference kind \"" + kind_token + "\".";
			return JsonValue::make_null();
		}
		if (graph)
			edges = graph->referrers_of(kind, args.text("name"), args.text("scope"));
	} else {
		error = "it names a file (\"path\"), or a symbol (\"kind\" and \"name\").";
		return JsonValue::make_null();
	}
	return edges_page(graph, edges, page_of(args), edges.size());
}

JsonValue answer_referrers(const QueryContext &context, const QueryArgs &args, std::string &error) {
	return answer_users(context, args, error, false);
}

JsonValue answer_usages(const QueryContext &context, const QueryArgs &args, std::string &error) {
	return answer_users(context, args, error, true);
}

JsonValue answer_missing(const QueryContext &context, const QueryArgs &args, std::string &) {
	const AssetGraph *graph = context.core.view().findings.graph.get();
	if (!graph)
		return edges_page(nullptr, {}, page_of(args), 0);
	// The count is the graph's own, kept as it updates; the page walks the missing edges.
	return edges_page(graph, graph->missing(), page_of(args), graph->missing_count());
}

JsonValue answer_symbols(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const AssetGraph *graph = context.core.view().findings.graph.get();
	std::vector<const GraphSymbol *> symbols;
	if (args.has("kind")) {
		ReferenceKind kind = ReferenceKind::None;
		if (!reference_kind_from_token(args.text("kind"), kind)) {
			error = "no reference kind \"" + args.text("kind") + "\".";
			return JsonValue::make_null();
		}
		if (graph)
			symbols = graph->symbols_of_kind(kind);
	} else if (graph) {
		graph->for_each_symbol(
				[&symbols](const GraphSymbol &symbol) { symbols.push_back(&symbol); });
	}
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	set_page(out, page, symbols.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(symbols.size()); i < page.last(symbols.size()); ++i)
		list.push(graph_symbol_to_json(*symbols[i]));
	out.set("symbols", std::move(list));
	return out;
}

JsonValue answer_project_search(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	const AssetGraph *graph = context.core.view().findings.graph.get();
	const std::string text = args.text("text");
	if (text.empty()) {
		error = "the text to find is empty.";
		return JsonValue::make_null();
	}
	return graph_search_to_json(
			graph ? graph->search(text) : std::vector<GraphSearchHit>(), page_of(args));
}

// Why no menu answers: the path names none, or, left out, no document is active or the active one
// is no menu (a pathless read never falls back to another menu: a pathless edit's records are the
// active document's).
std::string no_menu(const SessionView &view, const std::string &path) {
	if (!path.empty())
		return "no menu '" + path + "' in the project.";
	if (view.documents.active.empty())
		return "no document is active: name a menu with \"path\".";
	return "the active document, " + view.documents.active +
			", is not a menu: name one with \"path\".";
}

JsonValue answer_menu_tree(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string path = args.text("path");
	JsonValue tree = menu_tree_to_json(context.core.view(), path);
	if (tree.is_null()) {
		error = no_menu(context.core.view(), path);
		return tree;
	}
	// Each screen's windows a page (window_count the screen's whole count); one screen alone
	// when `screen` names it.
	const bool one = args.has("screen");
	const uint64_t only = one ? uint64_t(args.integer("screen")) : 0;
	const JsonPage page = page_of(args);
	JsonValue *screens = tree.get("screens");
	if (!screens || !screens->is_array())
		return tree;
	std::vector<JsonValue> kept;
	for (JsonValue &screen : screens->array) {
		const JsonValue *id = screen.get("id");
		if (one && !(id && id->is_number() && uint64_t(id->number) == only))
			continue;
		page_list(screen, "windows", page);
		kept.push_back(std::move(screen));
	}
	if (one && kept.empty()) {
		error = tree.get_string("path", path) + " has no screen " + std::to_string(only) + ".";
		return JsonValue::make_null();
	}
	screens->array = std::move(kept);
	return tree;
}

JsonValue answer_menu_findings(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string path = args.text("path");
	JsonValue report = menu_findings_to_json(context.core.view(), path);
	if (report.is_null()) {
		error = no_menu(context.core.view(), path);
		return report;
	}
	if (args.has("severity")) {
		const std::string severity = args.text("severity");
		DiagnosticSeverity level = DiagnosticSeverity::Error;
		if (!diagnostic_severity_from_token(severity, level)) {
			error = "no severity \"" + severity + "\" (error, warning, info).";
			return JsonValue::make_null();
		}
		if (JsonValue *problems = report.get("problems")) {
			std::vector<JsonValue> matching;
			for (JsonValue &row : problems->array)
				if (row.get_string("severity", "") == severity)
					matching.push_back(std::move(row));
			problems->array = std::move(matching);
		}
	}
	page_list(report, "problems", page_of(args));
	return report;
}

JsonValue answer_menu_render(
		const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string path = args.text("path");
	JsonValue render = menu_render_to_json(
			context.core.view(), path, NodeId(args.integer("screen")), page_of(args));
	if (render.is_null())
		error = no_menu(context.core.view(), path);
	return render;
}

JsonValue answer_import_preview(const QueryContext &context, const QueryArgs &args, std::string &) {
	return import_preview_to_json(context.core.view(), page_of(args));
}

JsonValue answer_output(const QueryContext &context, const QueryArgs &args, std::string &) {
	return output_page_to_json(context.core.view().activity.output, args.cursor(), args.limit());
}

JsonValue answer_operation(const QueryContext &context, const QueryArgs &, std::string &) {
	return activity_operation_to_json(context.core.view());
}

// What a build started now would be refused for, nothing built (S13 A7): the build's own plan
// (project_build/build_plan.h) over the files as scanned, the requirements and the Problems rows
// the build gates on, as start_build plans it; `blocked` exactly when that plan would not pack.
JsonValue answer_build_gate(const QueryContext &context, const QueryArgs &args, std::string &error) {
	SessionCore &core = context.core;
	const SessionView &view = core.view();
	if (!view.project.open) {
		error = "no project is open.";
		return JsonValue::make_null();
	}
	const BuildPlan plan = plan_build(core.paths(), *view.project.scan, *view.project.requirements,
			core.problems().gate_findings());
	std::vector<const Diagnostic *> blocking;
	for (const Diagnostic &d : plan.diagnostics)
		if (d.severity == DiagnosticSeverity::Error)
			blocking.push_back(&d);
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("blocked", JsonValue::make_bool(!plan.ok));
	set_page(out, page, blocking.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(blocking.size()); i < page.last(blocking.size()); ++i)
		list.push(diagnostic_to_json(*blocking[i]));
	out.set("blocking", std::move(list));
	return out;
}

JsonValue answer_events(const QueryContext &context, const QueryArgs &args, std::string &) {
	return events_page_to_json(context.core.view().events, args.cursor(), args.limit());
}

JsonValue answer_catalog(const QueryContext &context, const QueryArgs &, std::string &);

// --- the table -----------------------------------------------------------------------------------

// A row built up column by column, as the request table's are.
struct Query {
	EditorQueryRow row;
	template <size_t N>
	constexpr Query(K kind, const char *token, QueryHandler handler, const QueryParam (&params)[N],
			ConcernSet reads, const char *doc) :
			row() {
		row.kind = kind;
		row.token = token;
		row.handler = handler;
		row.params = params;
		row.param_count = N;
		row.reads = reads;
		row.doc = doc;
	}
	constexpr Query(
			K kind, const char *token, QueryHandler handler, ConcernSet reads, const char *doc) :
			row() {
		row.kind = kind;
		row.token = token;
		row.handler = handler;
		row.reads = reads;
		row.doc = doc;
	}
	// The key of the list it pages.
	constexpr Query pages(const char *key) const {
		Query out = *this;
		out.row.list_key = key;
		return out;
	}
};

using C = ViewConcern;

// What the answers read, beyond one concern each: an open document read by its path or, pathless,
// the active one; a record's references as the graph resolves them; a Go to's files by the scan;
// a menu, open or as the render check read its file, with the render check's findings; and the
// Problems rows with the fixes each row carries (problem_query_key, problem_fix_key).
constexpr ConcernSet kDocumentReads =
		concern_set({ C::Documents, C::DocumentSet, C::ActiveDocument });
constexpr ConcernSet kRecordReads =
		concern_set({ C::Documents, C::DocumentSet, C::ActiveDocument, C::Graph });
constexpr ConcernSet kTargetReads =
		concern_set({ C::Documents, C::DocumentSet, C::ActiveDocument, C::Graph, C::Files });
constexpr ConcernSet kMenuReads =
		concern_set({ C::Documents, C::DocumentSet, C::ActiveDocument, C::Files, C::Findings });
constexpr ConcernSet kProblemsReads = concern_set({ C::Findings, C::ActiveDocument, C::DocumentSet,
		C::Project, C::Files, C::Graph, C::Preferences });
constexpr ConcernSet kGraphReads = concern_set({ C::Graph });

constexpr EditorQueryRow kRows[] = {
	Query(K::State, "state", answer_state, kStateParams, kEveryConcern,
			"The view by section (the catalog's sections: the status line, the project, the "
			"requirements, the open documents, the selection, the operation, Play, the import "
			"dialog, the dialogs, the problem and graph counts, the preferences, the output lines "
			"and the events held): `view_revision` the view's clock, `revisions` each concern's "
			"stamp (the clock value at which it last moved); with `since`, a view_revision an "
			"earlier answer carried, the sections none of whose concerns moved since left out.")
			.row,
	Query(K::Files, "files", answer_files, kPageParams, concern_set({ C::Files }),
			"A page of the files the project's scan lists, in its order: each file's path, name, "
			"kind (its asset kind's token) and editable, whether the editor opens it.")
			.pages("files")
			.row,
	Query(K::Documents, "documents", answer_documents, kPageParams, kDocumentReads,
			"The active document and a page of the open documents, each's lifecycle state: path, "
			"kind, dirty, blocked, revision, can_undo, can_redo, ignored_lines and its source "
			"issues; a record document's also file_state_changed, row_count, last_added and the "
			"kinds of row its outline adds (top_kinds).")
			.pages("documents")
			.row,
	Query(K::Document, "document", answer_document, kDocumentParams, kDocumentReads,
			"One open document's lifecycle state (as the documents query gives it) and, for a "
			"record document, a page of its rows, each with its id, kind, name, change since the "
			"save (unchanged, changed, added) and the collections it holds, their records at "
			"every depth.")
			.pages("rows")
			.row,
	Query(K::Record, "record", answer_record, kRecordParams, kRecordReads,
			"One record of an open record document, by its id or by the symbol it defines: its id, "
			"address (row, kind, child), name, path, locator, change since the save, owner and "
			"index, every field as it applies to it (value, label, unit, range, choices, whether "
			"an optional one is present, a reference's status, what it defines, and a changed "
			"one's saved value) and the collections it holds.")
			.row,
	Query(K::ReferenceChoices, "reference_choices", answer_reference_choices, kFieldParams,
			kRecordReads,
			"A page of the names a record's reference field's picker offers there: each with "
			"its name, the kind it names, the file and record that define it, the status the "
			"field set to it would have, and why no lookup of the game finds an inert one.")
			.pages("choices")
			.row,
	Query(K::ReferenceTargets, "reference_targets", answer_reference_targets, kFieldParams,
			kTargetReads,
			"A page of the places a record's reference field's Go to leads with the value it "
			"holds: each with its label, file, locator and field, and whether the editor opens "
			"the file.")
			.pages("targets")
			.row,
	Query(K::DocumentSearch, "document_search", answer_document_search, kDocumentSearchParams,
			kDocumentReads,
			"A page of the fields of an open document whose value, as the Inspector shows it, "
			"holds the text, in document order: each hit its record's id, address, record path "
			"and locator, the field's id and label, the text as shown and where the text is in "
			"it.")
			.pages("hits")
			.row,
	Query(K::Problems, "problems", answer_problems, kProblemsParams, kProblemsReads,
			"The Problems rows as the Problems window shows them: errors, then warnings, then "
			"notes; total, shown (the rows matching), counts by severity, a page of the rows, "
			"each with what it is about and its fixes ({label, detail, bulk, request}: the "
			"request an editor_request passes back as it is), and grouped, the page's groups.")
			.pages("problems")
			.row,
	Query(K::References, "references", answer_references, kReferencesParams, kGraphReads,
			"A page of what a file names in the asset graph: each edge with its record, locator, "
			"field, kind, value, target, scope, status and the file it resolves to.")
			.pages("edges")
			.row,
	Query(K::Referrers, "referrers", answer_referrers, kReferrersParams, kGraphReads,
			"A page of the edges that name a file, or a symbol of a kind and name in a scope.")
			.pages("edges")
			.row,
	Query(K::Usages, "usages", answer_usages, kReferrersParams, kGraphReads,
			"A page of who uses a file: the edges that name it, then those naming each symbol it "
			"defines; for a symbol, as referrers.")
			.pages("edges")
			.row,
	Query(K::Missing, "missing", answer_missing, kPageParams, kGraphReads,
			"A page of every reference that resolves to nothing (count the graph's own count of "
			"them).")
			.pages("edges")
			.row,
	Query(K::Symbols, "symbols", answer_symbols, kSymbolsParams, kGraphReads,
			"A page of the names the files define: each with its kind, name, file, record, "
			"locator, address, field, scope, line, a style variable's value, and inert with why "
			"where no lookup of the game finds it.")
			.pages("symbols")
			.row,
	Query(K::ProjectSearch, "project_search", answer_project_search, kProjectSearchParams,
			kGraphReads,
			"A page of the files whose names and the symbols whose names hold the text, without "
			"case, files first, each with its usages.")
			.pages("hits")
			.row,
	Query(K::MenuTree, "menu_tree", answer_menu_tree, kMenuTreeParams, kMenuReads,
			"A menu's screens (id, name, the render check's status and whether it is current) and "
			"a page of each screen's windows in pre-order: id, name, type, parent, depth, index, "
			"text, the lists it holds and, while the render is current, its rect and local rect "
			"in 800x600 design units and whether it is shown.")
			.pages("windows")
			.row,
	Query(K::MenuFindings, "menu_findings", answer_menu_findings, kMenuFindingsParams, kMenuReads,
			"A menu's Problems rows, each with its source (graph, render, menu, ...), the counts "
			"by severity and by source, each screen's render status, notes and problems, and a "
			"page of the rows.")
			.pages("problems")
			.row,
	Query(K::MenuRender, "menu_render", answer_menu_render, kMenuRenderParams, kMenuReads,
			"A menu's screen as the render check compiled it headless with the last validation, "
			"in the preview's schema: its status, a page of its widgets (index, id, name, type, "
			"shown, disabled, rect and local in 800x600 design units, text, font, text_color) and "
			"by the same page its compiler notes (note_count their whole count; next_offset runs "
			"to the end of the longer list).")
			.pages("widgets")
			.row,
	Query(K::ImportPreview, "import_preview", answer_import_preview, kPageParams,
			concern_set({ C::Dialogs, C::Preferences, C::Files }),
			"The import dialog's preview: open, with_dependencies, a page of its plan's rows in "
			"its order, the chosen files first (state, name, kind, source, destination, "
			"made_from, needed_by, found_in, selected, problem, rivals), by the same page what it "
			"offers and chose (choices, roots) and the files not found, each list with its own "
			"count (next_offset runs to the end of the longest), then the kinds not followed, "
			"truncated and the plan's findings.")
			.pages("rows")
			.row,
	Query(K::Output, "output", answer_output, kCursorParams, concern_set({ C::Output }),
			"A page of the output lines by absolute index: first (the oldest held), next (one "
			"past the newest), cursor (the page's first) and next_cursor. Paging by next_cursor "
			"repeats no line; the log keeps its last 2000, so a client more than 2000 lines behind "
			"misses the lines dropped, the cursor coming back larger than it asked.")
			.pages("lines")
			.row,
	Query(K::Operation, "operation", answer_operation, concern_set({ C::Operation }),
			"The operation that runs (running, and while one does its id, kind, label, done and "
			"total in its unit, cancellable, and what it reads and writes), what the last one "
			"came to (last_operation: id, kind, end, findings) and the last build.")
			.row,
	Query(K::BuildGate, "build_gate", answer_build_gate, kPageParams,
			concern_set({ C::Project, C::Files, C::Findings }),
			"What a build started now would be refused for, nothing built: blocked (a build would "
			"not pack) and a page of the findings that block it, the errors among the Problems rows "
			"the build gates on, the scan's and the requirements', and the build's own checks of "
			"the files (an archive in the project, a name no archive can store). A Problems row the "
			"build does not gate on (a project check's: the render check's) blocks nothing.")
			.pages("blocking")
			.row,
	// Events are posted beside a Selection or a Dialogs change (view_revisions.h).
	Query(K::Events, "events", answer_events, kCursorParams,
			concern_set({ C::Selection, C::Dialogs }),
			"A page of the view events by seq (the one-shot asks a request makes of a window): "
			"first, next, cursor, next_cursor and the items, each its seq, kind (reveal_record, "
			"reveal_file, ask_rename, settings_applied, import_planned) and the fields its kind "
			"sets. The last 64 are held: a client more than 64 behind misses the events dropped, "
			"the cursor coming back larger than it asked.")
			.pages("items")
			.row,
	Query(K::Catalog, "catalog", answer_catalog, concern_set({ C::Findings }),
			"What the session answers and takes: every request kind with the fields it takes and "
			"needs, who serves it and what it does; every request field; every query with its "
			"params, the list it pages and the concerns it reads; the state's sections; the view's "
			"concerns; and the codes of the findings the session holds now (S13 A6's findings "
			"table will list every code the session and the types know).")
			.row,
};

static_assert(std::size(kRows) == kEditorQueryKindCount, "every query kind has exactly one row");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kEditorQueryKindCount; ++i)
		if (kRows[i].kind != static_cast<EditorQueryKind>(i))
			return false;
	return true;
}
static_assert(rows_in_order(), "the query rows follow the enum's order");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Every row a token of its own, a handler, a doc and a concern it reads at least; every param a
// name of its own in its row and a doc; a paged row takes a limit and an offset or a cursor.
constexpr bool rows_named() {
	for (size_t i = 0; i < kEditorQueryKindCount; ++i) {
		const EditorQueryRow &row = kRows[i];
		if (!row.token[0] || !row.handler || !row.doc || !row.doc[0] || !row.reads ||
				(row.reads & ~kEveryConcern))
			return false;
		for (size_t j = i + 1; j < kEditorQueryKindCount; ++j)
			if (same_text(row.token, kRows[j].token))
				return false;
		bool limit = false, start = false;
		for (size_t p = 0; p < row.param_count; ++p) {
			const QueryParam &param = row.params[p];
			if (!param.name[0] || !param.doc[0])
				return false;
			for (size_t q = p + 1; q < row.param_count; ++q)
				if (same_text(param.name, row.params[q].name))
					return false;
			limit = limit || same_text(param.name, "limit");
			start = start || same_text(param.name, "offset") || same_text(param.name, "cursor");
		}
		if ((row.list_key != nullptr) != (limit && start))
			return false;
	}
	return true;
}
static_assert(rows_named(),
		"each query has a token of its own, a handler, a doc and a concern it reads, its params a "
		"name of their own and a doc, and a paged query its limit and its offset or cursor");

// A param a query takes, by name, or null.
const QueryParam *param_of(const EditorQueryRow &row, const char *name) {
	for (size_t i = 0; i < row.param_count; ++i)
		if (same_text(row.params[i].name, name))
			return &row.params[i];
	return nullptr;
}

std::string params_taken(const EditorQueryRow &row) {
	std::string out;
	for (size_t i = 0; i < row.param_count; ++i)
		out += (i ? ", " : "") + std::string(row.params[i].name);
	return out.empty() ? std::string("nothing") : out;
}

bool whole(const JsonValue &json) {
	return json.is_number() && json.number >= 0.0 && json.number == std::floor(json.number) &&
			json.number <= 9007199254740992.0;
}

// The args checked once against the row's params (QueryArgs' promise).
bool check_args(const EditorQueryRow &row, const JsonValue &args, std::string &error) {
	if (args.is_null()) {
		for (size_t i = 0; i < row.param_count; ++i)
			if (row.params[i].required) {
				error = "it needs \"" + std::string(row.params[i].name) + "\" (it takes " +
						params_taken(row) + ").";
				return false;
			}
		return true;
	}
	if (!args.is_object()) {
		error = "its args are an object of its params (it takes " + params_taken(row) + ").";
		return false;
	}
	for (const io::JsonMember &member : args.object) {
		const QueryParam *param = param_of(row, member.key.c_str());
		if (!param) {
			error = "it takes no \"" + member.key + "\" (it takes " + params_taken(row) + ").";
			return false;
		}
		const JsonValue &value = member.value;
		bool typed = false;
		switch (param->type) {
			case J::String:
				typed = value.is_string();
				break;
			case J::Integer:
				typed = whole(value);
				break;
			case J::Boolean:
				typed = value.is_bool();
				break;
			case J::Strings:
				typed = value.is_array() &&
						std::all_of(value.array.begin(), value.array.end(),
								[](const JsonValue &item) { return item.is_string(); });
				break;
		}
		if (!typed) {
			error = "\"" + member.key + "\" must be " +
					(param->type == J::Integer
									? std::string("a whole number, 0 or more")
									: std::string("a ") + query_json_token(param->type)) +
					".";
			return false;
		}
		if (member.key == "limit" && (value.number < 1.0 || value.number > double(kQueryPageMax))) {
			error = "\"limit\" is 1 to " + std::to_string(kQueryPageMax) + ", not " +
					std::to_string(int64_t(value.number)) + ".";
			return false;
		}
	}
	for (size_t i = 0; i < row.param_count; ++i)
		if (row.params[i].required && !args.get(row.params[i].name)) {
			error = "it needs \"" + std::string(row.params[i].name) + "\" (it takes " +
					params_taken(row) + ").";
			return false;
		}
	return true;
}

JsonValue default_to_json(const QueryParam &param) {
	const std::string text = param.default_value;
	switch (param.type) {
		case J::Integer:
			return json_number(double(std::strtoll(text.c_str(), nullptr, 10)));
		case J::Boolean:
			return JsonValue::make_bool(text == "true");
		default:
			return json_string(text);
	}
}

const char *served_token(ServedBy by) {
	switch (by) {
		case ServedBy::Session:
			return "session";
		case ServedBy::Shell:
			return "shell";
		case ServedBy::ShellNeedsPerson:
			return "person";
	}
	return "session";
}

JsonValue fields_of(RequestFieldSet set) {
	JsonValue out = JsonValue::make_array();
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (set & field_bit(id))
			out.push(json_string(request_field(id).token));
	}
	return out;
}

JsonValue answer_catalog(const QueryContext &context, const QueryArgs &, std::string &) {
	JsonValue out = JsonValue::make_object();
	JsonValue requests = JsonValue::make_array();
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		const RequestKindRow &row = request_kind_row(static_cast<EditorRequestKind>(i));
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", json_string(row.token));
		entry.set("served_by", json_string(served_token(row.served_by)));
		entry.set("takes", fields_of(row.params.takes));
		entry.set("needs", fields_of(row.params.required));
		entry.set("doc", json_string(row.doc));
		requests.push(std::move(entry));
	}
	out.set("requests", std::move(requests));
	JsonValue fields = JsonValue::make_array();
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const RequestField &field = request_field(static_cast<RequestFieldId>(i));
		JsonValue entry = JsonValue::make_object();
		entry.set("field", json_string(field.token));
		entry.set("type", json_string(request_json_token(field.json)));
		entry.set("doc", json_string(field.doc));
		fields.push(std::move(entry));
	}
	out.set("fields", std::move(fields));
	JsonValue queries = JsonValue::make_array();
	for (const EditorQueryRow &row : kRows) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(row.token));
		JsonValue params = JsonValue::make_array();
		for (size_t p = 0; p < row.param_count; ++p) {
			const QueryParam &param = row.params[p];
			JsonValue item = JsonValue::make_object();
			item.set("name", json_string(param.name));
			item.set("type", json_string(query_json_token(param.type)));
			item.set("required", JsonValue::make_bool(param.required));
			if (param.default_value)
				item.set("default", default_to_json(param));
			item.set("doc", json_string(param.doc));
			params.push(std::move(item));
		}
		entry.set("params", std::move(params));
		if (row.list_key)
			entry.set("list", json_string(row.list_key));
		JsonValue reads = JsonValue::make_array();
		for (size_t c = 0; c < kViewConcernCount; ++c)
			if (row.reads & concern_bit(static_cast<ViewConcern>(c)))
				reads.push(json_string(view_concern_token(static_cast<ViewConcern>(c))));
		entry.set("reads", std::move(reads));
		entry.set("doc", json_string(row.doc));
		queries.push(std::move(entry));
	}
	out.set("queries", std::move(queries));
	JsonValue sections = JsonValue::make_array();
	for (size_t i = 0; i < kViewSectionCount; ++i) {
		const ViewSectionRow &row = view_section_row(static_cast<ViewSection>(i));
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(row.token));
		JsonValue concerns = JsonValue::make_array();
		for (size_t c = 0; c < kViewConcernCount; ++c)
			if (row.concerns & concern_bit(static_cast<ViewConcern>(c)))
				concerns.push(json_string(view_concern_token(static_cast<ViewConcern>(c))));
		entry.set("concerns", std::move(concerns));
		entry.set("doc", json_string(row.doc));
		sections.push(std::move(entry));
	}
	out.set("sections", std::move(sections));
	JsonValue concerns = JsonValue::make_array();
	for (size_t i = 0; i < kViewConcernCount; ++i)
		concerns.push(json_string(view_concern_token(static_cast<ViewConcern>(i))));
	out.set("concerns", std::move(concerns));
	// The codes of the findings the session holds now (the Problems rows), each once with how many
	// rows carry it: S13 A6's findings table lists every code the session and the types know.
	std::map<std::string, size_t> codes;
	for (const Diagnostic &d : context.core.view().findings.diagnostics)
		++codes[d.code];
	JsonValue findings = JsonValue::make_array();
	for (const auto &[code, count] : codes) {
		JsonValue entry = JsonValue::make_object();
		entry.set("code", json_string(code));
		entry.set("count", json_number(double(count)));
		findings.push(std::move(entry));
	}
	out.set("finding_codes", std::move(findings));
	out.set("page_max", json_number(double(kQueryPageMax)));
	out.set("page_default", json_number(double(kQueryPageDefault)));
	return out;
}

} // namespace

const JsonValue *QueryArgs::value(const char *name) const {
	return args_.is_object() ? args_.get(name) : nullptr;
}

bool QueryArgs::has(const char *name) const {
	return value(name) != nullptr;
}

std::string QueryArgs::text(const char *name) const {
	if (const JsonValue *member = value(name); member && member->is_string())
		return member->string;
	const QueryParam *param = param_of(row_, name);
	return param && param->default_value ? std::string(param->default_value) : std::string();
}

int64_t QueryArgs::integer(const char *name) const {
	if (const JsonValue *member = value(name); member && member->is_number())
		return int64_t(member->number);
	const QueryParam *param = param_of(row_, name);
	return param && param->default_value ? std::strtoll(param->default_value, nullptr, 10) : 0;
}

bool QueryArgs::boolean(const char *name) const {
	if (const JsonValue *member = value(name); member && member->is_bool())
		return member->boolean;
	const QueryParam *param = param_of(row_, name);
	return param && param->default_value && same_text(param->default_value, "true");
}

std::vector<std::string> QueryArgs::strings(const char *name) const {
	std::vector<std::string> out;
	if (const JsonValue *member = value(name); member && member->is_array())
		for (const JsonValue &item : member->array)
			if (item.is_string())
				out.push_back(item.string);
	return out;
}

const EditorQueryRow &editor_query_row(EditorQueryKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return kRows[index < kEditorQueryKindCount ? index : static_cast<size_t>(K::Catalog)];
}

bool editor_query_from_token(std::string_view token, EditorQueryKind &out) {
	for (const EditorQueryRow &row : kRows) {
		if (token == row.token) {
			out = row.kind;
			return true;
		}
	}
	return false;
}

const char *query_json_token(QueryJson type) {
	switch (type) {
		case QueryJson::String:
			return "string";
		case QueryJson::Integer:
			return "integer";
		case QueryJson::Boolean:
			return "boolean";
		case QueryJson::Strings:
			return "string[]";
	}
	return "string";
}

JsonValue run_query(
		SessionCore &core, std::string_view name, const JsonValue &args, std::string &error) {
	error.clear();
	EditorQueryKind kind = K::State;
	if (!editor_query_from_token(name, kind)) {
		std::string names;
		for (const EditorQueryRow &row : kRows)
			names += (names.empty() ? "" : ", ") + std::string(row.token);
		error = "Unknown query \"" + std::string(name) + "\" (" + names + ").";
		return JsonValue::make_null();
	}
	const EditorQueryRow &row = editor_query_row(kind);
	const std::string what = std::string("query ") + row.token + ": ";
	if (!check_args(row, args, error)) {
		error = what + error;
		return JsonValue::make_null();
	}
	const QueryArgs parsed(row, args);
	JsonValue answer = row.handler(QueryContext{ core }, parsed, error);
	if (!error.empty()) {
		error = what + error;
		return JsonValue::make_null();
	}
	// The clock value at which what the answer reads last moved (the state's: the clock). An
	// answer's own `revision` (a document's, a menu's) is its own; view_revision is this alone.
	assert(!answer.get("view_revision") && "a query's view_revision is run_query's to stamp");
	answer.set("view_revision", json_number(double(core.view().revisions.stamp_of(row.reads))));
	return answer;
}

} // namespace opennova::editor
