#include <editor/session/editor_queries.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_uses.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_surfaces.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document_search.h>
#include <editor/preview/menu_report.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/catalog_json.h>
#include <editor/session/document_set.h>
#include <editor/session/environment_uses.h>
#include <editor/session/file_card.h>
#include <editor/session/file_page.h>
#include <editor/session/finding_codes.h>
#include <editor/session/mission_logic_json.h>
#include <editor/session/model_json.h>
#include <editor/session/problems_service.h>
#include <editor/session/record_batch.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>
#include <editor/session/script_assist.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/texture_budget_list.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view_json.h>
#include <editor/session/workspace_parts.h>

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

constexpr QueryParam kFilesParams[] = {
	{ "text", J::String, false, nullptr,
			"Only the files Files' filter lists for this text: a path holding it, then the files of a kind it "
			"names (\"texture\", \"waves\"; \"kind:texture\" that kind's alone)." },
	{ "kind", J::String, false, nullptr, "Only the files of this asset kind (its token: texture, wave, model...)." },
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
	{ "blocking", J::Boolean, false, "false",
			"Only the findings a build is refused for (each row's blocks_build: the gate's refusals)." },
	{ "group", J::String, false, "none",
			"How the rows are grouped: none, file or kind (the code's family)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kFilePageParams[] = {
	{ "path", J::String, false, nullptr,
			"A project file, as references takes it; left out, the file whose page the Document window "
			"shows." },
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

constexpr QueryParam kViewportParams[] = {
	{ "path", J::String, false, nullptr,
			"An open document by its project-relative path or its logical name, or the texture Files "
			"selects (S18); left out, the active one." },
	{ "kind", J::String, false, nullptr,
			"The viewport's kind (menu, model, script, mission, texture); left out, the one the document shows "
			"in: the Preview's kind that shows it (a menu's; a model's, over a model, a clip or an "
			"animation table), else its Main view." },
	{ "op", J::String, true, nullptr,
			"What is read: state (the envelope, a page of its items and by the same page its "
			"notes), items or notes (a page of one), hit (what lies under x, y), box (what the box from "
			"x, y to x2, y2 takes, as a marquee over it: a menu's windows it touches), render (one row "
			"of the document as the kind renders it apart: a menu's screen, as the render check "
			"compiled it), palette (what a mission's Place tool places: the items by name in their "
			"groups, the recently placed first, those whose name, id or model holds text)." },
	{ "x", J::Number, false, nullptr,
			"hit's point across (box's first corner), in the viewport's units (a menu's 800x600 "
			"design units, a model's picture pixels): a number a float holds." },
	{ "y", J::Number, false, nullptr, "hit's point down, as x." },
	{ "x2", J::Number, false, nullptr, "box's other corner across, as x (x and y its first)." },
	{ "y2", J::Number, false, nullptr, "box's other corner down, as x." },
	{ "row", J::Integer, false, nullptr, "render's row, by its identity (a menu's screen)." },
	{ "text", J::String, false, nullptr,
			"palette's search: the items whose name, id or model's file holds it, case aside (left "
			"out, every item)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kCursorParams[] = {
	{ "cursor", J::Integer, false, "0", kCursorDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kOutputParams[] = {
	{ "cursor", J::Integer, false, "0", kCursorDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
	{ "unfold", J::Integer, false, nullptr,
			"A line's absolute index: the lines folded under it instead (an import's files, the game's log), "
			"a page of them from cursor (here their own index, 0 the first)." },
};

constexpr QueryParam kImportPreviewParams[] = {
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
	{ "kind", J::String, false, nullptr,
			"An asset kind's token (the summary's): the page holds the plan's rows of that kind alone, "
			"count theirs; every kind when left out." },
};

// --- helpers -------------------------------------------------------------------------------------

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

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

JsonValue answer_files(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::vector<AssetEntry> &entries = view.project.scan->entries;
	// Narrowed as Files narrows its list (the UX round's project lane): by a kind's token, and by a text.
	AssetKind kind = AssetKind::kCount;
	if (args.has("kind")) {
		kind = asset_kind_from_token(args.text("kind"));
		if (kind == AssetKind::Unknown && args.text("kind") != asset_kind_token(AssetKind::Unknown)) {
			error = "no file kind \"" + args.text("kind") + "\".";
			return JsonValue::make_null();
		}
	}
	std::vector<size_t> shown;
	if (args.has("text") || kind != AssetKind::kCount) shown = match_files(*view.project.scan, args.text("text"), kind);
	else for (size_t i = 0; i < entries.size(); ++i) shown.push_back(i);
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	set_page(out, page, shown.size());
	JsonValue files = JsonValue::make_array();
	for (size_t at = page.first(shown.size()); at < page.last(shown.size()); ++at) {
		const AssetEntry &entry = entries[shown[at]];
		JsonValue file = JsonValue::make_object();
		file.set("path", json_string(entry.relative_path));
		file.set("name", json_string(entry.logical_name));
		file.set("kind", json_string(asset_kind_token(entry.kind)));
		file.set("editable", JsonValue::make_bool(is_editable_kind(entry.kind)));
		// The import that makes it (S18).
		if (!entry.imported_from.empty()) file.set("imported_from", json_string(entry.imported_from));
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
	// Each row's title worded with the graph's names (ADR 0046 S15).
	const AssetGraph *graph = context.core.view().findings.graph.get();
	if (!graph) return document_to_json(*document, &page);
	const GraphNameSource names(*graph);
	return document_to_json(*document, &page, &names);
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
	query.blocking = args.boolean("blocking");
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

// A file's page (session/file_page.h): what it is, what reads it, where a build puts it, who names it and
// what it names, in words.
JsonValue answer_file_page(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = args.text("path").empty() ? view.documents.page : args.text("path");
	const FilePage page = file_page(view, path);
	if (!page.found) {
		error = path.empty() ? std::string("name the file with \"path\" (no page shows).") : "no project file " + path + ".";
		return JsonValue::make_null();
	}
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(page.path));
	out.set("name", json_string(page.name));
	out.set("kind", json_string(page.kind));
	out.set("size", json_number(double(page.size)));
	out.set("what", json_string(page.what));
	out.set("read_by", json_string(page.read_by));
	out.set("cite", json_string(page.cite));
	out.set("build", json_string(page.build));
	out.set("editor", json_string(page.editor));
	out.set("errors", json_number(double(page.errors)));
	out.set("warnings", json_number(double(page.warnings)));
	const auto lines = [](const std::vector<FilePageLine> &from) {
		JsonValue list = JsonValue::make_array();
		for (const FilePageLine &line : from) {
			JsonValue entry = JsonValue::make_object();
			entry.set("text", json_string(line.text));
			if (line.missing) entry.set("missing", JsonValue::make_bool(true));
			if (!line.target.file.empty()) entry.set("file", json_string(line.target.file));
			if (!line.target.locator.empty()) entry.set("locator", json_string(line.target.locator));
			list.push(std::move(entry));
		}
		return list;
	};
	out.set("used_by", lines(page.used_by));
	out.set("names", lines(page.names));
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
		// A record of a record set goes by its index in its own file alone (a Record reference,
		// ADR 0046 S13 D8): with no file to count in, the index names nothing.
		if (reference_row(kind).resolution == ReferenceResolution::Record && args.text("scope").empty()) {
			error = "a " + kind_token + " names a record of one file by its index: give that file as \"scope\".";
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

// --- the viewport query's ops (S13 V7) -------------------------------------------------------------

// What an op answers from: the session's view, the viewport the args name (resolved and followed
// now), the args, and the page they ask.
struct ViewportReadContext {
	const SessionView &view;
	const ViewportModel &model;
	const QueryArgs &args;
	JsonPage page;
};
using ViewportAnswer = JsonValue (*)(const ViewportReadContext &read, std::string &error);

JsonValue viewport_state(const ViewportReadContext &read, std::string &) {
	return viewport_to_json(read.view, read.model, read.page);
}
JsonValue viewport_items(const ViewportReadContext &read, std::string &) {
	return viewport_items_to_json(read.view, read.model, read.page);
}
JsonValue viewport_hit(const ViewportReadContext &read, std::string &error) {
	// A kind with no canvas (the script's: its device is a control that owns the pointer) has no picture
	// a point of it names anything of: refused, never answered as a hit on nothing.
	if (!viewport_kind_row(read.model.kind()).canvas) {
		error = std::string("a ") + viewport_kind_token(read.model.kind()) +
				" viewport has no canvas: its device is a control that owns the pointer, so no point of it names "
				"anything (op items reads what it lists).";
		return JsonValue::make_null();
	}
	// The point checked as a number a float holds (check_args).
	const ViewportHit hit = read.model.hit(viewport_context(read.view, read.model), float(read.args.number("x")),
			float(read.args.number("y")));
	return viewport_hit_to_json(read.model, hit);
}
JsonValue viewport_box(const ViewportReadContext &read, std::string &error) {
	if (!viewport_kind_row(read.model.kind()).canvas) {
		error = std::string("a ") + viewport_kind_token(read.model.kind()) +
				" viewport has no canvas: no box of it takes anything (op items reads what it lists).";
		return JsonValue::make_null();
	}
	return viewport_box_to_json(read.view, read.model,
			read.model.box(viewport_context(read.view, read.model), float(read.args.number("x")),
					float(read.args.number("y")), float(read.args.number("x2")), float(read.args.number("y2"))));
}
JsonValue viewport_notes(const ViewportReadContext &read, std::string &) {
	return viewport_notes_to_json(read.view, read.model, read.page);
}
JsonValue viewport_render(const ViewportReadContext &read, std::string &error) {
	return read.model.render_json(viewport_context(read.view, read.model).input, NodeId(read.args.integer("row")),
			read.page, error);
}
JsonValue viewport_palette(const ViewportReadContext &read, std::string &error) {
	return read.model.palette_json(read.view, read.args.text("text"), read.page, error);
}

// The ops by name, in their order on the wire.
enum class ViewportOp : uint8_t { State, Items, Hit, Box, Notes, Render, Palette, kCount };

// What an op takes beside the params every op takes (path, kind, op): a page (offset, limit), a point
// of the picture (x, y), a row (row), a box's other corner (x2, y2), a search (text).
enum ViewportTakes : uint8_t {
	kViewportPage = 1u << 0,
	kViewportPoint = 1u << 1,
	kViewportRow = 1u << 2,
	kViewportCorner = 1u << 3,
	kViewportText = 1u << 4,
};

// One op: its token, what it takes and of that what it needs (each named, a point both its params),
// and what answers it.
struct ViewportOpRow {
	ViewportOp op;
	const char *token;
	uint8_t takes;
	uint8_t needs;
	ViewportAnswer answer;
};

constexpr ViewportOpRow kViewportOps[] = {
	{ ViewportOp::State, "state", kViewportPage, 0, viewport_state },
	{ ViewportOp::Items, "items", kViewportPage, 0, viewport_items },
	{ ViewportOp::Hit, "hit", kViewportPoint, kViewportPoint, viewport_hit },
	{ ViewportOp::Box, "box", kViewportPoint | kViewportCorner, kViewportPoint | kViewportCorner, viewport_box },
	{ ViewportOp::Notes, "notes", kViewportPage, 0, viewport_notes },
	{ ViewportOp::Render, "render", kViewportPage | kViewportRow, kViewportRow, viewport_render },
	{ ViewportOp::Palette, "palette", kViewportPage | kViewportText, 0, viewport_palette },
};

// The flag of a viewport query param: 0 for those every op takes (path, kind, op), 0xFF for one no
// op takes (none: static_asserted).
constexpr uint8_t viewport_param_flag(const char *name) {
	return same_text(name, "path") || same_text(name, "kind") || same_text(name, "op") ? 0
			: same_text(name, "offset") || same_text(name, "limit")                    ? kViewportPage
			: same_text(name, "x") || same_text(name, "y")                             ? kViewportPoint
			: same_text(name, "x2") || same_text(name, "y2")                           ? kViewportCorner
			: same_text(name, "row")                                                   ? kViewportRow
			: same_text(name, "text")                                                  ? kViewportText
																					   : 0xFF;
}

// The ops' tokens and the kinds', the choices of the query's op and kind (QueryChoices).
const char *viewport_op_choice(size_t index) {
	return index < std::size(kViewportOps) ? kViewportOps[index].token : nullptr;
}
const char *viewport_kind_choice(size_t index) {
	return index < kViewportKindCount ? viewport_kind_token(static_cast<ViewportKind>(index)) : nullptr;
}
constexpr QueryChoices kViewportChoices[] = {
	{ "op", viewport_op_choice },
	{ "kind", viewport_kind_choice },
};

JsonValue answer_viewport(const QueryContext &context, const QueryArgs &args, std::string &error) {
	// The op (one of its tokens: check_args refused any other), then a param it does not take refused,
	// naming those it takes, as is one it needs left out.
	const std::string op = args.text("op");
	const ViewportOpRow *read = &kViewportOps[0];
	for (const ViewportOpRow &row : kViewportOps)
		if (op == row.token) read = &row;
	const EditorQueryRow &row = editor_query_row(EditorQueryKind::Viewport);
	std::string takes;
	for (size_t i = 0; i < row.param_count; ++i) {
		const uint8_t flag = viewport_param_flag(row.params[i].name);
		if (!flag || (read->takes & flag)) takes += (takes.empty() ? "" : ", ") + std::string(row.params[i].name);
	}
	for (size_t i = 0; i < row.param_count; ++i) {
		const uint8_t flag = viewport_param_flag(row.params[i].name);
		if (args.has(row.params[i].name) && flag && !(read->takes & flag)) {
			error = "op " + op + " takes no \"" + row.params[i].name + "\" (it takes " + takes + ").";
			return JsonValue::make_null();
		}
	}
	if ((read->needs & kViewportPoint) && !(args.has("x") && args.has("y"))) {
		error = "op " + op + " needs \"x\" and \"y\", the point in the viewport's units (a menu's design units, a "
							 "model's picture pixels).";
		return JsonValue::make_null();
	}
	if ((read->needs & kViewportCorner) && !(args.has("x2") && args.has("y2"))) {
		error = "op " + op + " needs \"x2\" and \"y2\", the box's other corner in the viewport's units.";
		return JsonValue::make_null();
	}
	if ((read->needs & kViewportRow) && !args.has("row")) {
		error = "op " + op + " needs \"row\", the row by its identity (a menu's screen).";
		return JsonValue::make_null();
	}
	// The viewport of the kind named (else the one the document shows in), followed now, so what it
	// answers is the document as it is.
	SessionCore &core = context.core;
	ViewportKind kind = ViewportKind::kCount;
	if (args.has("kind")) viewport_kind_from_token(args.text("kind"), kind);
	const DocumentBase *document = open_document_of(context, args, error);
	std::string at = document ? document->path() : std::string();
	// A file Files selects that a kind draws open or not (S18: a texture), by its path or its name.
	if (!document && args.has("path") && core.view().project.open) {
		const AssetScan &scan = *core.view().project.scan;
		const AssetEntry *entry = scan.at_path(args.text("path"));
		if (!entry) entry = scan.find(args.text("path"));
		for (size_t i = 0; entry && i < kViewportKindCount; ++i)
			if ((kind == ViewportKind::kCount || kind == static_cast<ViewportKind>(i)) &&
			    draws_selected_file(core.view(), entry->relative_path, static_cast<ViewportKind>(i))) {
				at = entry->relative_path;
				error.clear();
			}
	}
	const ViewportModel *model = at.empty() ? nullptr : core.viewports().resolve(core.view(), at, kind, error);
	if (!model) return JsonValue::make_null();
	return read->answer(ViewportReadContext{ core.view(), *model, args, page_of(args) }, error);
}

JsonValue answer_import_preview(const QueryContext &context, const QueryArgs &args, std::string &error) {
	AssetKind kind = AssetKind::kCount;
	if (args.has("kind")) {
		kind = asset_kind_from_token(args.text("kind"));
		if (kind == AssetKind::Unknown && args.text("kind") != asset_kind_token(AssetKind::Unknown)) {
			error = "no asset kind \"" + args.text("kind") + "\".";
			return JsonValue::make_null();
		}
	}
	return import_preview_to_json(context.core.view(), page_of(args), kind);
}

JsonValue answer_output(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const OutputLog &output = context.core.view().activity.output;
	if (!args.has("unfold")) return output_page_to_json(output, args.cursor(), args.limit());
	const int64_t at = args.integer("unfold");
	if (at < 0 || uint64_t(at) < output.first_index() || uint64_t(at) >= output.next_index()) {
		error = "no line " + std::to_string(at) + " is held (the lines held are " + std::to_string(output.first_index()) +
		        " to " + std::to_string(int64_t(output.next_index()) - 1) + ").";
		return JsonValue::make_null();
	}
	return output_folded_to_json(output, uint64_t(at), args.cursor(), args.limit());
}

JsonValue answer_operation(const QueryContext &context, const QueryArgs &, std::string &) {
	return activity_operation_to_json(context.core.view());
}

// What a build would be refused for, nothing built (S13 A7): the build's own plan
// (project_build/build_plan.h) over the files as last scanned, the requirements and the Problems
// rows the build gates on (an error whose code gates: blocks_build); `blocked` exactly when that
// plan would not pack. start_build reads the
// changed documents again and refreshes first, and a build request joins a running build and waits
// on unsaved edits: the gate says none of that.
JsonValue answer_build_gate(const QueryContext &context, const QueryArgs &args, std::string &error) {
	SessionCore &core = context.core;
	const SessionView &view = core.view();
	if (!view.project.open) {
		error = "no project is open.";
		return JsonValue::make_null();
	}
	// The gate is the Problems rows a validation makes: the one left due or under way runs to its
	// end first (S13 A3: the polls step it, and no request runs it).
	core.problems().validate_pending();
	// An expansion's gate reads over its base game's names (ADR 0046 S16).
	const BaseNames base{&view.project.base_files};
	const BuildTarget target = core.build_target();
	const std::vector<Diagnostic> gate = core.problems().gate_findings();
	// The game's own bytes, packed as stored, gate on no finding that they do not serialize (S16).
	const ShippedFiles shipped = core.shipped_files();
	const BuildPlan plan = plan_build(core.paths(), *view.project.scan, *view.project.requirements, gate, target,
			&base, &shipped);
	const std::vector<Diagnostic> blocking = build_blockers(plan);
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("blocked", JsonValue::make_bool(!plan.ok));
	// The line a build would be refused with (the UX round's problems lane), each refusal with its why.
	if (!plan.ok) out.set("refusal", JsonValue::make_string(refusal_words(blocking)));
	set_page(out, page, blocking.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(blocking.size()); i < page.last(blocking.size()); ++i) {
		JsonValue row = diagnostic_to_json(blocking[i]);
		row.set("because", JsonValue::make_string(blocker_reason(blocking[i])));
		list.push(std::move(row));
	}
	out.set("blocking", std::move(list));
	return out;
}

JsonValue answer_events(const QueryContext &context, const QueryArgs &args, std::string &) {
	return events_page_to_json(context.core.view().events, args.cursor(), args.limit());
}

// --- a mission's logic (S15) ---------------------------------------------------------------------

constexpr const char *kLogicOps[] = { "form", "types", "add", "retype", "move", "negate", "join" };
const char *logic_op_choice(size_t index) { return index < std::size(kLogicOps) ? kLogicOps[index] : nullptr; }
constexpr const char *kLogicLists[] = { "trigger", "action" };
const char *logic_list_choice(size_t index) { return index < std::size(kLogicLists) ? kLogicLists[index] : nullptr; }
const char *logic_join_choice(size_t index) {
	constexpr LogicJoin joins[] = { LogicJoin::And, LogicJoin::Or, LogicJoin::Xor };
	return index < std::size(joins) ? logic_join_token(joins[index]) : nullptr;
}
constexpr QueryChoices kLogicChoices[] = {
	{ "op", logic_op_choice },
	{ "list", logic_list_choice },
	{ "join", logic_join_choice },
};

constexpr QueryParam kMissionLogicParams[] = {
	{ "path", J::String, false, nullptr, "An open mission by its project-relative path or its logical name; left out, the "
			"active document." },
	{ "op", J::String, true, nullptr,
			"What is asked: form (an event's, or a trigger's or an action's: its words and what it holds), types "
			"(what Add trigger or Add action offers, by name and group), add, retype, move, negate, join (the "
			"edits that change the logic so, in the batch form an edit_record takes back)." },
	{ "id", J::Integer, false, nullptr,
			"The record: an event (form, add) or a trigger or an action (form, retype, move, negate, join), by "
			"its identity." },
	{ "list", J::String, false, nullptr, "types' and add's list: trigger or action." },
	{ "type", J::Integer, false, nullptr, "add's and retype's type (a trigger's main type, an action's type)." },
	{ "sub", J::Integer, false, "0", "add's and retype's sub-type (a trigger's, an AI action's command)." },
	{ "to", J::Integer, false, nullptr, "move's event, by its identity." },
	{ "position", J::Integer, false, nullptr, "add's and move's place in the list; left out, the end." },
	{ "negated", J::Boolean, false, nullptr, "negate's: whether the trigger is negated." },
	{ "join", J::String, false, nullptr, "join's: how the trigger joins the next (and, or, or_else)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

constexpr QueryParam kMissionUsesParams[] = {
	{ "path", J::String, false, nullptr, "An open mission, as mission_logic takes it." },
	{ "id", J::Integer, true, nullptr,
			"The record by its identity: an entity, an area trigger, a waypoint path, an event, or a group (a "
			"record of the mission row's Groups)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

// The open mission `args` names, or null with `error`.
const MissionDocument *mission_of(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const Document *document = document_of(context, args, error);
	const auto *mission = dynamic_cast<const MissionDocument *>(document);
	if (document && !mission) error = document->path() + " is no mission.";
	return mission;
}

// A mission's words with the graph's names where it stands (documents/mission_labels.h, S15 Names).
class MissionWords {
public:
	MissionWords(const QueryContext &context, const MissionDocument &mission) {
		if (const AssetGraph *graph = context.core.view().findings.graph.get()) source_.emplace(*graph);
		names_ = mission_label_names(mission, source_ ? &*source_ : nullptr);
	}
	MissionWords(const MissionWords &) = delete;
	MissionWords &operator=(const MissionWords &) = delete;
	const MissionNames &operator*() const { return *names_; }

private:
	std::optional<GraphNameSource> source_;
	std::unique_ptr<MissionNames> names_;
};

// The planned edits in the batch form an edit_record takes back, or the refusal that says why none.
JsonValue planned(const Document &document, bool ok, const std::vector<Edit> &edits, const std::string &refusal) {
	JsonValue out = JsonValue::make_object();
	out.set("edits", ok ? record_batch_to_json(edits, &document, RecordBatchForm::Edits) : JsonValue::make_array());
	if (!ok) out.set("refusal", json_string(refusal));
	return out;
}

JsonValue answer_mission_logic(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string op = args.text("op");
	const bool actions = args.text("list") == "action";
	if (op == "types") {
		if (!args.has("list")) {
			error = "op types needs \"list\": trigger or action.";
			return JsonValue::make_null();
		}
		return logic_types_to_json(actions, page_of(args));
	}
	const MissionDocument *mission = mission_of(context, args, error);
	if (!mission) return JsonValue::make_null();
	if (!args.has("id")) {
		error = "op " + op + " needs \"id\", the record by its identity.";
		return JsonValue::make_null();
	}
	NodeAddress address;
	if (!record_of(*mission, args, address, error)) return JsonValue::make_null();
	const MissionWords names(context, *mission);
	const bool event = !address.child && address.kind == node_kind(MissionKind::Event);
	if (op == "form") {
		if (event) {
			LogicEventForm form;
			logic_event_form(*mission, address.row, *names, form);
			return logic_event_form_to_json(form);
		}
		LogicForm form;
		if (!logic_form(*mission, address, *names, form)) {
			error = "record " + std::to_string(identity_of(address)) + " is no event, trigger or action.";
			return JsonValue::make_null();
		}
		return logic_form_to_json(form);
	}
	std::vector<Edit> edits;
	std::string refusal;
	bool ok = false;
	if (op == "add" || op == "retype") {
		if (!args.has("type") || (op == "add" && !args.has("list"))) {
			error = "op " + op + " needs \"type\"" + (op == "add" ? std::string(" and \"list\"") : std::string()) + ".";
			return JsonValue::make_null();
		}
		const bool list_actions = op == "add" ? actions : address.kind == node_kind(MissionKind::Action);
		const LogicType *type = logic_type(list_actions, int32_t(args.integer("type")), int32_t(args.integer("sub")));
		if (!type) {
			error = "no " + std::string(list_actions ? "action" : "trigger") + " type " + std::to_string(args.integer("type")) +
			        "/" + std::to_string(args.integer("sub")) + " is offered (op types lists them).";
			return JsonValue::make_null();
		}
		const size_t position = args.has("position") ? size_t(args.integer("position")) : SIZE_MAX;
		ok = op == "add" ? logic_add_edits(*mission, address.row, *type, position, edits, refusal)
		                 : logic_retype_edits(*mission, address, *type, edits, refusal);
	} else if (op == "move") {
		if (!args.has("to")) {
			error = "op move needs \"to\", the event by its identity.";
			return JsonValue::make_null();
		}
		const size_t position = args.has("position") ? size_t(args.integer("position")) : SIZE_MAX;
		ok = logic_move_edits(*mission, address, NodeId(args.integer("to")), position, edits, refusal);
	} else if (op == "negate" || op == "join") {
		Edit edit;
		LogicJoin join = LogicJoin::And;
		if (op == "join" && !logic_join_from_token(args.text("join"), join)) {
			error = "op join needs \"join\": and, or or or_else.";
			return JsonValue::make_null();
		}
		ok = op == "negate" ? logic_negate_edit(*mission, address, args.boolean("negated"), edit)
		                    : logic_join_edit(*mission, address, join, edit);
		if (ok) edits.push_back(edit);
		else refusal = "That is no trigger of the mission.";
	}
	return planned(*mission, ok, edits, refusal);
}

JsonValue answer_mission_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const MissionDocument *mission = mission_of(context, args, error);
	NodeAddress address;
	if (!mission || !record_of(*mission, args, address, error)) return JsonValue::make_null();
	return mission_uses_to_json(mission_uses(*mission, address, *MissionWords(context, *mission)), page_of(args));
}

// --- a model's surfaces (S17) --------------------------------------------------------------------

constexpr const char *kSurfaceOps[] = { "materials", "set", "flag", "faces" };
const char *surface_op_choice(size_t index) { return index < std::size(kSurfaceOps) ? kSurfaceOps[index] : nullptr; }
const char *surface_flag_choice(size_t index) {
	return index < model_face_flags().size() ? model_face_flags()[index].token : nullptr;
}
constexpr QueryChoices kSurfaceChoices[] = {
	{ "op", surface_op_choice },
	{ "flag", surface_flag_choice },
};

constexpr QueryParam kModelSurfacesParams[] = {
	{ "path", J::String, false, nullptr, "An open model by its project-relative path or its logical name; left out, the "
			"active document." },
	{ "op", J::String, true, nullptr,
			"materials (the surfaces and flags a material takes by name, and a page of the model's materials, "
			"each its bullet faces' surface in words), set (the edits that give every face made from the material "
			"the surface), flag (the edits that set or clear a flag on every face made from it), faces (a page of "
			"its faces whose surface is not its common one)." },
	{ "id", J::Integer, false, nullptr, "The material by its identity (set, flag, faces)." },
	{ "surface", J::Integer, false, nullptr,
			"set's surface: a face byte, 0 to 23 by the game's names (materials lists them); any other byte is "
			"played as obj." },
	{ "flag", J::String, false, nullptr, "flag's flag: both_sides, bullets_pass or front_only." },
	{ "on", J::Boolean, false, "true", "flag's: set it (true) or clear it (false)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

JsonValue answer_model_surfaces(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const Document *document = document_of(context, args, error);
	const auto *model = dynamic_cast<const ModelDocument *>(document);
	if (document && !model) error = document->path() + " is no model.";
	if (!model) return JsonValue::make_null();
	const std::string op = args.text("op");
	if (op == "materials") return model_surfaces_to_json(*model, page_of(args));
	if (!args.has("id")) {
		error = "op " + op + " needs \"id\", the material by its identity.";
		return JsonValue::make_null();
	}
	const NodeId material = NodeId(args.integer("id"));
	if (op == "faces") return model_differing_faces_to_json(*model, material, page_of(args));
	std::vector<Edit> edits;
	std::string refusal;
	bool ok = false;
	if (op == "set") {
		if (!args.has("surface")) {
			error = "op set needs \"surface\", a face byte.";
			return JsonValue::make_null();
		}
		ok = model_surface_edits(*model, material, args.integer("surface"), edits, refusal);
	} else {
		if (!args.has("flag")) {
			error = "op flag needs \"flag\": both_sides, bullets_pass or front_only.";
			return JsonValue::make_null();
		}
		uint32_t bit = 0;
		for (const ModelFaceFlag &flag : model_face_flags())
			if (args.text("flag") == flag.token) bit = flag.bit;
		ok = model_face_flag_edits(*model, material, bit, args.boolean("on"), edits, refusal);
	}
	return planned(*model, ok, edits, refusal);
}

// --- a script's help (S15) -----------------------------------------------------------------------

constexpr const char *kScriptOps[] = { "complete", "hover", "definition", "mission_script" };
const char *script_op_choice(size_t index) { return index < std::size(kScriptOps) ? kScriptOps[index] : nullptr; }
constexpr QueryChoices kScriptChoices[] = { { "op", script_op_choice } };

constexpr QueryParam kScriptAssistParams[] = {
	{ "path", J::String, false, nullptr,
			"An open script (complete, hover, definition) or mission (mission_script) by its project-relative path "
			"or its logical name; left out, the active document." },
	{ "op", J::String, true, nullptr,
			"complete (what may complete the word at line, column), hover (what the word there is), definition "
			"(the request that goes where it is defined), mission_script (the script a mission's name finds)." },
	{ "line", J::Integer, false, "1", "The place's line, from 1." },
	{ "column", J::Integer, false, "1", "The place's column, from 1 (complete: the caret, the word ending there)." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

JsonValue answer_script_assist(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string op = args.text("op");
	const DocumentBase *open = open_document_of(context, args, error);
	if (!open) return JsonValue::make_null();
	const SessionView &view = context.core.view();
	JsonValue out = JsonValue::make_object();
	if (op == "mission_script") {
		if (!dynamic_cast<const MissionDocument *>(records_of(*open))) {
			error = open->path() + " is no mission.";
			return JsonValue::make_null();
		}
		const MissionScript script = mission_script(view, open->path());
		out.set("name", json_string(script.name));
		out.set("path", json_string(script.path));
		out.set("create_at", json_string(script.create_at));
		out.set("held", JsonValue::make_bool(!script.path.empty()));
		return out;
	}
	const TextDocument *script = text_of(*open);
	if (!script) {
		error = open->path() + " is no text document.";
		return JsonValue::make_null();
	}
	const size_t line = size_t(std::max<int64_t>(1, args.integer("line")));
	const size_t column = size_t(std::max<int64_t>(1, args.integer("column")));
	if (op == "complete") {
		const ScriptCompletions completions = script_completions(view, *script, line, column);
		out.set("column", json_number(double(completions.column)));
		out.set("typed", json_string(completions.typed));
		out.set("expected", json_string(completions.expected));
		const JsonPage page = page_of(args);
		set_page(out, page, completions.items.size());
		JsonValue items = JsonValue::make_array();
		for (size_t i = page.first(completions.items.size()); i < page.last(completions.items.size()); ++i) {
			const ScriptCompletion &item = completions.items[i];
			JsonValue entry = JsonValue::make_object();
			entry.set("label", json_string(item.label));
			entry.set("insert", json_string(item.insert));
			entry.set("kind", json_string(item.kind));
			entry.set("detail", json_string(item.detail));
			items.push(std::move(entry));
		}
		out.set("items", std::move(items));
		return out;
	}
	if (op == "hover") {
		ScriptHover hover;
		const bool known = script_hover(view, *script, line, column, hover);
		out.set("word", json_string(known ? hover.word : std::string()));
		out.set("column", json_number(double(known ? hover.column : column)));
		out.set("length", json_number(double(known ? hover.length : 0)));
		out.set("text", json_string(known ? hover.text : std::string()));
		return out;
	}
	EditorRequest request;
	out.set("request", script_definition(view, *script, line, column, request) ? editor_request_to_json(request)
	                                                                            : JsonValue::make_null());
	return out;
}

// Every way the game uses a texture (ADR 0046 S18, documents/texture_roles), in the catalog's order.
JsonValue answer_texture_roles(const QueryContext &, const QueryArgs &args, std::string &) {
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	set_page(out, page, kTextureRoleCount);
	JsonValue roles = JsonValue::make_array();
	for (size_t i = page.first(kTextureRoleCount); i < page.last(kTextureRoleCount); ++i)
		roles.push(texture_role_json(texture_role_row(static_cast<TextureRoleId>(i))));
	out.set("roles", std::move(roles));
	return out;
}

const char *transform_choice(size_t index) {
	return index < size_t(TextureLoadTransform::kCount) ? texture_load_transform_token(static_cast<TextureLoadTransform>(index))
	                                                    : nullptr;
}

constexpr QueryParam kThumbnailParams[] = {
	{ "path", J::String, true, nullptr,
			"A texture file of the project, by its project-relative path or its logical name." },
	{ "transform", J::String, false, "\"none\"",
			"What the use's loader makes of the texels first (a texture reference's texture.transform): none, "
			"luminance_alpha, white_alpha_from_blue, alpha_only or normal_from_height." },
};
constexpr QueryChoices kThumbnailChoices[] = { { "transform", transform_choice } };

// A texture file as a small picture (ADR 0046 S18, preview/texture_thumbnails), made now when the cache
// lacks it: its facts and the picture as a PNG.
JsonValue answer_texture_thumbnail(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = args.text("path");
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path) : nullptr;
	if (!entry && view.project.scan) entry = view.project.scan->find(path);
	if (!entry || entry->kind != AssetKind::Texture) {
		error = "no texture file " + path + " in the project.";
		return JsonValue::make_null();
	}
	TextureLoadTransform transform = TextureLoadTransform::None;
	for (size_t i = 0; i < size_t(TextureLoadTransform::kCount); ++i)
		if (args.text("transform") == transform_choice(i)) transform = static_cast<TextureLoadTransform>(i);
	const std::shared_ptr<const TextureThumbnail> thumbnail =
			view.documents.thumbnails ? view.documents.thumbnails->make_now(view, entry->relative_path, transform) : nullptr;
	if (!thumbnail) {
		error = entry->relative_path + " did not read.";
		return JsonValue::make_null();
	}
	return texture_thumbnail_json(*thumbnail, true);
}

constexpr QueryParam kTextureUsesParams[] = {
	{ "path", J::String, true, nullptr,
			"A texture file of the project, by its project-relative path or its logical name." },
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

// What uses a texture (ADR 0046 S18, graph/texture_uses over the session's index).
JsonValue answer_texture_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = args.text("path");
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path) : nullptr;
	if (!entry && view.project.scan) entry = view.project.scan->find(path);
	if (!entry || entry->kind != AssetKind::Texture) {
		error = "no texture file " + path + " in the project.";
		return JsonValue::make_null();
	}
	static const std::vector<TextureUse> kNone;
	const std::vector<TextureUse> &uses =
			view.documents.texture_uses ? view.documents.texture_uses->uses_of(view, entry->relative_path) : kNone;
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("file", json_string(entry->relative_path));
	set_page(out, page, uses.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(uses.size()); i < page.last(uses.size()); ++i) list.push(texture_use_json(uses[i]));
	out.set("uses", std::move(list));
	return out;
}

constexpr QueryParam kTextureBudgetParams[] = {
	{ "offset", J::Integer, false, "0", kOffsetDoc },
	{ "limit", J::Integer, false, "100", kLimitDoc },
};

// What the project's model textures cost the game (ADR 0046 S18, session/texture_budget_list).
JsonValue answer_texture_budget(const QueryContext &context, const QueryArgs &args, std::string &) {
	const TextureBudgetList list = texture_budget_list(context.core.view());
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("totals", texture_budget_totals_json(list));
	set_page(out, page, list.rows.size());
	JsonValue rows = JsonValue::make_array();
	for (size_t i = page.first(list.rows.size()); i < page.last(list.rows.size()); ++i)
		rows.push(texture_budget_row_json(list.rows[i]));
	out.set("textures", std::move(rows));
	return out;
}

constexpr QueryParam kImportOptionsParams[] = {
	{ "path", J::String, true, nullptr,
			"An import source of the project (a file holding its .import record), or a file an import "
			"makes, by its project-relative path or its logical name." },
};

// An import's options and what its uses ask of it (ADR 0046 S18, session/texture_import_state).
JsonValue answer_import_options(const QueryContext &context, const QueryArgs &args, std::string &error) {
	TextureImportState state;
	if (!texture_import_state(context.core.view(), args.text("path"), state, error)) return JsonValue::make_null();
	return texture_import_state_json(state);
}

JsonValue answer_catalog(const QueryContext &context, const QueryArgs &, std::string &);

constexpr QueryParam kFileCardParams[] = {
	{ "path", J::String, true, nullptr,
			"A project file: its project-relative path, or its name alone (the file the name resolves to)." },
};

JsonValue answer_file_card(const QueryContext &context, const QueryArgs &args, std::string &) {
	return file_card_json(file_card(context.core.view(), args.text("path")));
}

// Who names a file, in one look (the deep-integration plan's DI-05): the Inspector's Used by.
constexpr QueryParam kUsedByParams[] = {
	{ "path", J::String, false, nullptr,
			"A project file, as file_card takes it; left out, the active document's file." },
};

JsonValue answer_used_by(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = args.text("path").empty() ? view.documents.active : args.text("path");
	if (path.empty()) {
		error = "name the file with \"path\" (no document is active).";
		return JsonValue::make_null();
	}
	return file_users_json(file_users(view, path));
}

// The missions that run on an environment (DI-19a): a .env named by its path, else the active document.
constexpr QueryParam kEnvironmentUsesParams[] = {
	{ "path", J::String, false, nullptr, "An environment (.env); left out, the active document's file." },
};

JsonValue answer_environment_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = args.text("path").empty() ? view.documents.active : args.text("path");
	if (path.empty()) {
		error = "name the environment with \"path\" (no document is active).";
		return JsonValue::make_null();
	}
	const AssetEntry *entry = view.project.scan ? view.project.scan->named(path) : nullptr;
	if (entry && entry->kind != AssetKind::Environment) {
		error = path + " is not an environment (.env).";
		return JsonValue::make_null();
	}
	return environment_uses_json(environment_uses(view, path));
}

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
	// The tokens its string params take (QueryChoices).
	template <size_t N>
	constexpr Query chooses(const QueryChoices (&choices)[N]) const {
		Query out = *this;
		out.row.choices = choices;
		out.row.choice_count = N;
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
// A file's page: the graph (who names it, what it names), the files (its kind and size), the findings (its
// rows) and the page showing (the active document's concern).
constexpr ConcernSet kFilePageReads = concern_set({ C::Graph, C::Files, C::Findings, C::ActiveDocument });
// A viewport's answer: its state and the clock, its document and the selection in it, the documents
// open and the active one (a pathless read's), the files its picture read, the graph (a rig's model),
// the project, and the render check's findings (render's screen).
constexpr ConcernSet kViewportConcerns = concern_set({ C::Viewports, C::Documents, C::Selection,
		C::DocumentSet, C::ActiveDocument, C::Files, C::Graph, C::Project, C::Findings });

constexpr EditorQueryRow kRows[] = {
	Query(K::State, "state", answer_state, kStateParams, kEveryConcern,
			"The view by section (the catalog's sections: the status line, the project, the "
			"requirements, the open documents, the selection, the operation, Play, the import "
			"dialog, the dialogs, the problem and graph counts, the preferences, the output lines, "
			"the events held and what the windows show of their own): `view_revision` the view's clock, `revisions` each concern's "
			"stamp (the clock value at which it last moved); with `since`, a view_revision an "
			"earlier answer carried, the sections none of whose concerns moved since left out.")
			.row,
	Query(K::Files, "files", answer_files, kFilesParams, concern_set({ C::Files }),
			"A page of the files the project's scan lists, in its order: each file's path, name, "
			"kind (its asset kind's token) and editable, whether the editor opens it; imported_from, the "
			"source of the import that makes it (S18); with text or kind (an asset kind's token), the files "
			"Files' filter lists for them: the paths holding the text, then, where the text names a kind "
			"(\"texture\", \"waves\", \"kind:texture\"), that kind's files.")
			.pages("files")
			.row,
	Query(K::Documents, "documents", answer_documents, kPageParams, kDocumentReads,
			"The active document and a page of the open documents, each's lifecycle state: path, "
			"kind, dirty, blocked, revision, can_undo, can_redo, ignored_lines, save_words (what a save "
			"changes beyond the edits, in words, where it changes anything) and its source "
			"issues; a record document's also file_state_changed, row_count, last_added and the "
			"kinds of row its outline adds (top_kinds); a text document's its line_count.")
			.pages("documents")
			.row,
	Query(K::Document, "document", answer_document, kDocumentParams, kRecordReads,
			"One open document's lifecycle state (as the documents query gives it) and, for a "
			"record document, a page of its rows, each with its id, kind, name, title (its words "
			"as the outline shows them, the project's names read: a mission's entity by its item's "
			"name and its SSN), change since the save (unchanged, changed, added) and the "
			"collections it holds, their records at every depth; for a text document, by the same "
			"offset and limit, a page of its lines (each its line, from 1, and its text); for a texture, "
			"texture: its reader, whether the game loads it and why not, its sides, levels, palette "
			"size, alpha and facts in words.")
			.pages("rows, or a text document's lines")
			.row,
	Query(K::Record, "record", answer_record, kRecordParams, kRecordReads,
			"One record of an open record document, by its id or by the symbol it defines: its id, "
			"address (row, kind, child), name, title (its words), path, locator, change since the "
			"save, owner and index, every field as it applies to it (value, and display, what the "
			"value names in words where it names something: an item by its catalog's name, an SSN "
			"by its entity, a zone, an event, a text key by its string; dangling where it names "
			"nothing; label, unit, range, choices, whether an optional one is present, a "
			"reference's status, what it defines, and a changed one's saved value) and the "
			"collections it holds.")
			.row,
	Query(K::ReferenceChoices, "reference_choices", answer_reference_choices, kFieldParams,
			kRecordReads,
			"A page of the names a record's reference field's picker offers there: each with "
			"its name, its label (what it names in words: an item id by its catalog's name, an "
			"SSN by its entity's title), the kind it names, the file and record that define it, "
			"the status the field set to it would have, and why no lookup of the game finds an "
			"inert one.")
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
			"notes; total, shown (the rows matching), counts by severity (and blocking, the rows a "
			"build is refused for), a page of the rows, each with what it is about, blocks_build "
			"and why where a build is refused for it, and its fixes ({label, detail, bulk, request}: "
			"the request an editor_request passes back as it is), and grouped, the page's groups.")
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
			"case, files first, each with its usages; an item also by its catalog's name (its words).")
			.pages("hits")
			.row,
	// The plain-words lane (the audit's 4.6).
	Query(K::FilePage, "file_page", answer_file_page, kFilePageParams, kFilePageReads,
			"A file's page as the Document window shows it for a kind the editor has no editor for (path "
			"left out: the page showing, the documents section's page): its name, kind and size, what it "
			"holds and what in the game reads it (cite: the witness), where a build puts it, what the "
			"editor does with it, its Problems rows' errors and warnings, who names it (used_by) and what "
			"it names (names), each {text, missing?, file?, locator?}.")
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
	Query(K::Viewport, "viewport", answer_viewport, kViewportParams, kViewportConcerns,
			"A document's viewport (of the kind named, else the Preview's kind that shows it, else "
			"its Main view), followed first so it answers the document as it is now (a first read "
			"of a document makes its viewport, at its kind's defaults, and what its follow derives "
			"moves the viewports concern), by op. state: the envelope (kind, path, as_saved, "
			"status and reason, message, detail, revision, shown_revision, current, builds, units, "
			"device {attached, width, height, canvas_sized}, options, camera, clock {playing, rate, "
			"time_ms, ticks}, body, a page of its items and by the same page its notes, "
			"note_count). items or notes: a page of one (a menu's widgets, each with its rect and "
			"the device_rect the Shell's device placed it at; a model's markers, each with its "
			"record, position and picture pixel; a menu's compiler notes), while the picture is "
			"current. hit: the item under the point x, y (viewport and path, the viewport's kind "
			"and document; kind, index, id, name, current, the item's; a menu's pointer, the game's "
			"pointer with the mouse there: drawn, and file, width, height, window and name, the window "
			"whose CURSOR it is; a mission's ground). render: one row of the "
			"document as its kind renders it apart (a menu's screen as the render check compiled "
			"it, the menu_render query's answer; a model's whole document is its picture, and it "
			"refuses). palette: what a mission's Place tool places (count, matching, groups {group, "
			"words, pool, count}, a page of items {item, name, type, type_words, group, pool, model, "
			"file, recent}: the recently placed first, then each TYPE's group by name; text a "
			"search); another kind refuses it. A viewport neither the Preview window nor a canvas "
			"draws holds no device (device.attached false, no device_rect). Its changes are "
			"set_viewport's and edit_in_viewport's.")
			.pages("items, notes or render's widgets")
			.chooses(kViewportChoices)
			.row,
	Query(K::ImportPreview, "import_preview", answer_import_preview, kImportPreviewParams,
			concern_set({ C::Dialogs, C::Preferences, C::Files, C::Workspace }),
			"The import dialog's preview: open, with_dependencies, all (every file of the game "
			"install chosen, with no walk), a page of its plan's rows in "
			"its order, the chosen files first (state, name, kind, source, destination, size, "
			"made_from, needed_by (with its words: the record and the field as the file's type "
			"words them), found_in, selected, held (a chosen file the project has, kept unless the "
			"import replaces) and held_same (whether the project's holds the same bytes, where the "
			"plan compared them), problem, rivals, group, index (its place in the plan, what the "
			"workspace's import check and uncheck name) and checked (the dialog's check, the "
			"workspace's: what Import and import_files planned take); those of one kind alone "
			"with kind, count theirs), total_bytes (what the whole plan copies), summary (the "
			"whole plan's files by kind, the largest first: kind, files, bytes) and groups (its rows "
			"by what they come for: each chosen file, the kinds of the files it brings under it: "
			"depth, parent, chosen, kind, files, bytes), by the same page what it offers (choices, "
			"each with its kind and size) and chose (roots) and the files not found, each list with its "
			"own count (next_offset runs to the end of the longest), then the kinds not followed, "
			"the symbols no place defines (undefined), those only a place's copy of a file the "
			"project has defines, its own being kept (shadowed), truncated and the plan's findings.")
			.pages("rows")
			.row,
	Query(K::Output, "output", answer_output, kOutputParams, concern_set({ C::Output }),
			"A page of the output lines by absolute index: first (the oldest held), next (one "
			"past the newest), cursor (the page's first) and next_cursor. Paging by next_cursor "
			"repeats no line; the log keeps its last 2000, so a client more than 2000 lines behind "
			"misses the lines dropped, the cursor coming back larger than it asked. A line with others "
			"folded under it (an import's files, the game's log) is listed in folded ({at, count}); "
			"unfold pages them.")
			.pages("lines")
			.row,
	Query(K::Operation, "operation", answer_operation, concern_set({ C::Operation }),
			"The operation that runs (running, and while one does its id, kind (open, refresh, "
			"build, import_plan, import_apply, rename_apply), label, done and total in its unit, "
			"cancellable, and what it reads and writes), what the last one came to "
			"(last_operation: id, kind, end, findings), the validation the polls step "
			"(validation: running, done and total files) and the last build.")
			.row,
	Query(K::BuildGate, "build_gate", answer_build_gate, kPageParams,
			concern_set({ C::Project, C::Files, C::Findings }),
			"What a build would be refused for over the files as last scanned, nothing built: "
			"blocked (a build would not pack) and a page of the findings that block it, the errors "
			"among the Problems rows the build gates on whose code gates (the catalog's "
			"gates_build), the scan's and the requirements', and the "
			"build's own checks of the files (an archive in the project, a name no archive can "
			"store). The build follows retail: it is refused where the built game would fail to load "
			"or run as retail does, each such row citing the original's refusal, and where the "
			"editor cannot vouch for what it packs; every other code is listed and blocks nothing, "
			"as does a Problems row the build does not gate on (a project check's: the render "
			"check's). A listed code blocks where its subject names the refusal: a reference, missing "
			"or naming a file of another kind, of a kind whose row cites it (gates_when_missing: a "
			"mission's terrain; by its role, a terrain's colour map and its blend map), and a required "
			"file missing or of another kind whose manifest row "
			"is the boot's refusal (its fatal rows). A code that says the file does not serialize "
			"(blocks_save) blocks nothing over a file that is the game's own bytes with no unsaved edits: "
			"the build packs it as stored, never through the editor's writer. The query runs the "
			"validation left due to its end first, and the check of which files are the game's own data, "
			"so the rows it reads are the files' as they stand. A build request reads changed files "
			"again first, joins a build that runs and waits on unsaved edits, which the gate does "
			"not weigh.")
			.pages("blocking")
			.row,
	// Events are posted beside a Selection, a Dialogs or a Workspace change (view_revisions.h).
	Query(K::Events, "events", answer_events, kCursorParams,
			concern_set({ C::Selection, C::Dialogs, C::Workspace }),
			"A page of the view events by seq (the one-shot asks a request makes of a window): "
			"first, next, cursor, next_cursor and the items, each its seq, kind (reveal_record, "
			"reveal_text, reveal_file, ask_rename, settings_applied, import_planned, build_ended, "
			"open_externally, reveal_preview, focus_window) and the fields its kind sets (a reveal_text's "
			"locator, line:column; a focus_window's path, the window). The last 64 are held: a client "
			"more than 64 behind misses the events dropped, the cursor coming back larger than it "
			"asked.")
			.pages("items")
			.row,
	Query(K::MissionLogic, "mission_logic", answer_mission_logic, kMissionLogicParams, kRecordReads,
			"A mission's logic without its format (ADR 0046 S15), by op. form: an event's (its sentence and its "
			"parts, when, then, delay, repeat; repeats, at_start, at_end; delay_steps and repeat_steps with their "
			"seconds and most_steps; its triggers' and actions' counts, the most an event holds and why it takes "
			"no more, trigger_refusal and action_refusal), or a trigger's or an action's (its type by title and "
			"group, a trigger's negation, its join to the next and whether it is the last, the params its type "
			"reads, each its slot, field, kind, label, value, the value in words and its unit, those it does not "
			"read that hold a value, its words and its event's sentence). types: what Add trigger or Add action "
			"offers (list), each type, sub, title, group and tip, in their groups' order. add, retype, move, "
			"negate, join: the edits that add a record of a type with its kinds' defaults, give one another type "
			"keeping what still applies, move it (to another event: added there and removed here, one batch), "
			"negate it or join it to the next, in the batch form an edit_record takes back, or the refusal that "
			"says why none (an event holding the most it holds).")
			.pages("types")
			.chooses(kLogicChoices)
			.row,
	Query(K::MissionUses, "mission_uses", answer_mission_uses, kMissionUsesParams, kRecordReads,
			"What happens when (ADR 0046 S15): the events whose triggers or actions name a record of a mission "
			"(an entity by its SSN, an area by its zone id, a waypoint path, an event, a group), each its event "
			"(address and index), the record and the field naming it, the record's words and its event's "
			"sentence; what the record is to them, and why a second holder of an SSN or a zone id is named by "
			"none (inert).")
			.pages("uses")
			.row,
	Query(K::ScriptAssist, "script_assist", answer_script_assist, kScriptAssistParams, kRecordReads,
			"A script's help from the WAC compiler's tables and the project (ADR 0046 S15), by op. complete: what "
			"may complete the word at line, column (the caret): the word's column and what is typed, what is "
			"expected there in words, and a page of the items (label, insert, kind: command, keyword, entity, "
			"area, path, group, text key, effect, ammo; detail), the commands and keywords where a statement goes, "
			"else the names the command's parameter takes (the mission of the script's name's entities by SSN, "
			"areas by zone id, paths; the script groups; the graph's text keys, effects and ammo). hover: what the "
			"word at line, column is, in words (word, column, length, text; text \"\" for none). definition: the "
			"request that goes where it is defined (request, null for none). mission_script: the script a "
			"mission's name finds (name, path where the project holds it, held, create_at beside the mission).")
			.pages("items")
			.chooses(kScriptChoices)
			.row,
	// The roles are compiled in: no concern moves what it answers, Project the one it is stamped by.
	Query(K::TextureRoles, "texture_roles", answer_texture_roles, kPageParams, concern_set({ C::Project }),
			"Every way the game uses a texture (ADR 0046 S18), a page of the roles in the catalog's order: each "
			"its role token, words, group, loader (the name rule that picks the file and its reader: stage, "
			"plain, normal, producer, chunk, archive, hud, file, menu, ptl, tga, pcx, pcx8, cube), the "
			"extensions it takes (formats), its size rule {rule, words, width, height}, what its alpha means, "
			"whether its loader reads the alpha at all, how it is sampled, what the game does when the file is "
			"missing or wrong, and the witness.")
			.pages("roles")
			.row,
	Query(K::TextureThumbnail, "texture_thumbnail", answer_texture_thumbnail, kThumbnailParams,
			concern_set({ C::Files, C::Project }),
			"A texture file as the windows' thumbnails show it (ADR 0046 S18): read by the reader its name "
			"picks, what the use's loader makes of its texels applied (transform), shrunk to fit 128 pixels a "
			"side by averaging; made now when the cache lacks it for the file as the scan last read it. file, "
			"state (ready, or unloadable: refusal says why the game cannot load it), transform, width and "
			"height (the picture's), source_width, source_height, levels, format, texels and alpha in words, "
			"and png, the picture as a base64 PNG.")
			.chooses(kThumbnailChoices)
			.row,
	Query(K::TextureUses, "texture_uses", answer_texture_uses, kTextureUsesParams,
			concern_set({ C::Graph, C::Files, C::Documents, C::DocumentSet }),
			"What uses a texture file (ADR 0046 S18): a page of its uses, the graph's references whose loader "
			"names it (those that load it and those whose name is the file's though the loader opens another, "
			"a .tga beside the .dds the game takes: reads_file false) and the names the game opens itself; each "
			"its role token, words (the role and where: a model's material and its shader and cut-out, a "
			"terrain's key, a HUD keyword), referrer, record, locator and field (none for a fixed name: fixed, "
			"fixed_for, witness), name_written, load {file, reader, transform}, served (the project file the "
			"loader opens) and context (a model row's material, slot, type, row_flags, shader, alpha_test, "
			"alpha_ref; key; hud_mode).")
			.pages("uses")
			.row,
	Query(K::TextureBudget, "texture_budget", answer_texture_budget, kTextureBudgetParams,
			concern_set({ C::Graph, C::Files, C::Documents, C::DocumentSet }),
			"What the project's model textures cost the game (ADR 0046 S18): totals {detail (the bytes of every "
			"texture at each object texture detail level, 0 the lowest to 3 full), as_dds (the same at full "
			"detail had every texture whose loader reads a .dds first that .dds), textures, past_warning (how "
			"many hold more than the texture.memory warning)} and a page of textures, the costliest first at "
			"full detail: each texture the game makes for the model rows (one a name written, any case, the "
			"stage and plain loaders sharing it, the normal-map loader's apart), its name (as its first row "
			"writes it), file (the file its loader opens), uses (how many rows take it) and its budget as "
			"texture_uses gives it.")
			.pages("textures")
			.row,
	Query(K::ImportOptions, "import_options", answer_import_options, kImportOptionsParams,
			concern_set({ C::Files, C::Graph, C::Documents, C::DocumentSet }),
			"An import's options and what its uses ask of it (ADR 0046 S18): its source, record, "
			"importer and version; the record's options and every option's value in effect; its option "
			"rows (key, label, words, values {token, words}, forms, fallback, applies {option, values}, "
			"applies_now); the files it makes; and needs, what the uses of those files and of every "
			"name of the source's stem ask of it (options, reasons, conflicts, uses).")
			.row,
	Query(K::ModelSurfaces, "model_surfaces", answer_model_surfaces, kModelSurfacesParams, kRecordReads,
			"A model's bullet-face surfaces on its materials (ADR 0046 S17), by op. materials: the surfaces a face "
			"takes by name (surface, name, the effects row tag it plays, note: what else the game does with it), the "
			"flags a material sets (flag, bit, label, tip), the model's faces and those no material made "
			"(without_material), its collision LOD, and a page of its materials (id, index, title, faces, surface "
			"(null where none or mixed), words, mixed, counts of each surface, each flag's count, differing). set, "
			"flag: the edits that give every face made from the material `id` the surface, or set or clear the "
			"flag on them, in the batch form an edit_record takes back (one undo step), or the refusal that says "
			"why none. faces: a page of the material's faces whose surface is not its common one (id, index, "
			"surface, name).")
			.pages("materials")
			.chooses(kSurfaceChoices)
			.row,
	Query(K::Catalog, "catalog", answer_catalog, concern_set({ C::Findings }),
			"What the session answers and takes: every request kind with the fields it takes and "
			"needs, who serves it and what it does; every request field; the batch form of a "
			"request's edits (batch: its forms, its ops, each with the form it is read in, and the "
			"members an edit takes, each with its JSON type, the forms that read it, its least "
			"value or the one string it takes); what the windows show of their own (workspace: the parts a "
			"set_workspace names, each with its members, their JSON types and docs, and the windows its "
			"focus brings forward); every query with its "
			"params, the list it pages and the concerns it reads; the state's sections; the view's "
			"concerns; and every finding code the session and the document types know (the "
			"editor's own table's, then each type's): its code, its table (core or the type's "
			"name), the fixes Problems offers, what a Rewrite does, whether the finding says the "
			"file does not serialize (blocks_save), whether an error of it refuses a build "
			"(gates_build: the build follows retail, so true where the game's refusal is witnessed "
			"or the editor cannot vouch for what it packs, false for a listed code, whose errors "
			"still block where their subject names the refusal: a reference of a kind whose row "
			"cites it, a mission's terrain; a required file whose manifest row is the boot's; a "
			"blocks_save code's errors block nothing over the game's own bytes held unedited), "
			"where Problems takes it (content or file), the "
			"group it shows under (its key), where it comes from (source), for a render check's note "
			"that is a Problems row its severity (problem: info or warning, left out for none) and "
			"how many of the findings held now carry it (count); a row a held finding carries that "
			"no table lists comes last, as table none.")
			.row,
	Query(K::FileCard, "file_card", answer_file_card, kFileCardParams,
			concern_set({ C::Files, C::Graph, C::Project, C::Operation }),
			"A project file as Files' card shows it (the UX round's project lane): found, its path, name, "
			"kind and kind_label, about (what a file of its kind is to the game), size, build (where a build "
			"puts it, in words), imported_from, opens (the editor opens a document of it), a wave's sound "
			"as the game decodes it {decoded, error, rate, channels, seconds}, names (what it names: field, "
			"record, value, status in words, the file it resolves to, whether that file is a wave), "
			"named_by (file, record, field), and reading: true while the project's references are being read "
			"(names and named_by then as far as the graph has read).")
			.row,
	Query(K::UsedBy, "used_by", answer_used_by, kUsedByParams,
			concern_set({ C::Files, C::Graph, C::Project, C::Operation, C::ActiveDocument }),
			"Who names a file, in one look, as the Inspector shows it with nothing selected (the deep-"
			"integration plan's DI-05): found, its path and name, count (its uses: the records naming it, then "
			"those naming what it defines) and further_count, files (by the naming file: file, name, uses), "
			"each use's words (the record and its field in words), field, the file and locator Go to opens "
			"(editable false: shown in Files), and further, one hop on where the naming record defines what "
			"others name (an item naming a model: the mission entities placing it), by file likewise; reading "
			"while the project's references are being read.")
			.row,
	Query(K::EnvironmentUses, "environment_uses", answer_environment_uses, kEnvironmentUsesParams,
			concern_set({ C::Files, C::Graph, C::Project, C::Documents, C::ActiveDocument }),
			"The missions that run on an environment (the deep-integration plan's DI-19a), as its Inspector shows "
			"them: path, found, reading (the project's references not read yet), and missions, each its file "
			"(mission), its header's name (title), the locator and field Go to opens it at (its environment field), "
			"the terrain it pairs it with (name, file where the project has it, field, water_height in metres where "
			"the terrain reads), the overrides its header sets over the environment (field: the header's field, "
			"words), start_time (8.8 hours) and minutes_per_day with the clock in words, and water: from (mission, "
			"terrain, environment, none: the game's ladder), height in metres and words.")
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

// A row's choices (QueryChoices) each name a string param of the row, once.
constexpr bool choices_named() {
	for (const EditorQueryRow &row : kRows)
		for (size_t c = 0; c < row.choice_count; ++c) {
			const QueryChoices &choice = row.choices[c];
			bool named = false;
			for (size_t p = 0; p < row.param_count; ++p)
				named = named || (same_text(row.params[p].name, choice.param) && row.params[p].type == J::String);
			if (!named || !choice.token) return false;
			for (size_t d = c + 1; d < row.choice_count; ++d)
				if (same_text(choice.param, row.choices[d].param)) return false;
		}
	return true;
}
static_assert(choices_named(), "a query's choices each name a string param of its own, once");

// The viewport query's ops (S13 V7): one row per ViewportOp in its order, each a token of its own
// and an answer, what it needs among what it takes, a point no page; every param of the query one
// every op takes or one an op takes by its flag (none taken unnamed).
constexpr bool viewport_ops_hold() {
	if (std::size(kViewportOps) != static_cast<size_t>(ViewportOp::kCount)) return false;
	for (size_t i = 0; i < std::size(kViewportOps); ++i) {
		const ViewportOpRow &op = kViewportOps[i];
		if (op.op != static_cast<ViewportOp>(i) || !op.token[0] || !op.answer) return false;
		if ((op.needs & ~op.takes) || ((op.takes & kViewportPoint) && (op.takes & kViewportPage))) return false;
		for (size_t j = i + 1; j < std::size(kViewportOps); ++j)
			if (same_text(op.token, kViewportOps[j].token)) return false;
	}
	for (const QueryParam &param : kViewportParams)
		if (viewport_param_flag(param.name) == 0xFF) return false;
	return true;
}
static_assert(viewport_ops_hold(),
		"the viewport ops follow ViewportOp, each a token, an answer and what it needs among what it takes, "
		"every param of the query taken by name");

// Whether a param's default reads as its type: a whole number's its digits (a '-' before them), a
// number's digits with one '.' among them, a flag's true or false; a text's anything.
constexpr bool default_reads(const QueryParam &param) {
	const char *text = param.default_value;
	if (!text) return true;
	switch (param.type) {
		case J::Integer:
		case J::Number: {
			if (*text == '-') ++text;
			bool digit = false, point = false;
			for (; *text; ++text) {
				if (*text >= '0' && *text <= '9') digit = true;
				else if (*text == '.' && param.type == J::Number && !point) point = true;
				else return false;
			}
			return digit;
		}
		case J::Boolean: return same_text(text, "true") || same_text(text, "false");
		default: return true;
	}
}
constexpr bool defaults_read() {
	for (const EditorQueryRow &row : kRows)
		for (size_t p = 0; p < row.param_count; ++p)
			if (!default_reads(row.params[p])) return false;
	return true;
}
static_assert(defaults_read(), "every param's default reads as its type");

// A param's default as its type reads it (strutil's parse, ADR 0049 d5; every default reads,
// static_asserted above); 0 for a param with none.
int64_t integer_default(const QueryParam &param) {
	if (!param.default_value) return 0;
	return static_cast<int64_t>(strutil::parse_llong(param.default_value).value_or(0));
}
double number_default(const QueryParam &param) {
	if (!param.default_value) return 0.0;
	return strutil::parse_double(param.default_value).value_or(0.0);
}

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
			case J::Number: {
				// One a float holds: a point read as a float is never converted from past its range.
				float held = 0.0f;
				typed = io::json_float(value, held);
				break;
			}
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
		// A param with choices takes one of its tokens.
		for (size_t c = 0; c < row.choice_count; ++c) {
			const QueryChoices &choice = row.choices[c];
			if (!same_text(choice.param, member.key.c_str())) continue;
			std::string tokens;
			bool chosen = false;
			for (size_t i = 0; const char *token = choice.token(i); ++i) {
				tokens += (i ? ", " : "") + std::string(token);
				chosen = chosen || value.string == token;
			}
			if (!chosen) {
				error = "\"" + member.key + "\" is one of " + tokens + ", not \"" + value.string + "\".";
				return false;
			}
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
			return json_number(double(integer_default(param)));
		case J::Number:
			return json_number(number_default(param));
		case J::Boolean:
			return JsonValue::make_bool(text == "true");
		default:
			return json_string(text);
	}
}

JsonValue answer_catalog(const QueryContext &context, const QueryArgs &, std::string &) {
	JsonValue out = JsonValue::make_object();
	request_catalog_json(out);
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
			// The tokens a param with choices takes (the viewport query's op and kind).
			for (size_t c = 0; c < row.choice_count; ++c) {
				if (!same_text(row.choices[c].param, param.name)) continue;
				JsonValue tokens = JsonValue::make_array();
				for (size_t i = 0; const char *token = row.choices[c].token(i); ++i)
					tokens.push(json_string(token));
				item.set("enum", std::move(tokens));
			}
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
	view_catalog_json(out, context.core.view().findings.diagnostics);
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
	return param ? integer_default(*param) : 0;
}

double QueryArgs::number(const char *name) const {
	if (const JsonValue *member = value(name); member && member->is_number())
		return member->number;
	const QueryParam *param = param_of(row_, name);
	return param ? number_default(*param) : 0.0;
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
		case QueryJson::Number:
			return "number";
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
