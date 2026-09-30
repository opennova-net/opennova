// A document's reference queries (ADR 0046 S12 D, S13 D3: graph/reference_queries, free functions
// over the graph and the scan): the badge, the missing finding and the picker agree on a menu's
// references; where Go to leads on a menu's ACTION targets, and who uses a file; and
// find_definition, over the graph's slot while it is current for the document and over the
// document's own extraction when it is not, giving the same record each way, each definition as
// the document's own lookup makes it (a stylesheet's line brand.mns overrides is still the one its
// own file reads last).
#include <algorithm>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/graph_test_support.h"
#include "editor/menu_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

using editor_test::NoProcess;
using graph_test::add_record;
using graph_test::edit_window;
using graph_test::go_to;
using graph_test::has_missing;
using graph_test::missing_of;
using graph_test::screen;
using graph_test::set_image;
using graph_test::targets_of;
using graph_test::window;

// find_definition over the graph (its slot, when current) and over the document alone: the same
// answer.
bool found_alike(const AssetGraph &graph, const Document &document, const std::string &symbol,
		NodeAddress &out, const std::string &scope = std::string()) {
	NodeAddress alone;
	const bool by_graph = find_definition(graph, document, symbol, out, scope);
	const bool by_document = find_definition(AssetGraph(), document, symbol, alone, scope);
	return by_graph == by_document && (!by_graph || out == alone);
}

} // namespace

// The badge, the finding and the picker agree on a menu's references, through the
// reference queries and the graph alike.
static int test_menu_references() {
	editor_test::TempProjectDir dir("opennova_reference_queries_menu");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Menus"));
	editor_test::create_missing_files(session);
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	const Document *document = session.document_for("main.mnu");
	TEST_EXPECT(document);
	if (!document) return 1;
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.findings.graph;
	NodeAddress exit;
	TEST_EXPECT(find_definition(graph, *document, "EXIT", exit));
	const FieldSchema *font = nullptr, *value = nullptr;
	for (const FieldSchema &field : document->fields(exit.kind))
		if (field.id == "font.name") font = &field;
	const NodeAddress row = menu_test::child_of(*document, exit, "appearance");
	for (const FieldSchema &field : document->fields(row.kind))
		if (field.id == "value") value = &field;
	TEST_EXPECT(font && value);
	if (!font || !value) return 1;
	edit_window(session, *document, exit, "font.name", std::string("%NOPE%"));
	TEST_EXPECT(has_missing(view.findings.diagnostics, "font.name", DiagnosticSeverity::Warning));
	const FieldUse font_use = document->field_on(exit, *font);
	std::string symbol;
	TEST_EXPECT(reference_status(graph, font_use, std::string("%NOPE%"), &symbol) ==
					ReferenceStatus::Missing &&
			symbol == "%NOPE%");
	TEST_EXPECT(reference_status(graph, font_use, std::string("%DEF_FONTNAME_LG%")) ==
			ReferenceStatus::Present);
	TEST_EXPECT(reference_status(graph, font_use, std::string()) == ReferenceStatus::NotAReference);
	// The finding says what it misses (S11b): the reference's kind and name, as written.
	const Diagnostic *nope =
			missing_of(view.findings.diagnostics, "font.name", ReferenceKind::StyleVar);
	TEST_EXPECT(nope && nope->target == "%NOPE%");
	// The picker's finding of the value is the same: the variable's (the stylesheet opened to
	// define it), never a font file named %NOPE%; a variable that resolves makes none.
	Diagnostic picked;
	TEST_EXPECT(missing_finding(graph, *document, exit, font_use, std::string("%NOPE%"), picked) &&
	            picked.reference == ReferenceKind::StyleVar && picked.target == "%NOPE%" && picked.field == "font.name");
	const std::vector<ProblemFix> define = fixes_for(picked, view);
	TEST_EXPECT(!define.empty() && define.front().request.kind == EditorRequestKind::OpenDocument &&
	            define.front().request.path.find("menu_style.mns") != std::string::npos);
	TEST_EXPECT(!missing_finding(
			graph, *document, exit, font_use, std::string("%DEF_FONTNAME_LG%"), picked));
	// The file a resolving value loads: the variable's value's .fnt.
	const std::string loaded =
			reference_target_file(graph, font_use, std::string("%DEF_FONTNAME_LG%"));
	TEST_EXPECT(!loaded.empty() && loaded.size() > 4 && loaded.substr(loaded.size() - 4) == ".fnt");
	TEST_EXPECT(reference_target_file(graph, font_use, std::string("%NOPE%")).empty());
	// A name the game's expansion stops inside (a space) is no variable: a font file of
	// that name (S12 B2).
	edit_window(session, *document, exit, "font.name", std::string("%NO PE%"));
	TEST_EXPECT(!missing_of(view.findings.diagnostics, "font.name", ReferenceKind::StyleVar));
	const Diagnostic *spaced =
			missing_of(view.findings.diagnostics, "font.name", ReferenceKind::Font);
	TEST_EXPECT(spaced && spaced->target == "%NO PE%");
	edit_window(session, *document, exit, "font.name", std::string("nofont.fnt"));
	TEST_EXPECT(has_missing(view.findings.diagnostics, "font.name", DiagnosticSeverity::Error));
	const Diagnostic *nofont =
			missing_of(view.findings.diagnostics, "font.name", ReferenceKind::Font);
	TEST_EXPECT(nofont && nofont->target == "nofont.fnt" && nofont->scope.empty() && nofont->role.empty());
	TEST_EXPECT(
			missing_finding(graph, *document, exit, font_use, std::string("nofont.fnt"), picked) &&
			picked.reference == ReferenceKind::Font && picked.target == "nofont.fnt");
	// An APPEARANCE row's value is a texture for an IMAGE row, nothing for a typeless one.
	set_image(session, *document, exit, "missing.tga");
	TEST_EXPECT(has_missing(view.findings.diagnostics, "value", DiagnosticSeverity::Error));
	TEST_EXPECT(reference_status(graph, document->field_on(row, *value),
						std::string("missing.tga")) == ReferenceStatus::Missing);
	const Diagnostic *texture =
			missing_of(view.findings.diagnostics, "value", ReferenceKind::MenuTexture);
	TEST_EXPECT(texture && texture->target == "missing.tga");
	edit_window(session, *document, exit, "string.type", std::string("ID"));
	edit_window(session, *document, exit, "string.value", std::string("NO_SUCH_ID"));
	TEST_EXPECT(
			has_missing(view.findings.diagnostics, "string.value", DiagnosticSeverity::Warning));
	// A string id's scope is where it was looked up: the "menu" section of its window's table.
	const Diagnostic *text_id =
			missing_of(view.findings.diagnostics, "string.value", ReferenceKind::TextId);
	TEST_EXPECT(text_id && text_id->target == "NO_SUCH_ID" && text_id->scope.find("/menu") != std::string::npos);
	// Every ACTION's file is an edge (a second action's too), every SOUND's file a sound
	// bank the game opens by that name (a warning while the project lacks it).
	for (const char *file : {"other.mnu", "third.mnu"}) {
		const NodeAddress action = add_record(session, *document, exit, "action");
		edit_window(session, *document, action, "type", std::string("SCREEN"));
		edit_window(session, *document, action, "file", std::string(file));
	}
	const NodeAddress sound = add_record(session, *document, exit, "sound");
	edit_window(session, *document, sound, "file", std::string("click.lwf"));
	size_t action_files = 0, sounds = 0;
	for (const GraphEdge *edge : graph.references_of(document->path())) {
		if (edge->field == "file" && edge->kind == ReferenceKind::Menu) ++action_files;
		if (edge->field == "file" && edge->kind == ReferenceKind::SoundBank && edge->value == "click.lwf") ++sounds;
	}
	TEST_EXPECT(action_files == 2 && sounds == 1);
	TEST_EXPECT(graph.resolve(ReferenceKind::SoundBank, "click.lwf") == ReferenceStatus::Missing);
	TEST_EXPECT(has_missing(view.findings.diagnostics, "file", DiagnosticSeverity::Warning));
	// The font picker: the project's fonts, then the stylesheet's variables, each as the field
	// would reference it.
	const std::vector<ReferenceChoice> fonts = reference_choices(graph, font_use);
	TEST_EXPECT(std::any_of(fonts.begin(), fonts.end(), [](const ReferenceChoice &c) { return c.kind == ReferenceKind::Font; }));
	TEST_EXPECT(std::any_of(fonts.begin(), fonts.end(), [](const ReferenceChoice &c) {
		return c.kind == ReferenceKind::StyleVar && c.name.front() == '%' && !c.file.empty() && !c.record.empty();
	}));
	const auto first_variable = std::find_if(fonts.begin(), fonts.end(),
			[](const ReferenceChoice &c) { return c.kind == ReferenceKind::StyleVar; });
	TEST_EXPECT(std::none_of(first_variable, fonts.end(),
			[](const ReferenceChoice &c) { return c.kind == ReferenceKind::Font; }));
	// The finding names the record and the session's address, so Problems can select it.
	const NodeAddress second_action = menu_test::child_of(*document, exit, "action", 1);
	bool located = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code == "reference.missing" && d.field == "file" && d.row_id == second_action.row &&
		    d.child_id == second_action.child && d.record == "STARTUP/MAIN/EXIT/Action 2")
			located = true;
	TEST_EXPECT(located);
	// A saved menu with a missing texture is blocked by the build (an error).
	session.handle(make_request(EditorRequestKind::SaveAll));
	session.handle(make_request(EditorRequestKind::Build));
	session.run_operations();
	TEST_EXPECT(!view.activity.last_build->ok);
	return 0;
}

// Go to on a menu's ACTION targets (S12 D3): a WINDOW target on each of two screens that both
// have a TITLE goes to its own screen's TITLE, in the same file, which the Go to selects (the
// locator found again, the NAME shown); a SCREEN target to the screen of its FILE; the file's
// usages are the uses of its screens and windows, another menu's too. A use, a definition and a
// file each go where the scan says: opened when the editor edits the file's kind, else shown.
static int test_go_to_targets() {
	editor_test::TempProjectDir dir("opennova_reference_queries_go_to");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "GoTo"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string go = "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">TITLE</ACTION>\r\n";
	const std::string back = "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">HOME</ACTION>\r\n"
	                         "<ACTION TYPE=\"WINDOW\" STATE=\"HIDE\">TITLE</ACTION>\r\n";
	TEST_EXPECT(editor_test::write_text(
	        root + "/flow.mnu",
	        screen("HOME", window("STATIC", "PANEL",
	                              window("BUTTON", "GO", go) + window("STATIC", "TITLE") + window("STATIC", "AWAY"))) +
	                screen("AWAY", window("STATIC", "BACKDROP", window("BUTTON", "BACK", back) + window("STATIC", "TITLE")))));
	TEST_EXPECT(editor_test::write_text(
	        root + "/other.mnu",
	        screen("OTHER", window("BUTTON", "JUMP", "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">AWAY</ACTION>\r\n"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "flow.mnu"));
	Document *menu = session.document_for("flow.mnu");
	TEST_EXPECT(menu && menu->rows().size() == 2);
	if (!menu || menu->rows().size() != 2) return 1;
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.findings.graph;
	NodeAddress go_window, back_window, home_title, away_title;
	TEST_EXPECT(found_alike(graph, *menu, "GO", go_window) &&
			found_alike(graph, *menu, "BACK", back_window));
	TEST_EXPECT(found_alike(graph, *menu, "TITLE", home_title, "FLOW.MNU/HOME") &&
	            found_alike(graph, *menu, "TITLE", away_title, "FLOW.MNU/AWAY"));
	TEST_EXPECT(home_title != away_title && home_title.row == menu->rows()[0]->id && away_title.row == menu->rows()[1]->id);
	const NodeAddress go_title = menu_test::child_of(*menu, go_window, "action", 0);
	const NodeAddress back_home = menu_test::child_of(*menu, back_window, "action", 0);
	const NodeAddress back_title = menu_test::child_of(*menu, back_window, "action", 1);
	const std::vector<ReferenceTarget> to_home_title = targets_of(*menu, go_title, "target", view);
	const std::vector<ReferenceTarget> to_away_title = targets_of(*menu, back_title, "target", view);
	TEST_EXPECT(to_home_title.size() == 1 && to_home_title[0].file == menu->path() && to_home_title[0].editable &&
	            to_home_title[0].field == "name" && menu->address_at(to_home_title[0].locator) == home_title);
	TEST_EXPECT(to_away_title.size() == 1 && menu->address_at(to_away_title[0].locator) == away_title);
	const std::vector<ReferenceTarget> to_home = targets_of(*menu, back_home, "target", view);
	TEST_EXPECT(to_home.size() == 1 && menu->address_at(to_home[0].locator) == NodeAddress({menu->rows()[0]->id, 0, 0}));
	// HOME holds a window named AWAY, a later screen is named AWAY: a find by the name is the
	// screen (a row's definition before a nested one's), and so is the SCREEN target's Go to.
	const NodeAddress away_screen{menu->rows()[1]->id, 0, 0};
	NodeAddress away;
	TEST_EXPECT(found_alike(graph, *menu, "AWAY", away) && away == away_screen);
	TEST_EXPECT(found_alike(graph, *menu, "AWAY", away, "FLOW.MNU/HOME") &&
			away.row == menu->rows()[0]->id && away.child != 0);
	const GraphEdge *jump = nullptr;
	for (const GraphEdge *edge : graph.references_of("other.mnu"))
		if (edge->kind == ReferenceKind::MenuScreen) jump = edge;
	TEST_EXPECT(jump != nullptr);
	if (jump) {
		const GraphSymbol *screen_symbol =
				graph.resolve_symbol(jump->kind, jump->value, jump->scope);
		TEST_EXPECT(screen_symbol && menu->address_at(screen_symbol->locator) == away_screen);
		// A definition opens where it is defined, the use where it is made.
		if (screen_symbol) {
			const ReferenceTarget defined = symbol_target(*view.project.scan, *screen_symbol);
			TEST_EXPECT(defined.file == menu->path() && defined.editable &&
					defined.field == screen_symbol->field &&
					defined.label.find("AWAY") != std::string::npos);
		}
		const ReferenceTarget used = usage_target(*view.project.scan, *jump);
		TEST_EXPECT(used.file == "other.mnu" && used.editable && used.locator == jump->locator &&
		            used.label == "other.mnu: OTHER/JUMP/Action 1");
	}
	// A file the editor does not edit (a font) is shown in Files; a menu opened.
	std::string font_file;
	TEST_EXPECT(graph.resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(),
						&font_file) == ReferenceStatus::Present);
	TEST_EXPECT(!file_target(*view.project.scan, font_file).editable &&
			file_target(*view.project.scan, "other.mnu").editable &&
			file_target(*view.project.scan, "other.mnu").locator.empty());
	// The same file: the Go to selects the record there, its NAME shown.
	if (to_away_title.size() == 1) go_to(session, to_away_title[0]);
	TEST_EXPECT(view.documents.active == menu->path() && view.documents.selection == away_title && editor_test::revealed_field(view) == "name");
	const std::vector<const GraphEdge *> uses = graph.usages_of(menu->path());
	const auto used_by = [&uses](const char *source, const char *record) {
		return std::any_of(uses.begin(), uses.end(), [&](const GraphEdge *edge) {
			return edge->source == source && edge->record == record;
		});
	};
	TEST_EXPECT(used_by("other.mnu", "OTHER/JUMP/Action 1") && used_by("flow.mnu", "AWAY/BACKDROP/BACK/Action 2") &&
	            used_by("flow.mnu", "HOME/PANEL/GO/Action 1"));
	return 0;
}

// find_definition reads the graph's slot while it is current for the document (the same
// instance at the same revision), and the document's own extraction otherwise, with one answer:
// each definition as the document's own lookup makes it. menu_style.mns defines DEF_TEXT_FG twice
// and brand.mns once more: the graph publishes both of menu_style.mns's inert (the game reads
// brand.mns's), the stylesheet's own lookup reads its later line, and that is the one found.
static int test_find_definition() {
	editor_test::TempProjectDir dir("opennova_reference_queries_find");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Find"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const std::string style_path = style->relative_path;
	const std::string style_file = view.project.root + "/" + style_path;
	const std::string style_dir = style_file.substr(0, style_file.rfind('/'));
	std::string text, problem;
	TEST_EXPECT(read_file_text(style_file, text, problem));
	TEST_EXPECT(editor_test::write_text(
			style_file, text + "\r\nDEF_TEXT_FG FF00FF00\r\nTWICE 1\r\nTWICE 2\r\n"));
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG FF102030\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, style_path));
	Document *sheet = session.document_for(style_path);
	TEST_EXPECT(sheet != nullptr);
	if (!sheet) return 1;
	const AssetGraph &graph = *view.findings.graph;
	// The slot is current: the graph's definitions are read, each with the stylesheet's own inert.
	size_t read = 0, own_inert = 0, published_inert = 0;
	TEST_EXPECT(graph.for_each_definition(*sheet, [&](const GraphSymbol &symbol, bool inert) {
		++read;
		if (symbol.name != "DEF_TEXT_FG") return;
		own_inert += inert;
		published_inert += symbol.inert;
	}));
	TEST_EXPECT(read > 0 && own_inert == 1 && published_inert == 2);
	NodeAddress fg, twice, by_variable;
	TEST_EXPECT(found_alike(graph, *sheet, "DEF_TEXT_FG", fg) &&
			found_alike(graph, *sheet, "%def_text_fg%", by_variable) && fg == by_variable);
	Value value;
	TEST_EXPECT(sheet->get(fg, "value", value) && std::get<std::string>(value) == "FF00FF00");
	TEST_EXPECT(found_alike(graph, *sheet, "TWICE", twice) && sheet->get(twice, "value", value) &&
	            std::get<std::string>(value) == "2");
	NodeAddress none;
	TEST_EXPECT(!find_definition(graph, *sheet, "NO_SUCH_NAME", none) &&
	            !find_definition(graph, *sheet, "TWICE", none, "ELSEWHERE.MNS"));
	// An edit the graph has not read: the slot is stale, the document's own extraction answers,
	// the same way.
	Edit set;
	set.operation = EditOperation::Set;
	set.address = twice;
	set.field = "value";
	set.value = std::string("3");
	Diagnostic error;
	TEST_EXPECT(sheet->apply(set, error));
	TEST_EXPECT(!graph.for_each_definition(*sheet, [](const GraphSymbol &, bool) {}));
	NodeAddress again;
	TEST_EXPECT(find_definition(graph, *sheet, "TWICE", again) && again == twice);
	TEST_EXPECT(find_definition(graph, *sheet, "DEF_TEXT_FG", again) && again == fg);
	// A record found by its name where no field defines it: a menu's window by its NAME in the
	// graph's slot, and a closed file never current.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(
			menu && found_alike(graph, *menu, "EXIT", exit) && menu->record_name(exit) == "EXIT");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_menu_references();
	failures += test_go_to_targets();
	failures += test_find_definition();
	if (failures == 0) std::printf("editor_reference_queries: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
