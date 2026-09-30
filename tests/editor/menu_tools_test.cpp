// The editor MCP's menu tools (ADR 0046 S9m, `editor_menu`), over a real session and
// through JSON as the MCP sends it. The tree: a closed menu read as the last validation
// read it, every window of a screen with its owner, depth, type, text, lists and the rect
// the render check placed it at. The batch: a button with two ACTIONs (one filled in) and
// a SOUND and a list with an ITEM, added and filled in by label in one undo step, the
// labels answered with the identities they got, the selection the two windows; a record
// duplicated and renamed by label; the refusals (an unknown label, kind, record or op, a
// set with no value, a batch the document refuses: nothing committed). The list op: a
// window's ACTIONs replaced in one undo step, an empty list replaced by nothing, a record's
// fields set in the order written (a column body's draw kind and its flag). The findings: the
// menu's Problems rows by source, the sound bank the project lacks the graph's and a label
// cut short the render check's, and each screen's notes. The menu every op finds: with no
// path the previewed menu, else the active document when it is a menu, so a pathless edit
// by an id a pathless tree gave lands on that menu while a stylesheet is active; with no
// menu previewed and a stylesheet active, or a path naming a stylesheet, refused.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_report.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_view.h>

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

JsonValue batch(ProjectSession &session, const std::string &path, const std::string &json) {
	return menu_edit_request(session, path, parse(json));
}

bool refused_with(const JsonValue &answer, const char *says) {
	const std::string error = answer.get_string("error", "");
	if (error.find(says) == std::string::npos) std::printf("  refusal said: %s\n", error.c_str());
	return !answer.get_bool("ok", true) && error.find(says) != std::string::npos;
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

bool done(const JsonValue &answer) {
	const JsonValue *outcome = answer.get("outcome");
	return answer.get_bool("ok", false) && outcome && outcome->get_bool("done", false);
}

uint64_t id_of(const JsonValue &object, const char *key) {
	const JsonValue *value = object.get(key);
	return value && value->is_number() ? uint64_t(value->number) : 0;
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

} // namespace

static int test_menu_tools() {
	editor_test::TempProjectDir dir("opennova_editor_menu_tools");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Tools"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();

	// The tree of a closed menu: the file as the last validation read it.
	JsonValue tree = menu_tree_to_json(view, "main.mnu");
	TEST_EXPECT(tree.is_object() && !tree.get_bool("open", true) && tree.get_string("path", "") == "menus/main.mnu");
	TEST_EXPECT(tree.get("screens") && tree.get("screens")->array.size() == 1);
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
	TEST_EXPECT(startup.get_int("window_count", 0) == int(startup.get("windows")->array.size()));
	TEST_EXPECT(menu_tree_to_json(view, "nothing.mnu").is_null() && menu_findings_to_json(view, "nothing.mnu").is_null());

	// No menu previewed and the stylesheet active: a pathless edit names no menu, nor does a
	// path naming the stylesheet, and neither asks anything of it.
	session.handle(request::open_document("menu_style.mns"));
	Document *style = session.document_for("menu_style.mns");
	TEST_EXPECT(style && view.active_document == style->path() && view.menu_preview.path.empty());
	if (!style) return 1;
	const uint64_t style_revision = style->revision();
	const std::string rename_first = R"({"edits": [{"op": "set", "id": 1, "field": "name", "value": "RENAMED"}]})";
	TEST_EXPECT(refused_with(batch(session, "", rename_first), "No menu is previewed"));
	TEST_EXPECT(refused_with(batch(session, "menu_style.mns", rename_first), "No menu 'menu_style.mns'"));
	TEST_EXPECT(menu_tree_to_json(view, "").is_null() && style->revision() == style_revision);

	// The batch: a button and a list added and filled in by label, one undo step.
	session.handle(request::open_document("main.mnu"));
	auto *menu = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	NodeAddress main;
	TEST_EXPECT(menu->find("MAIN", main));
	const std::string m = std::to_string(main.child);
	const uint64_t before = menu->revision();
	JsonValue answer = batch(session, "main.mnu", R"({"edits": [
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
	const JsonValue *made = answer.get("made");
	TEST_EXPECT(made && made->object.size() == 6 && answer.get("added") && answer.get("added")->array.size() == 6);
	NodeAddress hello, choices, show;
	TEST_EXPECT(menu->find("HELLO", hello) && menu->find("CHOICES", choices));
	TEST_EXPECT(made && id_of(*made, "hello") == hello.child && id_of(*made, "list") == choices.child);
	show = menu->address_of(NodeId(made ? id_of(*made, "show") : 0));
	Value value;
	TEST_EXPECT(menu->get(show, "type", value) && std::get<std::string>(value) == "WINDOW");
	TEST_EXPECT(menu->get(show, "target", value) && std::get<std::string>(value) == "TITLE");
	TEST_EXPECT(menu->collections_of(hello).size() > 0);
	// The selection is the two windows (their ACTIONs, SOUND and ITEM held by them).
	TEST_EXPECT(view.selected == std::vector<NodeAddress>({hello, choices}) && view.selection == hello);

	// The tree now: the button with its lists, text and rect; the list with its item.
	tree = menu_tree_to_json(view, "");
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
	// In MAIN (its own rect below the screen's top) as authored; on the screen by MAIN's.
	TEST_EXPECT(rect_edge(*hello_json, 0, "local") == 340 && rect_edge(*hello_json, 1, "local") == 430 &&
	            rect_edge(*hello_json, 2, "local") == 460);
	TEST_EXPECT(rect_edge(*hello_json, 1) == 430 + rect_edge(*main_after, 1) && rect_edge(*hello_json, 0) == 340);
	TEST_EXPECT(hello_json->get_bool("shown", false) && hello_json->get_int("index", -1) == menu->window_index(hello));
	TEST_EXPECT(rect_edge(*choices_json, 3, "local") == 520);

	// One undo step takes the whole batch.
	session.handle(request::undo(menu->path()));
	NodeAddress gone;
	TEST_EXPECT(!menu->find("HELLO", gone) && !menu->find("CHOICES", gone) && !menu->dirty());
	session.handle(request::redo(menu->path()));
	TEST_EXPECT(menu->find("HELLO", hello) && menu->find("CHOICES", choices) && menu->dirty());
	const uint64_t after = menu->revision();
	TEST_EXPECT(after != before);

	// A record duplicated and renamed by its label: right after the original.
	const std::string h = std::to_string(hello.child);
	answer = batch(session, "", R"({"edits": [{"op": "duplicate", "id": )" + h + R"(, "as": "copy"},
		{"op": "set", "id": "copy", "field": "name", "value": "HELLO_TWO"}]})");
	TEST_EXPECT(done(answer));
	NodeAddress copy;
	Document::Placement at, original;
	TEST_EXPECT(menu->find("HELLO_TWO", copy) && menu->placement(copy, at) && menu->placement(hello, original) &&
	            at.index == original.index + 1 && at.owner == original.owner);
	TEST_EXPECT(answer.get("made") && id_of(*answer.get("made"), "copy") == copy.child);
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->find("HELLO_TWO", copy) && menu->revision() == after);

	// Refused before the session sees it.
	const uint64_t kept = menu->revision();
	const auto refused = [&](const std::string &json, const char *says) {
		return refused_with(batch(session, "", json), says);
	};
	TEST_EXPECT(refused(R"({"edits": [{"op": "set", "id": "nobody", "field": "name", "value": "X"}]})", "nobody"));
	TEST_EXPECT(refused(R"({"edits": [{"op": "add", "kind": "gizmo", "parent": )" + m + "}]}", "gizmo"));
	TEST_EXPECT(refused(R"({"edits": [{"op": "set", "id": 999999, "field": "name", "value": "X"}]})", "No record"));
	TEST_EXPECT(refused(R"({"edits": [{"op": "teleport", "id": )" + h + "}]}", "teleport"));
	TEST_EXPECT(refused(R"({"edits": [{"op": "set", "id": )" + h + R"(, "field": "name"}]})", "value"));
	TEST_EXPECT(refused(R"({"edits": [{"op": "add", "kind": "window", "parent": )" + m +
	                    R"(, "as": "x"}, {"op": "add", "kind": "window", "parent": )" + m + R"(, "as": "x"}]})",
	                    "twice"));
	TEST_EXPECT(refused(R"({"edits": [{"op": "set", "id": )" + h + R"(, "field": "name", "value": "Y", "colour": 1}]})",
	                    "colour"));
	TEST_EXPECT(refused(R"({"edits": []})", "edits"));
	TEST_EXPECT(refused(R"({"id": )" + h + R"(, "list": "gizmos", "records": []})", "gizmos"));
	TEST_EXPECT(refused(R"({"id": )" + h + R"(, "list": "action"})", "records"));
	// Refused by the document: nothing committed, the reason in the outcome.
	answer = batch(session, "", R"({"edits": [{"op": "add", "kind": "window", "parent": )" + m +
	                                      R"(, "as": "x"}, {"op": "set", "id": "x", "field": "no_such_field", "value": 1}]})");
	TEST_EXPECT(answer.get_bool("ok", false) && !done(answer));
	TEST_EXPECT(answer.get("outcome") && answer.get("outcome")->get("findings") &&
	            !answer.get("outcome")->get("findings")->array.empty());
	TEST_EXPECT(menu->revision() == kept && answer.get("added")->array.empty());
	// Refused by the session, an operation holding the documents (S13 A2): the batch parsed, its
	// outcome not done, and it names nothing made, not the records the batch before it made.
	const std::string duplicate = R"({"edits": [{"op": "duplicate", "id": )" + h + R"(, "as": "copy"}]})";
	answer = batch(session, "", duplicate);
	TEST_EXPECT(done(answer) && answer.get("made") && id_of(*answer.get("made"), "copy") != 0);
	const uint64_t copied = menu->revision();
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	answer = batch(session, "", duplicate);
	TEST_EXPECT(answer.get_bool("ok", false) && !done(answer) && menu->revision() == copied);
	TEST_EXPECT(answer.get("added") && answer.get("added")->array.empty() && answer.get("made") &&
	            answer.get("made")->object.empty());
	session.run_operations();
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(menu->revision() == kept);

	// The list op: HELLO's two ACTIONs replaced by one, one undo step; an empty list replaced
	// by nothing is done with nothing to do.
	answer = batch(session, "", R"({"id": )" + h +
	                                      R"(, "list": "action", "records": [{"type": "WINDOW", "state": "HIDE", "target": "TITLE"}]})");
	TEST_EXPECT(done(answer) && answer.get("added") && answer.get("added")->array.size() == 1);
	std::vector<NodeId> actions;
	for (const Document::Collection &collection : menu->collections_of(hello))
		if (std::string(menu->kind_token(collection.spec.kind)) == "action") actions = collection.ids;
	TEST_EXPECT(actions.size() == 1);
	if (actions.size() == 1) {
		const NodeAddress action = menu->address_of(actions.front());
		TEST_EXPECT(menu->get(action, "state", value) && std::get<std::string>(value) == "HIDE");
	}
	session.handle(request::undo(menu->path()));
	for (const Document::Collection &collection : menu->collections_of(hello))
		if (std::string(menu->kind_token(collection.spec.kind)) == "action") actions = collection.ids;
	TEST_EXPECT(actions.size() == 2);
	const uint64_t unchanged = menu->revision();
	TEST_EXPECT(done(batch(session, "", R"({"id": )" + h + R"(, "list": "hotkey", "records": []})")) &&
	            menu->revision() == unchanged);
	// A record's fields in the order written: a body's draw kind, then its flag cleared,
	// leaves no draw kind; the flag cleared first, then the kind, leaves the kind.
	std::string display;
	int64_t custom = -1;
	TEST_EXPECT(done(batch(session, "", R"({"id": )" + h +
	                                          R"(, "list": "column.body", "records": [{"display": "CUSTOM_DRAW", "custom_draw": 0}]})")));
	TEST_EXPECT(body_draw(*menu, hello, display, custom) && display.empty() && custom == 0);
	TEST_EXPECT(done(batch(session, "", R"({"id": )" + h +
	                                          R"(, "list": "column.body", "records": [{"custom_draw": 0, "display": "CUSTOM_DRAW"}]})")));
	TEST_EXPECT(body_draw(*menu, hello, display, custom) && display == "CUSTOM_DRAW" && custom == 1);
	session.handle(request::undo(menu->path()));
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!body_draw(*menu, hello, display, custom) && menu->revision() == unchanged);

	// The findings: HELLO's label cut short (the render check's) beside the sound bank the
	// project lacks (the graph's), each with its source; every screen's notes.
	TEST_EXPECT(done(batch(session, "", R"({"edits": [{"op": "set", "id": )" + h +
	                                               R"(, "field": "position.right", "value": 350}]})")));
	const JsonValue findings = menu_findings_to_json(view, "main.mnu");
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

	// The menu previewed and the stylesheet active: a pathless tree and a pathless edit find
	// the one menu, so the edit of an id the tree gave lands on it, not on the stylesheet's
	// record of that id.
	session.handle(request::open_document("menu_style.mns"));
	TEST_EXPECT(view.active_document == style->path() && view.menu_preview.path == menu->path());
	TEST_EXPECT(menu_tree_to_json(view, "").get_string("path", "") == menu->path() &&
	            menu_findings_to_json(view, "").get_string("path", "") == menu->path());
	const uint64_t style_before = style->revision();
	TEST_EXPECT(done(batch(session, "", R"({"edits": [{"op": "set", "id": )" + h +
	                                               R"(, "field": "name", "value": "HELLO_AGAIN"}]})")));
	NodeAddress again;
	TEST_EXPECT(menu->find("HELLO_AGAIN", again) && again == hello && style->revision() == style_before && !style->dirty());
	TEST_EXPECT(refused_with(batch(session, "menu_style.mns", R"({"edits": [{"op": "set", "id": )" + h +
	                                                                  R"(, "field": "name", "value": "X"}]})"),
	                         "No menu"));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_menu_tools();
	if (failures == 0) std::printf("editor_menu_tools: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
