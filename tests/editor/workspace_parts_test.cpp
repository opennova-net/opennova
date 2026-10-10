// What the windows show of their own, held by the session (ADR 0046, the MCP gaps lane): the set_workspace
// request and the workspace section, over a session on a fake process platform. Each part a person's controls
// change has its wire form: the file card (about_file opens it, set_workspace closes it), the build result's
// panel (a build's end opens it), the new-project form, the project settings (opened over the settings in
// effect, closed by its Apply that wrote everything and with its project), focus (a focus_window event),
// Files' filter, the find bars, the New file prompt and Rename..., Rename everywhere and Rename back,
// Problems' filters and confirmation, and what a document's views show of it. A change the table does not
// have is refused as it is read, naming what it takes; one the session cannot take is a workspace.refused
// warning, nothing changed.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/import/import_plan.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

JsonValue parsed(const std::string &text) {
	JsonValue out;
	std::string error;
	opennova::io::json_parse(text, out, error);
	return out;
}

// A set_workspace on the wire, as the editor MCP raises it: the answer handle_json gives.
JsonValue wire(ProjectSession &session, const std::string &workspace) {
	JsonValue request = JsonValue::make_object();
	request.set("kind", JsonValue::make_string("set_workspace"));
	request.set("workspace", parsed(workspace));
	return session.handle_json(request);
}

// The workspace section, as the state query writes it.
JsonValue section(ProjectSession &session) {
	JsonValue args = JsonValue::make_object();
	JsonValue sections = JsonValue::make_array();
	sections.push(JsonValue::make_string("workspace"));
	args.set("sections", sections);
	std::string error;
	const JsonValue state = session.query("state", args, error);
	const JsonValue *workspace = state.get("workspace");
	return workspace ? *workspace : JsonValue();
}

bool refused_with(const ProjectSession &session, const char *code) {
	const ActionOutcome &outcome = session.outcome();
	if (outcome.done()) return false;
	for (const Diagnostic &d : outcome.findings)
		if (d.code() == code) return true;
	return false;
}

// The table on the wire: every part with its members, each its JSON type and doc; a change naming a part, a
// member or a type the table has not is refused as it is read, naming what it takes; focus names a window.
int test_table_and_wire() {
	size_t count = 0;
	const WorkspacePartRow *parts = workspace_parts(count);
	TEST_EXPECT(count >= 4);
	for (size_t p = 0; p < count; ++p) {
		TEST_EXPECT(*parts[p].token && *parts[p].doc && parts[p].member_count > 0);
		for (size_t m = 0; m < parts[p].member_count; ++m) TEST_EXPECT(*parts[p].members[m].token && *parts[p].members[m].doc);
	}
	std::string error;
	TEST_EXPECT(check_workspace_change(parsed(R"({"new_project": {"title": "A", "open": true}, "focus": "output"})"), error));
	TEST_EXPECT(!check_workspace_change(parsed(R"({"no_part": {}})"), error) && error.find("no part \"no_part\"") != std::string::npos);
	TEST_EXPECT(!check_workspace_change(parsed(R"({"card": {"file": "a"}})"), error) &&
	            error.find("takes no \"file\" (it takes path)") != std::string::npos);
	TEST_EXPECT(!check_workspace_change(parsed(R"({"build_result": {"open": "yes"}})"), error) &&
	            error.find("must be true or false") != std::string::npos);
	TEST_EXPECT(!check_workspace_change(parsed(R"({"focus": "nowhere"})"), error) && error.find("files, document") != std::string::npos);
	TEST_EXPECT(!check_workspace_change(parsed(R"({"card": "a"})"), error) && error.find("an object of its members") != std::string::npos);
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const uint64_t before = session.view().revisions.of(ViewConcern::Workspace);
	const JsonValue answer = wire(session, R"({"card": {"file": "a"}})");
	TEST_EXPECT(!answer.get_bool("ok", true) && answer.get_string("error", "").find("takes no \"file\"") != std::string::npos);
	TEST_EXPECT(session.view().revisions.of(ViewConcern::Workspace) == before);
	return 0;
}

// The new-project form: its fields set as a person types them (nothing made), the install named once given,
// building on an expansion building as one; the state section writes the form as it shows; the modal opened
// closes once new_project makes the project.
int test_new_project_form() {
	editor_test::TempProjectDir dir("opennova_editor_workspace_form");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	TEST_EXPECT(v.workspace.new_project.title == "My Game" && !v.workspace.new_project.install_named);
	JsonValue answer = wire(session, R"({"new_project": {"open": true, "title": "Operation Nightfall", "dir": ")" +
	                                         dir.file("nightfall") + R"(", "builds_on": "jox01", "expansion": "nightfall"}})");
	TEST_EXPECT(answer.get_bool("ok", false) && answer.get("outcome") && answer.get("outcome")->get_bool("done", false));
	const WorkspaceView::NewProject &form = v.workspace.new_project;
	TEST_EXPECT(form.open && form.title == "Operation Nightfall" && form.dir == dir.file("nightfall") && form.builds_on == "jox01" &&
	            form.as_expansion && form.expansion == "nightfall" && !form.install_named && !v.project.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_project": {"game_install": "C:/Games/JO"}})")));
	TEST_EXPECT(form.install_named && form.game_install == "C:/Games/JO");
	JsonValue shown = section(session);
	const JsonValue *made = shown.get("new_project");
	TEST_EXPECT(made && made->get_bool("open", false) && made->get_string("title", "") == "Operation Nightfall" &&
	            made->get_string("game_install", "") == "C:/Games/JO" && made->get_bool("install_named", false) &&
	            made->get_bool("as_expansion", false) && made->get_string("builds_on", "") == "jox01");
	// Building on nothing: as_expansion stays as set.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_project": {"builds_on": "", "as_expansion": false}})")));
	TEST_EXPECT(!form.as_expansion && form.builds_on.empty() && form.expansion == "nightfall");
	// new_project makes the project (standalone, no install named in the request): the modal closes.
	TEST_EXPECT(session.handle(request::new_project(dir.file("plain"), "Plain")) && session.outcome().done());
	session.run_operations();
	TEST_EXPECT(v.project.open && !form.open && form.title == "Operation Nightfall");
	return 0;
}

// The build result's panel: a build's end opens it (a refused one's too), set_workspace closes it and opens
// it again; the project closing closes it. The card of a file the project lacks, or with no project open,
// refused: a warning, nothing changed. focus posts a focus_window event naming the window.
int test_build_result_card_and_focus() {
	editor_test::TempProjectDir dir("opennova_editor_workspace_build");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	TEST_EXPECT(session.handle(request::set_workspace(R"({"card": {"path": "a.mnu"}})")) &&
	            refused_with(session, "workspace.refused") && v.workspace.card.path.empty());
	TEST_EXPECT(session.handle(request::new_project(dir.file("project"), "Built")));
	session.run_operations();
	TEST_EXPECT(!v.workspace.build_result.open);
	TEST_EXPECT(session.handle(request::build()));
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && v.workspace.build_result.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"build_result": {"open": false}})")) && !v.workspace.build_result.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"build_result": {"open": true}})")) && v.workspace.build_result.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"card": {"path": "nothing.wav"}})")) &&
	            refused_with(session, "workspace.refused") && v.workspace.card.path.empty());
	const uint64_t next = v.events.next_seq();
	TEST_EXPECT(session.handle(request::set_workspace(R"({"focus": "problems"})")) && session.outcome().done());
	TEST_EXPECT(v.events.next_seq() == next + 1 && v.events.held().back().kind == ViewEventKind::FocusWindow &&
	            v.events.held().back().path == "problems");
	TEST_EXPECT(session.handle(request::close_project()));
	TEST_EXPECT(!v.workspace.build_result.open);
	return 0;
}

// The project settings: refused with no project open; opened over the settings in effect; a field set while
// closed refused; the fields set (a play_mode no mode has refused); its Apply (a serial) that wrote everything
// closes it, one that failed keeps it; the project closing closes it.
int test_settings() {
	editor_test::TempProjectDir dir("opennova_editor_workspace_settings");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"open": true}})")) &&
	            refused_with(session, "workspace.refused") && !v.workspace.settings.open);
	TEST_EXPECT(session.handle(request::new_project(dir.file("project"), "Armory")));
	session.run_operations();
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"title": "Early"}})")) &&
	            refused_with(session, "workspace.refused") && !v.workspace.settings.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"open": true, "multiplayer": true}})")) &&
	            session.outcome().done());
	const WorkspaceView::Settings &settings = v.workspace.settings;
	TEST_EXPECT(settings.open && settings.title == "Armory" && settings.multiplayer && !settings.mission &&
	            !v.project.document->features.multiplayer);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"title": "Harbor"}})")) && settings.title == "Harbor" &&
	            v.project.document->title == "Armory");
	TEST_EXPECT(settings.play_mode == PlayMode::Runtime &&
	            session.handle(request::set_workspace(R"({"settings": {"play_mode": "strict"}})")) &&
	            settings.play_mode == PlayMode::Strict && v.project.play_mode == PlayMode::Runtime);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"play_mode": "retail"}})")) &&
	            refused_with(session, "workspace.refused") && settings.play_mode == PlayMode::Strict);
	JsonValue shown = section(session);
	TEST_EXPECT(shown.get("settings") && shown.get("settings")->get_string("title", "") == "Harbor" &&
	            shown.get("settings")->get_bool("multiplayer", false) &&
	            shown.get("settings")->get_string("play_mode", "") == "strict");
	// A settings change with no serial (a menu's Play in the game install) leaves the dialog open.
	ProjectSettingsChange play;
	play.play_mode = PlayMode::Runtime;
	TEST_EXPECT(session.handle(request::apply_project_settings(play)) && settings.open);
	// The dialog's Apply, its serial: written, it closes; the project then plays as it chose.
	ProjectSettingsChange apply;
	apply.serial = 4;
	apply.title = settings.title;
	apply.multiplayer = settings.multiplayer;
	apply.play_mode = settings.play_mode;
	TEST_EXPECT(session.handle(request::apply_project_settings(apply)));
	TEST_EXPECT(!settings.open && v.project.document->title == "Harbor" && v.project.document->features.multiplayer &&
	            v.project.play_mode == PlayMode::Strict);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"open": true}})")) && settings.open &&
	            settings.title == "Harbor" && settings.play_mode == PlayMode::Strict);
	TEST_EXPECT(session.handle(request::close_project()) && !settings.open);
	return 0;
}

// A project with the files the game reads by name (a new project's, made), its catalogs' records, open.
struct FilesProject {
	editor_test::TempProjectDir dir{ "opennova_editor_workspace_files" };
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	std::string weapons_path, items_path;

	bool open() {
		session.handle(request::new_project(dir.file("project"), "Files"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &v = session.view();
		const AssetEntry *weapons = v.project.scan->find("weapon.def");
		const AssetEntry *items = v.project.scan->find("items.def");
		if (!weapons || !items) return false;
		weapons_path = weapons->relative_path;
		items_path = items->relative_path;
		if (!editor_test::write_text(v.project.root + "/" + weapons_path, "weapon \"GUN_A\"\nend\n") ||
		    !editor_test::write_text(v.project.root + "/" + items_path,
		                             "begin \"Carrier\"\nid 100300\ntype vehicle\nprimary_weapon GUN_A\nend\n"))
			return false;
		session.handle(request::rescan());
		session.run_operations();
		return true;
	}
};

// Files' filter and kind, the find bar and Find in project, the New file prompt and Rename...: each set as a
// person's controls set it, refused where it names what the project lacks or while its part is closed, opened
// and closed by the requests a dialog's button raises; the project closing starts them afresh.
int test_files_find_and_prompts() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const WorkspaceView &w = v.workspace;
	// Files' filter and kind; a kind no file has is refused.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"files": {"filter": "def", "kind": "menu"}})")) &&
	            session.outcome().done() && w.files.filter == "def" && w.files.kind == AssetKind::Menu);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"files": {"kind": "nothing"}})")) &&
	            refused_with(session, "workspace.refused") && w.files.kind == AssetKind::Menu);
	JsonValue shown = section(session);
	TEST_EXPECT(shown.get("files") && shown.get("files")->get_string("filter", "") == "def" &&
	            shown.get("files")->get_string("kind", "") == "menu" && !shown.get("files")->get_bool("by_cost", true));
	// Files by cost (S18, the texture budget), the filter and kind kept.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"files": {"by_cost": true}})")) && session.outcome().done() &&
	            w.files.by_cost && w.files.filter == "def" && w.files.kind == AssetKind::Menu);
	shown = section(session);
	TEST_EXPECT(shown.get("files") && shown.get("files")->get_bool("by_cost", false));
	// The find bar and Find in project.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"find": {"open": true, "text": "GUN", "match_case": true}})")) &&
	            w.find.open && w.find.text == "GUN" && w.find.match_case);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"project_find": {"open": true, "text": "carrier"}})")) &&
	            w.project_find.open && w.project_find.text == "carrier");
	// The New file prompt: a field before its kind refused; opened on a kind, emptied; the create_file of the name
	// it holds closes it.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_file": {"name": "extra.mnu"}})")) &&
	            refused_with(session, "workspace.refused") && w.new_file.kind == AssetKind::kCount);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_file": {"kind": "menu", "name": "extra.mnu"}})")) &&
	            w.new_file.kind == AssetKind::Menu && w.new_file.name == "extra.mnu");
	shown = section(session);
	TEST_EXPECT(shown.get("new_file") && shown.get("new_file")->get_string("kind", "") == "menu" &&
	            shown.get("new_file")->get_string("name", "") == "extra.mnu");
	TEST_EXPECT(session.handle(request::create_file("other.mnu", "menu")) && w.new_file.kind == AssetKind::Menu);
	TEST_EXPECT(session.handle(request::create_file("extra.mnu", "menu")) && w.new_file.kind == AssetKind::kCount &&
	            w.new_file.name.empty());
	// Rename...: a file the project lacks refused; opened on a file, its name the file's; show_in_files that asks
	// the name opens it; rename_asset of its file closes it.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"file_rename": {"path": "gone.def"}})")) &&
	            refused_with(session, "workspace.refused") && w.file_rename.path.empty());
	TEST_EXPECT(session.handle(request::set_workspace("{\"file_rename\": {\"path\": \"" + project.items_path + "\"}}")) &&
	            w.file_rename.path == project.items_path && w.file_rename.name == "items.def");
	TEST_EXPECT(session.handle(request::set_workspace(R"({"file_rename": {"name": "things.def"}})")) && w.file_rename.name == "things.def");
	TEST_EXPECT(session.handle(request::set_workspace(R"({"file_rename": {"path": ""}})")) && w.file_rename.path.empty());
	TEST_EXPECT(session.handle(request::show_in_files(project.weapons_path, true)) && w.file_rename.path == project.weapons_path &&
	            w.file_rename.name == "weapon.def");
	TEST_EXPECT(session.handle(request::rename_asset(project.weapons_path, "arms.def")));
	TEST_EXPECT(w.file_rename.path.empty());
	session.run_operations();
	// The project closing: Files' filter and the prompts start afresh, the find bars close keeping their text.
	TEST_EXPECT(session.handle(request::close_project()));
	TEST_EXPECT(w.files.filter.empty() && w.files.kind == AssetKind::kCount && !w.find.open && w.find.text == "GUN" &&
	            !w.project_find.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"project_find": {"open": true}})")) &&
	            refused_with(session, "workspace.refused") && !w.project_find.open);
	return 0;
}

// Rename everywhere and Rename back: refused with no plan of theirs; a preview_rename that asks the name opens
// Rename everywhere over the plan (its target, its name), a plan of the open one's name moves the name typed,
// rename_symbol closes it; a preview_rename_back that asks opens Rename back, rename_back closes it.
int test_rename_dialogs() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const WorkspaceView &w = v.workspace;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"rename": {"open": true}})")) &&
	            refused_with(session, "workspace.refused") && !w.rename.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"rename_back": {"open": true}})")) &&
	            refused_with(session, "workspace.refused") && !w.rename_back.open);
	session.handle(request::open_document(project.weapons_path));
	const Document *weapons = session.document_for(project.weapons_path);
	NodeAddress gun;
	TEST_EXPECT(weapons && find_definition(AssetGraph(), *weapons, "GUN_A", gun));
	if (!weapons) return 1;
	const std::string locator = weapons->locator(gun);
	TEST_EXPECT(session.handle(request::preview_rename(project.weapons_path, locator, "weapon_name", "GUN_A", true)));
	TEST_EXPECT(w.rename.open && w.rename.path == project.weapons_path && w.rename.locator == locator &&
	            w.rename.field == "weapon_name" && w.rename.old_name == "GUN_A" && w.rename.name == "GUN_A");
	TEST_EXPECT(session.handle(request::preview_rename(project.weapons_path, locator, "weapon_name", "GUN_B")) &&
	            w.rename.name == "GUN_B");
	TEST_EXPECT(session.handle(request::set_workspace(R"({"rename": {"name": "GUN_C"}})")) && w.rename.name == "GUN_C");
	const JsonValue shown = section(session);
	TEST_EXPECT(shown.get("rename") && shown.get("rename")->get_bool("open", false) &&
	            shown.get("rename")->get_string("name", "") == "GUN_C" && shown.get("rename")->get_string("field", "") == "weapon_name");
	TEST_EXPECT(session.handle(request::rename_symbol(project.weapons_path, locator, "weapon_name", "GUN_C")));
	TEST_EXPECT(!w.rename.open);
	session.run_operations();
	TEST_EXPECT(session.handle(request::preview_rename_back(true)) && w.rename_back.open);
	TEST_EXPECT(session.handle(request::rename_back()));
	TEST_EXPECT(!w.rename_back.open);
	session.run_operations();
	return 0;
}

// Problems' filters: severities, a text, the scope and the grouping set (a token no filter takes refused);
// Blocks the build sets aside the filters that could hide a refusal and puts them back, what else changed kept;
// a confirmation asked moves the serial (a finding named by no index refused), {} closes it.
int test_problems() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const WorkspaceView &w = session.view().workspace;
	const WorkspaceView::Problems &p = w.problems;
	TEST_EXPECT(p.errors && p.warnings && p.infos && p.grouping == ProblemGrouping::Kind);
	TEST_EXPECT(session.handle(request::set_workspace(
			R"({"problems": {"severities": ["error"], "text": "gun", "scope": "open_files", "fixable": true}})")));
	TEST_EXPECT(p.errors && !p.warnings && !p.infos && p.text == "gun" && p.scope == ProblemScope::OpenFiles && p.fixable);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"severities": ["fatal"]}})")) &&
	            refused_with(session, "workspace.refused") && !p.warnings);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"group": "row"}})")) &&
	            refused_with(session, "workspace.refused") && p.grouping == ProblemGrouping::Kind);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"blocking": true}})")));
	TEST_EXPECT(p.blocking && p.errors && p.warnings && p.infos && p.text.empty() && p.scope == ProblemScope::Project && !p.fixable);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"group": "file"}})")) && p.grouping == ProblemGrouping::File);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"blocking": false}})")));
	TEST_EXPECT(!p.blocking && p.errors && !p.warnings && p.text == "gun" && p.scope == ProblemScope::OpenFiles && p.fixable &&
	            p.grouping == ProblemGrouping::File);
	// A confirmation of nothing Problems offers is refused (review X10): a group no grouping makes, a finding's
	// index that is no whole number.
	const uint64_t serial = p.confirm_serial;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"confirm": {"group": "requirements"}}})")) &&
	            refused_with(session, "workspace.refused") && !p.confirm.open() && p.confirm_serial == serial);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"confirm": {"finding": "x", "label": "Fix"}}})")) &&
	            refused_with(session, "workspace.refused") && !p.confirm.open());
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"confirm": {}}})")) && !p.confirm.open() &&
	            p.confirm_serial == serial + 1);
	const JsonValue shown = section(session);
	const JsonValue *problems = shown.get("problems");
	TEST_EXPECT(problems && problems->get("severities") && problems->get("severities")->array.size() == 1 &&
	            problems->get_string("scope", "") == "open_files" && problems->get_string("group", "") == "file");
	return 0;
}

// What a document's views show of it: set for an open document (the active one when none is named), its kinds
// by their tokens (one the document has not refused, all of them every kind), refused for a document not open;
// the section lists every open document's; closing the document forgets them, and reading a menu again drops
// the screen its Remove prompt named.
int test_document_views() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const WorkspaceView &w = v.workspace;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"filter": "a"}})")) &&
	            refused_with(session, "workspace.refused") && w.documents.empty());
	session.handle(request::open_document(project.items_path));
	const Document *items = session.document_for(project.items_path);
	TEST_EXPECT(items && !items->kinds().empty());
	if (!items) return 1;
	const std::string kind = items->kinds().front().token;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"filter": "car", "sort": true, "inspector_filter": "id"}})")) &&
	            session.outcome().done());
	const WorkspaceView::DocumentView &shown = w.document(project.items_path);
	TEST_EXPECT(shown.filter == "car" && shown.sort && shown.inspector_filter == "id");
	// What no view of a definition table shows is refused (review X10): kinds to choose (a mission's chips), a
	// menu's window type or Remove prompt, a PCX's palette remap.
	TEST_EXPECT(session.handle(request::set_workspace("{\"document\": {\"kinds\": [\"" + kind + "\"]}}")) &&
	            refused_with(session, "workspace.refused") && shown.kinds == ~uint64_t(0));
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"new_window_type": "button"}})")) &&
	            refused_with(session, "workspace.refused") && shown.new_window_type == "static");
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"remove_screen": 1}})")) &&
	            refused_with(session, "workspace.refused") && shown.remove_screen == 0);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"remap_from": 3}})")) &&
	            refused_with(session, "workspace.refused") && shown.remap_from == 0);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"path": "gone.def", "filter": "x"}})")) &&
	            refused_with(session, "workspace.refused"));
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"remap_from": 300}})")) &&
	            refused_with(session, "workspace.refused"));
	// A text past what the window's field holds is refused as it is read, naming the limit (review X13).
	TEST_EXPECT(!session.handle_json(parsed("{\"kind\": \"set_workspace\", \"workspace\": {\"document\": {\"filter\": \"" +
	                                        std::string(kWorkspaceText, 'a') + "\"}}}")).get_bool("ok", true) &&
	            shown.filter == "car");
	const JsonValue section_json = section(session);
	const JsonValue *documents = section_json.get("documents");
	TEST_EXPECT(documents && documents->array.size() == 1 && documents->array[0].get_string("path", "") == project.items_path &&
	            documents->array[0].get_string("filter", "") == "car" && documents->array[0].get_bool("active", false));
	TEST_EXPECT(session.handle(request::close_document(project.items_path)) && w.documents.empty());
	// A menu's Remove prompt: a screen of the menu while it keeps a second (its last, or an id that is no screen
	// of it, refused: review X10), dropped as the menu is read again; its window type one of TYPE's.
	TEST_EXPECT(editor_test::write_text(v.project.root + "/menus/two.mnu",
	                                    "<SCREEN>\r\n<NAME>FIRST</NAME>\r\n</SCREEN>\r\n<SCREEN>\r\n<NAME>SECOND</NAME>\r\n</SCREEN>\r\n"));
	session.handle(request::rescan());
	session.run_operations();
	const AssetEntry *menu = v.project.scan->find("two.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string menu_path = menu->relative_path;
	session.handle(request::open_document(menu_path));
	const Document *two = session.document_for(menu_path);
	TEST_EXPECT(two && two->rows().size() == 2);
	if (!two || two->rows().size() != 2) return 1;
	const std::string second = std::to_string(two->rows()[1]->id);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"remove_screen": 999}})")) &&
	            refused_with(session, "workspace.refused") && w.document(menu_path).remove_screen == 0);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"document": {"new_window_type": "nonsense"}})")) &&
	            refused_with(session, "workspace.refused") && w.document(menu_path).new_window_type == "static");
	TEST_EXPECT(session.handle(request::set_workspace("{\"document\": {\"remove_screen\": " + second +
	                                                  ", \"new_window_type\": \"button\"}}")) &&
	            session.outcome().done() && w.document(menu_path).remove_screen == two->rows()[1]->id &&
	            w.document(menu_path).new_window_type == "button");
	// The prompt is a dialog that takes the whole editor: the one the session shows (shown_modal).
	TEST_EXPECT(shown_modal(v).modal == HeldModal::RemoveScreen && shown_modal(v).path == menu_path);
	EditorRequest reload = request::of(EditorRequestKind::ReloadDocument);
	reload.path = menu_path;
	TEST_EXPECT(session.handle(reload));
	TEST_EXPECT(w.document(menu_path).remove_screen == 0 && w.document(menu_path).new_window_type == "button");
	return 0;
}

// The import dialog's own: refused with no preview open; a plan made takes its checks (the plan's own);
// an uncheck by the row's index the workspace's (its serial moved, the import_preview query's row saying
// so), and import_files planned takes the checked rows alone; the filters and kinds set (a kind no file has
// refused, an index past the plan refused); Cancel forgets it.
int test_import_dialog() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const WorkspaceView::Import &import = v.workspace.import;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"import": {"filter": "a"}})")) &&
	            refused_with(session, "workspace.refused") && import.filter.empty());
	const std::string source = project.dir.file("source");
	TEST_EXPECT(editor_test::write_text(source + "/extra.mnu", "<SCREEN>\r\n<NAME>EXTRA</NAME>\r\n</SCREEN>\r\n") &&
	            editor_test::write_text(source + "/notes.txt", "notes"));
	TEST_EXPECT(session.handle(request::preview_import({ source + "/extra.mnu", source + "/notes.txt" }, false)));
	session.run_operations();
	const DialogsView::ImportPreview &preview = v.dialogs.import_preview;
	TEST_EXPECT(preview.open && preview.plan && preview.plan->rows.size() == 2 && import.checked.size() == 2 &&
	            import.checked[0] && import.checked[1]);
	if (!preview.plan || preview.plan->rows.size() != 2) return 1;
	size_t menu_row = 0;
	for (size_t i = 0; i < preview.plan->rows.size(); ++i)
		if (preview.plan->rows[i].name == "extra.mnu") menu_row = i;
	const uint64_t serial = import.serial;
	const std::string plan = std::to_string(preview.plan_serial);
	TEST_EXPECT(preview.plan_serial != 0);
	// An index names a row of a plan: check and uncheck name the plan (review X7), as the import_preview query
	// says it, and the indices are whole numbers, as its rows' index is (review X19).
	TEST_EXPECT(session.handle(request::set_workspace("{\"import\": {\"uncheck\": [" + std::to_string(menu_row) + "]}}")) &&
	            refused_with(session, "workspace.refused") && import.checked[menu_row]);
	TEST_EXPECT(!session.handle_json(parsed("{\"kind\": \"set_workspace\", \"workspace\": {\"import\": {\"plan\": " + plan +
	                                        ", \"uncheck\": [\"0\"]}}}")).get_bool("ok", true));
	TEST_EXPECT(session.handle(request::set_workspace("{\"import\": {\"plan\": " + plan + ", \"uncheck\": [" +
	                                                  std::to_string(menu_row) + "]}}")) &&
	            session.outcome().done() && !import.checked[menu_row] && import.serial == serial + 1);
	std::string error;
	const JsonValue rows = session.query("import_preview", JsonValue::make_object(), error);
	TEST_EXPECT(rows.get_number("plan", 0) == double(preview.plan_serial));
	bool said = false;
	for (const JsonValue &row : rows.get("rows") ? rows.get("rows")->array : std::vector<JsonValue>())
		if (row.get_number("index", -1) == double(menu_row)) said = !row.get_bool("checked", true);
	TEST_EXPECT(said);
	TEST_EXPECT(session.handle(request::set_workspace("{\"import\": {\"plan\": " + plan + ", \"check\": [9]}}")) &&
	            refused_with(session, "workspace.refused") && !import.checked[menu_row]);
	// Planned again (the dependencies' setting): the plan is another, the row unchecked stays unchecked (its
	// check carried by row), and the old plan's indices are refused rather than retargeted.
	TEST_EXPECT(session.handle(request::set_import_dependencies(true)));
	session.run_operations();
	TEST_EXPECT(preview.plan_serial != uint64_t(std::stoull(plan)) && import.checked.size() == preview.plan->rows.size());
	for (size_t i = 0; i < preview.plan->rows.size(); ++i)
		if (preview.plan->rows[i].name == "extra.mnu") menu_row = i;
	TEST_EXPECT(!import.checked[menu_row]);
	TEST_EXPECT(session.handle(request::set_workspace("{\"import\": {\"plan\": " + plan + ", \"check\": [" +
	                                                  std::to_string(menu_row) + "]}}")) &&
	            refused_with(session, "workspace.refused") && !import.checked[menu_row]);
	EditorRequest stale = request::import_planned(std::stoull(plan));
	TEST_EXPECT(session.handle(stale) && !session.outcome().done() && v.dialogs.import_preview.open);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"import": {"filter": "ext", "kind_shown": "menu", "rows_filter": "e"}})")) &&
	            import.filter == "ext" && import.kind_shown == AssetKind::Menu && import.rows_filter == "e");
	TEST_EXPECT(session.handle(request::set_workspace(R"({"import": {"choice_kind": "nothing"}})")) &&
	            refused_with(session, "workspace.refused") && import.choice_kind == AssetKind::kCount);
	const JsonValue shown = section(session);
	TEST_EXPECT(shown.get("import") && shown.get("import")->get_number("checked", -1) == 1.0 &&
	            shown.get("import")->get_string("kind_shown", "") == "menu");
	// Import takes the checked rows alone: the notes, not the menu.
	TEST_EXPECT(session.handle(request::import_planned(preview.plan_serial)));
	session.run_operations();
	TEST_EXPECT(v.project.scan->find("notes.txt") != nullptr && v.project.scan->find("extra.mnu") == nullptr);
	TEST_EXPECT(!preview.open && import.checked.empty() && import.filter.empty());
	// A new preview starts afresh; Cancel forgets it.
	TEST_EXPECT(session.handle(request::preview_import({ source + "/extra.mnu" }, false)));
	session.run_operations();
	TEST_EXPECT(session.handle(request::set_workspace(R"({"import": {"replace_existing": true, "filter": "x"}})")) &&
	            import.replace_existing && import.filter == "x");
	TEST_EXPECT(session.handle(request::cancel_import()) && !import.replace_existing && import.filter.empty());
	return 0;
}

// The dialogs that take the whole editor (review X3): one shows at a time, the first the session holds in its
// order (shown_modal; the workspace section's modal); a new_project closes its form's modal as the session takes
// it, so with unsaved edits the unsaved prompt shows alone, and its Cancel leaves neither. Opening a dialog moves
// the workspace's opened (X17); focus moves nothing of the workspace (X22); a set_workspace refused says so in its
// outcome alone, no Problems row and the status line as it was (X18).
int test_modals() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const WorkspaceView &w = v.workspace;
	const std::string root = v.project.root;
	TEST_EXPECT(shown_modal(v).modal == HeldModal::None && section(session).get_string("modal", "?").empty());
	const uint64_t opened = w.opened;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_project": {"open": true}, "settings": {"open": true}})")));
	TEST_EXPECT(w.new_project.open && w.settings.open && w.opened == opened + 1 && shown_modal(v).modal == HeldModal::Settings &&
	            section(session).get_string("modal", "") == "settings");
	TEST_EXPECT(session.handle(request::set_workspace(R"({"settings": {"open": false}})")) && w.opened == opened + 1 &&
	            shown_modal(v).modal == HeldModal::NewProject);
	// Unsaved edits, then Create: the guard holds it on the unsaved prompt, and the form's modal closes as it is taken.
	const AssetEntry *menu = v.project.scan->find("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string menu_path = menu->relative_path;
	session.handle(request::open_document(menu_path));
	const Document *main_menu = session.document_for(menu_path);
	TEST_EXPECT(main_menu && !main_menu->rows().empty());
	if (!main_menu || main_menu->rows().empty()) return 1;
	Edit rename;
	rename.operation = EditOperation::Set;
	rename.address = { main_menu->rows().front()->id, main_menu->rows().front()->kind, 0 };
	rename.field = "name";
	rename.value = std::string("RENAMED");
	TEST_EXPECT(session.handle(request::edit_record(menu_path, rename)) && main_menu->dirty());
	TEST_EXPECT(session.handle(request::new_project(project.dir.file("other"), "Other")));
	TEST_EXPECT(v.dialogs.unsaved_prompt.open && !w.new_project.open && shown_modal(v).modal == HeldModal::Unsaved);
	TEST_EXPECT(session.handle(request::resolve_unsaved(UnsavedChoice::Cancel)));
	TEST_EXPECT(!v.dialogs.unsaved_prompt.open && shown_modal(v).modal == HeldModal::None && v.project.root == root);
	// focus: the window brought forward (an event), nothing of the workspace moved.
	const uint64_t revision = v.revisions.of(ViewConcern::Workspace), dialogs = v.revisions.of(ViewConcern::Dialogs);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"focus": "files"})")) && session.outcome().done() &&
	            v.revisions.of(ViewConcern::Workspace) == revision && v.revisions.of(ViewConcern::Dialogs) > dialogs &&
	            v.events.held().back().kind == ViewEventKind::FocusWindow);
	// A refusal in the outcome alone.
	const size_t rows = v.findings.diagnostics.size();
	const std::string status = v.activity.status;
	TEST_EXPECT(session.handle(request::set_workspace(R"({"card": {"path": "nothing.wav"}})")) &&
	            refused_with(session, "workspace.refused") && v.findings.diagnostics.size() == rows && v.activity.status == status);
	// The New file prompt opens on a kind Files' New asks a name of, its values by the kind's params (X10).
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_file": {"kind": "music_bank"}})")) &&
	            refused_with(session, "workspace.refused") && w.new_file.kind == AssetKind::kCount);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"new_file": {"kind": "menu", "values": {"terrain": "x"}}})")) &&
	            refused_with(session, "workspace.refused") && w.new_file.kind == AssetKind::kCount);
	return 0;
}

// The card and Rename... are the session's to keep true (review X9): a rename of the card's file moves it to the
// new name; the file gone (deleted on disk, the files read again), the card and Rename... close, the sound it
// played stopped; Rename everywhere closes once another rename is planned in its place, Rename back once the plan
// is no way back (X20).
int test_files_followed() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const WorkspaceView &w = v.workspace;
	TEST_EXPECT(editor_test::write_text(v.project.root + "/menus/extra.mnu", "<SCREEN>\r\n<NAME>EXTRA</NAME>\r\n</SCREEN>\r\n"));
	session.handle(request::rescan());
	session.run_operations();
	const AssetEntry *extra = v.project.scan->find("extra.mnu");
	TEST_EXPECT(extra != nullptr);
	if (!extra) return 1;
	const std::string extra_path = extra->relative_path;
	TEST_EXPECT(session.handle(request::about_file(extra_path)) && w.card.path == extra_path);
	TEST_EXPECT(session.handle(request::rename_asset(extra_path, "things.mnu")));
	session.run_operations();
	const AssetEntry *things = v.project.scan->find("things.mnu");
	TEST_EXPECT(things && w.card.path == things->relative_path);
	if (!things) return 1;
	const std::string things_path = things->relative_path;
	TEST_EXPECT(session.handle(request::set_workspace("{\"file_rename\": {\"path\": \"" + project.weapons_path + "\"}}")) &&
	            w.file_rename.path == project.weapons_path);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"file_rename": {"name": "arms.def"}})")));
	std::error_code ec;
	std::filesystem::remove(std::filesystem::path(v.project.root) / things_path, ec);
	std::filesystem::remove(std::filesystem::path(v.project.root) / project.weapons_path, ec);
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(w.card.path.empty() && w.file_rename.path.empty());
	return 0;
}

// Problems' confirmation on the wire (review X11): a group's Fix all and the summary's offered while the project
// lacks required files, what it proposes in the workspace section, apply_confirmation raising it (one
// create_missing naming every role) and closing it; one that proposes nothing refused, nothing raised.
int test_confirmation_applied() {
	editor_test::TempProjectDir dir("opennova_editor_workspace_confirm");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	TEST_EXPECT(session.handle(request::new_project(dir.file("project"), "Bare")));
	session.run_operations();
	const SessionView &v = session.view();
	const WorkspaceView::Problems &p = v.workspace.problems;
	TEST_EXPECT(session.handle(request::apply_confirmation()) && refused_with(session, "workspace.refused"));
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"confirm": {"required": "create_missing"}}})")) &&
	            session.outcome().done() && p.confirm.required == "create_missing");
	const JsonValue shown = section(session);
	const JsonValue *proposal = shown.get("problems") && shown.get("problems")->get("confirm")
	                                    ? shown.get("problems")->get("confirm")->get("proposal")
	                                    : nullptr;
	TEST_EXPECT(proposal && proposal->get("requests") && proposal->get("requests")->array.size() == 1 &&
	            proposal->get("requests")->array[0].get_string("kind", "") == "create_missing" &&
	            !proposal->get("lines")->array.empty());
	TEST_EXPECT(session.handle(request::apply_confirmation()));
	session.run_operations();
	TEST_EXPECT(!p.confirm.open() && v.project.scan->find("main.mnu") != nullptr);
	// Nothing missing now: the summary's Fix all is no longer offered.
	TEST_EXPECT(session.handle(request::set_workspace(R"({"problems": {"confirm": {"required": "create_missing"}}})")) &&
	            refused_with(session, "workspace.refused") && !p.confirm.open());
	return 0;
}

// The problems query names each row's finding by its index (what a confirmation of its fix names).
int test_problems_index() {
	FilesProject project;
	TEST_EXPECT(project.open());
	ProjectSession &session = project.session;
	std::string error;
	const JsonValue answer = session.query("problems", JsonValue::make_object(), error);
	const JsonValue *rows = answer.get("problems");
	TEST_EXPECT(rows && !rows->array.empty());
	for (size_t i = 0; rows && i < rows->array.size(); ++i) {
		const double index = rows->array[i].get_number("index", -1.0);
		TEST_EXPECT(index >= 0.0 && size_t(index) < session.view().findings.diagnostics.size() &&
		            session.view().findings.diagnostics[size_t(index)].message == rows->array[i].get_string("message", ""));
	}
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_table_and_wire();
	failed += test_new_project_form();
	failed += test_build_result_card_and_focus();
	failed += test_settings();
	failed += test_files_find_and_prompts();
	failed += test_rename_dialogs();
	failed += test_problems();
	failed += test_document_views();
	failed += test_problems_index();
	failed += test_import_dialog();
	failed += test_modals();
	failed += test_files_followed();
	failed += test_confirmation_applied();
	if (failed == 0) std::printf("editor_workspace_parts: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
