// The query seam (ADR 0046 S13 A5): every read of the session is a row of one table, asked by
// name through ProjectSession::query, and every write a request, in its wire form through
// ProjectSession::handle_json. Paging is the queries' own: the files, the problems, a document's
// rows and its search hits, the project's search hits, the graph's edges and symbols, a menu's
// windows and widgets and the events each come a page at a time, the pages concatenating to the
// whole list with no gap and no repeat, `count` the whole length, and an offset or a limit out of
// range refused naming the query. The state comes by section, and with `since` only the sections
// a concern of which moved since that revision (each concern moving only with its own changes,
// S13 D1). The catalog names every request kind, request field, query (with its params), section
// and concern from the tables themselves. The menu reads editor_menu served are the menu_tree,
// menu_findings and menu_render queries; its batch is edit_record's wire form: records by identity
// or label, kinds by token, a list replaced, the outcome naming what each label made and every
// record added, one undo step, refusals by the edit's place, nothing committed when the document
// or an operation refuses it.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/editor_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::NoProcess;

JsonValue parse(const std::string &text) {
	JsonValue out;
	std::string error;
	if (!opennova::io::json_parse(text, out, error)) std::printf("  bad test JSON: %s\n", error.c_str());
	return out;
}

// A query's answer (null, and the refusal printed, when it was refused).
JsonValue ask(ProjectSession &session, const char *name, const std::string &args = "{}") {
	std::string error;
	JsonValue answer = session.query(name, parse(args), error);
	if (!error.empty()) std::printf("  %s refused: %s\n", name, error.c_str());
	return answer;
}

// What a query refused, "" when it answered.
std::string refusal(ProjectSession &session, const char *name, const std::string &args) {
	std::string error;
	session.query(name, parse(args), error);
	return error;
}

// A request in its wire form.
JsonValue send(ProjectSession &session, const std::string &json) {
	return session.handle_json(parse(json));
}

bool done(const JsonValue &answer) {
	const JsonValue *outcome = answer.get("outcome");
	return answer.get_bool("ok", false) && outcome && outcome->get_bool("done", false);
}

bool refused_with(const JsonValue &answer, const char *says) {
	const std::string error = answer.get_string("error", "");
	if (error.find(says) == std::string::npos) std::printf("  refusal said: %s\n", error.c_str());
	return !answer.get_bool("ok", true) && error.find(says) != std::string::npos;
}

uint64_t id_of(const JsonValue &object, const char *key) {
	const JsonValue *value = object.get(key);
	return value && value->is_number() ? uint64_t(value->number) : 0;
}

// Every page of a paged query, `limit` entries each, read by next_offset: the entries in order.
// `consistent` stays true while every page says the same whole count, starts where the last ended
// and holds `limit` entries but the last.
std::vector<JsonValue> every_page(ProjectSession &session, const char *name, JsonValue args,
		const char *key, size_t limit, size_t &count, bool &consistent) {
	std::vector<JsonValue> items;
	count = 0;
	consistent = true;
	size_t offset = 0;
	for (int guard = 0; guard < 1000; ++guard) {
		if (!args.is_object()) args = JsonValue::make_object();
		args.set("offset", JsonValue::make_number(double(offset)));
		args.set("limit", JsonValue::make_number(double(limit)));
		std::string error;
		const JsonValue page = session.query(name, args, error);
		if (!error.empty() || !page.get(key)) {
			std::printf("  %s page from %zu: %s\n", name, offset, error.c_str());
			consistent = false;
			return items;
		}
		const size_t total = size_t(page.get_number("count", -1.0));
		if (guard == 0) count = total;
		const std::vector<JsonValue> &list = page.get(key)->array;
		const JsonValue *next = page.get("next_offset");
		consistent = consistent && total == count && size_t(page.get_number("offset", -1.0)) == offset &&
				(next->is_null() || (list.size() == limit && size_t(next->number) == offset + limit));
		items.insert(items.end(), list.begin(), list.end());
		if (!next || next->is_null()) return items;
		offset = size_t(next->number);
	}
	consistent = false;
	return items;
}

// The pages of `name` at `limit` concatenate to the one page that holds the whole list.
bool pages_concatenate(ProjectSession &session, const char *name, const std::string &args, const char *key,
		size_t limit, size_t at_least) {
	size_t count = 0;
	bool consistent = false;
	const std::vector<JsonValue> paged = every_page(session, name, parse(args), key, limit, count, consistent);
	JsonValue whole_args = parse(args);
	whole_args.set("limit", JsonValue::make_number(200.0));
	std::string error;
	const JsonValue whole = session.query(name, whole_args, error);
	const JsonValue *list = whole.get(key);
	bool same = list && list->array.size() == paged.size() && paged.size() == count;
	for (size_t i = 0; same && i < paged.size(); ++i)
		same = opennova::io::json_write(paged[i]) == opennova::io::json_write(list->array[i]);
	if (!same || !consistent || count < at_least)
		std::printf("  %s pages: %zu entries of %zu counted, %s, at least %zu wanted\n", name, paged.size(), count,
				consistent ? "consistent" : "inconsistent", at_least);
	return same && consistent && count >= at_least;
}

// The sections a state answer holds.
std::set<std::string> sections_of(const JsonValue &state) {
	std::set<std::string> out;
	for (const opennova::io::JsonMember &member : state.object)
		if (member.key != "revision" && member.key != "revisions") out.insert(member.key);
	return out;
}

// The sections that follow a concern of which moved between two views' revisions.
std::set<std::string> sections_moved(const ViewRevisions &before, const ViewRevisions &after) {
	std::set<std::string> out;
	for (size_t s = 0; s < kViewSectionCount; ++s) {
		const ViewSectionRow &row = view_section_row(static_cast<ViewSection>(s));
		for (size_t c = 0; c < kViewConcernCount; ++c) {
			const ViewConcern concern = static_cast<ViewConcern>(c);
			if ((row.concerns & concern_bit(concern)) && before.of(concern) != after.of(concern)) out.insert(row.token);
		}
	}
	return out;
}

const JsonValue *window_named(const JsonValue &tree, const char *name) {
	const JsonValue *screens = tree.get("screens");
	for (size_t s = 0; screens && s < screens->array.size(); ++s) {
		const JsonValue *windows = screens->array[s].get("windows");
		for (size_t w = 0; windows && w < windows->array.size(); ++w)
			if (windows->array[w].get_string("name", "") == name) return &windows->array[w];
	}
	return nullptr;
}

int rect_edge(const JsonValue &window, size_t edge, const char *which = "rect") {
	const JsonValue *rect = window.get(which);
	return rect && rect->array.size() == 4 ? int(rect->array[edge].number) : -99999;
}

int list_count(const JsonValue &window, const char *list) {
	const JsonValue *lists = window.get("lists");
	return lists ? lists->get_int(list, 0) : -1;
}

// The draw kind and the custom-draw flag of the one column body `owner` holds.
bool body_draw(const MnuDocument &menu, const NodeAddress &owner, std::string &display, int64_t &custom) {
	for (const Document::Collection &collection : menu.collections_of(owner)) {
		if (std::string(menu.kind_token(collection.spec.kind)) != "column.body" || collection.ids.size() != 1) continue;
		const NodeAddress body = menu.address_of(collection.ids.front());
		Value value;
		if (!menu.get(body, "display", value) || !std::holds_alternative<std::string>(value)) return false;
		display = std::get<std::string>(value);
		if (!menu.get(body, "custom_draw", value) || !std::holds_alternative<int64_t>(value)) return false;
		custom = std::get<int64_t>(value);
		return true;
	}
	return false;
}

} // namespace

// Every list a query serves comes a page at a time, and the pages make the list.
static int test_paging() {
	editor_test::TempProjectDir dir("opennova_editor_query_paging");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Pages"));
	// A new project's problems: every required file missing, each an error, and the notes.
	TEST_EXPECT(pages_concatenate(session, "problems", "{}", "problems", 3, 7));
	TEST_EXPECT(pages_concatenate(session, "problems", R"({"group": "kind", "severities": ["error"]})", "problems", 2, 4));
	editor_test::create_missing_files(session);
	TEST_EXPECT(pages_concatenate(session, "files", "{}", "files", 3, 7));
	// A document's rows (a stylesheet's lines) and the fields its search finds.
	session.handle(request::open_document("menu_style.mns"));
	TEST_EXPECT(pages_concatenate(session, "document", R"({"path": "menu_style.mns"})", "rows", 3, 7));
	TEST_EXPECT(pages_concatenate(session, "document_search", R"({"path": "menu_style.mns", "text": "DEF"})", "hits", 2, 4));
	TEST_EXPECT(pages_concatenate(session, "project_search", R"({"text": "DEF"})", "hits", 2, 4));
	// The graph's lists: what the menu names, who uses the stylesheet, the names it defines.
	TEST_EXPECT(pages_concatenate(session, "references", R"({"path": "main.mnu"})", "edges", 2, 3));
	TEST_EXPECT(pages_concatenate(session, "usages", R"({"path": "menu_style.mns"})", "edges", 2, 3));
	TEST_EXPECT(pages_concatenate(session, "symbols", "{}", "symbols", 3, 7));
	TEST_EXPECT(pages_concatenate(session, "symbols", R"({"kind": "style_var"})", "symbols", 2, 4));
	TEST_EXPECT(pages_concatenate(session, "documents", "{}", "documents", 1, 1));
	// Three references to files the project lacks: `missing` pages them, its count the graph's own.
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	NodeAddress main;
	TEST_EXPECT(find_definition(*session.view().findings.graph, *menu, "MAIN", main));
	const std::string m = std::to_string(main.child);
	TEST_EXPECT(done(send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [
		{"op": "add", "kind": "sound", "parent": )" + m + R"(, "as": "a"},
		{"op": "set", "id": "a", "field": "file", "value": "one.lwf"},
		{"op": "add", "kind": "sound", "parent": )" + m + R"(, "as": "b"},
		{"op": "set", "id": "b", "field": "file", "value": "two.lwf"},
		{"op": "add", "kind": "sound", "parent": )" + m + R"(, "as": "c"},
		{"op": "set", "id": "c", "field": "file", "value": "three.lwf"}]})")));
	TEST_EXPECT(pages_concatenate(session, "missing", "{}", "edges", 2, 3));
	TEST_EXPECT(ask(session, "missing").get_number("count", -1.0) ==
			double(session.view().findings.graph->missing_count()));
	TEST_EXPECT(pages_concatenate(session, "referrers", R"({"kind": "style_var", "name": "DEF_TEXT_FG"})", "edges", 1, 2));

	// The events by seq: seven asks of Files, three at a time by next_cursor, each once, in order.
	const uint64_t from = session.view().events.next_seq();
	for (int i = 0; i < 7; ++i) session.handle(request::show_in_files("main.mnu"));
	std::vector<uint64_t> seqs;
	uint64_t cursor = from;
	for (int guard = 0; guard < 10; ++guard) {
		const JsonValue page = ask(session, "events", R"({"cursor": )" + std::to_string(cursor) + R"(, "limit": 3})");
		for (const JsonValue &item : page.get("items")->array) seqs.push_back(uint64_t(item.get_number("seq", 0.0)));
		const uint64_t next = uint64_t(page.get_number("next_cursor", 0.0));
		TEST_EXPECT(page.get_number("count", -1.0) == double(session.view().events.held().size()));
		if (next == cursor) break;
		cursor = next;
	}
	bool in_order = seqs.size() == 7;
	for (size_t i = 0; in_order && i < seqs.size(); ++i) in_order = seqs[i] == from + i;
	TEST_EXPECT(in_order);
	// A cursor below the oldest held starts at it.
	TEST_EXPECT(ask(session, "events", R"({"cursor": 0, "limit": 1})").get_number("cursor", -1.0) ==
			double(session.view().events.first_seq()));

	// The output lines by absolute index: pages by next_cursor, each line once.
	const JsonValue output = ask(session, "output", R"({"cursor": 0, "limit": 200})");
	const size_t held = output.get("lines")->array.size();
	TEST_EXPECT(held > 3 && output.get_number("count", -1.0) == double(held));
	size_t seen = 0;
	cursor = uint64_t(output.get_number("first", 0.0));
	for (int guard = 0; guard < 1000; ++guard) {
		const JsonValue page = ask(session, "output", R"({"cursor": )" + std::to_string(cursor) + R"(, "limit": 2})");
		seen += page.get("lines")->array.size();
		const uint64_t next = uint64_t(page.get_number("next_cursor", 0.0));
		if (next == cursor) break;
		cursor = next;
	}
	TEST_EXPECT(seen == held);
	return 0;
}

// An offset, a cursor or a limit out of range, a member the query does not take, one it needs
// left out, a wrongly typed one and an unknown query: each refused, naming the query.
static int test_refusals() {
	editor_test::TempProjectDir dir("opennova_editor_query_refusals");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Refusals"));
	const auto says = [&](const char *name, const std::string &args, const char *what) {
		const std::string error = refusal(session, name, args);
		const bool named = error.find(std::string("query ") + name + ":") == 0;
		if (!named || error.find(what) == std::string::npos) std::printf("  %s %s: %s\n", name, args.c_str(), error.c_str());
		return named && error.find(what) != std::string::npos;
	};
	for (const char *name : {"files", "problems", "symbols", "missing", "documents", "import_preview"}) {
		TEST_EXPECT(says(name, R"({"limit": 0})", "\"limit\" is 1 to 200"));
		TEST_EXPECT(says(name, R"({"limit": 201})", "\"limit\" is 1 to 200"));
		TEST_EXPECT(says(name, R"({"offset": -1})", "\"offset\" must be a whole number"));
		TEST_EXPECT(says(name, R"({"offset": 1.5})", "\"offset\" must be a whole number"));
		TEST_EXPECT(says(name, R"({"limit": "3"})", "\"limit\" must be a whole number"));
		TEST_EXPECT(says(name, R"({"page": 1})", "takes no \"page\""));
	}
	TEST_EXPECT(says("output", R"({"cursor": -2})", "\"cursor\" must be a whole number"));
	TEST_EXPECT(says("events", R"({"limit": 0})", "\"limit\" is 1 to 200"));
	TEST_EXPECT(says("references", "{}", "needs \"path\""));
	TEST_EXPECT(says("references", "null", "needs \"path\""));
	TEST_EXPECT(says("menu_render", R"({"path": "main.mnu"})", "needs \"screen\""));
	TEST_EXPECT(says("files", "[]", "its args are an object"));
	TEST_EXPECT(says("state", R"({"sections": ["problems"]})", "no section \"problems\""));
	TEST_EXPECT(says("state", R"({"sections": "status"})", "must be a string[]"));
	TEST_EXPECT(says("operation", R"({"offset": 0})", "takes no \"offset\" (it takes nothing)"));
	TEST_EXPECT(says("document", R"({"path": "nothing.mnu"})", "no open document nothing.mnu"));
	TEST_EXPECT(says("document_search", R"({"text": ""})", "no document is open"));
	std::string error;
	TEST_EXPECT(session.query("gizmos", JsonValue::make_null(), error).is_null() &&
			error.find("Unknown query \"gizmos\"") == 0 && error.find("files") != std::string::npos);
	// A page past the list: none, the count still all of them, no next page.
	const JsonValue past = ask(session, "problems", R"({"offset": 5000})");
	TEST_EXPECT(past.get("problems")->array.empty() && past.get_number("count", 0.0) > 0 &&
			past.get("next_offset")->is_null());
	return 0;
}

// The Problems query's params (S13 A5: the problems row, which the Problems query's own JSON
// reader became): nothing asked is the default query; each param reads; an empty severity list
// shows nothing; a token the query does not know is refused.
static int test_problems_params() {
	editor_test::TempProjectDir dir("opennova_editor_query_problems");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Problems"));
	const JsonValue all = ask(session, "problems");
	const double total = all.get_number("total", -1.0);
	TEST_EXPECT(total > 0 && all.get_number("shown", -1.0) == total && all.get_number("count", -1.0) == total);
	TEST_EXPECT(all.get_number("revision", -1.0) == double(session.view().revisions.of(ViewConcern::Findings)));
	const JsonValue errors = ask(session, "problems", R"({"severities": ["error"]})");
	TEST_EXPECT(errors.get_number("shown", -1.0) == all.get("counts")->get_number("errors", -2.0));
	const JsonValue grouped = ask(session, "problems",
			R"({"severities": ["error", "info"], "text": "req", "scope": "open_files", "fixable": true, "group": "kind", "offset": 0, "limit": 5})");
	TEST_EXPECT(grouped.is_object() && grouped.get("groups") != nullptr);
	TEST_EXPECT(ask(session, "problems", R"({"severities": []})").get_number("shown", -1.0) == 0.0);
	for (const char *bad : {R"({"severity": "error"})", R"({"severities": "error"})", R"({"severities": ["fatal"]})",
				 R"({"scope": "everything"})", R"({"group": "folder"})", R"({"fixable": 1})", R"({"offset": -1})",
				 R"({"limit": 2.5})", R"({"text": 3})"}) {
		const std::string error = refusal(session, "problems", bad);
		if (error.find("query problems: ") != 0) std::printf("  problems %s answered: %s\n", bad, error.c_str());
		TEST_EXPECT(error.find("query problems: ") == 0);
	}
	return 0;
}

// The state by section, and `since`: a section comes back only when a concern it follows moved
// after the revision a client last read, and a change moves its own concerns alone.
static int test_state_since() {
	editor_test::TempProjectDir dir("opennova_editor_query_state");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "State"));
	editor_test::create_missing_files(session);
	session.handle(request::open_document("main.mnu"));
	const SessionView &view = session.view();

	// Every section by default, each its row's token; `revision` is any, `revisions` each concern.
	const JsonValue everything = ask(session, "state");
	TEST_EXPECT(sections_of(everything).size() == kViewSectionCount);
	TEST_EXPECT(everything.get_number("revision", -1.0) == double(view.revisions.any()));
	for (const ViewConcernRow &row : kViewConcernRows)
		TEST_EXPECT(everything.get("revisions")->get_number(row.token, -1.0) == double(view.revisions.of(row.concern)));
	TEST_EXPECT(everything.get("project")->get_bool("open", false) &&
			everything.get("project")->get_string("title", "") == "State");
	TEST_EXPECT(everything.get("project")->get("files") == nullptr && everything.get("project")->get_number("file_count", 0.0) > 0);
	TEST_EXPECT(everything.get("documents")->get_string("active", "") == view.documents.active);
	// Sections asked by name: those alone.
	TEST_EXPECT(sections_of(ask(session, "state", R"({"sections": ["documents", "problem_counts"]})")) ==
			std::set<std::string>({"documents", "problem_counts"}));

	// Nothing moved since the revision read: no section.
	const uint64_t read = view.revisions.any();
	TEST_EXPECT(sections_of(ask(session, "state", R"({"since": )" + std::to_string(read) + "}")).empty());

	// A record picked: Selection alone moves (the events and the selection follow it); the project,
	// its files, the problems and the graph stay out.
	const Document *menu = session.document_for("main.mnu");
	NodeAddress main;
	TEST_EXPECT(menu && find_definition(*view.findings.graph, *menu, "MAIN", main));
	ViewRevisions before = view.revisions;
	session.handle(request::select_record("main.mnu", main));
	std::set<std::string> answered =
			sections_of(ask(session, "state", R"({"since": )" + std::to_string(before.any()) + "}"));
	TEST_EXPECT(answered == sections_moved(before, view.revisions));
	TEST_EXPECT(answered.count("selection") == 1 && answered.count("project") == 0 && answered.count("requirements") == 0 &&
			answered.count("problem_counts") == 0 && answered.count("graph_counts") == 0);

	// Output's Clear: the output and the status line alone.
	before = view.revisions;
	session.handle(request::clear_output());
	answered = sections_of(ask(session, "state", R"({"since": )" + std::to_string(before.any()) + "}"));
	TEST_EXPECT(answered == sections_moved(before, view.revisions));
	TEST_EXPECT(answered == std::set<std::string>({"status", "output"}));

	// An edit of a window's number: the documents, the status and the output; the files and the
	// preferences stay out.
	before = view.revisions;
	TEST_EXPECT(done(send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "set", "id": )" +
			std::to_string(main.child) + R"(, "field": "position.left", "value": 3}]})")));
	answered = sections_of(ask(session, "state", R"({"since": )" + std::to_string(before.any()) + "}"));
	TEST_EXPECT(answered == sections_moved(before, view.revisions));
	TEST_EXPECT(answered.count("documents") == 1 && answered.count("project") == 0 && answered.count("preferences") == 0);

	// `since` with sections: those of them that moved.
	before = view.revisions;
	session.handle(request::clear_output());
	TEST_EXPECT(sections_of(ask(session, "state", R"({"sections": ["status", "documents"], "since": )" +
			std::to_string(before.any()) + "}")) == std::set<std::string>({"status"}));
	// Each section as its row writes it.
	for (size_t s = 0; s < kViewSectionCount; ++s) {
		const ViewSectionRow &row = view_section_row(static_cast<ViewSection>(s));
		ViewSection back = ViewSection::kCount;
		TEST_EXPECT(view_section_from_token(row.token, back) && back == row.section && row.concerns != 0);
		const JsonValue section = ask(session, "state", std::string(R"({"sections": [")") + row.token + R"("]})");
		TEST_EXPECT(section.get(row.token) &&
				opennova::io::json_write(*section.get(row.token)) ==
						opennova::io::json_write(view_section_to_json(view, row.section)));
	}
	return 0;
}

// The catalog: every request kind with its fields and doc, every field, every query with its params
// (as its row declares them), every section and every concern, from the tables themselves.
static int test_catalog() {
	editor_test::TempProjectDir dir("opennova_editor_query_catalog");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const JsonValue before_project = ask(session, "catalog");
	TEST_EXPECT(before_project.get("finding_codes") && before_project.get("finding_codes")->array.empty());
	session.handle(request::new_project(dir.file("project"), "Catalog"));
	const JsonValue catalog = ask(session, "catalog");
	const JsonValue *requests = catalog.get("requests");
	TEST_EXPECT(requests && requests->array.size() == kEditorRequestKindCount);
	for (size_t i = 0; requests && i < requests->array.size(); ++i) {
		const RequestKindRow &row = request_kind_row(static_cast<EditorRequestKind>(i));
		const JsonValue &entry = requests->array[i];
		TEST_EXPECT(entry.get_string("kind", "") == row.token && entry.get_string("doc", "") == row.doc);
		size_t takes = 0, needs = 0;
		for (size_t f = 0; f < kRequestFieldCount; ++f) {
			takes += row.params.has(static_cast<RequestFieldId>(f)) ? 1 : 0;
			needs += row.params.needs(static_cast<RequestFieldId>(f)) ? 1 : 0;
		}
		TEST_EXPECT(entry.get("takes")->array.size() == takes && entry.get("needs")->array.size() == needs);
		for (const JsonValue &field : entry.get("takes")->array) {
			RequestFieldId id = RequestFieldId::Dir;
			TEST_EXPECT(request_field_from_token(field.string, id) && row.params.has(id));
		}
		const std::string served = entry.get_string("served_by", "");
		TEST_EXPECT((row.served_by == ServedBy::Session) == (served == "session"));
		TEST_EXPECT((row.served_by == ServedBy::ShellNeedsPerson) == (served == "person"));
	}
	const JsonValue *fields = catalog.get("fields");
	TEST_EXPECT(fields && fields->array.size() == kRequestFieldCount);
	for (size_t i = 0; fields && i < fields->array.size(); ++i) {
		const RequestField &field = request_field(static_cast<RequestFieldId>(i));
		TEST_EXPECT(fields->array[i].get_string("field", "") == field.token &&
				fields->array[i].get_string("type", "") == request_json_token(field.json) &&
				fields->array[i].get_string("doc", "") == field.doc);
	}
	const JsonValue *queries = catalog.get("queries");
	TEST_EXPECT(queries && queries->array.size() == kEditorQueryKindCount);
	for (size_t i = 0; queries && i < queries->array.size(); ++i) {
		const EditorQueryRow &row = editor_query_row(static_cast<EditorQueryKind>(i));
		const JsonValue &entry = queries->array[i];
		EditorQueryKind back = EditorQueryKind::kCount;
		TEST_EXPECT(entry.get_string("name", "") == row.token && editor_query_from_token(row.token, back) && back == row.kind);
		TEST_EXPECT(entry.get_string("doc", "") == row.doc && !std::string(row.doc).empty());
		TEST_EXPECT(entry.get_string("list", "") == (row.list_key ? row.list_key : ""));
		TEST_EXPECT(entry.get_string("reads", "") ==
				(row.reads == ViewConcern::kCount ? std::string("any") : std::string(view_concern_token(row.reads))));
		const JsonValue *params = entry.get("params");
		TEST_EXPECT(params && params->array.size() == row.param_count);
		for (size_t p = 0; params && p < params->array.size() && p < row.param_count; ++p) {
			const QueryParam &param = row.params[p];
			const JsonValue &item = params->array[p];
			TEST_EXPECT(item.get_string("name", "") == param.name && item.get_string("type", "") == query_json_token(param.type) &&
					item.get_bool("required", !param.required) == param.required && item.get_string("doc", "") == param.doc);
			TEST_EXPECT((item.get("default") != nullptr) == (param.default_value != nullptr));
		}
		// Every query the catalog names answers (or asks for what it needs) by that name.
		std::string error;
		session.query(row.token, JsonValue::make_null(), error);
		TEST_EXPECT(error.find("Unknown query") == std::string::npos);
	}
	const JsonValue *sections = catalog.get("sections");
	TEST_EXPECT(sections && sections->array.size() == kViewSectionCount);
	for (size_t i = 0; sections && i < sections->array.size(); ++i)
		TEST_EXPECT(sections->array[i].get_string("name", "") == view_section_row(static_cast<ViewSection>(i)).token &&
				!sections->array[i].get("concerns")->array.empty());
	const JsonValue *concerns = catalog.get("concerns");
	TEST_EXPECT(concerns && concerns->array.size() == kViewConcernCount);
	for (size_t i = 0; concerns && i < concerns->array.size(); ++i)
		TEST_EXPECT(concerns->array[i].string == kViewConcernRows[i].token);
	// The codes of the findings the session holds, each once with its rows.
	std::set<std::string> codes;
	for (const Diagnostic &d : session.view().findings.diagnostics) codes.insert(d.code);
	const JsonValue *held = catalog.get("finding_codes");
	TEST_EXPECT(held && held->array.size() == codes.size() && !codes.empty());
	for (size_t i = 0; held && i < held->array.size(); ++i)
		TEST_EXPECT(codes.count(held->array[i].get_string("code", "")) == 1 && held->array[i].get_number("count", 0.0) > 0);
	TEST_EXPECT(catalog.get_number("page_max", 0.0) == double(kQueryPageMax));
	return 0;
}

// What editor_menu read and wrote, as queries and edit_record's wire form (ported from its S9m
// test): the tree, the batch by label in one undo step, the refusals, the list replaced, the
// findings by source, the render check's render; and the menu every read finds.
static int test_menu_reads_and_batches() {
	editor_test::TempProjectDir dir("opennova_editor_query_menus");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Tools"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();

	// The tree of a closed menu: the file as the last validation read it.
	JsonValue tree = ask(session, "menu_tree", R"({"path": "main.mnu"})");
	TEST_EXPECT(tree.is_object() && !tree.get_bool("open", true) && tree.get_string("path", "") == "menus/main.mnu");
	TEST_EXPECT(tree.get("screens") && tree.get("screens")->array.size() == 1);
	if (!tree.get("screens") || tree.get("screens")->array.size() != 1) return 1;
	const JsonValue &startup = tree.get("screens")->array.front();
	TEST_EXPECT(startup.get_string("name", "") == "STARTUP" && startup.get_string("status", "") == "ready" &&
			startup.get_bool("current", false));
	const JsonValue *main_json = window_named(tree, "MAIN");
	const JsonValue *title_json = window_named(tree, "TITLE");
	TEST_EXPECT(main_json && title_json);
	if (!main_json || !title_json) return 1;
	TEST_EXPECT(id_of(*main_json, "parent") == 0 && main_json->get_int("depth", -1) == 0 && main_json->get_int("index", -1) == 0);
	TEST_EXPECT(id_of(*title_json, "parent") == id_of(*main_json, "id") && title_json->get_int("depth", -1) == 1);
	TEST_EXPECT(title_json->get_string("text", "") == "Tools" && rect_edge(*title_json, 3) > rect_edge(*title_json, 1));
	TEST_EXPECT(startup.get_int("window_count", 0) == int(startup.get("windows")->array.size()) &&
			startup.get_int("count", -1) == startup.get_int("window_count", -2));
	// A page of a screen's windows: one at a time, the screen named by its row.
	const std::string startup_id = std::to_string(id_of(startup, "id"));
	const JsonValue one = ask(session, "menu_tree", R"({"path": "main.mnu", "screen": )" + startup_id + R"(, "offset": 1, "limit": 1})");
	TEST_EXPECT(one.get("screens")->array.size() == 1 && one.get("screens")->array[0].get("windows")->array.size() == 1 &&
			one.get("screens")->array[0].get("windows")->array[0].get_string("name", "") ==
					startup.get("windows")->array[1].get_string("name", "x"));
	TEST_EXPECT(refusal(session, "menu_tree", R"({"path": "main.mnu", "screen": 999999})").find("has no screen") != std::string::npos);
	TEST_EXPECT(refusal(session, "menu_tree", R"({"path": "nothing.mnu"})").find("no menu 'nothing.mnu'") != std::string::npos);
	TEST_EXPECT(refusal(session, "menu_findings", R"({"path": "nothing.mnu"})").find("no menu") != std::string::npos);

	// No menu previewed and the stylesheet active: a pathless read names no menu.
	session.handle(request::open_document("menu_style.mns"));
	TEST_EXPECT(view.documents.previews.menu.path.empty());
	TEST_EXPECT(refusal(session, "menu_tree", "{}").find("no menu is previewed") != std::string::npos);
	TEST_EXPECT(refusal(session, "menu_tree", R"({"path": "menu_style.mns"})").find("no menu 'menu_style.mns'") != std::string::npos);

	// The batch: a button and a list added and filled in by label, one undo step.
	session.handle(request::open_document("main.mnu"));
	auto *menu = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	NodeAddress main;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "MAIN", main));
	const std::string m = std::to_string(main.child);
	const uint64_t before = menu->revision();
	JsonValue answer = send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [
		{"op": "add", "kind": "window", "parent": )" + m + R"(, "as": "hello"},
		{"op": "set", "id": "hello", "field": "name", "value": "HELLO"},
		{"op": "set", "id": "hello", "field": "type", "value": "BUTTON"},
		{"op": "set", "id": "hello", "field": "string.value", "value": "Hello"},
		{"op": "set", "id": "hello", "field": "position.left", "value": 340},
		{"op": "set", "id": "hello", "field": "position.top", "value": 430},
		{"op": "set", "id": "hello", "field": "position.right", "value": 460},
		{"op": "add", "kind": "action", "parent": "hello", "as": "back"},
		{"op": "add", "kind": "action", "parent": "hello", "as": "show"},
		{"op": "set", "id": "show", "field": "type", "value": "WINDOW"},
		{"op": "set", "id": "show", "field": "state", "value": "SHOW"},
		{"op": "set", "id": "show", "field": "target", "value": "TITLE"},
		{"op": "add", "kind": "sound", "parent": "hello", "as": "click"},
		{"op": "set", "id": "click", "field": "file", "value": "menu.lwf"},
		{"op": "add", "kind": "window", "parent": )" + m + R"(, "as": "list"},
		{"op": "set", "id": "list", "field": "name", "value": "CHOICES"},
		{"op": "set", "id": "list", "field": "type", "value": "LIST"},
		{"op": "set", "id": "list", "field": "position.left", "value": 340},
		{"op": "set", "id": "list", "field": "position.top", "value": 470},
		{"op": "set", "id": "list", "field": "position.right", "value": 460},
		{"op": "set", "id": "list", "field": "position.bottom", "value": 520},
		{"op": "add", "kind": "items.item", "parent": "list", "as": "one"},
		{"op": "set", "id": "one", "field": "text", "value": "One"}]})");
	TEST_EXPECT(done(answer));
	const JsonValue *outcome = answer.get("outcome");
	const JsonValue *made = outcome ? outcome->get("made") : nullptr;
	TEST_EXPECT(made && made->object.size() == 6 && outcome->get("added") && outcome->get("added")->array.size() == 6);
	NodeAddress hello, choices;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "HELLO", hello) && find_definition(AssetGraph(), *menu, "CHOICES", choices));
	TEST_EXPECT(made && id_of(*made, "hello") == hello.child && id_of(*made, "list") == choices.child);
	const NodeAddress show = menu->address_of(NodeId(made ? id_of(*made, "show") : 0));
	Value value;
	TEST_EXPECT(menu->get(show, "type", value) && std::get<std::string>(value) == "WINDOW");
	TEST_EXPECT(menu->get(show, "target", value) && std::get<std::string>(value) == "TITLE");
	// The selection is the two windows (their ACTIONs, SOUND and ITEM held by them).
	TEST_EXPECT(view.documents.selected == std::vector<NodeAddress>({hello, choices}) && view.documents.selection == hello);

	// The tree now, pathless: the menu previewed.
	tree = ask(session, "menu_tree");
	TEST_EXPECT(tree.get_bool("open", false) && tree.get_bool("dirty", false));
	const JsonValue *hello_json = window_named(tree, "HELLO");
	const JsonValue *choices_json = window_named(tree, "CHOICES");
	const JsonValue *main_after = window_named(tree, "MAIN");
	TEST_EXPECT(hello_json && choices_json && main_after);
	if (!hello_json || !choices_json || !main_after) return 1;
	TEST_EXPECT(hello_json->get_string("type", "") == "BUTTON" && hello_json->get_string("text", "") == "Hello");
	TEST_EXPECT(id_of(*hello_json, "parent") == main.child && hello_json->get_int("depth", -1) == 1);
	TEST_EXPECT(list_count(*hello_json, "action") == 2 && list_count(*hello_json, "sound") == 1);
	TEST_EXPECT(list_count(*choices_json, "items.item") == 1 && hello_json->get("lists")->get("window") == nullptr);
	TEST_EXPECT(rect_edge(*hello_json, 0, "local") == 340 && rect_edge(*hello_json, 1, "local") == 430 &&
			rect_edge(*hello_json, 2, "local") == 460);
	TEST_EXPECT(rect_edge(*hello_json, 1) == 430 + rect_edge(*main_after, 1) && rect_edge(*hello_json, 0) == 340);
	TEST_EXPECT(hello_json->get_bool("shown", false) && hello_json->get_int("index", -1) == menu->window_index(hello));

	// One undo step takes the whole batch.
	session.handle(request::undo(menu->path()));
	NodeAddress gone;
	TEST_EXPECT(!find_definition(AssetGraph(), *menu, "HELLO", gone) && !find_definition(AssetGraph(), *menu, "CHOICES", gone) &&
			!menu->dirty());
	session.handle(request::redo(menu->path()));
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "HELLO", hello) && find_definition(AssetGraph(), *menu, "CHOICES", choices) &&
			menu->dirty());
	const uint64_t after = menu->revision();
	TEST_EXPECT(after != before);

	// A record duplicated and renamed by its label: right after the original.
	const std::string h = std::to_string(hello.child);
	answer = send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "duplicate", "id": )" + h +
			R"(, "as": "copy"}, {"op": "set", "id": "copy", "field": "name", "value": "HELLO_TWO"}]})");
	TEST_EXPECT(done(answer));
	NodeAddress copy;
	Document::Placement at, original;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "HELLO_TWO", copy) && menu->placement(copy, at) &&
			menu->placement(hello, original) && at.index == original.index + 1 && at.owner == original.owner);
	TEST_EXPECT(id_of(*answer.get("outcome")->get("made"), "copy") == copy.child);
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!find_definition(AssetGraph(), *menu, "HELLO_TWO", copy) && menu->revision() == after);

	// Refused as it is read, by the edit's place: nothing asked of the session.
	const uint64_t kept = menu->revision();
	const auto refused = [&](const std::string &edits, const char *says) {
		return refused_with(send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": )" + edits + "}"), says);
	};
	TEST_EXPECT(refused(R"([{"op": "set", "id": "nobody", "field": "name", "value": "X"}])", "nobody"));
	TEST_EXPECT(refused(R"([{"op": "add", "kind": "gizmo", "parent": )" + m + "}]", "unknown record kind \"gizmo\""));
	TEST_EXPECT(refused(R"([{"op": "set", "id": 999999, "field": "name", "value": "X"}])", "edits[0]: no record 999999"));
	TEST_EXPECT(refused(R"([{"op": "teleport", "id": )" + h + "}]", "teleport"));
	TEST_EXPECT(refused(R"([{"op": "set", "id": )" + h + R"(, "field": "name"}])", "names its \"value\""));
	TEST_EXPECT(refused(R"([{"op": "add", "kind": "window", "parent": )" + m + R"(, "as": "x"}, {"op": "add", "kind": "window", "parent": )" +
			m + R"(, "as": "x"}])", "edits[1]: the label \"x\" is given twice"));
	TEST_EXPECT(refused(R"([{"op": "set", "id": )" + h + R"(, "field": "name", "value": "Y", "colour": 1}])",
			"Unknown edits[0] member \"colour\""));
	TEST_EXPECT(refused("[]", "edits"));
	TEST_EXPECT(refused(R"([{"op": "set", "id": 1}, 3])", "\"edits[1]\" must be an object."));
	TEST_EXPECT(refused(R"([{"op": "replace_list", "id": )" + h + R"(, "list": "gizmos", "records": []}])", "gizmos"));
	TEST_EXPECT(refused(R"([{"op": "replace_list", "id": )" + h + R"(, "list": "action"}])", "records"));
	TEST_EXPECT(refused(R"([{"op": "paste", "id": )" + h + "}]", "unknown edit op \"paste\""));
	TEST_EXPECT(menu->revision() == kept);
	// Refused by the document: nothing committed, the reason in the outcome, nothing added.
	answer = send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "add", "kind": "window", "parent": )" + m +
			R"(, "as": "x"}, {"op": "set", "id": "x", "field": "no_such_field", "value": 1}]})");
	TEST_EXPECT(answer.get_bool("ok", false) && !done(answer));
	TEST_EXPECT(!answer.get("outcome")->get("findings")->array.empty());
	TEST_EXPECT(menu->revision() == kept && answer.get("outcome")->get("added")->array.empty() &&
			answer.get("outcome")->get("made")->object.empty());
	// Refused by the session, an operation holding the documents: read, not done, nothing made.
	const std::string duplicate =
			R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "duplicate", "id": )" + h + R"(, "as": "copy"}]})";
	answer = send(session, duplicate);
	TEST_EXPECT(done(answer) && id_of(*answer.get("outcome")->get("made"), "copy") != 0);
	const uint64_t copied = menu->revision();
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	answer = send(session, duplicate);
	TEST_EXPECT(answer.get_bool("ok", false) && !done(answer) && menu->revision() == copied);
	TEST_EXPECT(answer.get("outcome")->get("added")->array.empty() && answer.get("outcome")->get("made")->object.empty());
	session.run_operations();
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(menu->revision() == kept);

	// A list replaced: HELLO's two ACTIONs by one, one undo step; an empty list replaced by nothing
	// is done with nothing to do.
	const auto list_edit = [&](const std::string &list, const std::string &records) {
		return send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "replace_list", "id": )" + h +
				R"(, "list": ")" + list + R"(", "records": )" + records + "}]}");
	};
	answer = list_edit("action", R"([{"type": "WINDOW", "state": "HIDE", "target": "TITLE"}])");
	TEST_EXPECT(done(answer) && answer.get("outcome")->get("added")->array.size() == 1);
	std::vector<NodeId> actions;
	for (const Document::Collection &collection : menu->collections_of(hello))
		if (std::string(menu->kind_token(collection.spec.kind)) == "action") actions = collection.ids;
	TEST_EXPECT(actions.size() == 1);
	if (actions.size() == 1) TEST_EXPECT(menu->get(menu->address_of(actions.front()), "state", value) &&
				std::get<std::string>(value) == "HIDE");
	session.handle(request::undo(menu->path()));
	for (const Document::Collection &collection : menu->collections_of(hello))
		if (std::string(menu->kind_token(collection.spec.kind)) == "action") actions = collection.ids;
	TEST_EXPECT(actions.size() == 2);
	const uint64_t unchanged = menu->revision();
	TEST_EXPECT(done(list_edit("hotkey", "[]")) && menu->revision() == unchanged);
	// A record's fields in the order written: a body's draw kind, then its flag cleared, leaves no
	// draw kind; the flag cleared first, then the kind, leaves the kind.
	std::string display;
	int64_t custom = -1;
	TEST_EXPECT(done(list_edit("column.body", R"([{"display": "CUSTOM_DRAW", "custom_draw": 0}])")));
	TEST_EXPECT(body_draw(*menu, hello, display, custom) && display.empty() && custom == 0);
	TEST_EXPECT(done(list_edit("column.body", R"([{"custom_draw": 0, "display": "CUSTOM_DRAW"}])")));
	TEST_EXPECT(body_draw(*menu, hello, display, custom) && display == "CUSTOM_DRAW" && custom == 1);
	session.handle(request::undo(menu->path()));
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!body_draw(*menu, hello, display, custom) && menu->revision() == unchanged);

	// The findings: HELLO's label cut short (the render check's) beside the sound bank the project
	// lacks (the graph's), each with its source; every screen's notes; a page of the rows.
	TEST_EXPECT(done(send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "set", "id": )" + h +
			R"(, "field": "position.right", "value": 350}]})")));
	const JsonValue findings = ask(session, "menu_findings", R"({"path": "main.mnu", "limit": 200})");
	TEST_EXPECT(findings.is_object() && findings.get_string("path", "") == menu->path());
	bool cut = false, bank = false;
	for (const JsonValue &row : findings.get("problems")->array) {
		cut = cut || (row.get_string("code", "") == "menu.render.text_truncated" && row.get_string("source", "") == "render" &&
				id_of(row, "child") == hello.child);
		bank = bank || (row.get_string("code", "") == "reference.missing" && row.get_string("source", "") == "graph" &&
				row.get_string("message", "").find("menu.lwf") != std::string::npos);
	}
	TEST_EXPECT(cut && bank);
	TEST_EXPECT(findings.get("sources")->get_int("render", 0) >= 1 && findings.get("sources")->get_int("graph", 0) >= 1);
	TEST_EXPECT(findings.get_int("count", 0) == int(findings.get("problems")->array.size()));
	TEST_EXPECT(findings.get("counts")->get_int("warning", 0) >= 2);
	const JsonValue &screen = findings.get("screens")->array.front();
	TEST_EXPECT(screen.get_string("name", "") == "STARTUP" && screen.get_int("notes", 0) > 0 && screen.get_int("problems", 0) >= 2);
	const JsonValue warnings = ask(session, "menu_findings", R"({"path": "main.mnu", "severity": "warning", "limit": 1})");
	TEST_EXPECT(warnings.get("problems")->array.size() == 1 && warnings.get_int("count", 0) == findings.get("counts")->get_int("warning", -1));
	TEST_EXPECT(refusal(session, "menu_findings", R"({"path": "main.mnu", "severity": "fatal"})").find("no severity") != std::string::npos);

	// The render check's render of the screen: the preview's schema, its widgets a page (the
	// pages making the whole), its notes by the same page.
	const std::string render_args = R"({"path": ")" + menu->path() + R"(", "screen": )" + startup_id + "}";
	TEST_EXPECT(pages_concatenate(session, "menu_render", render_args, "widgets", 3, 4));
	const JsonValue render = ask(session, "menu_render", render_args);
	TEST_EXPECT(render.get_string("status", "") == "ready" && render.get_int("widget_count", -1) == render.get_int("count", -2) &&
			render.get_int("note_count", 0) > 0);
	TEST_EXPECT(ask(session, "menu_render", R"({"path": ")" + menu->path() + R"(", "screen": 999999})").get_string("status", "") ==
			"no_screen");

	// The menu previewed and the stylesheet active: a pathless read finds the menu, and an edit of
	// the menu by its path lands on it, not on the stylesheet.
	session.handle(request::open_document("menu_style.mns"));
	const Document *style = session.document_for("menu_style.mns");
	TEST_EXPECT(style && view.documents.active == style->path() && view.documents.previews.menu.path == menu->path());
	TEST_EXPECT(ask(session, "menu_tree").get_string("path", "") == menu->path() &&
			ask(session, "menu_findings").get_string("path", "") == menu->path());
	const uint64_t style_before = style ? style->revision() : 0;
	TEST_EXPECT(done(send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "set", "id": )" + h +
			R"(, "field": "name", "value": "HELLO_AGAIN"}]})")));
	NodeAddress again;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "HELLO_AGAIN", again) && again == hello && style &&
			style->revision() == style_before && !style->dirty());
	return 0;
}

// The wire's other edits: a record's fields given back their saved value, a fix's edit that opens
// its document first, and a request no document names the records of.
static int test_wire_edits() {
	editor_test::TempProjectDir dir("opennova_editor_query_wire_edits");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Wire"));
	editor_test::create_missing_files(session);
	// An edit naming a record of a document that is not open: refused as it is read.
	TEST_EXPECT(refused_with(send(session, R"({"kind": "edit_record", "path": "main.mnu", "edits": [{"op": "set", "id": 2, "field": "name", "value": "X"}]})"),
			"which is not open"));
	// With open_first the document opens first, and the records are named in it.
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress title;
	TEST_EXPECT(menu && find_definition(*session.view().findings.graph, *menu, "TITLE", title));
	if (!menu) return 1;
	const std::string t = std::to_string(title.child);
	session.handle(request::close_document(menu->path()));
	TEST_EXPECT(session.document_for("main.mnu") == nullptr);
	JsonValue answer = send(session, R"({"kind": "edit_record", "path": "main.mnu", "open_first": true, "edits": [{"op": "set", "id": )" +
			t + R"(, "field": "string.value", "value": "Changed"}]})");
	TEST_EXPECT(done(answer));
	menu = session.document_for("main.mnu");
	Value value;
	TEST_EXPECT(menu && menu->get(title, "string.value", value) && std::get<std::string>(value) == "Changed");
	// revert_to_saved names each field by its record's identity: the saved value back, one step.
	answer = send(session, R"({"kind": "revert_to_saved", "path": "main.mnu", "edits": [{"id": )" + t +
			R"(, "field": "string.value"}]})");
	TEST_EXPECT(done(answer) && menu->get(title, "string.value", value) && std::get<std::string>(value) == "Wire");
	TEST_EXPECT(refused_with(send(session, R"({"kind": "revert_to_saved", "path": "main.mnu", "edits": [{"id": )" + t +
			R"(, "field": "string.value", "op": "set"}]})"), "Unknown edits[0] member \"op\""));
	TEST_EXPECT(!done(send(session, R"({"kind": "revert_to_saved", "path": "main.mnu", "edits": [{"id": )" + t +
			R"(, "field": "string.value"}]})")));
	// A request of the shell (reveal_path) is left to the shell, read and not served.
	EditorRequest shell;
	answer = session.handle_json(parse(R"({"kind": "reveal_path", "path": "C:/x"})"), &shell);
	TEST_EXPECT(answer.get_bool("ok", false) && !answer.get_bool("served", true) && shell.kind == EditorRequestKind::RevealPath &&
			shell.path == "C:/x" && done(answer));
	// The pickers need a person, refused by their kind before their fields are read.
	TEST_EXPECT(refused_with(session.handle_json(parse(R"({"kind": "pick_directory", "zzz": 1})")), "need a person"));
	TEST_EXPECT(refused_with(session.handle_json(parse("[]")), "A request is a JSON object."));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_paging();
	failures += test_refusals();
	failures += test_problems_params();
	failures += test_state_since();
	failures += test_catalog();
	failures += test_menu_reads_and_batches();
	failures += test_wire_edits();
	if (failures == 0) std::printf("editor_query: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
