// The asset graph (ADR 0046 d10, S7): a blank project's references all resolve; the
// document types' edges and symbols (a catalog's weapon and ammo names and item ids, a
// model's textures, a table's string ids, the stylesheet's variables, a menu's fonts
// through them, S9l: every menu reference, its screens and windows by NAME and its
// ACTIONs' targets); the native extractors (an environment's sky and celestial models, a
// particle file's effects and textures, a mission's terrain, environment and item
// ids); a missing target as a finding the session shows, naming the reference's kind,
// name and scope (S11b), and the file names a reference loads (S11f: a model's texture row
// by its type, as the runtime's loaders pick the file); unchanged files reused on the
// next update; the rename transaction: the sites rewritten and the file moved, or
// the refusals that leave everything as it was; a menu name followed into its file's
// ACTIONs; and (a SKIP-LEG without OPENNOVA_JO_ASSETS) the shipped menus' targets. S12: the
// reference kinds' table (D1), a symbol per defining field with its place (D2), and where Go
// to leads and who uses a file (D3).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <base/resource_index/texture_candidates.h>
#include <editor/assets/asset_import.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/project/project_files.h>
#include <editor/session/project_session.h>
#include <editor/session/session_json.h>
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_mis.h>
#include <runtime/renderer/material_texture.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/menu_test_support.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

using editor_test::NoProcess;

const GraphEdge *edge_to(const AssetGraph &graph, const std::string &source, ReferenceKind kind, const std::string &value) {
	for (const GraphEdge &edge : graph.edges())
		if (edge.source == source && edge.kind == kind && edge.value == value) return &edge;
	return nullptr;
}

// A field as it applies to a record, and where its Go to leads (Document::reference_targets).
FieldUse field_on(const Document &document, const NodeAddress &record, const std::string &id) {
	for (const FieldSchema &schema : document.fields(record.kind))
		if (schema.id == id) return document.field_on(record, schema);
	return FieldUse();
}

std::vector<ReferenceTarget> targets_of(const Document &document, const NodeAddress &record, const std::string &id,
                                        const SessionView &view) {
	Value value;
	if (!document.get(record, id, value)) return {};
	return document.reference_targets(field_on(document, record, id), value, view);
}

// A Go to served as the Inspector raises it (window_requests::go_to): the file opened at the
// record by its locator, its field shown.
void go_to(ProjectSession &session, const ReferenceTarget &target) {
	EditorRequest open = make_request(EditorRequestKind::OpenDocument, target.file, target.locator);
	open.edit.field = target.field;
	session.handle(open);
}

bool has_symbol(const AssetGraph &graph, ReferenceKind kind, const std::string &display) {
	for (const GraphSymbol &symbol : graph.symbols())
		if (symbol.kind == kind && symbol.display == display) return true;
	return false;
}

bool has_missing(const std::vector<Diagnostic> &diagnostics, const std::string &field, DiagnosticSeverity severity) {
	for (const Diagnostic &d : diagnostics)
		if (d.code == "reference.missing" && d.field == field && d.severity == severity) return true;
	return false;
}

// The missing-reference finding on a field that says what it misses: the reference's kind.
const Diagnostic *missing_of(const std::vector<Diagnostic> &diagnostics, const std::string &field, ReferenceKind kind) {
	for (const Diagnostic &d : diagnostics)
		if (d.code == "reference.missing" && d.field == field && d.reference == kind) return &d;
	return nullptr;
}

size_t count_code(const std::vector<Diagnostic> &diagnostics, const char *code) {
	size_t n = 0;
	for (const Diagnostic &d : diagnostics) if (d.code == code) ++n;
	return n;
}

std::string read_text(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

void edit_window(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field,
                 Value value) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.address = address;
	request.edit.field = field;
	request.edit.value = std::move(value);
	session.handle(request);
}

// A window's first APPEARANCE row made an image of `texture` (one batch).
void set_image(ProjectSession &session, const Document &document, const NodeAddress &window, const std::string &texture) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edits = menu_test::image_edits(document, window, texture);
	session.handle(request);
}

// A new record of `token` inside `owner`: its address.
NodeAddress add_record(ProjectSession &session, const Document &document, const NodeAddress &owner, const char *token) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = EditOperation::Add;
	request.edit.address = {owner.row, document.kind_from_name(token), 0};
	request.edit.parent = owner.child;
	session.handle(request);
	return document.address_of(document.last_added());
}

} // namespace

// A new project after Create all missing: every reference the blank files make resolves.
static int test_blank_project() {
	editor_test::TempProjectDir dir("opennova_asset_graph_blank");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Graph"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.graph != nullptr);
	const AssetGraph &graph = *view.graph;
	TEST_EXPECT(!graph.edges().empty() && !graph.symbols().empty());
	TEST_EXPECT(graph.missing().empty());
	TEST_EXPECT(count_code(view.diagnostics, "reference.missing") == 0);
	TEST_EXPECT(has_symbol(graph, ReferenceKind::StyleVar, "DEF_FONTNAME_LG"));
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%DEF_FONTNAME_LG%") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%NOPE%") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "NOPE") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "def_fontname_lg") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve_style("%DEF_FONTNAME_LG%") != "%DEF_FONTNAME_LG%");
	// The blank menu names its fonts through the stylesheet: a StyleVar edge and a Font
	// edge per window, both present.
	const std::vector<const GraphEdge *> references = graph.references_of("main.mnu");
	TEST_EXPECT(!references.empty());
	bool font_edge = false, var_edge = false;
	for (const GraphEdge *edge : references) {
		if (edge->kind == ReferenceKind::Font) { font_edge = true; TEST_EXPECT(edge->rewritable); }
		if (edge->kind == ReferenceKind::StyleVar) var_edge = true;
		TEST_EXPECT(!edge->record.empty());
	}
	TEST_EXPECT(font_edge && var_edge);
	// A font file is referenced by the menu (through the variable it resolves).
	std::string font_file;
	TEST_EXPECT(graph.resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &font_file) == ReferenceStatus::Present);
	TEST_EXPECT(!font_file.empty() && !graph.referrers_of_file(font_file).empty());
	TEST_EXPECT(graph.referrers_of_file("nothing.fnt").empty());
	// The blank tables define nothing yet: the stylesheet's variables, the blank item
	// table's null marker and the blank menus' screens and windows are the symbols.
	for (const GraphSymbol &symbol : graph.symbols())
		TEST_EXPECT(symbol.kind == ReferenceKind::StyleVar || symbol.kind == ReferenceKind::Item ||
		            symbol.kind == ReferenceKind::MenuScreen || symbol.kind == ReferenceKind::MenuWindow);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuScreen, "startup", "MAIN.MNU") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuWindow, "exit", "MAIN.MNU/STARTUP") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuWindow, "EXIT", "MAIN.MNU/ELSEWHERE") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.choices(ReferenceKind::Font).size() >= 7);
	TEST_EXPECT(graph.choices(ReferenceKind::StyleVar).front().name.front() == '%');
	for (const ReferenceChoice &choice : graph.choices(ReferenceKind::Font))
		TEST_EXPECT(choice.kind == ReferenceKind::Font && choice.status == ReferenceStatus::Present && !choice.file.empty() &&
		            choice.record.empty() && !choice.inert);
	TEST_EXPECT(graph.resolve(ReferenceKind::Sound, "boom.wav") == ReferenceStatus::Unverified);
	TEST_EXPECT(graph.resolve(ReferenceKind::None, "x") == ReferenceStatus::NotAReference);
	TEST_EXPECT(graph.stats().files_extracted > 0);
	// Nothing changed: the next update reuses every extraction.
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(graph.stats().files_extracted == 0 && graph.stats().files_reused > 0);
	return 0;
}

// The badge, the finding and the picker agree on a menu's references, through the
// document's queries and the graph alike.
static int test_menu_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_menu");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Menus"));
	editor_test::create_missing_files(session);
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	const Document *document = session.document_for("main.mnu");
	TEST_EXPECT(document);
	const SessionView &view = session.view();
	NodeAddress exit;
	TEST_EXPECT(document->find("EXIT", exit));
	const FieldSchema *font = nullptr, *value = nullptr;
	for (const FieldSchema &field : document->fields(exit.kind))
		if (field.id == "font.name") font = &field;
	const NodeAddress row = menu_test::child_of(*document, exit, "appearance");
	for (const FieldSchema &field : document->fields(row.kind))
		if (field.id == "value") value = &field;
	TEST_EXPECT(font && value);
	edit_window(session, *document, exit, "font.name", std::string("%NOPE%"));
	TEST_EXPECT(has_missing(view.diagnostics, "font.name", DiagnosticSeverity::Warning));
	const FieldUse font_use = document->field_on(exit, *font);
	TEST_EXPECT(document->reference_status(font_use, std::string("%NOPE%"), view, nullptr) == ReferenceStatus::Missing);
	TEST_EXPECT(document->reference_status(font_use, std::string("%DEF_FONTNAME_LG%"), view, nullptr) ==
	            ReferenceStatus::Present);
	// The finding says what it misses (S11b): the reference's kind and name, as written.
	const Diagnostic *nope = missing_of(view.diagnostics, "font.name", ReferenceKind::StyleVar);
	TEST_EXPECT(nope && nope->target == "%NOPE%");
	// The picker's finding of the value is the same: the variable's (the stylesheet opened to
	// define it), never a font file named %NOPE%; a variable that resolves makes none.
	Diagnostic picked;
	TEST_EXPECT(document->missing_finding(exit, font_use, std::string("%NOPE%"), view, picked) &&
	            picked.reference == ReferenceKind::StyleVar && picked.target == "%NOPE%" && picked.field == "font.name");
	const std::vector<ProblemFix> define = fixes_for(picked, view);
	TEST_EXPECT(!define.empty() && define.front().request.kind == EditorRequestKind::OpenDocument &&
	            define.front().request.path.find("menu_style.mns") != std::string::npos);
	TEST_EXPECT(!document->missing_finding(exit, font_use, std::string("%DEF_FONTNAME_LG%"), view, picked));
	// A name the game's expansion stops inside (a space) is no variable: a font file of
	// that name (S12 B2).
	edit_window(session, *document, exit, "font.name", std::string("%NO PE%"));
	TEST_EXPECT(!missing_of(view.diagnostics, "font.name", ReferenceKind::StyleVar));
	const Diagnostic *spaced = missing_of(view.diagnostics, "font.name", ReferenceKind::Font);
	TEST_EXPECT(spaced && spaced->target == "%NO PE%");
	edit_window(session, *document, exit, "font.name", std::string("nofont.fnt"));
	TEST_EXPECT(has_missing(view.diagnostics, "font.name", DiagnosticSeverity::Error));
	const Diagnostic *nofont = missing_of(view.diagnostics, "font.name", ReferenceKind::Font);
	TEST_EXPECT(nofont && nofont->target == "nofont.fnt" && nofont->scope.empty() && nofont->role.empty());
	TEST_EXPECT(document->missing_finding(exit, font_use, std::string("nofont.fnt"), view, picked) &&
	            picked.reference == ReferenceKind::Font && picked.target == "nofont.fnt");
	// An APPEARANCE row's value is a texture for an IMAGE row, nothing for a typeless one.
	set_image(session, *document, exit, "missing.tga");
	TEST_EXPECT(has_missing(view.diagnostics, "value", DiagnosticSeverity::Error));
	TEST_EXPECT(document->reference_status(document->field_on(row, *value), std::string("missing.tga"), view, nullptr) ==
	            ReferenceStatus::Missing);
	const Diagnostic *texture = missing_of(view.diagnostics, "value", ReferenceKind::MenuTexture);
	TEST_EXPECT(texture && texture->target == "missing.tga");
	edit_window(session, *document, exit, "string.type", std::string("ID"));
	edit_window(session, *document, exit, "string.value", std::string("NO_SUCH_ID"));
	TEST_EXPECT(has_missing(view.diagnostics, "string.value", DiagnosticSeverity::Warning));
	// A string id's scope is where it was looked up: the "menu" section of its window's table.
	const Diagnostic *text_id = missing_of(view.diagnostics, "string.value", ReferenceKind::TextId);
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
	for (const GraphEdge *edge : view.graph->references_of(document->path())) {
		if (edge->field == "file" && edge->kind == ReferenceKind::Menu) ++action_files;
		if (edge->field == "file" && edge->kind == ReferenceKind::WaveBank && edge->value == "click.lwf") ++sounds;
	}
	TEST_EXPECT(action_files == 2 && sounds == 1);
	TEST_EXPECT(view.graph->resolve(ReferenceKind::WaveBank, "click.lwf") == ReferenceStatus::Missing);
	TEST_EXPECT(has_missing(view.diagnostics, "file", DiagnosticSeverity::Warning));
	// The font picker: the project's fonts, then the stylesheet's variables, each as the field
	// would reference it.
	const std::vector<ReferenceChoice> fonts = document->reference_choices(font_use, view);
	TEST_EXPECT(std::any_of(fonts.begin(), fonts.end(), [](const ReferenceChoice &c) { return c.kind == ReferenceKind::Font; }));
	TEST_EXPECT(std::any_of(fonts.begin(), fonts.end(), [](const ReferenceChoice &c) {
		return c.kind == ReferenceKind::StyleVar && c.name.front() == '%' && !c.file.empty() && !c.record.empty();
	}));
	// The finding names the record and the session's address, so Problems can select it.
	const NodeAddress second_action = menu_test::child_of(*document, exit, "action", 1);
	bool located = false;
	for (const Diagnostic &d : view.diagnostics)
		if (d.code == "reference.missing" && d.field == "file" && d.row_id == second_action.row &&
		    d.child_id == second_action.child && d.record == "STARTUP/MAIN/EXIT/Action 2")
			located = true;
	TEST_EXPECT(located);
	// The open document's edges follow its revision: the same edit count, no re-read.
	const size_t before = view.graph->stats().files_extracted;
	TEST_EXPECT(before >= 1);
	// A saved menu with a missing texture is blocked by the build (an error).
	session.handle(make_request(EditorRequestKind::SaveAll));
	session.handle(make_request(EditorRequestKind::Build));
	session.finish_build();
	TEST_EXPECT(!view.last_build.ok);
	return 0;
}

// A menu's string id resolves where the game looks it up: the "menu" section (the first
// of that name) of the table its window reads, its own TEXT_RSRC else its root window's
// [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0; CUIStringTable_LookupString @ 0x6527c0];
// with no table up the chain it resolves nowhere (the game shows the id).
static int test_menu_text_scope() {
	editor_test::TempProjectDir dir("opennova_asset_graph_text_scope");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Scope"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	// menutxt.bin: TITLE_ID in its Menu section, STATS_ONLY in another; gametext.bin: a
	// "menu" section with GAME_TITLE.
	session.handle(make_request(EditorRequestKind::CreateFile, "menutxt.bin", "strings"));
	Document *menutxt = session.document_for("menutxt.bin");
	TEST_EXPECT(menutxt);
	const auto add_id = [&](Document &table, NodeId section, const char *key) {
		EditorRequest add = make_request(EditorRequestKind::EditRecord, table.path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {section, table.kind_from_name("string"), 0};
		session.handle(add);
		edit_window(session, table, {section, table.kind_from_name("string"), table.last_added()}, "key", std::string(key));
	};
	const auto add_section = [&](Document &table, const char *name) {
		EditorRequest add = make_request(EditorRequestKind::EditRecord, table.path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {0, table.kind_from_name("section"), 0};
		session.handle(add);
		const NodeId section = table.last_added();
		edit_window(session, table, {section, table.kind_from_name("section"), 0}, "name", std::string(name));
		return section;
	};
	NodeId menu_section = 0;
	for (const auto &row : menutxt->rows())
		if (row->name() == "Menu") menu_section = row->id;
	TEST_EXPECT(menu_section != 0);
	add_id(*menutxt, menu_section, "TITLE_ID");
	add_id(*menutxt, add_section(*menutxt, "Stats"), "STATS_ONLY");
	session.handle(make_request(EditorRequestKind::OpenDocument, "gametext.bin"));
	Document *gametext = session.document_for("gametext.bin");
	TEST_EXPECT(gametext);
	add_id(*gametext, add_section(*gametext, "menu"), "GAME_TITLE");

	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	NodeAddress main, title;
	TEST_EXPECT(menu->find("MAIN", main) && menu->find("TITLE", title));
	edit_window(session, *menu, title, "string.type", std::string("ID"));
	edit_window(session, *menu, title, "string.value", std::string("TITLE_ID"));
	const auto title_edge = [&]() -> const GraphEdge * {
		for (const GraphEdge *edge : view.graph->references_of(menu->path()))
			if (edge->kind == ReferenceKind::TextId && edge->field == "string.value") return edge;
		return nullptr;
	};
	const auto missing_message = [&](const char *needle) {
		for (const Diagnostic &d : view.diagnostics)
			if (d.code == "reference.missing" && d.field == "string.value" && d.message.find(needle) != std::string::npos)
				return true;
		return false;
	};
	// No TEXT_RSRC up the chain: the id resolves in no table.
	TEST_EXPECT(title_edge() && title_edge()->scope == "/menu");
	TEST_EXPECT(missing_message("names no string table"));
	// MAIN names menutxt.bin: TITLE reads its root's table.
	EditorRequest write = make_request(EditorRequestKind::EditRecord, menu->path());
	write.edit.operation = EditOperation::Write;
	write.edit.address = main;
	write.edit.field = "text_rsrc";
	session.handle(write);
	edit_window(session, *menu, main, "text_rsrc", std::string("menutxt.bin"));
	TEST_EXPECT(title_edge() && title_edge()->scope == "MENUTXT.BIN/menu");
	TEST_EXPECT(!has_missing(view.diagnostics, "string.value", DiagnosticSeverity::Warning));
	TEST_EXPECT(view.graph->resolve(ReferenceKind::TextId, "TITLE_ID", "MENUTXT.BIN/menu") == ReferenceStatus::Present);
	TEST_EXPECT(view.graph->referrers_of(ReferenceKind::TextId, "TITLE_ID", "MENUTXT.BIN/Menu").size() == 1);
	TEST_EXPECT(view.graph->referrers_of(ReferenceKind::TextId, "TITLE_ID", "GAMETEXT.BIN/menu").empty());
	// Its picker offers the ids of that table's "menu" section alone: not another section's, nor
	// another table's.
	const auto offered = [&](const char *key, ReferenceChoice &out) {
		for (const FieldSchema &schema : menu->fields(title.kind))
			if (schema.id == "string.value")
				for (const ReferenceChoice &choice : menu->reference_choices(menu->field_on(title, schema), view))
					if (choice.name == key) {
						out = choice;
						return true;
					}
		return false;
	};
	ReferenceChoice choice;
	TEST_EXPECT(offered("TITLE_ID", choice) && choice.status == ReferenceStatus::Present && choice.file == menutxt->path());
	TEST_EXPECT(!offered("STATS_ONLY", choice) && !offered("GAME_TITLE", choice));
	// An id of another section of that table is not read; nor one another table defines.
	edit_window(session, *menu, title, "string.value", std::string("STATS_ONLY"));
	TEST_EXPECT(missing_message("\"menu\" section of MENUTXT.BIN"));
	edit_window(session, *menu, title, "string.value", std::string("GAME_TITLE"));
	TEST_EXPECT(has_missing(view.diagnostics, "string.value", DiagnosticSeverity::Warning));
	// TITLE's own TEXT_RSRC wins over its root's.
	EditorRequest own = write;
	own.edit.address = title;
	session.handle(own);
	edit_window(session, *menu, title, "text_rsrc", std::string("gametext.bin"));
	TEST_EXPECT(title_edge() && title_edge()->scope == "GAMETEXT.BIN/menu");
	TEST_EXPECT(!has_missing(view.diagnostics, "string.value", DiagnosticSeverity::Warning));
	// A table the project does not have.
	edit_window(session, *menu, title, "text_rsrc", std::string("nosuch.bin"));
	TEST_EXPECT(missing_message("NOSUCH.BIN, a string table the project does not have"));
	TEST_EXPECT(scope_matches("MENUTXT.BIN/Menu", "menutxt.bin/MENU") && scope_matches("A.BIN/x", "A.BIN") &&
	            !scope_matches("A.BIN/x", "/x") && scope_matches("A.BIN/x", ""));
	return 0;
}

// The native kinds through the engine's parsers.
static int test_native_extractors() {
	editor_test::TempProjectDir dir("opennova_asset_graph_native");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Native"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	// An environment naming sky textures and celestial models.
	{
		opennova::env::Config config;
		config.sky_map1 = "sky_a.pcx";
		config.sky_map2 = "sky_b.pcx";
		config.sun_3di = "sun.3di";
		std::ostringstream out;
		std::string error;
		TEST_EXPECT(opennova::env::save_env(out, config, error));
		TEST_EXPECT(editor_test::write_text(root + "/day.env", out.str()));
	}
	// A particle file with one effect and one particle drawing a texture.
	TEST_EXPECT(editor_test::write_text(root + "/fx.ptl",
	                                    "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff;\n}\n\n[particledef]\n{\n\tid = puff;\n\tgraphic1 = puff.tga, additive;\n}\n"));
	// A mission naming a terrain, an environment and an item id.
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		std::string error;
		TEST_EXPECT(opennova::mission::set_header_string(mission, "terrain", "island", error));
		TEST_EXPECT(opennova::mission::set_header_string(mission, "environment", "day", error));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(mission, bytes, error));
		TEST_EXPECT(editor_test::write_bytes(root + "/test.bms", bytes));
		// The same mission in the mission editors' text form (S13 PR0): a mission to the scan,
		// which the graph does not read (the mission document will), so it is no finding and
		// names nothing.
		std::string text;
		TEST_EXPECT(opennova::mission::write_mis_text(mission, text, error));
		TEST_EXPECT(editor_test::write_text(root + "/test.mis", text));
	}
	// A synthetic model from the fixtures, when the checkout carries them.
	const fs::path fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "synth" / "armory.3di";
	std::error_code ec;
	const bool have_model = fs::is_regular_file(fixture, ec);
	if (have_model) fs::copy_file(fixture, fs::path(root) / "armory.3di", ec);
	session.handle(make_request(EditorRequestKind::Rescan));
	const AssetGraph &graph = *session.view().graph;
	const GraphEdge *sky = edge_to(graph, "day.env", ReferenceKind::Texture, "sky_a.pcx");
	TEST_EXPECT(sky && !sky->rewritable && sky->field == "sky_map1");
	TEST_EXPECT(edge_to(graph, "day.env", ReferenceKind::Model, "sun.3di"));
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a.pcx") == ReferenceStatus::Missing);
	TEST_EXPECT(editor_test::write_text(root + "/sky_a.pcx", "x"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a.pcx") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a") == ReferenceStatus::Present); // without the extension
	TEST_EXPECT(!graph.referrers_of_file("sky_a.pcx").empty());
	TEST_EXPECT(has_symbol(graph, ReferenceKind::Particle, "BOOM"));
	TEST_EXPECT(graph.resolve(ReferenceKind::Particle, "boom") == ReferenceStatus::Present);
	const GraphEdge *puff = edge_to(graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(puff && puff->record == "puff" && puff->field == "graphic1");
	const GraphEdge *terrain = edge_to(graph, "test.bms", ReferenceKind::Terrain, "island");
	TEST_EXPECT(terrain && graph.resolve(ReferenceKind::Terrain, "island") == ReferenceStatus::Missing);
	TEST_EXPECT(edge_to(graph, "test.bms", ReferenceKind::Environment, "day"));
	TEST_EXPECT(graph.resolve(ReferenceKind::Environment, "day") == ReferenceStatus::Present);
	TEST_EXPECT(graph.references_of("test.mis").empty());
	if (have_model) {
		bool texture_edge = false;
		for (const GraphEdge *edge : graph.references_of("armory.3di"))
			if (edge->kind == ReferenceKind::Texture && edge->field == "name" && edge->rewritable) texture_edge = true;
		TEST_EXPECT(texture_edge);
	}
	const size_t missing = graph.missing().size();
	TEST_EXPECT(missing >= 4); // sky_b, sun, puff.tga, island
	TEST_EXPECT(count_code(session.view().diagnostics, "reference.missing") == missing);
	TEST_EXPECT(count_code(session.view().diagnostics, "graph.unreadable") == 0);
	// The .mis is skipped, never extracted (graph_reads_file): a changed one is read by nothing.
	TEST_EXPECT(!graph_reads_file(AssetKind::Mission, "test.mis") &&
			graph_reads_file(AssetKind::Mission, "TEST.BMS"));
	TEST_EXPECT(editor_test::write_text(root + "/test.mis", "; changed\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(graph.stats().files_extracted == 0 && graph.stats().files_failed == 0);
	TEST_EXPECT(graph.references_of("test.mis").empty() &&
			count_code(session.view().diagnostics, "graph.unreadable") == 0);

	// A native file the graph cannot read is a warning, its references unchecked, kept
	// while the file is unchanged; a document type's file that does not load is its
	// validator's error, not the graph's.
	TEST_EXPECT(editor_test::write_text(root + "/broken.bms", "not a mission"));
	TEST_EXPECT(editor_test::write_text(root + "/broken.3di", "not a model"));
	TEST_EXPECT(editor_test::write_bytes(root + "/broken.mnu", {0xFF, 0xFE, 0x41}));
	session.handle(make_request(EditorRequestKind::Rescan));
	const std::vector<Diagnostic> &diagnostics = session.view().diagnostics;
	const auto unreadable = [&diagnostics](const std::string &asset) {
		size_t n = 0;
		for (const Diagnostic &d : diagnostics)
			n += d.code == "graph.unreadable" && d.asset == asset && d.severity == DiagnosticSeverity::Warning ? 1 : 0;
		return n;
	};
	TEST_EXPECT(unreadable("broken.bms") == 1 && count_code(diagnostics, "graph.unreadable") == 1);
	TEST_EXPECT(graph.stats().files_failed == 3);
	bool menu_error = false;
	for (const Diagnostic &d : diagnostics)
		menu_error = menu_error || (d.asset == "broken.mnu" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(menu_error);
	bool model_error = false;
	for (const Diagnostic &d : diagnostics)
		model_error = model_error || (d.asset == "broken.3di" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(model_error);
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(graph.stats().files_failed == 0 && unreadable("broken.bms") == 1);
	fs::remove(fs::path(root) / "broken.bms");
	fs::remove(fs::path(root) / "broken.3di");
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(count_code(session.view().diagnostics, "graph.unreadable") == 0);
	return 0;
}

// The catalogs define weapon and ammo names and item ids; a weapon's round names an
// ammo record and "Referenced by" finds it.
static int test_catalog_symbols() {
	editor_test::TempProjectDir dir("opennova_asset_graph_catalog");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Catalog"));
	editor_test::create_missing_files(session);
	session.handle(make_request(EditorRequestKind::CreateFile, "ammo.def")); // not a menu project's requirement
	Document *ammo = session.document_for("ammo.def");
	TEST_EXPECT(ammo);
	{
		EditorRequest add = make_request(EditorRequestKind::EditRecord, ammo->path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {0, ammo->kind_from_name("ammo"), 0};
		session.handle(add);
		edit_window(session, *ammo, {ammo->last_added(), ammo->kind_from_name("ammo"), 0}, "name", std::string("AMMO_GRAPH"));
	}
	session.handle(make_request(EditorRequestKind::OpenDocument, "weapon.def"));
	Document *weapon = session.document_for("weapon.def");
	TEST_EXPECT(weapon);
	{
		EditorRequest add = make_request(EditorRequestKind::EditRecord, weapon->path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {0, weapon->kind_from_name("weapon"), 0};
		session.handle(add);
		const NodeAddress row{weapon->last_added(), weapon->kind_from_name("weapon"), 0};
		edit_window(session, *weapon, row, "weapon_name", std::string("WPN_GRAPH"));
		edit_window(session, *weapon, row, "round_type", std::string("AMMO_GRAPH"));
		edit_window(session, *weapon, row, "gfx1", std::string("gun.3di"));
	}
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.graph;
	TEST_EXPECT(has_symbol(graph, ReferenceKind::Ammo, "AMMO_GRAPH") && has_symbol(graph, ReferenceKind::Weapon, "WPN_GRAPH"));
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "ammo_graph") == ReferenceStatus::Present);
	const std::vector<const GraphEdge *> users = graph.referrers_of(ReferenceKind::Ammo, "AMMO_GRAPH");
	TEST_EXPECT(users.size() == 1 && users[0]->source == weapon->path() && users[0]->field == "round_type");
	TEST_EXPECT(graph.symbols_of(ammo->path(), "AMMO_GRAPH").size() == 1);
	TEST_EXPECT(has_missing(view.diagnostics, "gfx1", DiagnosticSeverity::Error));
	std::string file;
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "AMMO_GRAPH", std::string(), &file) == ReferenceStatus::Present && file == ammo->path());
	// A def's game-text field is a string id in a known table and section.
	const FieldSchema *textid = nullptr;
	for (const FieldSchema &field : weapon->fields(weapon->kind_from_name("weapon")))
		if (field.id == "loadout_menu_textid") textid = &field;
	TEST_EXPECT(textid);
	ReferenceKind kind;
	std::string name, scope;
	TEST_EXPECT(reference_target(field_use(*textid), std::string("WEP_X"), kind, name, scope) &&
	            kind == ReferenceKind::TextId && scope == "GAMETEXT.BIN/WepDes");
	TEST_EXPECT(!reference_target(field_use(*textid), std::string("NONE"), kind, name, scope));
	// A string table's ids carry their table and section: a def's game-text field
	// resolves in its witnessed section, a menu's id in its window's table.
	session.handle(make_request(EditorRequestKind::OpenDocument, "gametext.bin"));
	Document *strings = session.document_for("gametext.bin");
	TEST_EXPECT(strings);
	const auto add_id = [&](NodeId section, const char *key) {
		EditorRequest add_string = make_request(EditorRequestKind::EditRecord, strings->path());
		add_string.edit.operation = EditOperation::Add;
		add_string.edit.address = {section, strings->kind_from_name("string"), 0};
		session.handle(add_string);
		edit_window(session, *strings, {section, strings->kind_from_name("string"), strings->last_added()}, "key", std::string(key));
	};
	NodeId wepdes = 0; // the blank table's own WepDes section
	for (const auto &row : strings->rows())
		if (row->name() == "WepDes") wepdes = row->id;
	TEST_EXPECT(wepdes != 0);
	add_id(wepdes, "WEP_GRAPH");
	{
		// A second section of the same name: the lookup reads the first, so its ids are
		// defined but never read.
		EditorRequest add = make_request(EditorRequestKind::EditRecord, strings->path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {0, strings->kind_from_name("section"), 0};
		session.handle(add);
		const NodeId section = strings->last_added();
		edit_window(session, *strings, {section, strings->kind_from_name("section"), 0}, "name", std::string("wepdes"));
		add_id(section, "WEP_SHADOWED");
	}
	TEST_EXPECT(has_symbol(graph, ReferenceKind::TextId, "WEP_SHADOWED"));
	TEST_EXPECT(graph.resolve(ReferenceKind::TextId, "WEP_SHADOWED", "GAMETEXT.BIN/WepDes") == ReferenceStatus::Missing);
	TEST_EXPECT(has_symbol(graph, ReferenceKind::TextId, "WEP_GRAPH"));
	TEST_EXPECT(graph.resolve(ReferenceKind::TextId, "wep_graph") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::TextId, "WEP_GRAPH", "GAMETEXT.BIN/WepDes") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::TextId, "WEP_GRAPH", "GAMETEXT.BIN/Overlays") == ReferenceStatus::Missing);
	edit_window(session, *weapon, {weapon->rows()[0]->id, weapon->kind_from_name("weapon"), 0}, "loadout_menu_textid", std::string("WEP_GRAPH"));
	TEST_EXPECT(!has_missing(view.diagnostics, "loadout_menu_textid", DiagnosticSeverity::Warning));
	// A string's key is a symbol of its own (S12 D2): the record, the place and the field that
	// define it, its table and section; its users are its own, not its section's.
	const std::vector<const GraphSymbol *> key = graph.symbols_of(strings->path(), "WepDes/WEP_GRAPH");
	TEST_EXPECT(key.size() == 1 && key[0]->kind == ReferenceKind::TextId && key[0]->field == "key" &&
	            key[0]->scope == "GAMETEXT.BIN/WepDes" && !key[0]->locator.empty() && key[0]->address.child != 0);
	TEST_EXPECT(graph.symbols_of(strings->path(), "WepDes").empty());
	if (!key.empty()) {
		const std::vector<const GraphEdge *> key_users = graph.referrers_of(key[0]->kind, key[0]->name, key[0]->scope);
		TEST_EXPECT(key_users.size() == 1 && key_users[0]->source == weapon->path() &&
		            key_users[0]->field == "loadout_menu_textid");
		TEST_EXPECT(strings->address_at(key[0]->locator) == key[0]->address);
		NodeAddress found;
		TEST_EXPECT(strings->find("wep_graph", found) && found == key[0]->address);
	}
	const std::vector<const GraphSymbol *> shadowed = graph.symbols_named(ReferenceKind::TextId, "WEP_SHADOWED");
	TEST_EXPECT(shadowed.size() == 1 && shadowed[0]->inert && shadowed[0]->record == "wepdes/WEP_SHADOWED" &&
	            shadowed[0]->inert_reason.find("an earlier section") != std::string::npos);
	// The same key in Overlays too, the table's first section (S12 D3): the weapon's loadout
	// label goes to WepDes's, the section the game reads it from; the table's usages are the
	// uses of its keys.
	NodeId overlays = 0;
	for (const auto &row : strings->rows())
		if (row->name() == "Overlays") overlays = row->id;
	TEST_EXPECT(overlays != 0);
	add_id(overlays, "WEP_GRAPH");
	{
		const NodeAddress label{weapon->rows()[0]->id, weapon->kind_from_name("weapon"), 0};
		const std::vector<ReferenceTarget> targets = targets_of(*weapon, label, "loadout_menu_textid", view);
		const std::vector<const GraphSymbol *> wepdes_key = graph.symbols_of(strings->path(), "WepDes/WEP_GRAPH");
		TEST_EXPECT(targets.size() == 1 && wepdes_key.size() == 1 && targets[0].file == strings->path() &&
		            targets[0].locator == wepdes_key[0]->locator && targets[0].field == "key" && targets[0].editable);
		NodeAddress found;
		TEST_EXPECT(wepdes_key.size() == 1 && strings->find("wep_graph", found, "GAMETEXT.BIN/WepDes") &&
		            found == wepdes_key[0]->address);
		TEST_EXPECT(strings->find("WEP_GRAPH", found, "GAMETEXT.BIN/Overlays") && found != wepdes_key[0]->address);
		TEST_EXPECT(!strings->find("WEP_GRAPH", found, "GAMETEXT.BIN/WPNames"));
		// A key only the shadowed second WepDes defines: the lookup there finds none (the graph
		// says Missing), while a find by name alone still reaches the record.
		TEST_EXPECT(!strings->find("WEP_SHADOWED", found, "GAMETEXT.BIN/WepDes"));
		const std::vector<const GraphSymbol *> shadowed_now = graph.symbols_named(ReferenceKind::TextId, "WEP_SHADOWED");
		TEST_EXPECT(strings->find("WEP_SHADOWED", found) && shadowed_now.size() == 1 && found == shadowed_now[0]->address);
		const std::vector<const GraphEdge *> uses = graph.usages_of(strings->path());
		TEST_EXPECT(std::any_of(uses.begin(), uses.end(), [&](const GraphEdge *edge) {
			return edge->source == weapon->path() && edge->field == "loadout_menu_textid" && !edge->locator.empty();
		}));
		if (!targets.empty()) go_to(session, targets[0]);
		TEST_EXPECT(view.active_document == strings->path() && wepdes_key.size() == 1 &&
		            view.selection == wepdes_key[0]->address && view.reveal_field == "key");
	}
	edit_window(session, *weapon, {weapon->rows()[0]->id, weapon->kind_from_name("weapon"), 0}, "loadout_menu_textid", std::string("WEP_NOPE"));
	TEST_EXPECT(has_missing(view.diagnostics, "loadout_menu_textid", DiagnosticSeverity::Warning));
	// The items' identities.
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items && !items->rows().empty());
	Value id;
	TEST_EXPECT(items->get({items->rows()[0]->id, items->rows()[0]->kind, 0}, "id", id));
	TEST_EXPECT(graph.resolve(ReferenceKind::Item, std::to_string(std::get<int64_t>(id))) == ReferenceStatus::Present);
	return 0;
}

// The rename transaction: the sites rewritten and the file moved, and the refusals.
static int test_rename() {
	editor_test::TempProjectDir dir("opennova_asset_graph_rename");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Rename"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	NodeAddress exit;
	TEST_EXPECT(menu->find("EXIT", exit));
	set_image(session, *menu, exit, "logo.tga");
	const SessionView &view = session.view();
	// The menu naming it has unsaved edits: the rename waits on the unsaved prompt, which
	// lists the menu; cancelled, nothing moves.
	session.handle(make_request(EditorRequestKind::RenameAsset, "logo.tga", "logo2.tga"));
	TEST_EXPECT(session.outcome().unsaved_prompt && view.unsaved_prompt.files == std::vector<std::string>({menu->path()}));
	TEST_EXPECT(fs::exists(root + "/logo.tga") && !fs::exists(root + "/logo2.tga"));
	EditorRequest cancel = make_request(EditorRequestKind::ResolveUnsaved);
	cancel.unsaved_choice = UnsavedChoice::Cancel;
	session.handle(cancel);
	TEST_EXPECT(!view.unsaved_prompt.open && menu->dirty());
	session.handle(make_request(EditorRequestKind::SaveAll));
	TEST_EXPECT(!menu->dirty());
	{
		const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "logo.tga", "logo2.tga");
		TEST_EXPECT(plan.ok() && plan.sites.size() == 1 && plan.sites[0].file == menu->path() && plan.sites[0].after == "logo2.tga");
		TEST_EXPECT(plan.new_path == "logo2.tga" && plan.old_name == "logo.tga");
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "logo.tga", "logo.pcx").ok());
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "logo.tga", "main.mnu").ok());
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "logo.tga", "a_name_far_too_long.tga").ok());
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "nope.tga", "x.tga").ok());
	}
	session.handle(make_request(EditorRequestKind::RenameAsset, "logo.tga", "logo2.tga"));
	TEST_EXPECT(!fs::exists(root + "/logo.tga") && fs::exists(root + "/logo2.tga"));
	menu = session.document_for("main.mnu"); // reloaded after the rewrite
	TEST_EXPECT(menu && menu->find("EXIT", exit));
	Value image;
	TEST_EXPECT(menu->get(menu_test::child_of(*menu, exit, "appearance"), "value", image) &&
	            std::get<std::string>(image) == "logo2.tga");
	TEST_EXPECT(read_text(root + "/menus/main.mnu").find("logo2.tga") != std::string::npos ||
	            read_text(root + "/main.mnu").find("logo2.tga") != std::string::npos);
	TEST_EXPECT(view.graph->missing().empty());
	TEST_EXPECT(count_code(view.diagnostics, "reference.missing") == 0);
	// A site the editor cannot rewrite refuses the whole rename: an environment names
	// the texture too.
	{
		opennova::env::Config config;
		config.sky_map1 = "logo2.tga";
		std::ostringstream out;
		std::string error;
		TEST_EXPECT(opennova::env::save_env(out, config, error));
		TEST_EXPECT(editor_test::write_text(root + "/day.env", out.str()));
	}
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::RenameAsset, "logo2.tga", "logo3.tga"));
	TEST_EXPECT(view.diagnostics.back().code == "rename.site" || view.diagnostics.back().code == "rename.refused");
	TEST_EXPECT(fs::exists(root + "/logo2.tga") && !fs::exists(root + "/logo3.tga"));
	// Through a style variable: the variable's value is the site. (A Rescan keeps an open
	// document whose file did not change: the same one.)
	TEST_EXPECT(session.document_for("main.mnu") == menu);
	TEST_EXPECT(menu && menu->find("EXIT", exit));
	edit_window(session, *menu, menu_test::child_of(*menu, exit, "appearance"), "value", std::string("%DEF_FONTNAME_LG%"));
	session.handle(make_request(EditorRequestKind::SaveAll));
	fs::remove(root + "/day.env");
	session.handle(make_request(EditorRequestKind::Rescan));
	std::string font_file;
	TEST_EXPECT(view.graph->resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &font_file) == ReferenceStatus::Present);
	// The variable stays in the menu: the site is its value in menu_style.mns, the one
	// place that names the font.
	{
		const RenamePlan through_style = plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, font_file, "zz.fnt");
		TEST_EXPECT(through_style.ok() && !through_style.sites.empty());
		bool style_site = false;
		for (const RenameSite &site : through_style.sites) {
			TEST_EXPECT(site.kind == AssetKind::MenuStyle && site.field == "value" && site.after == "zz.fnt");
			style_site = style_site || site.record == "DEF_FONTNAME_LG";
		}
		TEST_EXPECT(style_site);
	}
	// The rename rewrites that value (the stylesheet saved CR LF) and moves the font; the
	// menu still names the variable, which now resolves to the new file.
	const std::string original_font = font_file;
	session.handle(make_request(EditorRequestKind::RenameAsset, font_file, "zz.fnt"));
	TEST_EXPECT(session.outcome().done());
	const std::string renamed_font = (fs::path(font_file).parent_path() / "zz.fnt").generic_string();
	TEST_EXPECT(!fs::exists(root + "/" + font_file) && fs::exists(root + "/" + renamed_font));
	const AssetEntry *style_asset = view.scan.find("menu_style.mns");
	TEST_EXPECT(style_asset != nullptr);
	if (!style_asset) return 1;
	TEST_EXPECT(read_text(root + "/" + style_asset->relative_path).find("\r\nDEF_FONTNAME_LG\tzz.fnt\r\n") != std::string::npos);
	menu = session.document_for("main.mnu");
	TEST_EXPECT(menu && menu->find("EXIT", exit));
	if (!menu) return 1;
	Value through;
	TEST_EXPECT(menu->get(menu_test::child_of(*menu, exit, "appearance"), "value", through) &&
	            std::get<std::string>(through) == "%DEF_FONTNAME_LG%");
	std::string resolved_font;
	TEST_EXPECT(view.graph->resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &resolved_font) ==
	                    ReferenceStatus::Present &&
	            resolved_font == renamed_font);
	for (const Diagnostic &d : view.diagnostics)
		TEST_EXPECT(!(d.code == "reference.missing" && d.field == "font.name"));
	font_file = renamed_font;
	// A value without the extension is no site the rename rewrites: refused, the variable named.
	session.handle(make_request(EditorRequestKind::OpenDocument, style_asset->relative_path));
	Document *style = session.document_for(style_asset->relative_path);
	NodeAddress large;
	TEST_EXPECT(style && style->find("%def_fontname_lg%", large));
	edit_window(session, *style, large, "value", std::string("zz"));
	TEST_EXPECT(view.graph->resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%") == ReferenceStatus::Present);
	const RenamePlan through_style = plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, font_file, "zz2.fnt");
	TEST_EXPECT(!through_style.ok() && through_style.refusals.front().code == "rename.style");
	session.handle(make_request(EditorRequestKind::Undo));
	session.handle(make_request(EditorRequestKind::CloseDocument, style_asset->relative_path));
	// And back: the required font is the project's again.
	session.handle(make_request(EditorRequestKind::RenameAsset, font_file, fs::path(original_font).filename().generic_string()));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(fs::exists(root + "/" + original_font) && !fs::exists(root + "/" + font_file));
	// Assign: a required name satisfied by renaming a file of the right kind.
	TEST_EXPECT(editor_test::write_text(root + "/spare.pcx", "x")); // the wrong kind for a font row
	std::string missing_role, missing_name;
	for (const RequirementRow &row : view.requirements.rows)
		if (row.state == RequirementState::Present && row.expected_kind == AssetKind::Strings) { missing_role = row.role; missing_name = row.name; break; }
	TEST_EXPECT(!missing_role.empty());
	fs::remove(root + "/" + view.scan.find(missing_name)->relative_path);
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.requirements.required_missing == 1);
	{
		EditorRequest assign = make_request(EditorRequestKind::AssignRequirement, "spare.pcx", missing_role);
		session.handle(assign);
		TEST_EXPECT(view.diagnostics.back().code == "requirement.kind");
		TEST_EXPECT(view.requirements.required_missing == 1);
	}
	// A table of the right kind, copied from another required table.
	const AssetEntry *some_table = nullptr;
	for (const AssetEntry &asset : view.scan.entries)
		if (asset.kind == AssetKind::Strings) { some_table = &asset; break; }
	TEST_EXPECT(some_table);
	std::error_code ec;
	fs::copy_file(root + "/" + some_table->relative_path, root + "/spare.bin", ec);
	TEST_EXPECT(!ec);
	session.handle(make_request(EditorRequestKind::Rescan));
	{
		EditorRequest assign = make_request(EditorRequestKind::AssignRequirement, "spare.bin", missing_role);
		session.handle(assign);
	}
	TEST_EXPECT(view.requirements.required_missing == 0);
	TEST_EXPECT(view.scan.find(missing_name) != nullptr && !fs::exists(root + "/spare.bin"));
	// Assigning a requirement already met renames nothing: a refusal the outcome carries.
	session.handle(make_request(EditorRequestKind::AssignRequirement, "spare.pcx", missing_role));
	TEST_EXPECT(!session.outcome().done() && !session.outcome().findings.empty() &&
	            session.outcome().findings.back().code == "requirement.assigned");
	TEST_EXPECT(fs::exists(root + "/spare.pcx"));
	return 0;
}

// Only the planned sites are rewritten: a weapon names its animation map and its model
// by the same stem, and renaming the map (m16.adm -> m16b.adm) rewrites the map's
// field while the model's keeps naming m16.3di.
static int test_rename_rewrites_planned_sites_only() {
	editor_test::TempProjectDir dir("opennova_asset_graph_rename_sites");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Sites"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	TEST_EXPECT(editor_test::write_text(root + "/m16.adm", "adm"));
	TEST_EXPECT(editor_test::write_text(root + "/m16.3di", "3di"));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "weapon.def"));
	Document *weapon = session.document_for("weapon.def");
	TEST_EXPECT(weapon);
	if (!weapon) return 1;
	const NodeKind kind = weapon->kind_from_name("weapon");
	{
		EditorRequest add = make_request(EditorRequestKind::EditRecord, weapon->path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {0, kind, 0};
		session.handle(add);
		const NodeAddress row{weapon->last_added(), kind, 0};
		edit_window(session, *weapon, row, "weapon_name", std::string("WPN_M16"));
		edit_window(session, *weapon, row, "animadm", std::string("m16"));
		edit_window(session, *weapon, row, "gfx1", std::string("m16"));
	}
	session.handle(make_request(EditorRequestKind::SaveAll));
	TEST_EXPECT(!weapon->dirty());
	const SessionView &view = session.view();
	std::string file;
	TEST_EXPECT(view.graph->resolve(ReferenceKind::AnimationMap, "m16", std::string(), &file) == ReferenceStatus::Present &&
	            file == "m16.adm");
	TEST_EXPECT(view.graph->resolve(ReferenceKind::Model, "m16", std::string(), &file) == ReferenceStatus::Present &&
	            file == "m16.3di");
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "m16.adm", "m16b.adm");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 1);
	TEST_EXPECT(!plan.sites.empty() && plan.sites[0].field == "animadm" && plan.sites[0].after == "m16b");
	session.handle(make_request(EditorRequestKind::RenameAsset, "m16.adm", "m16b.adm"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(!fs::exists(root + "/m16.adm") && fs::exists(root + "/m16b.adm") && fs::exists(root + "/m16.3di"));
	weapon = session.document_for("weapon.def"); // reloaded after the rewrite
	TEST_EXPECT(weapon && !weapon->rows().empty());
	if (!weapon || weapon->rows().empty()) return 1;
	const NodeAddress row{weapon->rows().back()->id, kind, 0};
	Value animation, model;
	TEST_EXPECT(weapon->get(row, "animadm", animation) && std::get<std::string>(animation) == "m16b");
	TEST_EXPECT(weapon->get(row, "gfx1", model) && std::get<std::string>(model) == "m16");
	TEST_EXPECT(count_code(view.diagnostics, "reference.missing") == 0);

	// A planned site gone by commit time (the file changed on disk after the last scan):
	// nothing else is rewritten in its place, the rename reports rename.partial as a
	// refusal that outlives the refresh, and the old file stays beside the copy.
	const std::string weapon_file = root + "/" + weapon->path();
	std::string text = read_text(weapon_file);
	const size_t at = text.find("m16b");
	TEST_EXPECT(at != std::string::npos && text.find("m16b", at + 1) == std::string::npos);
	if (at == std::string::npos) return 1;
	text.replace(at, 4, "m16x");
	TEST_EXPECT(editor_test::write_text(weapon_file, text));
	session.handle(make_request(EditorRequestKind::RenameAsset, "m16b.adm", "m16c.adm"));
	TEST_EXPECT(!session.outcome().done() && count_code(session.outcome().findings, "rename.partial") > 0);
	// The error names what the file still says (the site's own spelling), not the renamed file.
	bool partial_error = false;
	for (const Diagnostic &d : view.diagnostics)
		partial_error = partial_error || (d.code == "rename.partial" && d.severity == DiagnosticSeverity::Error &&
		                                  d.message.find("still names 'm16b' in 1 of its 1") != std::string::npos);
	TEST_EXPECT(partial_error);
	TEST_EXPECT(fs::exists(root + "/m16b.adm") && fs::exists(root + "/m16c.adm"));
	text = read_text(weapon_file);
	TEST_EXPECT(text.find("m16x") != std::string::npos && text.find("m16c") == std::string::npos);
	return 0;
}

// Two windows of one name in one screen name the same texture: the plan's sites share a
// record path but not a locator, and the rename rewrites each in its own place (S9g).
static int test_rename_by_locator() {
	editor_test::TempProjectDir dir("opennova_asset_graph_locator");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Locator"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	NodeAddress main;
	TEST_EXPECT(menu->find("MAIN", main));
	std::vector<NodeAddress> twins;
	for (int i = 0; i < 2; ++i) {
		EditorRequest add = make_request(EditorRequestKind::EditRecord, menu->path());
		add.edit.operation = EditOperation::Add;
		add.edit.address = {0, main.kind, 0};
		add.edit.parent = main.child;
		session.handle(add);
		twins.push_back(menu->address_of(menu->last_added()));
		edit_window(session, *menu, twins.back(), "name", std::string("TWIN"));
		set_image(session, *menu, twins.back(), "logo.tga");
	}
	session.handle(make_request(EditorRequestKind::SaveAll));
	const SessionView &view = session.view();
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), view.scan, *view.graph, "logo.tga", "logo2.tga");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 2);
	TEST_EXPECT(plan.sites[0].record == "STARTUP/MAIN/TWIN/Appearance 1" && plan.sites[1].record == plan.sites[0].record);
	TEST_EXPECT(!plan.sites[0].locator.empty() && plan.sites[0].locator != plan.sites[1].locator);
	const std::vector<std::string> places = {menu->locator(menu_test::child_of(*menu, twins[0], "appearance")),
	                                         menu->locator(menu_test::child_of(*menu, twins[1], "appearance"))};
	TEST_EXPECT(std::find(places.begin(), places.end(), plan.sites[0].locator) != places.end() &&
	            std::find(places.begin(), places.end(), plan.sites[1].locator) != places.end());
	session.handle(make_request(EditorRequestKind::RenameAsset, "logo.tga", "logo2.tga"));
	TEST_EXPECT(session.outcome().done());
	menu = session.document_for("main.mnu"); // reloaded after the rewrite
	TEST_EXPECT(menu);
	size_t renamed = 0;
	for (const std::string &place : places) {
		Value image;
		if (menu->get(menu->address_at(place), "value", image) && std::get<std::string>(image) == "logo2.tga") ++renamed;
	}
	TEST_EXPECT(renamed == 2 && view.graph->missing().empty());
	return 0;
}

// The stylesheets as the game reads them (S9i): menu_style.mns, then brand.mns over it;
// a stylesheet by another name, or a line after the place the game stops reading one,
// defines nothing a menu can use; a stylesheet's font values are edges of their own, and
// a missing font a menu names through a variable is reported once, there.
static int test_stylesheet_bindings() {
	editor_test::TempProjectDir dir("opennova_asset_graph_styles");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Styles"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = dir.file("project");
	const AssetEntry *style_asset = view.scan.find("menu_style.mns");
	TEST_EXPECT(style_asset != nullptr);
	const std::string style_path = style_asset->relative_path;
	const std::string style_dir = fs::path(root + "/" + style_path).parent_path().generic_string();
	// A symbol carries its record and the value the game reads.
	const GraphSymbol *large = view.graph->style_binding("%DEF_FONTNAME_LG%");
	TEST_EXPECT(large && large->record == "DEF_FONTNAME_LG" && large->value == "Arial16b.fnt" && large->file == style_path);
	TEST_EXPECT(!large->inert);
	// The stylesheet's font value is an edge of its own; a menu's font through the
	// variable says what the value must be there.
	const GraphEdge *value_edge = edge_to(*view.graph, style_path, ReferenceKind::Font, "Arial16b.fnt");
	TEST_EXPECT(value_edge && value_edge->rewritable && value_edge->field == "value" && value_edge->record == "DEF_FONTNAME_LG");
	bool through_font = false;
	for (const GraphEdge &edge : view.graph->edges())
		through_font = through_font || (edge.kind == ReferenceKind::StyleVar && edge.through == ReferenceKind::Font);
	TEST_EXPECT(through_font);
	TEST_EXPECT(count_code(view.diagnostics, "reference.missing") == 0);
	TEST_EXPECT(count_code(view.diagnostics, "style.not_a_color") == 0 && count_code(view.diagnostics, "style.mixed_use") == 0);

	// brand.mns over menu_style.mns: a later definition wins.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG FF102030\r\nBRAND_ONLY 1\r\n"));
	// A stylesheet by another name is never read.
	TEST_EXPECT(editor_test::write_text(style_dir + "/other.mns", "STRAY_ONLY 1\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	const GraphSymbol *text_fg = view.graph->style_binding("DEF_TEXT_FG");
	TEST_EXPECT(text_fg && fs::path(text_fg->file).filename() == "brand.mns" && text_fg->value == "FF102030");
	TEST_EXPECT(view.graph->resolve_style("%DEF_TEXT_FG%") == "FF102030");
	size_t inert = 0;
	for (const GraphSymbol &symbol : view.graph->symbols())
		if (symbol.kind == ReferenceKind::StyleVar && symbol.inert &&
		    (symbol.name == "DEF_TEXT_FG" || symbol.name == "STRAY_ONLY")) {
			++inert;
			// Why the game reads neither: brand.mns defines the one again; the other's file is not read.
			TEST_EXPECT(symbol.inert_reason.find(symbol.name == "STRAY_ONLY" ? "reads no stylesheet but" : "brand.mns defines it again") !=
			            std::string::npos);
		}
	TEST_EXPECT(inert == 2); // menu_style.mns's DEF_TEXT_FG and other.mns's name
	// The picker offers what the game reads, the rest as unreachable with the reason.
	bool stray_offered = false, brand_offered = false;
	for (const ReferenceChoice &choice : view.graph->choices(ReferenceKind::StyleVar)) {
		if (choice.name == "%STRAY_ONLY%") stray_offered = choice.inert && choice.status == ReferenceStatus::Missing && !choice.reason.empty();
		if (choice.name == "%DEF_TEXT_FG%") brand_offered = !choice.inert && fs::path(choice.file).filename() == "brand.mns";
	}
	TEST_EXPECT(stray_offered && brand_offered);
	TEST_EXPECT(view.graph->resolve(ReferenceKind::StyleVar, "%STRAY_ONLY%") == ReferenceStatus::Missing);
	TEST_EXPECT(view.graph->resolve(ReferenceKind::StyleVar, "%BRAND_ONLY%") == ReferenceStatus::Present);
	TEST_EXPECT(count_code(view.diagnostics, "style.overridden_by_brand") == 1);
	TEST_EXPECT(count_code(view.diagnostics, "style.not_loaded") == 1);
	// A colour the menus read through a variable follows wcstoul (a sign and eight digits
	// read whole, a 'G' stops the digits), and a value holds a %NAME% only where the
	// game's expansion finds one (S12 B2).
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG +FF102030\r\nBRAND_ONLY 50% of %A B%\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(count_code(view.diagnostics, "style.not_a_color") == 0 && count_code(view.diagnostics, "style.nested_var") == 0);
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG FF10203G\r\nBRAND_ONLY x%DEF_TEXT_FG%\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(count_code(view.diagnostics, "style.not_a_color") == 1 && count_code(view.diagnostics, "style.nested_var") == 1);

	// A menu naming the stray name: the finding says the game does not read that stylesheet.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && menu->find("EXIT", exit));
	edit_window(session, *menu, exit, "font.name", std::string("%STRAY_ONLY%"));
	bool stray_message = false;
	for (const Diagnostic &d : view.diagnostics)
		stray_message = stray_message || (d.code == "reference.missing" && d.message.find("does not read") != std::string::npos);
	TEST_EXPECT(stray_message);
	session.handle(make_request(EditorRequestKind::Undo));

	// A missing font named through a variable: reported once, where brand.mns names it.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_FONTNAME_LG nofont.fnt\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	size_t missing_fonts = 0;
	for (const Diagnostic &d : view.diagnostics)
		if (d.code == "reference.missing" && d.message.find("nofont.fnt") != std::string::npos) {
			++missing_fonts;
			TEST_EXPECT(fs::path(d.asset).filename() == "brand.mns" && d.record == "DEF_FONTNAME_LG");
			TEST_EXPECT(d.reference == ReferenceKind::Font && d.target == "nofont.fnt");
		}
	TEST_EXPECT(missing_fonts == 1);

	// A line after the place the game stops reading defines nothing it reads.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "EARLY 1\r\nBAD%NAME x\r\nLATE_ONLY 2\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.graph->resolve(ReferenceKind::StyleVar, "%EARLY%") == ReferenceStatus::Present);
	TEST_EXPECT(view.graph->resolve(ReferenceKind::StyleVar, "%LATE_ONLY%") == ReferenceStatus::Missing);
	TEST_EXPECT(count_code(view.diagnostics, "style.invalid_name_char") == 1);

	// A menu_style.mns font brand.mns replaces is never loaded: its missing file is no
	// finding. Once brand.mns no longer defines the name, the game reads it: reported, once.
	const std::string style_file = root + "/" + style_path;
	std::string style_text = read_text(style_file);
	const size_t normal_font = style_text.find("Arial16n.fnt");
	TEST_EXPECT(normal_font != std::string::npos);
	if (normal_font == std::string::npos) return 1;
	style_text.replace(normal_font, 12, "Nowhere.fnt");
	TEST_EXPECT(editor_test::write_text(style_file, style_text));
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_FONTNAME\tArial16n.fnt\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	const GraphSymbol *normal = view.graph->style_binding("DEF_FONTNAME");
	TEST_EXPECT(normal && fs::path(normal->file).filename() == "brand.mns" && normal->value == "Arial16n.fnt");
	const auto nowhere_findings = [&view]() {
		size_t n = 0;
		for (const Diagnostic &d : view.diagnostics)
			if (d.code == "reference.missing" && d.message.find("Nowhere.fnt") != std::string::npos) ++n;
		return n;
	};
	TEST_EXPECT(nowhere_findings() == 0);
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "// No fonts here.\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(nowhere_findings() == 1);
	return 0;
}

namespace {

// A window with a POSITION (a WINDOW with no child element is never created).
std::string window(const char *type, const char *name, const std::string &body = std::string()) {
	std::string out = std::string("<WINDOW TYPE=\"") + type + "\"" + (*name ? std::string(" NAME=\"") + name + "\"" : "") + ">\r\n";
	return out + "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n" + body +
	       "</WINDOW>\r\n";
}

std::string screen(const char *name, const std::string &body) {
	return std::string("<SCREEN>\r\n<NAME>") + name + "</NAME>\r\n" + body + "</SCREEN>\r\n";
}

const GraphEdge *edge_of(const AssetGraph &graph, const std::string &source, ReferenceKind kind, const std::string &value,
                         const char *field = nullptr) {
	for (const GraphEdge *edge : graph.references_of(source))
		if (edge->kind == kind && edge->value == value && (!field || edge->field == field)) return edge;
	return nullptr;
}

const GraphSymbol *symbol_at(const AssetGraph &graph, const std::string &file, ReferenceKind kind, const std::string &record) {
	for (const GraphSymbol &symbol : graph.symbols())
		if (symbol.kind == kind && symbol.file == file && symbol.record == record) return &symbol;
	return nullptr;
}

const Diagnostic *finding(const std::vector<Diagnostic> &diagnostics, const char *code, const std::string &record,
                          const char *needle = "") {
	for (const Diagnostic &d : diagnostics)
		if (d.code == code && d.record == record && d.message.find(needle) != std::string::npos) return &d;
	return nullptr;
}

} // namespace

// Every reference a menu makes is an edge (S9l): a SOUND's bank and a marquee's credits
// file by name, an APPEARANCE colour through a style variable, a table HEADER's string id in
// its window's table, a SUBST file, every SCREEN ACTION's file; the screens and the windows
// are the symbols the ACTIONs find by NAME, a SCREEN target in the file it loads (the last
// screen of a name), a WINDOW target and a URL's slot on the acting window's own screen (the
// first window of a name, none under a window with no NAME); a FILE on an ACTION that does
// not load one names nothing; two screens or windows of one name, a target no lookup finds
// and an ACTION the game never runs are warnings.
static int test_menu_names_and_targets() {
	editor_test::TempProjectDir dir("opennova_asset_graph_names");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Names"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	const std::string go_body =
	        "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%TRIM_COLOR%</APPEARANCE>\r\n"
	        "<SOUND STATE=\"MOUSEIN\" TRIGGER=\"MOUSE_OVER\">click.lwf</SOUND>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"graph.mnu\">AWAY</ACTION>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"main.mnu\">STARTUP</ACTION>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"graph.mnu\">NOWHERE</ACTION>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"gone.mnu\">THERE</ACTION>\r\n"
	        "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">TITLE</ACTION>\r\n"
	        "<ACTION TYPE=\"WINDOW\" STATE=\"HIDE\">HIDDEN_KID</ACTION>\r\n"
	        "<ACTION TYPE=\"WINDOW\" STATE=\"HIDE\">ELSEWHERE</ACTION>\r\n"
	        "<ACTION TYPE=\"WINDOW\">TITLE</ACTION>\r\n"
	        "<ACTION TYPE=\"URL\" FIELD=\"TITLE\">http://example.invalid/</ACTION>\r\n"
	        "<ACTION TYPE=\"POP_SCREEN\" FILE=\"nofile.mnu\"></ACTION>\r\n"
	        "<ACTION TYPE=\"JUMP\">TITLE</ACTION>\r\n"
	        "<ACTION TYPE=\"SCREEN\" FILE=\"main.mnu\">HOME</ACTION>\r\n";
	const std::string home = window(
	        "STATIC", "PANEL",
	        "<TEXT_RSRC>gametext.bin</TEXT_RSRC>\r\n" + window("BUTTON", "GO", go_body) + window("STATIC", "TITLE") +
	                window("STATIC", "TITLE") +
	                window("STATIC", "", "<ACTION TYPE=\"POP_SCREEN\"></ACTION>\r\n" + window("STATIC", "HIDDEN_KID")) +
	                window("MARQUEE_WND", "ROLL", "<DATASOURCE>credits.kda</DATASOURCE>\r\n") +
	                window("TABLE", "GRID",
	                       "<COLUMN COUNT=\"1\"><HEADER TYPE=\"ID\">HEAD_ID</HEADER><SUBST VALUE=\"1\" "
	                       "FILE>icon.tga</SUBST></COLUMN>\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/graph.mnu", screen("HOME", home) + screen("AWAY", window("STATIC", "BOARD")) +
	                                                              screen("AWAY", window("STATIC", "ELSEWHERE"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "graph.mnu"));
	const auto *menu = dynamic_cast<const MnuDocument *>(session.document_for("graph.mnu"));
	TEST_EXPECT(menu && !menu->blocked());
	if (!menu) return 1;
	for (const SourceIssue &issue : menu->issues()) std::printf("  issue: %s %s\n", issue.field.c_str(), issue.message.c_str());
	TEST_EXPECT(menu->issues().empty());
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.graph;
	const std::string path = menu->path();

	// The files, each a Warning while the project lacks it (the game does without it).
	const GraphEdge *bank = edge_of(graph, path, ReferenceKind::WaveBank, "click.lwf", "file");
	TEST_EXPECT(bank && bank->rewritable && bank->record == "HOME/PANEL/GO/Sound 1");
	TEST_EXPECT(finding(view.diagnostics, "reference.missing", "HOME/PANEL/GO/Sound 1", "plays no sound"));
	const GraphEdge *credits = edge_of(graph, path, ReferenceKind::Credits, "credits.kda", "value");
	TEST_EXPECT(credits && finding(view.diagnostics, "reference.missing", credits->record, "shows none of its lines"));
	TEST_EXPECT(edge_of(graph, path, ReferenceKind::MenuTexture, "icon.tga", "file"));
	const GraphEdge *header = edge_of(graph, path, ReferenceKind::TextId, "HEAD_ID", "text");
	TEST_EXPECT(header && header->scope == "GAMETEXT.BIN/menu");
	const GraphEdge *colour = edge_of(graph, path, ReferenceKind::StyleVar, "%TRIM_COLOR%", "value");
	TEST_EXPECT(colour && colour->through == ReferenceKind::None && colour->target == "TRIM_COLOR");
	TEST_EXPECT(graph.referrers_of(ReferenceKind::StyleVar, "TRIM_COLOR").size() == 1);
	TEST_EXPECT(edge_of(graph, path, ReferenceKind::Menu, "graph.mnu") && edge_of(graph, path, ReferenceKind::Menu, "main.mnu"));
	TEST_EXPECT(!edge_of(graph, path, ReferenceKind::Menu, "nofile.mnu")); // POP_SCREEN loads no FILE
	TEST_EXPECT(editor_test::write_text(root + "/click.lwf", "lwf") && editor_test::write_text(root + "/credits.kda", "[TEXT]\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	menu = dynamic_cast<const MnuDocument *>(session.document_for("graph.mnu"));
	TEST_EXPECT(menu);
	if (!menu) return 1;
	TEST_EXPECT(graph.resolve(ReferenceKind::WaveBank, "click.lwf") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::WaveBank, "click") == ReferenceStatus::Missing); // opened by the name as written
	TEST_EXPECT(graph.resolve(ReferenceKind::Credits, "CREDITS.KDA") == ReferenceStatus::Present);
	TEST_EXPECT(!finding(view.diagnostics, "reference.missing", "HOME/PANEL/GO/Sound 1"));

	// The screens: the last AWAY is the one found; the earlier one, and its windows, are inert.
	const GraphSymbol *home_screen = symbol_at(graph, path, ReferenceKind::MenuScreen, "HOME");
	TEST_EXPECT(home_screen && !home_screen->inert && home_screen->scope == "GRAPH.MNU" &&
	            home_screen->address == NodeAddress({menu->rows()[0]->id, 0, 0}));
	size_t aways = 0, found_aways = 0;
	for (const GraphSymbol &symbol : graph.symbols())
		if (symbol.kind == ReferenceKind::MenuScreen && symbol.name == "AWAY" && symbol.file == path) {
			++aways;
			if (!symbol.inert) {
				++found_aways;
				TEST_EXPECT(symbol.address.row == menu->rows()[2]->id);
			}
		}
	TEST_EXPECT(aways == 2 && found_aways == 1);
	const GraphSymbol *board = symbol_at(graph, path, ReferenceKind::MenuWindow, "AWAY/BOARD");
	TEST_EXPECT(board && board->inert && board->scope == "GRAPH.MNU/AWAY");
	// What the menu's lookups make of a definition (Document::refine_symbol, S13 D2): the window
	// on the shadowed screen inert and why, the screen they find as it is; a menu knows no line.
	if (!home_screen || !board) return 1;
	SymbolFacts shadowed, found;
	menu->refine_symbol(board->address, shadowed);
	menu->refine_symbol(home_screen->address, found);
	TEST_EXPECT(shadowed.inert && shadowed.inert_reason == board->inert_reason &&
	            shadowed.inert_reason.find("shadowed by a later screen") != std::string::npos);
	TEST_EXPECT(!found.inert && found.inert_reason.empty() && found.value.empty() && found.line == 0 && home_screen->line == 0);
	TEST_EXPECT(finding(view.diagnostics, "menu.duplicate_screen", "AWAY"));
	// The windows: the first TITLE is found, the second a duplicate; HIDDEN_KID sits under a
	// window with no NAME; a part is never a symbol.
	size_t titles = 0;
	for (const GraphSymbol &symbol : graph.symbols())
		if (symbol.kind == ReferenceKind::MenuWindow && symbol.name == "TITLE" && symbol.file == path) {
			TEST_EXPECT(symbol.scope == "GRAPH.MNU/HOME" && symbol.inert == (titles == 1));
			++titles;
		}
	TEST_EXPECT(titles == 2);
	const GraphSymbol *kid = symbol_at(graph, path, ReferenceKind::MenuWindow, "HOME/PANEL/Window 4/HIDDEN_KID");
	TEST_EXPECT(kid && kid->inert);
	const Diagnostic *twin = finding(view.diagnostics, "menu.duplicate_window", "HOME/PANEL/TITLE", "first window of a name");
	TEST_EXPECT(twin && twin->severity == DiagnosticSeverity::Warning && twin->field == "name" && twin->child_id != 0);

	// The targets, each resolved where the game looks.
	const GraphEdge *away = edge_of(graph, path, ReferenceKind::MenuScreen, "AWAY", "target");
	TEST_EXPECT(away && away->scope == "GRAPH.MNU" && away->rewritable);
	std::string file;
	TEST_EXPECT(away && graph.resolve(away->kind, away->target, away->scope, &file) == ReferenceStatus::Present && file == path);
	const GraphEdge *startup = edge_of(graph, path, ReferenceKind::MenuScreen, "STARTUP");
	TEST_EXPECT(startup && startup->scope == "MAIN.MNU" &&
	            graph.resolve(startup->kind, startup->target, startup->scope) == ReferenceStatus::Present);
	const GraphEdge *nowhere = edge_of(graph, path, ReferenceKind::MenuScreen, "NOWHERE");
	TEST_EXPECT(nowhere && graph.resolve(nowhere->kind, nowhere->target, nowhere->scope) == ReferenceStatus::Missing);
	const Diagnostic *no_screen = finding(view.diagnostics, "reference.missing", nowhere ? nowhere->record : std::string(),
	                                      "GRAPH.MNU does not have and no other menu of the project has");
	TEST_EXPECT(no_screen && no_screen->severity == DiagnosticSeverity::Warning && no_screen->field == "target");
	// A screen its FILE lacks that another menu has: the game selects over every screen it has
	// loaded, so it is found when that menu was loaded before, which the graph cannot know.
	const GraphEdge *other_home = edge_of(graph, path, ReferenceKind::MenuScreen, "HOME");
	TEST_EXPECT(other_home && other_home->scope == "MAIN.MNU" &&
	            graph.resolve(other_home->kind, other_home->target, other_home->scope) == ReferenceStatus::Unverified);
	TEST_EXPECT(other_home && !finding(view.diagnostics, "reference.missing", other_home->record));
	// The pickers offer what the lookup finds from the ACTION: a SCREEN target the screens of
	// its FILE, a WINDOW target the windows of the acting window's screen it reaches, each name
	// once, where it is defined; a window of the screen no lookup reaches is offered as
	// unreachable, with why; a window of another screen never.
	NodeAddress go_window;
	TEST_EXPECT(menu->find("GO", go_window));
	const auto picks = [&](size_t action) {
		const NodeAddress address = menu_test::child_of(*menu, go_window, "action", action);
		for (const FieldSchema &schema : menu->fields(address.kind))
			if (schema.id == "target") return menu->reference_choices(menu->field_on(address, schema), view);
		return std::vector<ReferenceChoice>();
	};
	const auto find_choice = [](const std::vector<ReferenceChoice> &choices, const char *name) -> const ReferenceChoice * {
		for (const ReferenceChoice &choice : choices)
			if (choice.name == name) return &choice;
		return nullptr;
	};
	const auto has = [&](const std::vector<ReferenceChoice> &choices, const char *name) {
		const ReferenceChoice *choice = find_choice(choices, name);
		return choice && !choice->inert;
	};
	const std::vector<ReferenceChoice> screens = picks(0), windows = picks(4);
	TEST_EXPECT(screens.size() == 2 && has(screens, "HOME") && has(screens, "AWAY"));
	for (const ReferenceChoice &screen : screens)
		TEST_EXPECT(screen.kind == ReferenceKind::MenuScreen && screen.file == path && screen.status == ReferenceStatus::Present);
	TEST_EXPECT(has(windows, "PANEL") && has(windows, "GO") && has(windows, "TITLE") && has(windows, "GRID"));
	TEST_EXPECT(find_choice(windows, "TITLE")->record == "HOME/PANEL/TITLE");
	const ReferenceChoice *hidden_kid = find_choice(windows, "HIDDEN_KID");
	TEST_EXPECT(hidden_kid && hidden_kid->inert && hidden_kid->reason.find("no NAME") != std::string::npos &&
	            hidden_kid->status == ReferenceStatus::Missing);
	TEST_EXPECT(!find_choice(windows, "BOARD") && !find_choice(windows, "ELSEWHERE"));
	// A screen of a menu file the project lacks: that file's own error says it, once.
	const GraphEdge *there = edge_of(graph, path, ReferenceKind::MenuScreen, "THERE");
	TEST_EXPECT(there && graph.resolve(there->kind, there->target, there->scope) == ReferenceStatus::Unverified);
	TEST_EXPECT(has_missing(view.diagnostics, "file", DiagnosticSeverity::Error));
	size_t window_targets = 0;
	for (const GraphEdge *edge : graph.references_of(path))
		if (edge->kind == ReferenceKind::MenuWindow) {
			++window_targets;
			TEST_EXPECT(edge->scope == "GRAPH.MNU/HOME");
		}
	TEST_EXPECT(window_targets == 5); // TITLE twice, HIDDEN_KID, ELSEWHERE, the URL's slot
	const GraphEdge *slot = edge_of(graph, path, ReferenceKind::MenuWindow, "TITLE", "field");
	TEST_EXPECT(slot && graph.resolve(slot->kind, slot->target, slot->scope) == ReferenceStatus::Present);
	TEST_EXPECT(!edge_of(graph, path, ReferenceKind::MenuWindow, "TITLE", "target") ||
	            graph.resolve(ReferenceKind::MenuWindow, "TITLE", "GRAPH.MNU/HOME") == ReferenceStatus::Present);
	bool hidden = false, elsewhere = false;
	for (const Diagnostic &d : view.diagnostics) {
		if (d.code != "reference.missing" || d.field != "target") continue;
		hidden = hidden || d.message.find("'HIDDEN_KID'") != std::string::npos &&
		                           d.message.find("never finds it") != std::string::npos;
		elsewhere = elsewhere || d.message.find("'ELSEWHERE', which no window of screen HOME is named") != std::string::npos;
	}
	TEST_EXPECT(hidden && elsewhere);
	// "Referenced by": the window's own symbol finds the ACTIONs that name it.
	NodeAddress title;
	TEST_EXPECT(menu->find("TITLE", title));
	const GraphSymbol *first_title = symbol_at(graph, path, ReferenceKind::MenuWindow, "HOME/PANEL/TITLE");
	TEST_EXPECT(first_title && first_title->address == title);
	TEST_EXPECT(graph.referrers_of(ReferenceKind::MenuWindow, "title", "GRAPH.MNU/HOME").size() == 3);
	// The ACTIONs the game never runs or ignores.
	TEST_EXPECT(finding(view.diagnostics, "menu.action_inert", "HOME/PANEL/Window 4", "no NAME"));
	bool unknown_type = false, no_state = false;
	for (const Diagnostic &d : view.diagnostics) {
		if (d.code != "menu.action_inert") continue;
		unknown_type = unknown_type || (d.field == "type" && d.message.find("'JUMP'") != std::string::npos);
		no_state = no_state || (d.field == "state" && d.record == "HOME/PANEL/GO/Action 8");
	}
	TEST_EXPECT(unknown_type && no_state);
	TEST_EXPECT(count_code(view.diagnostics, "menu.action_inert") == 3);
	// style.unused leaves TRIM_COLOR, which the colour names.
	for (const Diagnostic &d : view.diagnostics) TEST_EXPECT(!(d.code == "style.unused" && d.record == "TRIM_COLOR"));
	return 0;
}

// A screen's or a window's new name follows into the ACTIONs of its file that find it by
// the old one, in one undo step (S9l): the sites that resolve to it, and no others.
static int test_menu_rename_follows() {
	editor_test::TempProjectDir dir("opennova_asset_graph_follow");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Follow"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	const std::string go = "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">AWAY</ACTION>\r\n"
	                       "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">TITLE</ACTION>\r\n"
	                       "<ACTION TYPE=\"URL\" FIELD=\"TITLE\">x</ACTION>\r\n";
	const std::string back = "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">HOME</ACTION>\r\n"
	                         "<ACTION TYPE=\"WINDOW\" STATE=\"HIDE\">TITLE</ACTION>\r\n";
	TEST_EXPECT(editor_test::write_text(
	        root + "/flow.mnu",
	        screen("HOME", window("STATIC", "PANEL", window("BUTTON", "GO", go) + window("STATIC", "TITLE"))) +
	                screen("AWAY", window("STATIC", "BACKDROP", window("BUTTON", "BACK", back) + window("STATIC", "TITLE")))));
	TEST_EXPECT(editor_test::write_text(
	        root + "/other.mnu",
	        screen("OTHER", window("BUTTON", "JUMP", "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">HOME</ACTION>\r\n"))));
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "flow.mnu"));
	Document *menu = session.document_for("flow.mnu");
	TEST_EXPECT(menu && menu->rows().size() == 2);
	if (!menu || menu->rows().size() != 2) return 1;
	const SessionView &view = session.view();
	const NodeAddress home{menu->rows()[0]->id, 0, 0}, away{menu->rows()[1]->id, 0, 0};
	NodeAddress go_window, back_window;
	TEST_EXPECT(menu->find("GO", go_window) && menu->find("BACK", back_window));
	const NodeAddress go_screen = menu_test::child_of(*menu, go_window, "action", 0);
	const NodeAddress go_title = menu_test::child_of(*menu, go_window, "action", 1);
	const NodeAddress go_url = menu_test::child_of(*menu, go_window, "action", 2);
	const NodeAddress back_home = menu_test::child_of(*menu, back_window, "action", 0);
	const NodeAddress back_title = menu_test::child_of(*menu, back_window, "action", 1);
	const auto text = [&](const NodeAddress &address, const char *field) {
		Value value;
		return menu->get(address, field, value) ? std::get<std::string>(value) : std::string("?");
	};
	const auto rename = [&](const NodeAddress &record, const char *name) {
		EditorRequest request = make_request(EditorRequestKind::EditRecord, menu->path());
		request.edit.address = record;
		request.edit.field = "name";
		request.edit.value = std::string(name);
		request.edit.coalesce = true; // typed in the inspector
		session.handle(request);
		return session.outcome().done();
	};
	// The screen HOME renamed: AWAY's BACK follows (another row), one undo step; other.mnu's
	// JUMP keeps HOME (another file) and no longer resolves.
	TEST_EXPECT(!plan_symbol_rename(*menu, {menu_test::set_edit(home, "name", std::string("START"))}).empty());
	TEST_EXPECT(rename(home, "START"));
	TEST_EXPECT(text(home, "name") == "START" && text(back_home, "target") == "START");
	TEST_EXPECT(text(go_title, "target") == "TITLE" && text(back_title, "target") == "TITLE");
	bool jump_missing = false;
	for (const Diagnostic &d : view.diagnostics)
		jump_missing = jump_missing || (d.code == "reference.missing" && d.record == "OTHER/JUMP/Action 1" &&
		                                d.message.find("FLOW.MNU does not have") != std::string::npos);
	TEST_EXPECT(jump_missing);
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(text(home, "name") == "HOME" && text(back_home, "target") == "HOME");
	session.handle(make_request(EditorRequestKind::Redo, menu->path()));
	TEST_EXPECT(text(home, "name") == "START" && text(back_home, "target") == "START");
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(!menu->can_undo());
	// HOME's TITLE renamed: GO's WINDOW target and URL slot follow (the same row); AWAY's
	// BACK names AWAY's own TITLE and keeps it.
	NodeAddress home_title;
	TEST_EXPECT(menu->find("TITLE", home_title) && home_title.row == home.row);
	TEST_EXPECT(rename(home_title, "HEADLINE"));
	TEST_EXPECT(text(go_title, "target") == "HEADLINE" && text(go_url, "field") == "HEADLINE" &&
	            text(back_title, "target") == "TITLE");
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(text(go_title, "target") == "TITLE" && text(go_url, "field") == "TITLE" && !menu->can_undo());
	// A name another screen of the file has: nothing follows, the duplicate is a finding.
	TEST_EXPECT(rename(away, "HOME"));
	TEST_EXPECT(text(go_screen, "target") == "AWAY");
	TEST_EXPECT(finding(view.diagnostics, "menu.duplicate_screen", "HOME"));
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	// A change of case alone still finds the screen: nothing to follow.
	TEST_EXPECT(plan_symbol_rename(*menu, {menu_test::set_edit(away, "name", std::string("away"))}).empty());
	// A part's NAME is its owner's to give: renaming it follows nowhere.
	TEST_EXPECT(plan_symbol_rename(*menu, {menu_test::set_edit(go_window, "position.left", int64_t(4))}).empty());

	// Typed in the inspector, one coalesced Set per key: what follows is planned each time
	// from the NAME the group began with, so a NAME cleared on the way to a new one, or one
	// passing through a NAME another record has, leaves nothing behind, and the burst with
	// its sites is one undo step.
	const auto type = [&](const NodeAddress &record, std::initializer_list<const char *> keys) {
		bool ok = true;
		for (const char *key : keys) ok = rename(record, key) && ok;
		session.handle(make_request(EditorRequestKind::EndEdit, menu->path()));
		return ok;
	};
	TEST_EXPECT(!menu->can_undo());
	TEST_EXPECT(type(home, {"HOM", "HO", "H", "", "S", "ST", "STA", "STAR", "START"}));
	TEST_EXPECT(text(home, "name") == "START" && text(back_home, "target") == "START");
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(text(home, "name") == "HOME" && text(back_home, "target") == "HOME" && !menu->can_undo());
	// HOME's TITLE typed through GO, an earlier window of its screen.
	TEST_EXPECT(type(home_title, {"G", "GO", "GOA", "GOAL"}));
	TEST_EXPECT(text(home_title, "name") == "GOAL" && text(go_title, "target") == "GOAL" && text(go_url, "field") == "GOAL" &&
	            text(back_title, "target") == "TITLE");
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(text(go_title, "target") == "TITLE" && text(go_url, "field") == "TITLE" && !menu->can_undo());
	// The screen typed through AWAY, another screen's NAME.
	TEST_EXPECT(type(home, {"A", "AW", "AWA", "AWAY", "AWAYS"}));
	TEST_EXPECT(text(back_home, "target") == "AWAYS" && text(go_screen, "target") == "AWAY");
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(text(back_home, "target") == "HOME" && !menu->can_undo());
	// A NAME cleared and left so is no rename: the references keep the old one, reported.
	TEST_EXPECT(type(home, {"HOM", "HO", "H", ""}));
	TEST_EXPECT(text(home, "name").empty() && text(back_home, "target") == "HOME");
	TEST_EXPECT(finding(view.diagnostics, "reference.missing", "AWAY/BACKDROP/BACK/Action 1", "FLOW.MNU does not have"));
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	TEST_EXPECT(text(home, "name") == "HOME" && !menu->can_undo());
	return 0;
}

// The shipped menus loose at OPENNOVA_JO_ASSETS' root (the grill's census corpus) in a
// project of their own, through the graph (a SKIP-LEG without the root): every ACTION's
// target an edge, every WINDOW target resolving on its screen as the 2026-09-23 grill found
// (110 WINDOW rows, all found under retail's rules), every SCREEN target in a swept file
// resolving; the counts printed.
static int test_retail_menu_graph() {
	const std::string assets = retail::assets();
	std::vector<fs::path> menus;
	std::error_code ec;
	if (!assets.empty())
		for (const auto &entry : fs::directory_iterator(assets, ec)) {
			const std::string name = entry.path().filename().string();
			if (entry.is_regular_file(ec) && name.size() > 4 && retail::lower_ascii(name.substr(name.size() - 4)) == ".mnu")
				menus.push_back(entry.path());
		}
	if (menus.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/*.mnu (the shipped menus' ACTION targets through the asset graph)");
		return 0;
	}
	editor_test::TempProjectDir dir("opennova_asset_graph_retail");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Retail"));
	const std::string root = session.view().project_root;
	for (const fs::path &menu : menus) fs::copy_file(menu, fs::path(root) / menu.filename(), ec);
	// The shipped stylesheet, loose beside the menus or in the reference fixture set.
	std::string style = retail::asset_file("menu_style.mns");
	if (style.empty()) style = retail::reference_fixture("mns/menu_style.mns");
	if (!style.empty()) fs::copy_file(style, fs::path(root) / "menu_style.mns", ec);
	session.handle(make_request(EditorRequestKind::Rescan));
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.graph;
	std::map<std::string, size_t> edges, missing;
	size_t screen_targets = 0, screen_found = 0, screen_other = 0, window_targets = 0, window_found = 0;
	for (const GraphEdge &edge : graph.edges()) {
		if (retail::lower_ascii(fs::path(edge.source).extension().string()) != ".mnu") continue;
		const std::string token = reference_row(edge.kind).token;
		++edges[token];
		const ReferenceStatus status = graph.resolve(edge.kind, edge.target, edge.scope);
		if (status == ReferenceStatus::Missing) ++missing[token];
		if (edge.kind == ReferenceKind::MenuScreen) {
			++screen_targets;
			if (status == ReferenceStatus::Present) ++screen_found;
			else if (status == ReferenceStatus::Unverified) ++screen_other;
			else std::printf("  FAIL %s %s: the screen %s in %s\n", edge.source.c_str(), edge.record.c_str(), edge.value.c_str(), edge.scope.c_str());
		}
		if (edge.kind == ReferenceKind::MenuWindow && edge.field == "target") {
			++window_targets;
			if (status == ReferenceStatus::Present) ++window_found;
			else std::printf("  FAIL %s %s: the window %s on %s\n", edge.source.c_str(), edge.record.c_str(), edge.value.c_str(), edge.scope.c_str());
		}
	}
	size_t screens = 0, windows = 0, inert = 0;
	for (const GraphSymbol &symbol : graph.symbols()) {
		screens += symbol.kind == ReferenceKind::MenuScreen;
		windows += symbol.kind == ReferenceKind::MenuWindow;
		inert += (symbol.kind == ReferenceKind::MenuScreen || symbol.kind == ReferenceKind::MenuWindow) && symbol.inert;
	}
	std::printf("the shipped menus at %s through the asset graph (%zu menus and %s; their other files left out):\n",
	            assets.c_str(), menus.size(), style.empty() ? "no stylesheet" : "menu_style.mns");
	for (const auto &entry : edges)
		std::printf("  %-14s edges %5zu  missing here %4zu\n", entry.first.c_str(), entry.second, missing[entry.first]);
	std::printf("  symbols: %zu screens, %zu windows, %zu no lookup finds\n", screens, windows, inert);
	std::printf("  SCREEN targets %zu: %zu found, %zu unverified (a menu the corpus lacks, or a screen only another menu has)\n",
	            screen_targets, screen_found, screen_other);
	std::printf("  WINDOW / TAB / GLB_FILTER targets %zu: %zu found\n", window_targets, window_found);
	std::printf("  findings: duplicate_screen %zu, duplicate_window %zu, action_inert %zu, style.unused %zu\n",
	            count_code(view.diagnostics, "menu.duplicate_screen"), count_code(view.diagnostics, "menu.duplicate_window"),
	            count_code(view.diagnostics, "menu.action_inert"), count_code(view.diagnostics, "style.unused"));
	// The grill's census (B1): 110 WINDOW rows and 7 SCREEN rows, each found.
	TEST_EXPECT(window_targets == 110 && window_found == window_targets);
	TEST_EXPECT(screen_targets == 7 && screen_found + screen_other == screen_targets);
	TEST_EXPECT(count_code(view.diagnostics, "menu.duplicate_screen") == 0);
	return 0;
}

// An item's particle slot names a user point of its graphic model (S10h): found among the
// model's first 16 without case, a name it lacks a warning, a record with no graphic no
// reference at all.
static int test_user_point_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_user_points");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Points"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
	const fs::path fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "synth" / "armory.3di";
	std::error_code ec;
	fs::create_directories(fs::path(root) / "models", ec);
	fs::copy_file(fixture, fs::path(root) / "models" / "armory.3di", ec);
	TEST_EXPECT(!ec);
	// armory.3di's user points are "Armory" and "Ground".
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def",
	                                    "begin \"Armory Item\"\nid 100100\ntype building\ngraphic armory\n"
	                                    "particlefx Effect_x ARMORY\nend\n"
	                                    "begin \"Lost Point\"\nid 100101\ntype building\ngraphic armory\n"
	                                    "particlefx Effect_x Nowhere\nend\n"
	                                    "begin \"No Graphic\"\nid 100102\ntype marker\nparticlefx Effect_x Anything\nend\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	const AssetGraph &graph = *session.view().graph;
	TEST_EXPECT(graph.resolve(ReferenceKind::UserPoint, "armory", "ARMORY.3DI") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::UserPoint, "Nowhere", "ARMORY.3DI") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.resolve(ReferenceKind::UserPoint, "Ground", "OTHER.3DI") == ReferenceStatus::Missing);
	// The slot's edge is keyed as its point is (the whole name, without case, as the lookup's
	// stricmp compares them): the point's users are that slot's.
	const std::vector<const GraphEdge *> armory_users = graph.referrers_of(ReferenceKind::UserPoint, "Armory", "ARMORY.3DI");
	TEST_EXPECT(armory_users.size() == 1 && armory_users[0]->target == "ARMORY" && armory_users[0]->record == "Armory Item");
	// Two points of one name among the first 16 (the lookup sets a bit for each [orig:
	// ItemDef_GetBoneMaskByName @ 0x49ea40]): the slot naming them is one use of the model.
	session.handle(make_request(EditorRequestKind::OpenDocument, "models/armory.3di"));
	Document *model = session.document_for("models/armory.3di");
	NodeAddress ground;
	TEST_EXPECT(model && model->find("Ground", ground));
	if (!model || !ground.row) return 1;
	EditorRequest rename = make_request(EditorRequestKind::EditRecord, model->path());
	rename.edit.address = ground;
	rename.edit.field = "name";
	rename.edit.value = std::string("Armory");
	session.handle(rename);
	TEST_EXPECT(graph.symbols_named(ReferenceKind::UserPoint, "ARMORY").size() == 2);
	size_t slot_uses = 0;
	for (const GraphEdge *edge : graph.usages_of(model->path()))
		slot_uses += edge->kind == ReferenceKind::UserPoint && edge->record == "Armory Item" ? 1 : 0;
	TEST_EXPECT(slot_uses == 1);
	size_t missing_points = 0, stray = 0;
	for (const Diagnostic &d : session.view().diagnostics) {
		if (d.code != "reference.missing") continue;
		if (d.message.find("Nowhere") != std::string::npos && d.severity == DiagnosticSeverity::Warning) ++missing_points;
		if (d.message.find("Anything") != std::string::npos) ++stray;
	}
	TEST_EXPECT(missing_points == 1 && stray == 0);
	return 0;
}

// A symbol is the field that defines it (S12 D2): it carries its record's place, which a
// reload of the file finds again, and Document::find finds the record by the name it
// defines; a stylesheet's earlier definition of a name is a symbol no lookup reads.
static int test_symbol_locators() {
	editor_test::TempProjectDir dir("opennova_asset_graph_locators");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Locators"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::vector<const GraphSymbol *> exit = view.graph->symbols_named(ReferenceKind::MenuWindow, "EXIT");
	TEST_EXPECT(exit.size() == 1 && !exit[0]->locator.empty() && exit[0]->field == "name" && !exit[0]->inert);
	if (exit.empty()) return 1;
	MnuDocument reloaded;
	Diagnostic error;
	TEST_EXPECT(reloaded.load(view.project_root + "/" + exit[0]->file, exit[0]->file, AssetKind::Menu,
	                          view.document.target_game, error));
	const NodeAddress at = reloaded.address_at(exit[0]->locator);
	TEST_EXPECT(at.row != 0 && reloaded.record_name(at) == "EXIT");
	NodeAddress found;
	TEST_EXPECT(reloaded.find("exit", found) && found == at);
	TEST_EXPECT(reloaded.find("STARTUP", found) && found.kind == reloaded.kind_from_name("screen") && !found.child);
	// Two definitions of a name in the stylesheet the game reads: the last one is read.
	const AssetEntry *style = view.scan.find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const std::string style_path = style->relative_path; // the scan is read again below
	const std::string style_file = view.project_root + "/" + style_path;
	TEST_EXPECT(editor_test::write_text(style_file, read_text(style_file) + "\r\nTWICE 1\r\nTWICE 2\r\n"));
	session.handle(make_request(EditorRequestKind::Rescan));
	const std::vector<const GraphSymbol *> twice = view.graph->symbols_named(ReferenceKind::StyleVar, "twice");
	TEST_EXPECT(twice.size() == 2 && twice[0]->inert && !twice[1]->inert && twice[1]->value == "2" &&
	            twice[0]->inert_reason.find("defines it again below") != std::string::npos);
	TEST_EXPECT(view.graph->style_binding("TWICE") == (twice.size() == 2 ? twice[1] : nullptr));
	// A query names a variable as the graph keys it; Document::find takes a menu's %NAME% too.
	TEST_EXPECT(view.graph->symbols_named(ReferenceKind::StyleVar, "%twice%").empty());
	session.handle(make_request(EditorRequestKind::OpenDocument, style_path));
	const Document *sheet_document = session.document_for(style_path);
	NodeAddress by_name, by_variable;
	Value read;
	TEST_EXPECT(sheet_document && sheet_document->find("twice", by_name) && sheet_document->find("%TWICE%", by_variable) &&
	            by_name == by_variable && sheet_document->get(by_name, "value", read) &&
	            std::get<std::string>(read) == "2");
	// What the stylesheet's lookup makes of each definition (Document::refine_symbol, S13 D2): the
	// earlier TWICE inert and why, the last the value the game reads; each the line it is on,
	// which the graph's symbol and its JSON carry.
	const auto *styles = dynamic_cast<const MnsDocument *>(sheet_document);
	TEST_EXPECT(styles && twice.size() == 2);
	if (!styles || twice.size() != 2) return 1;
	SymbolFacts earlier, last;
	styles->refine_symbol(twice[0]->address, earlier);
	styles->refine_symbol(twice[1]->address, last);
	TEST_EXPECT(earlier.inert && earlier.inert_reason == twice[0]->inert_reason && !last.inert && last.value == "2");
	TEST_EXPECT(last.line > 1 && last.line == size_t(styles->line_of(twice[1]->address.row)) && earlier.line + 1 == last.line &&
	            twice[0]->line == earlier.line && twice[1]->line == last.line);
	const opennova::io::JsonValue symbol_json = graph_symbol_to_json(*twice[1]);
	TEST_EXPECT(symbol_json.get("line") && symbol_json.get("line")->number == double(last.line));
	// Go to on MAIN's font, a style variable (S12 D3): the variable where the game reads it, a
	// line of the stylesheet the editor opens at, its name shown; and the .fnt its value names,
	// which the editor does not edit (Files shows it). A colour through a variable goes to the
	// variable alone. The stylesheet's usages are the menus' uses of its variables.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress main;
	TEST_EXPECT(menu && menu->find("MAIN", main));
	if (!menu) return 1;
	const std::vector<ReferenceTarget> font = targets_of(*menu, main, "font.name", view);
	TEST_EXPECT(font.size() == 2);
	if (font.size() != 2) return 1;
	TEST_EXPECT(font[0].file == style_path && font[0].editable && font[0].field == "name" &&
	            font[0].label.find("DEF_FONTNAME_LG") != std::string::npos);
	TEST_EXPECT(font[1].locator.empty() && !font[1].editable && fs::path(font[1].file).extension() == ".fnt");
	TEST_EXPECT(targets_of(*menu, main, "font.default_fg", view).size() == 1);
	const std::vector<const GraphEdge *> style_uses = view.graph->usages_of(style_path);
	TEST_EXPECT(view.graph->referrers_of_file(style_path).empty() && !style_uses.empty());
	TEST_EXPECT(std::all_of(style_uses.begin(), style_uses.end(), [](const GraphEdge *edge) {
		return edge->kind == ReferenceKind::StyleVar && !edge->locator.empty();
	}));
	go_to(session, font[0]);
	const Document *sheet = session.document_for(style_path);
	TEST_EXPECT(sheet && view.active_document == sheet->path() && view.selection.row != 0 &&
	            sheet->record_name(view.selection) == "DEF_FONTNAME_LG" && view.reveal_field == "name");
	return 0;
}

// Go to on a menu's ACTION targets (S12 D3): a WINDOW target on each of two screens that both
// have a TITLE goes to its own screen's TITLE, in the same file, which the Go to selects (the
// locator found again, the NAME shown); a SCREEN target to the screen of its FILE; the file's
// usages are the uses of its screens and windows, another menu's too.
static int test_go_to_targets() {
	editor_test::TempProjectDir dir("opennova_asset_graph_go_to");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "GoTo"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project_root;
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
	NodeAddress go_window, back_window, home_title, away_title;
	TEST_EXPECT(menu->find("GO", go_window) && menu->find("BACK", back_window));
	TEST_EXPECT(menu->find("TITLE", home_title, "FLOW.MNU/HOME") && menu->find("TITLE", away_title, "FLOW.MNU/AWAY"));
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
	TEST_EXPECT(menu->find("AWAY", away) && away == away_screen);
	TEST_EXPECT(menu->find("AWAY", away, "FLOW.MNU/HOME") && away.row == menu->rows()[0]->id && away.child != 0);
	const GraphEdge *jump = nullptr;
	for (const GraphEdge *edge : view.graph->references_of("other.mnu"))
		if (edge->kind == ReferenceKind::MenuScreen) jump = edge;
	TEST_EXPECT(jump != nullptr);
	if (jump) {
		const GraphSymbol *screen_symbol = view.graph->resolve_symbol(jump->kind, jump->value, jump->scope);
		TEST_EXPECT(screen_symbol && menu->address_at(screen_symbol->locator) == away_screen);
	}
	// The same file: the Go to selects the record there, its NAME shown.
	if (to_away_title.size() == 1) go_to(session, to_away_title[0]);
	TEST_EXPECT(view.active_document == menu->path() && view.selection == away_title && view.reveal_field == "name");
	const std::vector<const GraphEdge *> uses = view.graph->usages_of(menu->path());
	const auto used_by = [&uses](const char *source, const char *record) {
		return std::any_of(uses.begin(), uses.end(), [&](const GraphEdge *edge) {
			return edge->source == source && edge->record == record;
		});
	};
	TEST_EXPECT(used_by("other.mnu", "OTHER/JUMP/Action 1") && used_by("flow.mnu", "AWAY/BACKDROP/BACK/Action 2") &&
	            used_by("flow.mnu", "HOME/PANEL/GO/Action 1"));
	return 0;
}

// The reference kinds' table (graph/reference_kinds, S12 D1): one row per kind, its token its
// own and read back; a file kind names the file it loads and a symbol none; the kinds the game
// tolerates missing are the warnings; what a stylesheet value stands for, and how the graph
// keys a name, read from the rows. A def's game-text field is a string id (no game_text kind).
static int test_reference_kind_rows() {
	std::set<std::string> tokens;
	for (size_t i = 0; i < kReferenceKindCount; ++i) {
		const ReferenceKind kind = static_cast<ReferenceKind>(i);
		const ReferenceKindRow &row = reference_row(kind);
		TEST_EXPECT(row.kind == kind && *row.token && *row.label && std::string(row.phrase).rfind("the ", 0) == 0);
		TEST_EXPECT(tokens.insert(row.token).second);
		ReferenceKind back = ReferenceKind::None;
		TEST_EXPECT(reference_kind_from_token(row.token, back) && back == kind);
		const bool file = row.resolution == ReferenceResolution::File;
		TEST_EXPECT(file == (row.file != AssetKind::Unknown));
		TEST_EXPECT(!row.extensions || file);
		TEST_EXPECT(!row.file_names || file);
		TEST_EXPECT(!row.scope_names_file || row.resolution == ReferenceResolution::Symbol);
		TEST_EXPECT(row.defined_in == AssetKind::Unknown || row.names_symbol());
		TEST_EXPECT((row.missing_message != nullptr) == (file || row.names_symbol()));
		const bool tolerated = kind == ReferenceKind::StyleVar || kind == ReferenceKind::TextId ||
		                       kind == ReferenceKind::WaveBank || kind == ReferenceKind::Credits ||
		                       kind == ReferenceKind::MenuScreen || kind == ReferenceKind::MenuWindow ||
		                       kind == ReferenceKind::Animation || kind == ReferenceKind::UserPoint;
		TEST_EXPECT(row.severity_when_missing == (tolerated ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error));
	}
	ReferenceKind kind = ReferenceKind::None;
	TEST_EXPECT(!reference_kind_from_token("game_text", kind) && !reference_kind_from_token("", kind));
	TEST_EXPECT(reference_row(ReferenceKind::Font).file == AssetKind::Font &&
	            reference_row(ReferenceKind::MenuTexture).file == AssetKind::Texture);
	TEST_EXPECT(reference_row(ReferenceKind::StyleVar).resolution == ReferenceResolution::StyleVariable);
	TEST_EXPECT(reference_row(ReferenceKind::Sound).resolution == ReferenceResolution::Unchecked);
	TEST_EXPECT(reference_row(ReferenceKind::OtherText).also_offers == ReferenceKind::TextId);
	TEST_EXPECT(style_value_reference(AssetKind::Font) == ReferenceKind::Font);
	TEST_EXPECT(style_value_reference(AssetKind::Texture) == ReferenceKind::MenuTexture);
	TEST_EXPECT(style_value_reference(AssetKind::Model) == ReferenceKind::None);
	TEST_EXPECT(graph_names::symbol_name(ReferenceKind::StyleVar, "def_text_fg") == "DEF_TEXT_FG");
	TEST_EXPECT(graph_names::symbol_name(ReferenceKind::StyleVar, "%def_text_fg%") == "%DEF_TEXT_FG%");
	TEST_EXPECT(graph_names::symbol_name(ReferenceKind::UserPoint, "Armory ") == "ARMORY ");
	TEST_EXPECT(graph_names::symbol_name(ReferenceKind::TextId, "Wep_x") == "WEP_X");
	TEST_EXPECT(graph_names::symbol_name(ReferenceKind::Item, "0042") == "0042");
	TEST_EXPECT(graph_names::symbol_name(ReferenceKind::Model, "gun.3di ") == "GUN.3DI");
	return 0;
}

// The names a file reference loads, in the order the game probes them (the graph takes the
// first the project has, a fix the first the game install has): the name, then with each
// extension the kind's loader appends; a texture of no model row what the runtime's texture
// lookup probes (S11h: the name, then its stem with each texture extension, never an
// extension appended to the whole name); a font the .fnt from the name's first dot; where the
// loader decides by what is there, the one file it opens: a menu texture's .tga, else its
// .dds; a model's texture row by the row's type (S11f: a diffuse row's .dds sibling of the
// name cut three characters after its first dot, the '.MDT' test with its case; a plain
// row's name alone; a normal map's .dds sibling of its .tga, its .mdt as written, and nothing
// for any other name; a height producer's .tga rule alone; a chunk row's name as written; an
// authored type the loader zeroes a diffuse row; none a name no decoder takes: S12,
// renderer::material_texture_source); none for a symbol, an unverified kind or an empty name.
static int test_reference_file_candidates() {
	using Names = std::vector<std::string>;
	const auto none = [](const std::string &) { return false; };
	const auto has = [](Names files) {
		return [files](const std::string &name) { return std::find(files.begin(), files.end(), name) != files.end(); };
	};
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Model, "soldier", -1, none) == Names({"soldier", "soldier.3di"}));
	const Names bare = reference_file_candidates(ReferenceKind::Texture, "wall", -1, none);
	TEST_EXPECT(bare == opennova::texture_candidate_filenames("wall"));
	TEST_EXPECT(bare.size() > 3 && bare[0] == "wall" && bare[1] == "wall.tga" && bare[2] == "wall.dds");
	const Names named = reference_file_candidates(ReferenceKind::Texture, "wall.png", -1, has({"wall.dds"}));
	TEST_EXPECT(named == opennova::texture_candidate_filenames("wall.png"));
	TEST_EXPECT(named.size() > 2 && named[0] == "wall.png" && named[1] == "wall.tga" && named[2] == "wall.dds");
	TEST_EXPECT(std::find(named.begin(), named.end(), "wall.png.tga") == named.end());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Font, "Arial99.fnt", -1, none) == Names({"Arial99.fnt"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Font, "arial12b", -1, none) == Names({"arial12b.fnt"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Font, "a.b.fnt", -1, none) == Names({"a.fnt"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::MenuTexture, "logo.tga", -1, has({"logo.tga", "logo.dds"})) ==
	            Names({"logo.tga"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::MenuTexture, "logo.tga", -1, none) == Names({"logo.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::MenuTexture, "Logo.Big.TGA", -1, none) == Names({"Logo.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::MenuTexture, "logo.pcx", -1, none) == Names({"logo.pcx"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::MenuTexture, "logo.bmp", -1, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "wall.tga", 0, has({"wall.dds"})) == Names({"wall.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "wall.tga", 0, none) == Names({"wall.tga"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bark.dds.tga", 0, has({"bark.dds"})) == Names({"bark.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "glow.mdt", 0, has({"glow.dds"})) == Names({"glow.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "glow.MDT", 0, has({"glow.dds"})) == Names({"glow.MDT"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "wall.tga", 1, has({"wall.dds"})) == Names({"wall.tga"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bump.tga", 5, has({"bump.dds"})) == Names({"bump.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bump.tga", 4, none) == Names({"bump.tga"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bump.mdt", 4, has({"bump.dds"})) == Names({"bump.mdt"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bump.pcx", 4, has({"bump.pcx", "bump.dds"})).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bump.mdt", 6, has({"bump.mdt"})).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "bump.tga", 7, has({"bump.dds"})) == Names({"bump.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "field.nq8", 16, has({"field.dds"})) == Names({"field.nq8"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "trim.tga", 3, has({"trim.dds"})) == Names({"trim.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "wall.bmp", 0, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::WaveBank, "click.lwf", -1, none) == Names({"click.lwf"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Weapon, "M16", -1, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Sound, "shot", -1, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Model, "", -1, none).empty());
	return 0;
}

// A model's texture row resolves to the file its type's loader opens (S11f): a diffuse row
// naming wall.tga to the .dds the project has beside no .tga, else to the .tga; a plain row
// (type 1) never to the .dds; a normal map by renderer::material_texture_source; an authored type the
// loader zeroes as a diffuse row. A particle's texture reads as the runtime's texture lookup
// does (S11h: texture_candidate_filenames). The badge, the finding and the edge's JSON read
// the same rule, the row's type carried on the edge.
static int test_model_texture_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_model_textures");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Textures"));
	const SessionView &view = session.view();
	const std::string root = view.project_root;
	// A model minted from scene text, its texture rows of six types (an .mdt normal map and a
	// chunk row among them); a particle file.
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/typed.o3d",
	                                    "o3d 1\nmodel TYPED\nmaterial FF_ST_OP\ntexture wall.tga 1 0\ntexture plain.tga 1 1\n"
	                                    "texture bump.tga 3 5\ntexture trim.tga 1 3\ntexture ready.mdt 3 4\n"
	                                    "texture field.nq8 1 16\nlod 0\npart 0 0 0 0\nstrip 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	const ImportResult imported = import_assets({{scene + "/typed.o3d", {}}}, ProjectPaths::for_root(root), view.document, false);
	TEST_EXPECT(imported.imported == std::vector<std::string>({"models/typed.3di"}));
	TEST_EXPECT(editor_test::write_text(root + "/fx.ptl",
	                                    "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff;\n}\n\n[particledef]\n{\n\tid = puff;\n\tgraphic1 = puff.tga, additive;\n}\n"));
	// The project's textures now exactly `files`.
	const auto textures = [&](std::initializer_list<const char *> files) {
		std::error_code ec;
		fs::remove_all(fs::path(root) / "textures", ec);
		for (const char *file : files) editor_test::write_text(root + "/textures/" + file, "x");
		session.handle(make_request(EditorRequestKind::Rescan));
	};
	const std::string model = "models/typed.3di";
	const auto row_of = [&](const char *value) { return edge_to(*view.graph, model, ReferenceKind::Texture, value); };
	const auto resolved = [&](const char *value, std::string *file = nullptr) {
		const GraphEdge *edge = row_of(value);
		return edge ? view.graph->resolve(*edge, file) : ReferenceStatus::NotAReference;
	};
	textures({"wall.dds", "plain.dds", "bump.dds", "trim.dds"});
	TEST_EXPECT(row_of("wall.tga") && row_of("wall.tga")->loader_arg == 0 && row_of("plain.tga")->loader_arg == 1 &&
	            row_of("bump.tga")->loader_arg == 5 && row_of("trim.tga")->loader_arg == 3);
	std::string file;
	TEST_EXPECT(resolved("wall.tga", &file) == ReferenceStatus::Present && file == "textures/wall.dds");
	TEST_EXPECT(resolved("plain.tga") == ReferenceStatus::Missing);
	TEST_EXPECT(resolved("bump.tga", &file) == ReferenceStatus::Present && file == "textures/bump.dds");
	TEST_EXPECT(resolved("trim.tga", &file) == ReferenceStatus::Present && file == "textures/trim.dds");
	TEST_EXPECT(!view.graph->referrers_of_file("wall.dds").empty());
	// A texture of anything else (no row type) reads as the runtime's texture lookup, which
	// takes the stem's .dds too.
	TEST_EXPECT(view.graph->resolve(ReferenceKind::Texture, "wall.tga", "", &file) == ReferenceStatus::Present &&
	            file == "textures/wall.dds");
	// Only the missing row is a finding, and it carries the row's type as its loader's argument
	// (in its JSON too).
	const auto finding_for = [&](const char *target) -> const Diagnostic * {
		for (const Diagnostic &d : view.diagnostics)
			if (d.code == "reference.missing" && d.asset == model && d.target == target) return &d;
		return nullptr;
	};
	const Diagnostic *plain = finding_for("plain.tga");
	TEST_EXPECT(plain && plain->reference == ReferenceKind::Texture && plain->loader_arg == 1);
	TEST_EXPECT(plain && diagnostic_to_json(*plain).get("loader_arg") &&
	            diagnostic_to_json(*plain).get("loader_arg")->number == 1.0 &&
	            !diagnostic_to_json(*plain).get("material_type"));
	TEST_EXPECT(!finding_for("wall.tga") && !finding_for("bump.tga") && !finding_for("trim.tga"));
	// With the .tga files alone, each row is its .tga.
	textures({"wall.tga", "plain.tga", "bump.tga", "trim.tga"});
	TEST_EXPECT(resolved("wall.tga", &file) == ReferenceStatus::Present && file == "textures/wall.tga");
	TEST_EXPECT(resolved("plain.tga", &file) == ReferenceStatus::Present && file == "textures/plain.tga");
	TEST_EXPECT(resolved("bump.tga", &file) == ReferenceStatus::Present && file == "textures/bump.tga");
	TEST_EXPECT(resolved("trim.tga", &file) == ReferenceStatus::Present && file == "textures/trim.tga");
	// Both there: a diffuse row and a normal map take the .dds, a plain row its .tga.
	textures({"wall.tga", "wall.dds", "plain.tga", "plain.dds", "bump.tga", "bump.dds"});
	TEST_EXPECT(resolved("wall.tga", &file) == ReferenceStatus::Present && file == "textures/wall.dds");
	TEST_EXPECT(resolved("plain.tga", &file) == ReferenceStatus::Present && file == "textures/plain.tga");
	TEST_EXPECT(resolved("bump.tga", &file) == ReferenceStatus::Present && file == "textures/bump.dds");
	// An .mdt normal map is a texture the TGA reader decodes (the scan says so, no finding of
	// an unused file); a chunk row reads its file by its name as written, whatever the name,
	// so a file the scan types by no extension serves it. Neither is a finding once there.
	TEST_EXPECT(resolved("ready.mdt") == ReferenceStatus::Missing && resolved("field.nq8") == ReferenceStatus::Missing);
	textures({"wall.tga", "wall.dds", "plain.tga", "plain.dds", "bump.tga", "bump.dds", "ready.mdt", "field.nq8"});
	TEST_EXPECT(view.scan.find("ready.mdt") && view.scan.find("ready.mdt")->kind == AssetKind::Texture);
	TEST_EXPECT(view.scan.find("field.nq8") && view.scan.find("field.nq8")->kind == AssetKind::Unknown);
	TEST_EXPECT(resolved("ready.mdt", &file) == ReferenceStatus::Present && file == "textures/ready.mdt");
	TEST_EXPECT(resolved("field.nq8", &file) == ReferenceStatus::Present && file == "textures/field.nq8");
	TEST_EXPECT(!finding_for("ready.mdt") && !finding_for("field.nq8"));
	for (const Diagnostic &d : view.diagnostics) TEST_EXPECT(!(d.code == "asset.kind.unknown" && d.asset == "textures/ready.mdt"));
	// No other texture takes a file the scan cannot type: a particle naming field.nq8 misses it.
	TEST_EXPECT(view.graph->resolve(ReferenceKind::Texture, "field.nq8") == ReferenceStatus::Missing);
	// The inspector's badge and Go to, and the edge's JSON, answer the same.
	session.handle(make_request(EditorRequestKind::OpenDocument, model));
	const Document *document = session.document_for(model);
	const GraphEdge *wall = row_of("wall.tga");
	TEST_EXPECT(document && wall);
	if (!document || !wall) return 1;
	FieldUse name;
	for (const FieldSchema &schema : document->fields(wall->address.kind))
		if (schema.id == "name") name = document->field_on(wall->address, schema);
	TEST_EXPECT(name.reference == ReferenceKind::Texture && name.loader_arg == 0);
	TEST_EXPECT(document->reference_status(name, std::string("wall.tga"), view, nullptr) == ReferenceStatus::Present);
	TEST_EXPECT(document->reference_target_file(name, std::string("wall.tga"), view) == "textures/wall.dds");
	const opennova::io::JsonValue json = graph_edge_to_json(*view.graph, *wall);
	TEST_EXPECT(json.get("loader_arg") && json.get("loader_arg")->number == 0.0 && !json.get("material_type") &&
	            json.get_string("status", "") == "present" && json.get_string("file", "") == "textures/wall.dds");
	// A particle's texture, as the runtime's lookup reads it: its stem's .dds or its overlay
	// twin serves it; an extension appended to the whole name does not.
	textures({"puff.dds"});
	const GraphEdge *puff = edge_to(*view.graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(puff && puff->loader_arg == -1 && view.graph->resolve(*puff, &file) == ReferenceStatus::Present &&
	            file == "textures/puff.dds" && !graph_edge_to_json(*view.graph, *puff).get("loader_arg"));
	textures({"puff_O.tga"});
	puff = edge_to(*view.graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(puff && view.graph->resolve(*puff) == ReferenceStatus::Present);
	textures({"puff.tga.dds"});
	puff = edge_to(*view.graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(puff && view.graph->resolve(*puff) == ReferenceStatus::Missing);
	return 0;
}

// A rename of a file a reference reaches from another spelling by its loader's rule keeps
// that spelling with the new stem (S11f): a model's rows of types 4 to 7 naming bump.tga,
// whose bump.dds is renamed stone.dds, name stone.tga, so the loader still makes a normal
// map, a horizon volume and an occlusion map of them where stone.dds is a checkerboard
// (renderer::material_texture_transform); a menu's logo.tga, served by logo.dds, takes the
// new name instead where its new spelling is another file of the project.
static int test_rename_keeps_loader_spelling() {
	editor_test::TempProjectDir dir("opennova_asset_graph_rename_spelling");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Spelling"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project_root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/relief.o3d",
	                                    "o3d 1\nmodel RELIEF\nmaterial FF_ST_OP\ntexture bump.tga 3 4\ntexture bump.tga 3 5\n"
	                                    "texture bump.tga 3 6\ntexture bump.tga 3 7\nlod 0\npart 0 0 0 0\nstrip 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	TEST_EXPECT(import_assets({{scene + "/relief.o3d", {}}}, paths, view.document, false).imported ==
	            std::vector<std::string>({"models/relief.3di"}));
	TEST_EXPECT(editor_test::write_text(root + "/textures/bump.dds", "dds"));
	session.handle(make_request(EditorRequestKind::Rescan));
	const RenamePlan plan = plan_rename(paths, view.scan, *view.graph, "textures/bump.dds", "stone.dds");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 4);
	for (const RenameSite &site : plan.sites) TEST_EXPECT(site.before == "bump.tga" && site.after == "stone.tga");
	session.handle(make_request(EditorRequestKind::RenameAsset, "textures/bump.dds", "stone.dds"));
	TEST_EXPECT(session.outcome().done() && view.scan.find("stone.dds") && !view.scan.find("bump.dds"));
	size_t rows = 0;
	for (const GraphEdge *edge : view.graph->references_of("models/relief.3di")) {
		if (edge->kind != ReferenceKind::Texture) continue;
		++rows;
		std::string file;
		TEST_EXPECT(edge->value == "stone.tga" && view.graph->resolve(*edge, &file) == ReferenceStatus::Present &&
		            file == "textures/stone.dds");
		const uint8_t type = opennova::renderer::material_texture_runtime_type(static_cast<uint8_t>(edge->loader_arg));
		TEST_EXPECT(opennova::renderer::material_texture_transform(type, edge->value, true) !=
		            opennova::renderer::MaterialTextureTransform::Checkerboard);
		TEST_EXPECT(opennova::renderer::material_texture_transform(type, "stone.dds", true) ==
		            opennova::renderer::MaterialTextureTransform::Checkerboard);
	}
	TEST_EXPECT(rows == 4);

	// A menu's logo.tga the project serves from logo.dds: renamed where the new spelling is no
	// file, the site keeps its .tga; where the project has that .tga (another file, which the
	// menu loader would take first), the site takes the new name.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && menu->find("EXIT", exit));
	if (!menu) return 1;
	EditorRequest set = make_request(EditorRequestKind::EditRecord, menu->path());
	set.edits = menu_test::image_edits(*menu, exit, "logo.tga");
	session.handle(set);
	session.handle(make_request(EditorRequestKind::SaveAll));
	TEST_EXPECT(editor_test::write_text(root + "/textures/logo.dds", "dds") && editor_test::write_text(root + "/textures/shine.tga", "tga"));
	session.handle(make_request(EditorRequestKind::Rescan));
	const RenamePlan kept = plan_rename(paths, view.scan, *view.graph, "textures/logo.dds", "glow.dds");
	TEST_EXPECT(kept.ok() && kept.sites.size() == 1 && kept.sites[0].after == "glow.tga");
	const RenamePlan taken = plan_rename(paths, view.scan, *view.graph, "textures/logo.dds", "shine.dds");
	TEST_EXPECT(taken.ok() && taken.sites.size() == 1 && taken.sites[0].after == "shine.dds");
	return 0;
}

// S13 D1: an update that finds the files as they were (their rows in the scan, what each read
// references and defines) changes nothing: the generation stands and every edge and symbol stays
// where it was, a file read again whose content is the same included. One whose files moved
// assembles again. A generation is a process-wide counter's value: no two graphs, a copy or a
// cleared graph share one. The symbols of a kind come in the order the files define them.
static int test_generation() {
	editor_test::TempProjectDir dir("opennova_asset_graph_generation");
	const std::string root = dir.file("G");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "G", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string items = root + "/defs/items.def";
	const std::string text = "begin \"A\"\nid 100301\ntype building\ngraphic gone\nend\n"
	                         "begin \"B\"\nid 100300\ntype building\nend\n";
	TEST_EXPECT(editor_test::write_text(items, text));
	AssetScan scan = scan_project_assets(paths, doc);
	AssetGraph graph;
	const uint64_t fresh = graph.generation();
	graph.update(paths, doc, scan, {});
	const uint64_t assembled = graph.generation();
	TEST_EXPECT(assembled != fresh && !graph.edges().empty() && !graph.symbols().empty());
	const GraphEdge *edge = &graph.edges().front();
	const GraphSymbol *symbol = &graph.symbols().front();
	const auto kept = [&graph, assembled, edge, symbol] {
		return graph.generation() == assembled && &graph.edges().front() == edge &&
				&graph.symbols().front() == symbol;
	};
	graph.update(paths, doc, scan, {});
	TEST_EXPECT(graph.stats().files_extracted == 0 && graph.stats().files_reused == 1);
	TEST_EXPECT(kept());
	// The same bytes written again: read again, and still the graph as it was.
	TEST_EXPECT(editor_test::write_text(items, text));
	std::error_code ec;
	fs::last_write_time(items, fs::last_write_time(items, ec) + std::chrono::seconds(5), ec);
	scan = scan_project_assets(paths, doc);
	graph.update(paths, doc, scan, {});
	TEST_EXPECT(graph.stats().files_extracted == 1);
	TEST_EXPECT(kept());
	// Another file in the project (a model the item may name): the rows moved.
	TEST_EXPECT(editor_test::write_text(root + "/models/gone.3di", "x"));
	scan = scan_project_assets(paths, doc);
	graph.update(paths, doc, scan, {});
	const uint64_t grown = graph.generation();
	TEST_EXPECT(grown != assembled && grown != fresh);
	// What a file references changed: assembled again.
	const std::string other = "begin \"A\"\nid 100301\ntype building\ngraphic other\nend\n"
	                          "begin \"B\"\nid 100300\ntype building\nend\n";
	TEST_EXPECT(editor_test::write_text(items, other));
	scan = scan_project_assets(paths, doc);
	graph.update(paths, doc, scan, {});
	const uint64_t changed = graph.generation();
	TEST_EXPECT(changed != grown && changed != assembled && changed != fresh);
	// A file the graph does not read (a mission's .mis, S13 PR0): its row counts, so one added
	// assembles again, while what it holds is read by nothing, so a change to it keeps the graph.
	TEST_EXPECT(editor_test::write_text(root + "/missions/m1.mis", "; one\n"));
	scan = scan_project_assets(paths, doc);
	graph.update(paths, doc, scan, {});
	const uint64_t with_mis = graph.generation();
	TEST_EXPECT(with_mis != changed && graph.stats().files_extracted == 0);
	TEST_EXPECT(editor_test::write_text(root + "/missions/m1.mis", "; two, a longer line\n"));
	scan = scan_project_assets(paths, doc);
	graph.update(paths, doc, scan, {});
	TEST_EXPECT(graph.generation() == with_mis && graph.stats().files_extracted == 0);
	// The item ids in the order the file defines them (by name, 100300 would lead).
	const std::vector<const GraphSymbol *> ids = graph.symbols_of_kind(ReferenceKind::Item);
	TEST_EXPECT(ids.size() == 2 && ids[0]->name == "100301" && ids[1]->name == "100300");
	TEST_EXPECT(graph.symbols_of_kind(ReferenceKind::Weapon).empty());
	// Two graphs, a copy and a cleared graph: each a generation of its own, never one seen before.
	std::set<uint64_t> seen = {fresh, assembled, grown, changed, with_mis};
	AssetGraph another;
	TEST_EXPECT(seen.insert(another.generation()).second);
	const AssetGraph copy = graph;
	TEST_EXPECT(seen.insert(copy.generation()).second);
	TEST_EXPECT(copy.edges().size() == graph.edges().size());
	graph.clear();
	TEST_EXPECT(seen.insert(graph.generation()).second);
	TEST_EXPECT(graph.edges().empty() && graph.symbols().empty());
	another.clear();
	TEST_EXPECT(seen.insert(another.generation()).second);
	// Emptied, the graph reads the files again as a new one would.
	graph.update(paths, doc, scan, {});
	TEST_EXPECT(seen.insert(graph.generation()).second);
	TEST_EXPECT(graph.symbols_of_kind(ReferenceKind::Item).size() == 2);
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_generation();
	failures += test_reference_kind_rows();
	failures += test_reference_file_candidates();
	failures += test_model_texture_references();
	failures += test_rename_keeps_loader_spelling();
	failures += test_user_point_references();
	failures += test_blank_project();
	failures += test_menu_references();
	failures += test_menu_names_and_targets();
	failures += test_menu_rename_follows();
	failures += test_retail_menu_graph();
	failures += test_menu_text_scope();
	failures += test_stylesheet_bindings();
	failures += test_native_extractors();
	failures += test_catalog_symbols();
	failures += test_symbol_locators();
	failures += test_go_to_targets();
	failures += test_rename();
	failures += test_rename_rewrites_planned_sites_only();
	failures += test_rename_by_locator();
	if (failures == 0) std::printf("editor_asset_graph: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
