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
// the refusals that leave everything as it was; and (a SKIP-LEG without OPENNOVA_JO_ASSETS)
// the shipped menus' targets. S12: the
// reference kinds' table (D1), a symbol per defining field with its place (D2), and where Go
// to leads and who uses a file (D3). S13 D3: an update patches only the files whose reading
// changed and equals a graph built fresh over the same files after every scripted step (also as
// a retail leg over the JO install, OPENNOVA_JO_DIR), resolving again only what a change reaches
// (GraphStats); references_of by a file's own slot; the base layer's rules (also over the JO
// install) and its choices after the project's. S13 D8: a model's registers and MTRX rows named by
// index (Record references) resolved within its file, Referenced by, users and the picker over its
// record sets, and an open model's register removed in the middle, its references renumbered, the
// graph equal to one built fresh (also as a retail leg over the JO install's models).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/install_view.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/documents/model_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/strings_document.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_layer.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/lwf/lwf.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_mis.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/graph_test_support.h"
#include "editor/test_platform.h"
#include "editor/menu_test_support.h"
#include "editor/rig_model.h"

using namespace opennova::editor;
using opennova::pff::normalized_logical_name;
namespace renderer = opennova::renderer;
namespace fs = std::filesystem;

namespace {

using editor_test::NoProcess;

// Every edge and every symbol of a graph, in its order (for_each_edge, for_each_symbol), where
// they are.
std::vector<std::reference_wrapper<const GraphEdge>> all_edges(const AssetGraph &graph) {
	std::vector<std::reference_wrapper<const GraphEdge>> out;
	graph.for_each_edge([&out](const GraphEdge &edge) { out.emplace_back(edge); });
	return out;
}

std::vector<std::reference_wrapper<const GraphSymbol>> all_symbols(const AssetGraph &graph) {
	std::vector<std::reference_wrapper<const GraphSymbol>> out;
	graph.for_each_symbol([&out](const GraphSymbol &symbol) { out.emplace_back(symbol); });
	return out;
}

const GraphEdge *edge_to(const AssetGraph &graph, const std::string &source, ReferenceKind kind, const std::string &value) {
	for (const GraphEdge &edge : all_edges(graph))
		if (edge.source == source && edge.kind == kind && edge.value == value) return &edge;
	return nullptr;
}

using graph_test::add_record;
using graph_test::edit_window;
using graph_test::field_on;
using graph_test::go_to;
using graph_test::has_missing;
using graph_test::missing_of;
using graph_test::screen;
using graph_test::set_image;
using graph_test::targets_of;
using graph_test::window;

bool has_symbol(const AssetGraph &graph, ReferenceKind kind, const std::string &display) {
	for (const GraphSymbol &symbol : all_symbols(graph))
		if (symbol.kind == kind && symbol.display == display) return true;
	return false;
}

size_t count_code(const std::vector<Diagnostic> &diagnostics, const char *code) {
	size_t n = 0;
	for (const Diagnostic &d : diagnostics) if (d.code() == code) ++n;
	return n;
}

std::string read_text(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

} // namespace

// A new project after Create all missing: every reference the blank files make resolves.
static int test_blank_project() {
	editor_test::TempProjectDir dir("opennova_asset_graph_blank");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Graph"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.findings.graph != nullptr);
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(!all_edges(graph).empty() && !all_symbols(graph).empty());
	TEST_EXPECT(graph.missing().empty());
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);
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
	// table's null marker (its id, its alias and its name), the blank menus' screens and windows, the blank
	// SndProf.def's "default" profile and the blank _ffp.fx's fixed-function shader tags are the symbols.
	for (const GraphSymbol &symbol : all_symbols(graph))
		TEST_EXPECT(symbol.kind == ReferenceKind::StyleVar || symbol.kind == ReferenceKind::Item ||
		            symbol.kind == ReferenceKind::ItemAlias || symbol.kind == ReferenceKind::ItemName ||
		            symbol.kind == ReferenceKind::MenuScreen || symbol.kind == ReferenceKind::MenuWindow ||
		            symbol.kind == ReferenceKind::SoundProfile || symbol.kind == ReferenceKind::Shader);
	TEST_EXPECT(graph.resolve(ReferenceKind::Shader, "FF_ST_OP") == ReferenceStatus::Present &&
	            graph.resolve(ReferenceKind::Shader, "FF_MT_AD_LUM#UV") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuScreen, "startup", "MAIN.MNU") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuWindow, "exit", "MAIN.MNU/STARTUP") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuWindow, "EXIT", "MAIN.MNU/ELSEWHERE") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.choices(ReferenceKind::Font).size() >= 7);
	TEST_EXPECT(graph.choices(ReferenceKind::StyleVar).front().name.front() == '%');
	for (const ReferenceChoice &choice : graph.choices(ReferenceKind::Font))
		TEST_EXPECT(choice.kind == ReferenceKind::Font && choice.status == ReferenceStatus::Present && !choice.file.empty() &&
		            choice.record.empty() && !choice.inert);
	// A sound set is a bank's symbol (the sound lane): the blank project has no bank, so none is found.
	TEST_EXPECT(graph.resolve(ReferenceKind::Sound, "BOOM") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.resolve(ReferenceKind::None, "x") == ReferenceStatus::NotAReference);
	TEST_EXPECT(graph.stats().files_extracted > 0);
	// Nothing changed: the next update reuses every extraction.
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(graph.stats().files_extracted == 0 && graph.stats().files_reused > 0);
	return 0;
}

// A menu's string id resolves where the game looks it up: the "menu" section (the first
// of that name) of the table its window reads, its own TEXT_RSRC else its root window's
// [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0; CUIStringTable_LookupString @ 0x6527c0];
// with no table up the chain it resolves nowhere (the game shows the id).
static int test_menu_text_scope() {
	editor_test::TempProjectDir dir("opennova_asset_graph_text_scope");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Scope"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	// menutxt.bin: TITLE_ID in its Menu section, STATS_ONLY in another; gametext.bin: a
	// "menu" section with GAME_TITLE.
	editor_test::handle_to_end(session, request::create_file("menutxt.bin", "strings"));
	Document *menutxt = session.document_for("menutxt.bin");
	TEST_EXPECT(menutxt);
	const auto add_id = [&](Document &table, NodeId section, const char *key) {
		EditorRequest add = request::edit_record(table.path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {section, table.kind_from_name("string"), 0};
		editor_test::handle_to_end(session, add);
		edit_window(session, table, {section, table.kind_from_name("string"), table.last_added()}, "key", std::string(key));
	};
	const auto add_section = [&](Document &table, const char *name) {
		EditorRequest add = request::edit_record(table.path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, table.kind_from_name("section"), 0};
		editor_test::handle_to_end(session, add);
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
	editor_test::handle_to_end(session, request::open_document("gametext.bin"));
	Document *gametext = session.document_for("gametext.bin");
	TEST_EXPECT(gametext);
	add_id(*gametext, add_section(*gametext, "menu"), "GAME_TITLE");

	// A lookup that tries a second scope after its own (GraphEdge::scopes_after, S14: a mission's text
	// key reads the mission's table, then gametext.bin): the name found in the first scope, else in
	// the next, else missing; the definition reached the one found, the name reached the value.
	{
		const AssetGraph &graph = *view.findings.graph;
		GraphEdge edge;
		edge.kind = ReferenceKind::TextId;
		edge.value = "GAME_TITLE";
		edge.scope = "MENUTXT.BIN/menu";
		edge.scopes_after = {"GAMETEXT.BIN/menu"};
		std::string file;
		TEST_EXPECT(graph.resolve(edge, &file) == ReferenceStatus::Present && file == gametext->path());
		TEST_EXPECT(graph.symbol_reached(edge) && graph.symbol_reached(edge)->file == gametext->path() &&
		            graph.reached_name(edge) == "GAME_TITLE");
		edge.value = "TITLE_ID";
		TEST_EXPECT(graph.resolve(edge, &file) == ReferenceStatus::Present && file == menutxt->path() &&
		            graph.symbol_reached(edge) && graph.symbol_reached(edge)->file == menutxt->path());
		edge.value = "STATS_ONLY";
		TEST_EXPECT(graph.resolve(edge, &file) == ReferenceStatus::Missing && !graph.symbol_reached(edge));
		edge.scopes_after = {"GAMETEXT.BIN/menu", "MENUTXT.BIN/Stats"};
		TEST_EXPECT(graph.resolve(edge, &file) == ReferenceStatus::Present && file == menutxt->path());
		edge.value = "NOPE";
		TEST_EXPECT(graph.resolve(edge, &file) == ReferenceStatus::Missing && file.empty() && !graph.symbol_reached(edge));
		// The table loaded in the place of one the project lacks (GraphEdge::scope_alternate: a mission's
		// medmssn.bin where it has no <stem>.bin, never both [orig: TextResource_LoadMissionTextBin
		// @0x51ed90]): the alternate's section where the own table is absent, the own table alone where
		// it is present.
		GraphEdge mission;
		mission.kind = ReferenceKind::TextId;
		mission.value = "GAME_TITLE";
		mission.scope = "ABSENT.BIN/menu";
		mission.scope_alternate = "GAMETEXT.BIN";
		TEST_EXPECT(graph.lookup_scope(mission) == "GAMETEXT.BIN/menu" &&
		            graph.resolve(mission, &file) == ReferenceStatus::Present && file == gametext->path());
		mission.scope = "MENUTXT.BIN/menu";
		TEST_EXPECT(graph.lookup_scope(mission) == "MENUTXT.BIN/menu" &&
		            graph.resolve(mission, &file) == ReferenceStatus::Missing && !graph.symbol_reached(mission));
		// A lookup whose owner the project lacks (GraphEdge::scope_owner: a script of a mission the
		// project does not have runs with whichever mission's table plays): any table, nothing after,
		// and no rename rewrites it; with the owner present, its own scope and a rewrite.
		mission.value = "STATS_ONLY";
		mission.rewritable = true;
		mission.scope_owner = "NOSUCH.BMS";
		mission.scopes_after = {"GAMETEXT.BIN/menu"};
		TEST_EXPECT(graph.lookup_scope(mission).empty() && graph.resolve(mission, &file) == ReferenceStatus::Present &&
		            file == menutxt->path() && !graph.rewrites(mission));
		mission.scope_owner = "MENUTXT.BIN";
		TEST_EXPECT(graph.lookup_scope(mission) == "MENUTXT.BIN/menu" &&
		            graph.resolve(mission, &file) == ReferenceStatus::Missing && graph.rewrites(mission));
	}

	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	NodeAddress main, title;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "MAIN", main) &&
			find_definition(AssetGraph(), *menu, "TITLE", title));
	edit_window(session, *menu, title, "string.type", std::string("ID"));
	edit_window(session, *menu, title, "string.value", std::string("TITLE_ID"));
	const auto title_edge = [&]() -> const GraphEdge * {
		for (const GraphEdge *edge : view.findings.graph->references_of(menu->path()))
			if (edge->kind == ReferenceKind::TextId && edge->field == "string.value") return edge;
		return nullptr;
	};
	const auto missing_message = [&](const char *needle) {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "reference.missing" && d.field == "string.value" && d.message.find(needle) != std::string::npos)
				return true;
		return false;
	};
	// No TEXT_RSRC up the chain: the id resolves in no table.
	TEST_EXPECT(title_edge() && title_edge()->scope == "/menu");
	TEST_EXPECT(missing_message("names no string table"));
	// MAIN names menutxt.bin: TITLE reads its root's table.
	EditorRequest write = request::edit_record(menu->path(), Edit());
	write.edits[0].operation = EditOperation::Write;
	write.edits[0].address = main;
	write.edits[0].field = "text_rsrc";
	editor_test::handle_to_end(session, write);
	edit_window(session, *menu, main, "text_rsrc", std::string("menutxt.bin"));
	TEST_EXPECT(title_edge() && title_edge()->scope == "MENUTXT.BIN/menu");
	TEST_EXPECT(
			!has_missing(view.findings.diagnostics, "string.value", DiagnosticSeverity::Warning));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::TextId, "TITLE_ID",
						"MENUTXT.BIN/menu") == ReferenceStatus::Present);
	TEST_EXPECT(
			view.findings.graph->referrers_of(ReferenceKind::TextId, "TITLE_ID", "MENUTXT.BIN/Menu")
					.size() == 1);
	TEST_EXPECT(view.findings.graph
					->referrers_of(ReferenceKind::TextId, "TITLE_ID", "GAMETEXT.BIN/menu")
					.empty());
	// Its picker offers the ids of that table's "menu" section alone: not another section's, nor
	// another table's.
	const auto offered = [&](const char *key, ReferenceChoice &out) {
		for (const FieldSchema &schema : menu->fields(title.kind))
			if (schema.id == "string.value")
				for (const ReferenceChoice &choice :
						reference_choices(*view.findings.graph, menu->field_on(title, schema)))
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
	TEST_EXPECT(
			has_missing(view.findings.diagnostics, "string.value", DiagnosticSeverity::Warning));
	// TITLE's own TEXT_RSRC wins over its root's.
	EditorRequest own = write;
	own.edits[0].address = title;
	editor_test::handle_to_end(session, own);
	edit_window(session, *menu, title, "text_rsrc", std::string("gametext.bin"));
	TEST_EXPECT(title_edge() && title_edge()->scope == "GAMETEXT.BIN/menu");
	TEST_EXPECT(
			!has_missing(view.findings.diagnostics, "string.value", DiagnosticSeverity::Warning));
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Native"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
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
		// The same mission in the original editor's text form (S14): a mission text to the scan, a
		// kind no document opens and the graph does not read, so it is no finding and names nothing.
		std::string text;
		TEST_EXPECT(opennova::mission::write_mis_text(mission, text, error));
		TEST_EXPECT(editor_test::write_text(root + "/test.mis", text));
	}
	// A synthetic model from the fixtures, when the checkout carries them.
	const fs::path fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "synth" / "armory.3di";
	std::error_code ec;
	const bool have_model = fs::is_regular_file(fixture, ec);
	if (have_model) fs::copy_file(fixture, fs::path(root) / "armory.3di", ec);
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &graph = *session.view().findings.graph;
	const GraphEdge *sky = edge_to(graph, "day.env", ReferenceKind::Texture, "sky_a.pcx");
	TEST_EXPECT(sky && sky->rewritable && sky->field == "sky_map1"); // a name its text writes (native_text_sites.h)
	TEST_EXPECT(edge_to(graph, "day.env", ReferenceKind::Model, "sun.3di"));
	// A sky map loads through the archive loader naming the file twice: its .dds sibling first,
	// else the name itself, no other extension. Its edge carries its role (ADR 0046 S18), whose loader
	// that is.
	const int32_t sky_loader = texture_loader_arg(opennova::renderer::TextureLoader::ArchiveSelfAlpha);
	TEST_EXPECT(sky && sky->loader_arg == texture_role_arg(renderer::TextureRoleId::SkyCloud, kTextureArgPcx));
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a.pcx", "", nullptr, sky_loader) == ReferenceStatus::Missing);
	TEST_EXPECT(editor_test::write_text(root + "/sky_a.pcx", "x"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a.pcx", "", nullptr, sky_loader) == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a", "", nullptr, sky_loader) == ReferenceStatus::Missing);
	TEST_EXPECT(editor_test::write_text(root + "/sky_c.dds", "x"));
	editor_test::handle_to_end(session, request::rescan());
	std::string sky_file;
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_c.pcx", "", &sky_file, sky_loader) == ReferenceStatus::Present &&
	            sky_file == "sky_c.dds");
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a.pcx") == ReferenceStatus::Present);
	// No loader of the game adds an extension (ADR 0046 S18): a name without one finds nothing.
	TEST_EXPECT(graph.resolve(ReferenceKind::Texture, "sky_a") == ReferenceStatus::Missing);
	// The sky map's own loader (ARCHIVE, the .dds beside the name first) finds the PCX, then its .dds.
	{
		std::string served;
		const GraphEdge *map = edge_to(graph, "day.env", ReferenceKind::Texture, "sky_a.pcx");
		TEST_EXPECT(map && graph.resolve(*map, &served) == ReferenceStatus::Present && served == "sky_a.pcx");
		TEST_EXPECT(editor_test::write_text(root + "/sky_a.dds", "x"));
		editor_test::handle_to_end(session, request::rescan());
		map = edge_to(graph, "day.env", ReferenceKind::Texture, "sky_a.pcx");
		TEST_EXPECT(map && graph.resolve(*map, &served) == ReferenceStatus::Present && served == "sky_a.dds");
		TEST_EXPECT(!graph.referrers_of_file("sky_a.dds").empty());
		std::error_code removed;
		fs::remove(fs::path(root) / "sky_a.dds", removed);
		editor_test::handle_to_end(session, request::rescan());
	}
	TEST_EXPECT(!graph.referrers_of_file("sky_a.pcx").empty());
	TEST_EXPECT(has_symbol(graph, ReferenceKind::Particle, "BOOM"));
	TEST_EXPECT(graph.resolve(ReferenceKind::Particle, "boom") == ReferenceStatus::Present);
	const GraphEdge *puff = edge_to(graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(puff && puff->record == "puff" && puff->field == "graphic1");
	// The mission's references are its document's fields (S14): rewritable, on its mission row.
	const GraphEdge *terrain = edge_to(graph, "test.bms", ReferenceKind::Terrain, "island");
	TEST_EXPECT(terrain && terrain->rewritable && terrain->field == "terrain" &&
			graph.resolve(ReferenceKind::Terrain, "island") == ReferenceStatus::Missing);
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
	TEST_EXPECT(count_code(session.view().findings.diagnostics, "reference.missing") == missing);
	TEST_EXPECT(count_code(session.view().findings.diagnostics, "graph.unreadable") == 0);
	// The .mis is skipped, never extracted (graph_reads_kind): a changed one is read by nothing.
	TEST_EXPECT(session.view().project.scan->find("test.mis") &&
	            session.view().project.scan->find("test.mis")->kind == AssetKind::MissionText &&
	            !graph_reads_kind(AssetKind::MissionText) && graph_reads_kind(AssetKind::Mission));
	TEST_EXPECT(editor_test::write_text(root + "/test.mis", "; changed\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(graph.stats().files_extracted == 0 && graph.stats().files_failed == 0);
	TEST_EXPECT(graph.references_of("test.mis").empty() &&
			count_code(session.view().findings.diagnostics, "graph.unreadable") == 0);

	// A script of the mission's name names the mission's entities and areas by its literal operands (S23 B): in
	// test.bms, warnings where it has none, the area's in the script's words; a script of no mission the project
	// has names none.
	{
		TEST_EXPECT(editor_test::write_text(root + "/test.wac", "v1=SSNarea(42,37) v2=SSNdead(10001)\r\n"));
		TEST_EXPECT(editor_test::write_text(root + "/lone.wac", "v1=SSNarea(42,37)\r\n"));
		editor_test::handle_to_end(session, request::rescan());
		const GraphEdge *entity = edge_to(graph, "test.wac", ReferenceKind::MissionEntity, "42");
		const GraphEdge *zone = edge_to(graph, "test.wac", ReferenceKind::MissionZone, "37");
		TEST_EXPECT(entity && zone && entity->scope == "TEST.BMS" && !entity->rewritable &&
		            graph.resolve(*entity) == ReferenceStatus::Missing && graph.resolve(*zone) == ReferenceStatus::Missing);
		const GraphEdge *lone = edge_to(graph, "lone.wac", ReferenceKind::MissionEntity, "42");
		TEST_EXPECT(lone && graph.resolve(*lone) == ReferenceStatus::NotAReference);
		// A player's SSN (10000 and its slot: 10001 the second player) names no record of the mission.
		TEST_EXPECT(!edge_to(graph, "test.wac", ReferenceKind::MissionEntity, "10001"));
		bool zone_words = false;
		for (const Diagnostic &d : session.view().findings.diagnostics)
			zone_words = zone_words || (d.asset == "test.wac" && d.severity == DiagnosticSeverity::Warning &&
			                            d.message.find("the script's command finds no area") != std::string::npos);
		TEST_EXPECT(zone_words);
		std::error_code gone;
		fs::remove(fs::path(root) / "test.wac", gone);
		fs::remove(fs::path(root) / "lone.wac", gone);
		editor_test::handle_to_end(session, request::rescan());
	}

	// A native file the graph cannot read is a warning, its references unchecked, kept
	// while the file is unchanged: a particle file, which opens as a text (DI-14), the particle type's
	// (particle.unreadable, the reader's words at the place it stops), the graph's own reading of it
	// failing still; a document type's file that does not load (a mission, a model, a menu) is its
	// validator's error, not the graph's.
	TEST_EXPECT(editor_test::write_text(root + "/broken.ptl", "[effectdef]\n{\n\tid = OPEN;\n"));
	TEST_EXPECT(editor_test::write_text(root + "/broken.bms", "not a mission"));
	TEST_EXPECT(editor_test::write_text(root + "/broken.3di", "not a model"));
	TEST_EXPECT(editor_test::write_bytes(root + "/broken.mnu", {0xFF, 0xFE, 0x41}));
	editor_test::handle_to_end(session, request::rescan());
	const std::vector<Diagnostic> &diagnostics = session.view().findings.diagnostics;
	const auto unreadable = [&diagnostics](const std::string &asset) {
		size_t n = 0;
		for (const Diagnostic &d : diagnostics)
			n += d.code() == "particle.unreadable" && d.asset == asset && d.severity == DiagnosticSeverity::Warning ? 1 : 0;
		return n;
	};
	TEST_EXPECT(unreadable("broken.ptl") == 1 && count_code(diagnostics, "graph.unreadable") == 0);
	TEST_EXPECT(graph.stats().files_failed == 4);
	bool mission_error = false;
	for (const Diagnostic &d : diagnostics)
		mission_error = mission_error || (d.asset == "broken.bms" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(mission_error);
	bool menu_error = false;
	for (const Diagnostic &d : diagnostics)
		menu_error = menu_error || (d.asset == "broken.mnu" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(menu_error);
	bool model_error = false;
	for (const Diagnostic &d : diagnostics)
		model_error = model_error || (d.asset == "broken.3di" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(model_error);
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(graph.stats().files_failed == 0 && unreadable("broken.ptl") == 1);
	fs::remove(fs::path(root) / "broken.ptl");
	fs::remove(fs::path(root) / "broken.bms");
	fs::remove(fs::path(root) / "broken.3di");
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(count_code(session.view().findings.diagnostics, "graph.unreadable") == 0);
	return 0;
}

// The catalogs define weapon and ammo names and item ids; a weapon's round names an
// ammo record and "Referenced by" finds it.
static int test_catalog_symbols() {
	editor_test::TempProjectDir dir("opennova_asset_graph_catalog");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Catalog"));
	editor_test::create_missing_files(session);
	session.handle(request::create_file("ammo.def")); // not a menu project's requirement
	Document *ammo = session.document_for("ammo.def");
	TEST_EXPECT(ammo);
	{
		EditorRequest add = request::edit_record(ammo->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, ammo->kind_from_name("ammo"), 0};
		editor_test::handle_to_end(session, add);
		edit_window(session, *ammo, {ammo->last_added(), ammo->kind_from_name("ammo"), 0}, "name", std::string("AMMO_GRAPH"));
	}
	editor_test::handle_to_end(session, request::open_document("weapon.def"));
	Document *weapon = session.document_for("weapon.def");
	TEST_EXPECT(weapon);
	{
		EditorRequest add = request::edit_record(weapon->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, weapon->kind_from_name("weapon"), 0};
		editor_test::handle_to_end(session, add);
		const NodeAddress row{weapon->last_added(), weapon->kind_from_name("weapon"), 0};
		edit_window(session, *weapon, row, "weapon_name", std::string("WPN_GRAPH"));
		edit_window(session, *weapon, row, "round_type", std::string("AMMO_GRAPH"));
		edit_window(session, *weapon, row, "gfx1", std::string("gun.3di"));
	}
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(has_symbol(graph, ReferenceKind::Ammo, "AMMO_GRAPH") && has_symbol(graph, ReferenceKind::Weapon, "WPN_GRAPH"));
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "ammo_graph") == ReferenceStatus::Present);
	const std::vector<const GraphEdge *> users = graph.referrers_of(ReferenceKind::Ammo, "AMMO_GRAPH");
	TEST_EXPECT(users.size() == 1 && users[0]->source == weapon->path() && users[0]->field == "round_type");
	TEST_EXPECT(graph.symbols_of(ammo->path(), "AMMO_GRAPH").size() == 1);
	TEST_EXPECT(has_missing(view.findings.diagnostics, "gfx1", DiagnosticSeverity::Error));
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
	editor_test::handle_to_end(session, request::open_document("gametext.bin"));
	Document *strings = session.document_for("gametext.bin");
	TEST_EXPECT(strings);
	const auto add_id = [&](NodeId section, const char *key) {
		EditorRequest add_string = request::edit_record(strings->path(), Edit());
		add_string.edits[0].operation = EditOperation::Add;
		add_string.edits[0].address = {section, strings->kind_from_name("string"), 0};
		editor_test::handle_to_end(session, add_string);
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
		EditorRequest add = request::edit_record(strings->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, strings->kind_from_name("section"), 0};
		editor_test::handle_to_end(session, add);
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
	TEST_EXPECT(!has_missing(
			view.findings.diagnostics, "loadout_menu_textid", DiagnosticSeverity::Warning));
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
		TEST_EXPECT(find_definition(AssetGraph(), *strings, "wep_graph", found) &&
				found == key[0]->address);
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
		TEST_EXPECT(wepdes_key.size() == 1 &&
				find_definition(
						AssetGraph(), *strings, "wep_graph", found, "GAMETEXT.BIN/WepDes") &&
				found == wepdes_key[0]->address);
		TEST_EXPECT(find_definition(
							AssetGraph(), *strings, "WEP_GRAPH", found, "GAMETEXT.BIN/Overlays") &&
				found != wepdes_key[0]->address);
		TEST_EXPECT(!find_definition(
				AssetGraph(), *strings, "WEP_GRAPH", found, "GAMETEXT.BIN/WPNames"));
		// A key only the shadowed second WepDes defines: the lookup there finds none (the graph
		// says Missing), while a find by name alone still reaches the record.
		TEST_EXPECT(!find_definition(
				AssetGraph(), *strings, "WEP_SHADOWED", found, "GAMETEXT.BIN/WepDes"));
		const std::vector<const GraphSymbol *> shadowed_now = graph.symbols_named(ReferenceKind::TextId, "WEP_SHADOWED");
		TEST_EXPECT(find_definition(AssetGraph(), *strings, "WEP_SHADOWED", found) &&
				shadowed_now.size() == 1 && found == shadowed_now[0]->address);
		const std::vector<const GraphEdge *> uses = graph.usages_of(strings->path());
		TEST_EXPECT(std::any_of(uses.begin(), uses.end(), [&](const GraphEdge *edge) {
			return edge->source == weapon->path() && edge->field == "loadout_menu_textid" && !edge->locator.empty();
		}));
		if (!targets.empty()) go_to(session, targets[0]);
		TEST_EXPECT(view.documents.active == strings->path() && wepdes_key.size() == 1 &&
				view.documents.selection.primary == wepdes_key[0]->address &&
				editor_test::revealed_field(view) == "key");
	}
	edit_window(session, *weapon, {weapon->rows()[0]->id, weapon->kind_from_name("weapon"), 0}, "loadout_menu_textid", std::string("WEP_NOPE"));
	TEST_EXPECT(has_missing(
			view.findings.diagnostics, "loadout_menu_textid", DiagnosticSeverity::Warning));
	// The items' identities.
	editor_test::handle_to_end(session, request::open_document("items.def"));
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Rename"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	NodeAddress exit;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "EXIT", exit));
	set_image(session, *menu, exit, "logo.tga");
	const SessionView &view = session.view();
	// The menu naming it has unsaved edits: the rename waits on the unsaved prompt, which
	// lists the menu; cancelled, nothing moves.
	editor_test::handle_to_end(session, request::rename_asset("logo.tga", "logo2.tga"));
	TEST_EXPECT(session.outcome().unsaved_prompt && view.dialogs.unsaved_prompt.files == std::vector<std::string>({menu->path()}));
	TEST_EXPECT(fs::exists(root + "/logo.tga") && !fs::exists(root + "/logo2.tga"));
	EditorRequest cancel = request::resolve_unsaved(UnsavedChoice::Cancel);
	editor_test::handle_to_end(session, cancel);
	TEST_EXPECT(!view.dialogs.unsaved_prompt.open && menu->dirty());
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(!menu->dirty());
	{
		const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "logo.tga", "logo2.tga");
		TEST_EXPECT(plan.ok() && plan.sites.size() == 1 && plan.sites[0].file == menu->path() && plan.sites[0].after == "logo2.tga");
		TEST_EXPECT(plan.new_path == "logo2.tga" && plan.old_name == "logo.tga");
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "logo.tga", "logo.pcx").ok());
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "logo.tga", "main.mnu").ok());
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "logo.tga", "a_name_far_too_long.tga").ok());
		TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "nope.tga", "x.tga").ok());
	}
	// S13 A8: the file under its new name is dated now, not when the file it was copied from was
	// written, so no cache that keys a file by its size and last write takes it for the file that
	// held the name before (two of a size swapping names through three renames).
	TEST_EXPECT(editor_test::backdate(root + "/logo.tga", std::chrono::hours(1)));
	editor_test::handle_to_end(session, request::rename_asset("logo.tga", "logo2.tga"));
	TEST_EXPECT(!fs::exists(root + "/logo.tga") && fs::exists(root + "/logo2.tga"));
	TEST_EXPECT(fs::last_write_time(root + "/logo2.tga") > fs::file_time_type::clock::now() - std::chrono::minutes(5));
	menu = session.document_for("main.mnu"); // reloaded after the rewrite
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	Value image;
	TEST_EXPECT(menu->get(menu_test::child_of(*menu, exit, "appearance"), "value", image) &&
	            std::get<std::string>(image) == "logo2.tga");
	TEST_EXPECT(read_text(root + "/menus/main.mnu").find("logo2.tga") != std::string::npos ||
	            read_text(root + "/main.mnu").find("logo2.tga") != std::string::npos);
	TEST_EXPECT(view.findings.graph->missing().empty());
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);
	// An environment names a sky map: its line is rewritten as text, every other byte kept (ADR 0046 S18,
	// graph/native_text_sites.h). A sky map's name is made .pcx as the game parses it, so the environment
	// naming logo2.tga uses no TGA: the TGA's rename leaves it as it is. (A Rescan keeps an open document
	// whose file did not change: the same one.)
	std::string environment;
	{
		opennova::env::Config config;
		config.sky_map1 = "cloud.pcx";
		config.sky_map2 = "logo2.tga";
		std::ostringstream out;
		std::string error;
		TEST_EXPECT(opennova::env::save_env(out, config, error));
		environment = out.str();
		TEST_EXPECT(editor_test::write_text(root + "/day.env", environment) && editor_test::write_text(root + "/cloud.pcx", "x"));
	}
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(session.document_for("main.mnu") == menu);
	editor_test::handle_to_end(session, request::rename_asset("logo2.tga", "logo3.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(!fs::exists(root + "/logo2.tga") && fs::exists(root + "/logo3.tga"));
	TEST_EXPECT(read_text(root + "/day.env") == environment);
	editor_test::handle_to_end(session, request::rename_asset("cloud.pcx", "haze.pcx"));
	TEST_EXPECT(session.outcome().done() && fs::exists(root + "/haze.pcx"));
	{
		const size_t at = environment.find("sky_map1 cloud.pcx");
		TEST_EXPECT(at != std::string::npos);
		environment.replace(at, 18, "sky_map1 haze.pcx");
		TEST_EXPECT(read_text(root + "/day.env") == environment);
	}
	menu = session.document_for("main.mnu"); // reloaded after the rewrite
	// Through a style variable: the variable's value is the site.
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	edit_window(session, *menu, menu_test::child_of(*menu, exit, "appearance"), "value", std::string("%DEF_FONTNAME_LG%"));
	editor_test::handle_to_end(session, request::save_all());
	fs::remove(root + "/day.env");
	editor_test::handle_to_end(session, request::rescan());
	std::string font_file;
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &font_file) == ReferenceStatus::Present);
	// The variable stays in the menu: the site is its value in menu_style.mns, the one
	// place that names the font.
	{
		const RenamePlan through_style = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, font_file, "zz.fnt");
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
	editor_test::handle_to_end(session, request::rename_asset(font_file, "zz.fnt"));
	TEST_EXPECT(session.outcome().done());
	const std::string renamed_font = (fs::path(font_file).parent_path() / "zz.fnt").generic_string();
	TEST_EXPECT(!fs::exists(root + "/" + font_file) && fs::exists(root + "/" + renamed_font));
	const AssetEntry *style_asset = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style_asset != nullptr);
	if (!style_asset) return 1;
	TEST_EXPECT(read_text(root + "/" + style_asset->relative_path).find("\r\nDEF_FONTNAME_LG\tzz.fnt\r\n") != std::string::npos);
	menu = session.document_for("main.mnu");
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	if (!menu) return 1;
	Value through;
	TEST_EXPECT(menu->get(menu_test::child_of(*menu, exit, "appearance"), "value", through) &&
	            std::get<std::string>(through) == "%DEF_FONTNAME_LG%");
	std::string resolved_font;
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &resolved_font) ==
	                    ReferenceStatus::Present &&
	            resolved_font == renamed_font);
	for (const Diagnostic &d : view.findings.diagnostics)
		TEST_EXPECT(!(d.code() == "reference.missing" && d.field == "font.name"));
	font_file = renamed_font;
	// A value without the extension is no site the rename rewrites: refused, the variable named.
	editor_test::handle_to_end(session, request::open_document(style_asset->relative_path));
	Document *style = session.document_for(style_asset->relative_path);
	NodeAddress large;
	TEST_EXPECT(style && find_definition(AssetGraph(), *style, "%def_fontname_lg%", large));
	edit_window(session, *style, large, "value", std::string("zz"));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%") == ReferenceStatus::Present);
	const RenamePlan through_style = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, font_file, "zz2.fnt");
	TEST_EXPECT(!through_style.ok() && through_style.refusals.front().code() == "rename.style");
	editor_test::handle_to_end(session, request::undo());
	editor_test::handle_to_end(session, request::close_document(style_asset->relative_path));
	// And back: the required font is the project's again.
	editor_test::handle_to_end(session, request::rename_asset(font_file, fs::path(original_font).filename().generic_string()));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(fs::exists(root + "/" + original_font) && !fs::exists(root + "/" + font_file));
	// Assign: a required name satisfied by renaming a file of the right kind.
	TEST_EXPECT(editor_test::write_text(root + "/spare.pcx", "x")); // the wrong kind for a font row
	std::string missing_role, missing_name;
	for (const RequirementRow &row : view.project.requirements->rows)
		if (row.state == RequirementState::Present && row.expected_kind == AssetKind::Strings) { missing_role = row.role; missing_name = row.name; break; }
	TEST_EXPECT(!missing_role.empty());
	fs::remove(root + "/" + view.project.scan->find(missing_name)->relative_path);
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.requirements->required_missing == 1);
	{
		EditorRequest assign = request::assign_requirement(missing_role, "spare.pcx");
		editor_test::handle_to_end(session, assign);
		// The request's own fault: refused in its outcome, no Problems row.
		TEST_EXPECT(!session.outcome().done() && !session.outcome().findings.empty() &&
		            session.outcome().findings.back().code() == "requirement.kind");
		TEST_EXPECT(std::none_of(view.findings.diagnostics.begin(), view.findings.diagnostics.end(),
		                         [](const Diagnostic &d) { return d.code() == "requirement.kind"; }));
		TEST_EXPECT(view.project.requirements->required_missing == 1);
	}
	// A table of the right kind, copied from another required table.
	const AssetEntry *some_table = nullptr;
	for (const AssetEntry &asset : view.project.scan->entries)
		if (asset.kind == AssetKind::Strings) { some_table = &asset; break; }
	TEST_EXPECT(some_table);
	std::error_code ec;
	fs::copy_file(root + "/" + some_table->relative_path, root + "/spare.bin", ec);
	TEST_EXPECT(!ec);
	editor_test::handle_to_end(session, request::rescan());
	{
		EditorRequest assign = request::assign_requirement(missing_role, "spare.bin");
		editor_test::handle_to_end(session, assign);
	}
	TEST_EXPECT(view.project.requirements->required_missing == 0);
	TEST_EXPECT(
			view.project.scan->find(missing_name) != nullptr && !fs::exists(root + "/spare.bin"));
	// Assigning a requirement already met renames nothing: a refusal the outcome carries.
	editor_test::handle_to_end(session, request::assign_requirement(missing_role, "spare.pcx"));
	TEST_EXPECT(!session.outcome().done() && !session.outcome().findings.empty() &&
	            session.outcome().findings.back().code() == "requirement.assigned");
	TEST_EXPECT(fs::exists(root + "/spare.pcx"));
	return 0;
}

// Only the planned sites are rewritten: a weapon names its animation map and its model
// by the same stem, and renaming the map (m16.adm -> m16b.adm) rewrites the map's
// field while the model's keeps naming m16.3di.
static int test_rename_rewrites_planned_sites_only() {
	editor_test::TempProjectDir dir("opennova_asset_graph_rename_sites");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Sites"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/m16.adm", "adm"));
	TEST_EXPECT(editor_test::write_text(root + "/m16.3di", "3di"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("weapon.def"));
	Document *weapon = session.document_for("weapon.def");
	TEST_EXPECT(weapon);
	if (!weapon) return 1;
	const NodeKind kind = weapon->kind_from_name("weapon");
	{
		EditorRequest add = request::edit_record(weapon->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, kind, 0};
		editor_test::handle_to_end(session, add);
		const NodeAddress row{weapon->last_added(), kind, 0};
		edit_window(session, *weapon, row, "weapon_name", std::string("WPN_M16"));
		edit_window(session, *weapon, row, "animadm", std::string("m16"));
		edit_window(session, *weapon, row, "gfx1", std::string("m16"));
	}
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(!weapon->dirty());
	const SessionView &view = session.view();
	std::string file;
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::AnimationMap, "m16", std::string(), &file) == ReferenceStatus::Present &&
	            file == "m16.adm");
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Model, "m16", std::string(), &file) == ReferenceStatus::Present &&
	            file == "m16.3di");
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "m16.adm", "m16b.adm");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 1);
	TEST_EXPECT(!plan.sites.empty() && plan.sites[0].field == "animadm" && plan.sites[0].after == "m16b");
	editor_test::handle_to_end(session, request::rename_asset("m16.adm", "m16b.adm"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(!fs::exists(root + "/m16.adm") && fs::exists(root + "/m16b.adm") && fs::exists(root + "/m16.3di"));
	weapon = session.document_for("weapon.def"); // reloaded after the rewrite
	TEST_EXPECT(weapon && !weapon->rows().empty());
	if (!weapon || weapon->rows().empty()) return 1;
	const NodeAddress row{weapon->rows().back()->id, kind, 0};
	Value animation, model;
	TEST_EXPECT(weapon->get(row, "animadm", animation) && std::get<std::string>(animation) == "m16b");
	TEST_EXPECT(weapon->get(row, "gfx1", model) && std::get<std::string>(model) == "m16");
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);

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
	// The request is done (its commit started); what the commit came to is its operation's.
	editor_test::handle_to_end(session, request::rename_asset("m16b.adm", "m16c.adm"));
	TEST_EXPECT(session.outcome().done() && view.activity.last_operation.end == OperationEnd::Failed &&
	            count_code(view.activity.last_operation.findings, "rename.partial") > 0);
	// The error names what the file still says (the site's own spelling), not the renamed file.
	bool partial_error = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		partial_error = partial_error || (d.code() == "rename.partial" && d.severity == DiagnosticSeverity::Error &&
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Locator"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	NodeAddress main;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "MAIN", main));
	std::vector<NodeAddress> twins;
	for (int i = 0; i < 2; ++i) {
		EditorRequest add = request::edit_record(menu->path(), Edit());
		add.edits[0].operation = EditOperation::Add;
		add.edits[0].address = {0, main.kind, 0};
		add.edits[0].parent = main.child;
		editor_test::handle_to_end(session, add);
		twins.push_back(menu->address_of(menu->last_added()));
		edit_window(session, *menu, twins.back(), "name", std::string("TWIN"));
		set_image(session, *menu, twins.back(), "logo.tga");
	}
	editor_test::handle_to_end(session, request::save_all());
	const SessionView &view = session.view();
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "logo.tga", "logo2.tga");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 2);
	TEST_EXPECT(plan.sites[0].record == "STARTUP/MAIN/TWIN/Appearance 1" && plan.sites[1].record == plan.sites[0].record);
	TEST_EXPECT(!plan.sites[0].locator.empty() && plan.sites[0].locator != plan.sites[1].locator);
	const std::vector<std::string> places = {menu->locator(menu_test::child_of(*menu, twins[0], "appearance")),
	                                         menu->locator(menu_test::child_of(*menu, twins[1], "appearance"))};
	TEST_EXPECT(std::find(places.begin(), places.end(), plan.sites[0].locator) != places.end() &&
	            std::find(places.begin(), places.end(), plan.sites[1].locator) != places.end());
	editor_test::handle_to_end(session, request::rename_asset("logo.tga", "logo2.tga"));
	TEST_EXPECT(session.outcome().done());
	menu = session.document_for("main.mnu"); // reloaded after the rewrite
	TEST_EXPECT(menu);
	size_t renamed = 0;
	for (const std::string &place : places) {
		Value image;
		if (menu->get(menu->address_at(place), "value", image) && std::get<std::string>(image) == "logo2.tga") ++renamed;
	}
	TEST_EXPECT(renamed == 2 && view.findings.graph->missing().empty());
	return 0;
}

// The stylesheets as the game reads them (S9i): menu_style.mns, then brand.mns over it;
// a stylesheet by another name, or a line after the place the game stops reading one,
// defines nothing a menu can use; a stylesheet's font values are edges of their own, and
// a missing font a menu names through a variable is reported once, there.
static int test_stylesheet_bindings() {
	editor_test::TempProjectDir dir("opennova_asset_graph_styles");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Styles"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = dir.file("project");
	const AssetEntry *style_asset = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style_asset != nullptr);
	const std::string style_path = style_asset->relative_path;
	const std::string style_dir = fs::path(root + "/" + style_path).parent_path().generic_string();
	// A symbol carries its record and the value the game reads.
	const GraphSymbol *large = view.findings.graph->style_binding("%DEF_FONTNAME_LG%");
	TEST_EXPECT(large && large->record == "DEF_FONTNAME_LG" && large->value == "Arial16b.fnt" && large->file == style_path);
	TEST_EXPECT(!large->inert);
	// The stylesheet's font value is an edge of its own; a menu's font through the
	// variable says what the value must be there.
	const GraphEdge *value_edge = edge_to(*view.findings.graph, style_path, ReferenceKind::Font, "Arial16b.fnt");
	TEST_EXPECT(value_edge && value_edge->rewritable && value_edge->field == "value" && value_edge->record == "DEF_FONTNAME_LG");
	bool through_font = false;
	for (const GraphEdge &edge : all_edges(*view.findings.graph))
		through_font = through_font || (edge.kind == ReferenceKind::StyleVar && edge.through == ReferenceKind::Font);
	TEST_EXPECT(through_font);
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);
	TEST_EXPECT(count_code(view.findings.diagnostics, "style.not_a_color") == 0 && count_code(view.findings.diagnostics, "style.mixed_use") == 0);

	// brand.mns over menu_style.mns: a later definition wins.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG FF102030\r\nBRAND_ONLY 1\r\n"));
	// A stylesheet by another name is never read.
	TEST_EXPECT(editor_test::write_text(style_dir + "/other.mns", "STRAY_ONLY 1\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const GraphSymbol *text_fg = view.findings.graph->style_binding("DEF_TEXT_FG");
	TEST_EXPECT(text_fg && fs::path(text_fg->file).filename() == "brand.mns" && text_fg->value == "FF102030");
	TEST_EXPECT(view.findings.graph->resolve_style("%DEF_TEXT_FG%") == "FF102030");
	size_t inert = 0;
	for (const GraphSymbol &symbol : all_symbols(*view.findings.graph))
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
	for (const ReferenceChoice &choice : view.findings.graph->choices(ReferenceKind::StyleVar)) {
		if (choice.name == "%STRAY_ONLY%") stray_offered = choice.inert && choice.status == ReferenceStatus::Missing && !choice.reason.empty();
		if (choice.name == "%DEF_TEXT_FG%") brand_offered = !choice.inert && fs::path(choice.file).filename() == "brand.mns";
	}
	TEST_EXPECT(stray_offered && brand_offered);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::StyleVar, "%STRAY_ONLY%") == ReferenceStatus::Missing);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::StyleVar, "%BRAND_ONLY%") == ReferenceStatus::Present);
	TEST_EXPECT(count_code(view.findings.diagnostics, "style.overridden_by_brand") == 1);
	TEST_EXPECT(count_code(view.findings.diagnostics, "style.not_loaded") == 1);
	// A colour the menus read through a variable follows wcstoul (a sign and eight digits
	// read whole, a 'G' stops the digits), and a value holds a %NAME% only where the
	// game's expansion finds one (S12 B2).
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG +FF102030\r\nBRAND_ONLY 50% of %A B%\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(count_code(view.findings.diagnostics, "style.not_a_color") == 0 && count_code(view.findings.diagnostics, "style.nested_var") == 0);
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_TEXT_FG FF10203G\r\nBRAND_ONLY x%DEF_TEXT_FG%\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(count_code(view.findings.diagnostics, "style.not_a_color") == 1 && count_code(view.findings.diagnostics, "style.nested_var") == 1);

	// A menu naming the stray name: the finding says the game does not read that stylesheet.
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	edit_window(session, *menu, exit, "font.name", std::string("%STRAY_ONLY%"));
	bool stray_message = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		stray_message = stray_message || (d.code() == "reference.missing" && d.message.find("does not read") != std::string::npos);
	TEST_EXPECT(stray_message);
	editor_test::handle_to_end(session, request::undo());

	// A missing font named through a variable: reported once, where brand.mns names it.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "DEF_FONTNAME_LG nofont.fnt\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	size_t missing_fonts = 0;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "reference.missing" && d.message.find("nofont.fnt") != std::string::npos) {
			++missing_fonts;
			TEST_EXPECT(fs::path(d.asset).filename() == "brand.mns" && d.record == "DEF_FONTNAME_LG");
			TEST_EXPECT(editor_test::reference_of(d).kind == ReferenceKind::Font && subject_target(d) == "nofont.fnt");
		}
	TEST_EXPECT(missing_fonts == 1);

	// A line after the place the game stops reading defines nothing it reads.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "EARLY 1\r\nBAD%NAME x\r\nLATE_ONLY 2\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::StyleVar, "%EARLY%") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::StyleVar, "%LATE_ONLY%") == ReferenceStatus::Missing);
	TEST_EXPECT(count_code(view.findings.diagnostics, "style.invalid_name_char") == 1);

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
	editor_test::handle_to_end(session, request::rescan());
	const GraphSymbol *normal = view.findings.graph->style_binding("DEF_FONTNAME");
	TEST_EXPECT(normal && fs::path(normal->file).filename() == "brand.mns" && normal->value == "Arial16n.fnt");
	const auto nowhere_findings = [&view]() {
		size_t n = 0;
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "reference.missing" && d.message.find("Nowhere.fnt") != std::string::npos) ++n;
		return n;
	};
	TEST_EXPECT(nowhere_findings() == 0);
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "// No fonts here.\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(nowhere_findings() == 1);
	return 0;
}

namespace {

const GraphEdge *edge_of(const AssetGraph &graph, const std::string &source, ReferenceKind kind, const std::string &value,
                         const char *field = nullptr) {
	for (const GraphEdge *edge : graph.references_of(source))
		if (edge->kind == kind && edge->value == value && (!field || edge->field == field)) return edge;
	return nullptr;
}

const GraphSymbol *symbol_at(const AssetGraph &graph, const std::string &file, ReferenceKind kind, const std::string &record) {
	for (const GraphSymbol &symbol : all_symbols(graph))
		if (symbol.kind == kind && symbol.file == file && symbol.record == record) return &symbol;
	return nullptr;
}

const Diagnostic *finding(const std::vector<Diagnostic> &diagnostics, const char *code, const std::string &record,
                          const char *needle = "") {
	for (const Diagnostic &d : diagnostics)
		if (d.code() == code && d.record == record && d.message.find(needle) != std::string::npos) return &d;
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Names"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
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
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("graph.mnu"));
	const auto *menu = dynamic_cast<const MnuDocument *>(session.document_for("graph.mnu"));
	TEST_EXPECT(menu && !menu->blocked());
	if (!menu) return 1;
	for (const SourceIssue &issue : menu->issues()) std::printf("  issue: %s %s\n", issue.field.c_str(), issue.message.c_str());
	TEST_EXPECT(menu->issues().empty());
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.findings.graph;
	const std::string path = menu->path();

	// The files, each a Warning while the project lacks it (the game does without it).
	const GraphEdge *bank = edge_of(graph, path, ReferenceKind::SoundBank, "click.lwf", "file");
	TEST_EXPECT(bank && bank->rewritable && bank->record == "HOME/PANEL/GO/Sound 1");
	TEST_EXPECT(finding(view.findings.diagnostics, "reference.missing", "HOME/PANEL/GO/Sound 1", "plays no sound"));
	const GraphEdge *credits = edge_of(graph, path, ReferenceKind::Credits, "credits.kda", "value");
	TEST_EXPECT(credits && finding(view.findings.diagnostics, "reference.missing", credits->record, "shows none of its lines"));
	TEST_EXPECT(edge_of(graph, path, ReferenceKind::MenuTexture, "icon.tga", "file"));
	const GraphEdge *header = edge_of(graph, path, ReferenceKind::TextId, "HEAD_ID", "text");
	TEST_EXPECT(header && header->scope == "GAMETEXT.BIN/menu");
	const GraphEdge *colour = edge_of(graph, path, ReferenceKind::StyleVar, "%TRIM_COLOR%", "value");
	TEST_EXPECT(colour && colour->through == ReferenceKind::None && colour->target == "TRIM_COLOR");
	TEST_EXPECT(graph.referrers_of(ReferenceKind::StyleVar, "TRIM_COLOR").size() == 1);
	TEST_EXPECT(edge_of(graph, path, ReferenceKind::Menu, "graph.mnu") && edge_of(graph, path, ReferenceKind::Menu, "main.mnu"));
	TEST_EXPECT(!edge_of(graph, path, ReferenceKind::Menu, "nofile.mnu")); // POP_SCREEN loads no FILE
	TEST_EXPECT(editor_test::write_bytes(root + "/click.lwf", editor_test::sound_bank_of({"MOUSE_OVER"})) &&
	            editor_test::write_text(root + "/credits.kda", "[TEXT]\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	menu = dynamic_cast<const MnuDocument *>(session.document_for("graph.mnu"));
	TEST_EXPECT(menu);
	if (!menu) return 1;
	TEST_EXPECT(graph.resolve(ReferenceKind::SoundBank, "click.lwf") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::SoundBank, "click") == ReferenceStatus::Missing); // opened by the name as written
	TEST_EXPECT(graph.resolve(ReferenceKind::Credits, "CREDITS.KDA") == ReferenceStatus::Present);
	TEST_EXPECT(!finding(view.findings.diagnostics, "reference.missing", "HOME/PANEL/GO/Sound 1"));
	// The SOUND's trigger is a set of its own bank (the sound lane), found there alone.
	const GraphEdge *trigger = edge_of(graph, path, ReferenceKind::Sound, "MOUSE_OVER", "trigger");
	TEST_EXPECT(trigger && trigger->scope == "CLICK.LWF" && graph.resolve(*trigger) == ReferenceStatus::Present);

	// The screens: the last AWAY is the one found; the earlier one, and its windows, are inert.
	const GraphSymbol *home_screen = symbol_at(graph, path, ReferenceKind::MenuScreen, "HOME");
	TEST_EXPECT(home_screen && !home_screen->inert && home_screen->scope == "GRAPH.MNU" &&
	            home_screen->address == NodeAddress({menu->rows()[0]->id, 0, 0}));
	size_t aways = 0, found_aways = 0;
	for (const GraphSymbol &symbol : all_symbols(graph))
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
	TEST_EXPECT(finding(view.findings.diagnostics, "menu.duplicate_screen", "AWAY"));
	// The windows: the first TITLE is found, the second a duplicate; HIDDEN_KID sits under a
	// window with no NAME; a part is never a symbol.
	size_t titles = 0;
	for (const GraphSymbol &symbol : all_symbols(graph))
		if (symbol.kind == ReferenceKind::MenuWindow && symbol.name == "TITLE" && symbol.file == path) {
			TEST_EXPECT(symbol.scope == "GRAPH.MNU/HOME" && symbol.inert == (titles == 1));
			++titles;
		}
	TEST_EXPECT(titles == 2);
	const GraphSymbol *kid = symbol_at(graph, path, ReferenceKind::MenuWindow, "HOME/PANEL/Window 4/HIDDEN_KID");
	TEST_EXPECT(kid && kid->inert);
	const Diagnostic *twin = finding(view.findings.diagnostics, "menu.duplicate_window", "HOME/PANEL/TITLE", "first window of a name");
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
	const Diagnostic *no_screen = finding(view.findings.diagnostics, "reference.missing", nowhere ? nowhere->record : std::string(),
	                                      "GRAPH.MNU does not have and no other menu of the project has");
	TEST_EXPECT(no_screen && no_screen->severity == DiagnosticSeverity::Warning && no_screen->field == "target");
	// A screen its FILE lacks that another menu has: the game selects over every screen it has
	// loaded, so it is found when that menu was loaded before, which the graph cannot know.
	const GraphEdge *other_home = edge_of(graph, path, ReferenceKind::MenuScreen, "HOME");
	TEST_EXPECT(other_home && other_home->scope == "MAIN.MNU" &&
	            graph.resolve(other_home->kind, other_home->target, other_home->scope) == ReferenceStatus::Unverified);
	TEST_EXPECT(other_home &&
			!finding(view.findings.diagnostics, "reference.missing", other_home->record));
	// The pickers offer what the lookup finds from the ACTION: a SCREEN target the screens of
	// its FILE, a WINDOW target the windows of the acting window's screen it reaches, each name
	// once, where it is defined; a window of the screen no lookup reaches is offered as
	// unreachable, with why; a window of another screen never.
	NodeAddress go_window;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "GO", go_window));
	const auto picks = [&](size_t action) {
		const NodeAddress address = menu_test::child_of(*menu, go_window, "action", action);
		for (const FieldSchema &schema : menu->fields(address.kind))
			if (schema.id == "target")
				return reference_choices(*view.findings.graph, menu->field_on(address, schema));
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
	TEST_EXPECT(has_missing(view.findings.diagnostics, "file", DiagnosticSeverity::Error));
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
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.code() != "reference.missing" || d.field != "target") continue;
		hidden = hidden || d.message.find("'HIDDEN_KID'") != std::string::npos &&
		                           d.message.find("never finds it") != std::string::npos;
		elsewhere = elsewhere || d.message.find("'ELSEWHERE', which no window of screen HOME is named") != std::string::npos;
	}
	TEST_EXPECT(hidden && elsewhere);
	// "Referenced by": the window's own symbol finds the ACTIONs that name it.
	NodeAddress title;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "TITLE", title));
	const GraphSymbol *first_title = symbol_at(graph, path, ReferenceKind::MenuWindow, "HOME/PANEL/TITLE");
	TEST_EXPECT(first_title && first_title->address == title);
	TEST_EXPECT(graph.referrers_of(ReferenceKind::MenuWindow, "title", "GRAPH.MNU/HOME").size() == 3);
	// The ACTIONs the game never runs or ignores.
	TEST_EXPECT(finding(
			view.findings.diagnostics, "menu.action_inert", "HOME/PANEL/Window 4", "no NAME"));
	bool unknown_type = false, no_state = false;
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.code() != "menu.action_inert") continue;
		unknown_type = unknown_type || (d.field == "type" && d.message.find("'JUMP'") != std::string::npos);
		no_state = no_state || (d.field == "state" && d.record == "HOME/PANEL/GO/Action 8");
	}
	TEST_EXPECT(unknown_type && no_state);
	TEST_EXPECT(count_code(view.findings.diagnostics, "menu.action_inert") == 3);
	// style.unused leaves TRIM_COLOR, which the colour names.
	for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(!(d.code() == "style.unused" && d.record == "TRIM_COLOR"));
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Retail"));
	const std::string root = session.view().project.root;
	for (const fs::path &menu : menus) fs::copy_file(menu, fs::path(root) / menu.filename(), ec);
	// The shipped stylesheet, loose beside the menus or in the reference fixture set.
	std::string style = retail::asset_file("menu_style.mns");
	if (style.empty()) style = retail::reference_fixture("mns/menu_style.mns");
	if (!style.empty()) fs::copy_file(style, fs::path(root) / "menu_style.mns", ec);
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.findings.graph;
	std::map<std::string, size_t> edges, missing;
	size_t screen_targets = 0, screen_found = 0, screen_other = 0, window_targets = 0, window_found = 0;
	for (const GraphEdge &edge : all_edges(graph)) {
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
	for (const GraphSymbol &symbol : all_symbols(graph)) {
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
	            count_code(view.findings.diagnostics, "menu.duplicate_screen"), count_code(view.findings.diagnostics, "menu.duplicate_window"),
	            count_code(view.findings.diagnostics, "menu.action_inert"), count_code(view.findings.diagnostics, "style.unused"));
	// The grill's census (B1): 110 WINDOW rows and 7 SCREEN rows, each found.
	TEST_EXPECT(window_targets == 110 && window_found == window_targets);
	TEST_EXPECT(screen_targets == 7 && screen_found + screen_other == screen_targets);
	TEST_EXPECT(count_code(view.findings.diagnostics, "menu.duplicate_screen") == 0);
	return 0;
}

// An item's particle slot names a user point of its graphic model (S10h): found among the
// model's first 16 without case, a name it lacks a warning, a record with no graphic no
// reference at all.
static int test_user_point_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_user_points");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Points"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
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
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &graph = *session.view().findings.graph;
	TEST_EXPECT(graph.resolve(ReferenceKind::UserPoint, "armory", "ARMORY.3DI") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::UserPoint, "Nowhere", "ARMORY.3DI") == ReferenceStatus::Missing);
	TEST_EXPECT(graph.resolve(ReferenceKind::UserPoint, "Ground", "OTHER.3DI") == ReferenceStatus::Missing);
	// The slot's edge is keyed as its point is (the whole name, without case, as the lookup's
	// stricmp compares them): the point's users are that slot's.
	// A point among the model's first 16 is defined in their section, which the slot's lookup reads.
	const std::vector<const GraphSymbol *> armory = graph.symbols_named(ReferenceKind::UserPoint, "Armory");
	TEST_EXPECT(armory.size() == 1 && armory[0]->scope == "ARMORY.3DI/FIRST16");
	const std::vector<const GraphEdge *> armory_users =
	        graph.referrers_of(ReferenceKind::UserPoint, "Armory", "ARMORY.3DI/FIRST16");
	TEST_EXPECT(armory_users.size() == 1 && armory_users[0]->target == "ARMORY" && armory_users[0]->record == "Armory Item");
	// Two points of one name among the first 16 (the lookup sets a bit for each [orig:
	// ItemDef_GetBoneMaskByName @ 0x49ea40]): the slot naming them is one use of the model.
	editor_test::handle_to_end(session, request::open_document("models/armory.3di"));
	Document *model = session.document_for("models/armory.3di");
	NodeAddress ground;
	TEST_EXPECT(model && find_definition(AssetGraph(), *model, "Ground", ground));
	if (!model || !ground.row) return 1;
	EditorRequest rename = request::edit_record(model->path(), Edit());
	rename.edits[0].address = ground;
	rename.edits[0].field = "name";
	rename.edits[0].value = std::string("Armory");
	editor_test::handle_to_end(session, rename);
	TEST_EXPECT(graph.symbols_named(ReferenceKind::UserPoint, "ARMORY").size() == 2);
	size_t slot_uses = 0;
	for (const GraphEdge *edge : graph.usages_of(model->path()))
		slot_uses += edge->kind == ReferenceKind::UserPoint && edge->record == "Armory Item" ? 1 : 0;
	TEST_EXPECT(slot_uses == 1);
	size_t missing_points = 0, stray = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		if (d.code() != "reference.missing") continue;
		if (d.message.find("Nowhere") != std::string::npos && d.severity == DiagnosticSeverity::Warning) ++missing_points;
		if (d.message.find("Anything") != std::string::npos) ++stray;
	}
	TEST_EXPECT(missing_points == 1 && stray == 0);
	return 0;
}

// A symbol is the field that defines it (S12 D2): it carries its record's place, which a
// reload of the file finds again, and find_definition finds the record by the name it
// defines; a stylesheet's earlier definition of a name is a symbol no lookup reads.
static int test_symbol_locators() {
	editor_test::TempProjectDir dir("opennova_asset_graph_locators");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Locators"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::vector<const GraphSymbol *> exit = view.findings.graph->symbols_named(ReferenceKind::MenuWindow, "EXIT");
	TEST_EXPECT(exit.size() == 1 && !exit[0]->locator.empty() && exit[0]->field == "name" && !exit[0]->inert);
	if (exit.empty()) return 1;
	MnuDocument reloaded;
	Diagnostic error;
	TEST_EXPECT(reloaded.load(view.project.root + "/" + exit[0]->file, exit[0]->file, AssetKind::Menu,
	                          view.project.document->target_game, error));
	const NodeAddress at = reloaded.address_at(exit[0]->locator);
	TEST_EXPECT(at.row != 0 && reloaded.record_name(at) == "EXIT");
	NodeAddress found;
	TEST_EXPECT(find_definition(AssetGraph(), reloaded, "exit", found) && found == at);
	TEST_EXPECT(find_definition(AssetGraph(), reloaded, "STARTUP", found) &&
			found.kind == reloaded.kind_from_name("screen") && !found.child);
	// Two definitions of a name in the stylesheet the game reads: the last one is read.
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const std::string style_path = style->relative_path; // the scan is read again below
	const std::string style_file = view.project.root + "/" + style_path;
	TEST_EXPECT(editor_test::write_text(style_file, read_text(style_file) + "\r\nTWICE 1\r\nTWICE 2\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const std::vector<const GraphSymbol *> twice = view.findings.graph->symbols_named(ReferenceKind::StyleVar, "twice");
	TEST_EXPECT(twice.size() == 2 && twice[0]->inert && !twice[1]->inert && twice[1]->value == "2" &&
	            twice[0]->inert_reason.find("defines it again below") != std::string::npos);
	TEST_EXPECT(view.findings.graph->style_binding("TWICE") ==
			(twice.size() == 2 ? twice[1] : nullptr));
	// A query names a variable as the graph keys it; find_definition takes a menu's %NAME% too.
	TEST_EXPECT(view.findings.graph->symbols_named(ReferenceKind::StyleVar, "%twice%").empty());
	editor_test::handle_to_end(session, request::open_document(style_path));
	const Document *sheet_document = session.document_for(style_path);
	NodeAddress by_name, by_variable;
	Value read;
	TEST_EXPECT(sheet_document &&
			find_definition(AssetGraph(), *sheet_document, "twice", by_name) &&
			find_definition(AssetGraph(), *sheet_document, "%TWICE%", by_variable) &&
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
	// a document as a whole (round S23). A colour through a variable goes to the
	// variable alone. The stylesheet's usages are the menus' uses of its variables.
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress main;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "MAIN", main));
	if (!menu) return 1;
	const std::vector<ReferenceTarget> font = targets_of(*menu, main, "font.name", view);
	TEST_EXPECT(font.size() == 2);
	if (font.size() != 2) return 1;
	TEST_EXPECT(font[0].file == style_path && font[0].editable && font[0].field == "name" &&
	            font[0].label.find("DEF_FONTNAME_LG") != std::string::npos);
	TEST_EXPECT(font[1].locator.empty() && font[1].editable && fs::path(font[1].file).extension() == ".fnt");
	TEST_EXPECT(targets_of(*menu, main, "font.default_fg", view).size() == 1);
	const std::vector<const GraphEdge *> style_uses = view.findings.graph->usages_of(style_path);
	TEST_EXPECT(view.findings.graph->referrers_of_file(style_path).empty() && !style_uses.empty());
	TEST_EXPECT(std::all_of(style_uses.begin(), style_uses.end(), [](const GraphEdge *edge) {
		return edge->kind == ReferenceKind::StyleVar && !edge->locator.empty();
	}));
	go_to(session, font[0]);
	const Document *sheet = session.document_for(style_path);
	TEST_EXPECT(sheet && view.documents.active == sheet->path() &&
			view.documents.selection.primary.row != 0 &&
			sheet->record_name(view.documents.selection.primary) == "DEF_FONTNAME_LG" &&
			editor_test::revealed_field(view) == "name");
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
		const bool record = row.resolution == ReferenceResolution::Record;
		TEST_EXPECT(file == (row.file != AssetKind::Unknown));
		TEST_EXPECT(!row.extensions || file);
		TEST_EXPECT(!row.file_names || file);
		TEST_EXPECT(!row.scope_names_file || row.resolution == ReferenceResolution::Symbol);
		TEST_EXPECT(row.defined_in == AssetKind::Unknown || row.names_symbol());
		// A Record reference names its collection, and the graph finds none missing (S13 D8).
		TEST_EXPECT(record == (*row.collection != '\0') && (!row.none || record));
		TEST_EXPECT((row.missing_message != nullptr) == (file || (row.names_symbol() && !record)));
		const bool tolerated = kind == ReferenceKind::Wave || kind == ReferenceKind::StyleVar || kind == ReferenceKind::TextId ||
		                       kind == ReferenceKind::SoundBank || kind == ReferenceKind::Credits ||
		                       kind == ReferenceKind::MenuScreen || kind == ReferenceKind::MenuWindow ||
		                       kind == ReferenceKind::Animation || kind == ReferenceKind::UserPoint ||
		                       kind == ReferenceKind::MissionEntity || kind == ReferenceKind::MissionZone ||
		                       kind == ReferenceKind::TilePlacement || kind == ReferenceKind::DialogBank ||
		                       kind == ReferenceKind::MissionStrings || kind == ReferenceKind::Sound ||
		                       kind == ReferenceKind::SoundProfile || kind == ReferenceKind::Shader ||
		                       kind == ReferenceKind::Particle || kind == ReferenceKind::AnimationKey ||
		                       kind == ReferenceKind::ItemAlias || kind == ReferenceKind::AvatarPart ||
		                       kind == ReferenceKind::Dialog || kind == ReferenceKind::MusicStream || kind == ReferenceKind::ItemName;
		TEST_EXPECT(row.severity_when_missing == (tolerated ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error));
	}
	ReferenceKind kind = ReferenceKind::None;
	TEST_EXPECT(!reference_kind_from_token("game_text", kind) && !reference_kind_from_token("", kind));
	TEST_EXPECT(reference_row(ReferenceKind::Font).file == AssetKind::Font &&
	            reference_row(ReferenceKind::MenuTexture).file == AssetKind::Texture);
	TEST_EXPECT(reference_row(ReferenceKind::StyleVar).resolution == ReferenceResolution::StyleVariable);
	TEST_EXPECT(reference_row(ReferenceKind::Sound).resolution == ReferenceResolution::Symbol &&
	            reference_row(ReferenceKind::MenuText).resolution == ReferenceResolution::Unchecked);
	// A model's register by its index, every whole number from 0 one; its frame row by the pose's
	// rule, a signed byte above 0.
	TEST_EXPECT(reference_row(ReferenceKind::ModelRegister).resolution == ReferenceResolution::Record &&
	            std::string(reference_row(ReferenceKind::ModelRegister).collection) == "register" &&
	            std::string(reference_row(ReferenceKind::ModelFrame).collection) == "frame");
	int64_t index = -1;
	TEST_EXPECT(record_index(ReferenceKind::ModelRegister, Value(int64_t(0)), index) && index == 0 &&
	            record_index(ReferenceKind::ModelRegister, Value(int64_t(300)), index) && index == 300);
	TEST_EXPECT(!record_index(ReferenceKind::ModelRegister, Value(int64_t(-1)), index) &&
	            !record_index(ReferenceKind::ModelRegister, Value(std::string("2")), index) &&
	            !record_index(ReferenceKind::Texture, Value(int64_t(2)), index));
	for (const int64_t byte : {int64_t(0), int64_t(128), int64_t(200), int64_t(255)})
		TEST_EXPECT(!record_index(ReferenceKind::ModelFrame, Value(byte), index));
	TEST_EXPECT(record_index(ReferenceKind::ModelFrame, Value(int64_t(1)), index) && index == 1 &&
	            record_index(ReferenceKind::ModelFrame, Value(int64_t(127)), index) && index == 127);
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
// extension the kind's loader appends; a texture of no model row the files its role's loader opens
// (ADR 0046 S18), or the game's loader for it where only that is given (texture_loader_arg:
// renderer::texture_load_attempts, no alternate name; the particle manager's loose tga\ leg is no
// project file), the name as written alone with neither; a font the .fnt from the name's first dot;
// where the
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
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "wall", -1, none) == Names({"wall"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "wall.png", -1, has({"wall.dds"})) ==
	            Names({"wall.png"}));
	const int32_t sky = texture_loader_arg(renderer::TextureLoader::ArchiveSelfAlpha);
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "sky.tga", sky, has({"sky.tga", "sky.dds"})) ==
	            Names({"sky.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "sky.tga", sky, none) == Names({"sky.tga"}));
	const int32_t particle = texture_loader_arg(renderer::TextureLoader::Particle);
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "fx\\puff.tga", particle, none) == Names({"puff.tga"}));
	const int32_t hud = texture_loader_arg(renderer::TextureLoader::HudAlpha);
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "art.tga.alpha", hud, none) == Names({"art.tga"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "icon.pcx", hud, none) == Names({"icon.pcx"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "icon.bmp", hud, has({"icon.bmp"})).empty());
	// A mission's tile set: its atlas, the name to its first dot plus .TGA, through the TGA reader.
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "jtt01.til", kTileSetTextureArg, none) ==
	            Names({"jtt01.TGA"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "jtt01", kTileSetTextureArg, none) == Names({"jtt01.TGA"}));
	renderer::TextureLoader loader = renderer::TextureLoader::Stage;
	TEST_EXPECT(texture_loader_of(sky, loader) && loader == renderer::TextureLoader::ArchiveSelfAlpha &&
	            !texture_loader_of(-1, loader) && !texture_loader_of(0, loader) &&
	            !texture_loader_of(texture_loader_arg(renderer::TextureLoader::Particle) - 1, loader));
	// One of a role, by its role's loader (ADR 0046 S18): a sky map's archive loader, the .dds beside it
	// first; a colour map's TGA reader, the name alone.
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "sky.pcx", texture_role_arg(renderer::TextureRoleId::SkyCloud),
	                                      has({"sky.dds"})) == Names({"sky.dds"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Texture, "map.tga", texture_role_arg(renderer::TextureRoleId::TerrainColourMap),
	                                      has({"map.dds"})) == Names({"map.tga"}));
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
	TEST_EXPECT(reference_file_candidates(ReferenceKind::SoundBank, "click.lwf", -1, none) == Names({"click.lwf"}));
	// S14: a terrain's height data by the name as written; a sound bank's wave by the file name of
	// the path its single holds, either separator.
	TEST_EXPECT(reference_file_candidates(ReferenceKind::TerrainData, "isle.cpt", -1, none) == Names({"isle.cpt"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Wave, "SFX\\MENU\\click.wav", -1, none) == Names({"click.wav"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Wave, "sfx/menu/click.wav", -1, none) == Names({"click.wav"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Wave, "click.wav", -1, none) == Names({"click.wav"}));
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Wave, "sfx\\", -1, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Weapon, "M16", -1, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Sound, "shot", -1, none).empty());
	TEST_EXPECT(reference_file_candidates(ReferenceKind::Model, "", -1, none).empty());
	return 0;
}

// S14: a terrain and a sound bank through the engine's parsers. A terrain names its height data,
// its maps, its tile atlas and each foliage block's model; a bank the wave of each single, found
// by the file name of the path it holds. A terrain's height data the project lacks is an error
// (the game refuses the terrain), a wave it lacks a warning (nothing plays); a terrain the game's
// gate refuses is a file the graph does not read.
static int test_terrain_and_bank_extractors() {
	editor_test::TempProjectDir dir("opennova_asset_graph_terrain_bank");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Terrain"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/isle.trn",
	                                    "polytrn_colormap isle_c.tga\r\npolytrn_detailmap det.tga\r\npolytrn_polydata isle.cpt\r\n"
	                                    "polytrn_tilestrip tiles.tga\r\npolytrn_charmap isle_m.pcx\r\npolytrn_sectorcount 1\r\n"
	                                    "polytrn_sectors 1\r\nfoliage\r\n  graphic palm\r\nend\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/isle_c.tga", "x") && editor_test::write_text(root + "/shot.wav", "RIFF"));
	{
		opennova::lwf::File bank;
		for (const auto &[name, path] : {std::pair<const char *, const char *>{"SHOT", "SFX\\WEAPON\\shot.wav"}, {"GONE", "gone.wav"}}) {
			opennova::lwf::Single single;
			single.name = name;
			single.path = path;
			bank.singles.push_back(single);
		}
		std::vector<uint8_t> bytes;
		std::string error;
		TEST_EXPECT(opennova::lwf::encode_lwf(bank, bytes, error) && editor_test::write_bytes(root + "/game.lwf", bytes));
	}
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &graph = *session.view().findings.graph;
	const GraphEdge *heights = edge_to(graph, "isle.trn", ReferenceKind::TerrainData, "isle.cpt");
	TEST_EXPECT(heights && heights->field == "polytrn_polydata" && heights->rewritable);
	for (const char *map : {"isle_c.tga", "det.tga", "tiles.tga", "isle_m.pcx"})
		TEST_EXPECT(edge_to(graph, "isle.trn", ReferenceKind::Texture, map));
	// Each map by its role (ADR 0046 S18) and so its game loader: the colour map and the atlas through
	// the TGA reader, a near detail map through the stage loader (its .dds sibling first), the
	// character map by its name.
	const auto loader_of_map = [&](const char *map, renderer::TextureLoader &loader) {
		renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
		const GraphEdge *edge = edge_to(graph, "isle.trn", ReferenceKind::Texture, map);
		if (!edge || !texture_arg_role(edge->loader_arg, role)) return false;
		loader = renderer::texture_role(role).loader;
		return renderer::texture_loader_has_attempts(loader);
	};
	renderer::TextureLoader loader = renderer::TextureLoader::Stage;
	TEST_EXPECT(loader_of_map("isle_c.tga", loader) && loader == renderer::TextureLoader::Tga);
	TEST_EXPECT(loader_of_map("tiles.tga", loader) && loader == renderer::TextureLoader::Tga);
	TEST_EXPECT(loader_of_map("det.tga", loader) && loader == renderer::TextureLoader::Stage);
	TEST_EXPECT(!loader_of_map("isle_m.pcx", loader));
	const GraphEdge *palm = edge_to(graph, "isle.trn", ReferenceKind::Model, "palm");
	TEST_EXPECT(palm && palm->record == "Terrain/Foliage 1" && palm->field == "graphic");
	TEST_EXPECT(graph.resolve(ReferenceKind::TerrainData, "isle.cpt") == ReferenceStatus::Missing &&
	            graph.resolve(ReferenceKind::Texture, "isle_c.tga") == ReferenceStatus::Present);
	const GraphEdge *shot = edge_to(graph, "game.lwf", ReferenceKind::Wave, "SFX\\WEAPON\\shot.wav");
	TEST_EXPECT(shot && shot->record == "SHOT" && shot->field == "file");
	TEST_EXPECT(graph.resolve(ReferenceKind::Wave, "SFX\\WEAPON\\shot.wav") == ReferenceStatus::Present &&
	            graph.resolve(ReferenceKind::Wave, "gone.wav") == ReferenceStatus::Missing &&
	            !graph.referrers_of_file("shot.wav").empty());
	const auto missing = [&session](const char *file, const char *name) {
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == "reference.missing" && d.asset == file && d.message.find(name) != std::string::npos) return &d;
		return static_cast<const Diagnostic *>(nullptr);
	};
	const Diagnostic *no_heights = missing("isle.trn", "isle.cpt"), *no_wave = missing("game.lwf", "gone.wav");
	TEST_EXPECT(no_heights && no_heights->severity == DiagnosticSeverity::Error);
	TEST_EXPECT(no_wave && no_wave->severity == DiagnosticSeverity::Warning &&
	            no_wave->message.find("plays nothing") != std::string::npos && !missing("game.lwf", "shot.wav"));
	TEST_EXPECT(count_code(session.view().findings.diagnostics, "graph.unreadable") == 0);
	// The height data in: the terrain's reference resolves. A terrain with no height data named is one the
	// game refuses: read through its document all the same (DI-30), its refusal the terrain's own finding.
	TEST_EXPECT(editor_test::write_text(root + "/isle.cpt", "x") &&
	            editor_test::write_text(root + "/bare.trn", "polytrn_colormap isle_c.tga\r\npolytrn_detailmap det.tga\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(graph.resolve(ReferenceKind::TerrainData, "isle.cpt") == ReferenceStatus::Present && !missing("isle.trn", "isle.cpt"));
	TEST_EXPECT(count_code(session.view().findings.diagnostics, "graph.unreadable") == 0 &&
	            count_code(session.view().findings.diagnostics, "terrain.refused") == 1 &&
	            graph.references_of("bare.trn").size() == 2);
	return 0;
}

// A model's texture row resolves to the file its type's loader opens (S11f): a diffuse row
// naming wall.tga to the .dds the project has beside no .tga, else to the .tga; a plain row
// (type 1) never to the .dds; a normal map by renderer::material_texture_source; an authored type the
// loader zeroes as a diffuse row. A particle's texture reads as its atlas loads it, the name alone
// (ADR 0046 S18: its role on the edge). The badge, the finding and the edge's JSON read the same
// rule, the row's type carried on the edge.
static int test_model_texture_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_model_textures");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Textures"));
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	// A model minted from scene text, its texture rows of six types (an .mdt normal map and a
	// chunk row among them); a particle file.
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/typed.o3d",
	                                    "o3d 2\nmodel TYPED\nmaterial FF_ST_OP\ntexture wall.tga 1 0\ntexture plain.tga 1 1\n"
	                                    "texture bump.tga 3 5\ntexture trim.tga 1 3\ntexture ready.mdt 3 4\n"
	                                    "texture field.nq8 1 16\nlod 0\npart 0 0 0 0\nmesh 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	const ImportResult imported = import_assets({{scene + "/typed.o3d", {}}}, ProjectPaths::for_root(root), *view.project.document, false);
	TEST_EXPECT(imported.imported == std::vector<std::string>({"models/typed.3di"}));
	TEST_EXPECT(editor_test::write_text(root + "/fx.ptl",
	                                    "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff;\n}\n\n[particledef]\n{\n\tid = puff;\n\tgraphic1 = puff.tga, additive;\n}\n"));
	// The project's textures now exactly `files` (a .nq8 a material chunk container: an 8-byte
	// header, then an NQ8B chunk of a 2 x 2 image).
	std::vector<uint8_t> chunk(8 + 8 + 28 + 2 * 2 * 4, 0);
	chunk[8] = 'N', chunk[9] = 'Q', chunk[10] = '8', chunk[11] = 'B';
	chunk[12] = uint8_t(chunk.size() - 16);
	chunk[28] = 2, chunk[32] = 2;
	const auto textures = [&](std::initializer_list<const char *> files) {
		std::error_code ec;
		fs::remove_all(fs::path(root) / "textures", ec);
		for (const char *file : files)
			if (fs::path(file).extension() == ".nq8")
				editor_test::write_bytes(root + "/textures/" + file, chunk);
			else
				editor_test::write_text(root + "/textures/" + file, "x");
		editor_test::handle_to_end(session, request::rescan());
	};
	const std::string model = "models/typed.3di";
	const auto row_of = [&](const char *value) { return edge_to(*view.findings.graph, model, ReferenceKind::Texture, value); };
	const auto resolved = [&](const char *value, std::string *file = nullptr) {
		const GraphEdge *edge = row_of(value);
		return edge ? view.findings.graph->resolve(*edge, file) : ReferenceStatus::NotAReference;
	};
	textures({"wall.dds", "plain.dds", "bump.dds", "trim.dds"});
	TEST_EXPECT(row_of("wall.tga") && row_of("wall.tga")->loader_arg == 0 && row_of("plain.tga")->loader_arg == 1 &&
	            row_of("bump.tga")->loader_arg == 5 && row_of("trim.tga")->loader_arg == 3);
	std::string file;
	TEST_EXPECT(resolved("wall.tga", &file) == ReferenceStatus::Present && file == "textures/wall.dds");
	TEST_EXPECT(resolved("plain.tga") == ReferenceStatus::Missing);
	TEST_EXPECT(resolved("bump.tga", &file) == ReferenceStatus::Present && file == "textures/bump.dds");
	TEST_EXPECT(resolved("trim.tga", &file) == ReferenceStatus::Present && file == "textures/trim.dds");
	TEST_EXPECT(!view.findings.graph->referrers_of_file("wall.dds").empty());
	// A texture of anything else reads as its game loader opens it: the stage loader takes the
	// .dds sibling; with no loader given, the name as written alone.
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "wall.tga", "", &file,
	                                         texture_loader_arg(opennova::renderer::TextureLoader::Stage)) ==
	                    ReferenceStatus::Present &&
	            file == "textures/wall.dds");
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "wall.tga", "") == ReferenceStatus::Missing);
	// Only the missing row is a finding, and it carries the row's type as its loader's argument
	// (in its JSON too).
	const auto finding_for = [&](const char *target) -> const Diagnostic * {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "reference.missing" && d.asset == model && subject_target(d) == target) return &d;
		return nullptr;
	};
	const Diagnostic *plain = finding_for("plain.tga");
	TEST_EXPECT(plain && editor_test::reference_of(*plain).kind == ReferenceKind::Texture && editor_test::reference_of(*plain).loader_arg == 1);
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
	// so a file no rule types by its name serves it when its chunk headers make it a material
	// chunk (S13 A8: the scan reads them; a file of no kind the game knows the build leaves out,
	// and serves nothing). Neither is a finding once there.
	TEST_EXPECT(resolved("ready.mdt") == ReferenceStatus::Missing && resolved("field.nq8") == ReferenceStatus::Missing);
	textures({"wall.tga", "wall.dds", "plain.tga", "plain.dds", "bump.tga", "bump.dds", "ready.mdt", "field.nq8"});
	TEST_EXPECT(view.project.scan->find("ready.mdt") && view.project.scan->find("ready.mdt")->kind == AssetKind::Texture);
	TEST_EXPECT(view.project.scan->find("field.nq8") && view.project.scan->find("field.nq8")->kind == AssetKind::MaterialChunk);
	TEST_EXPECT(resolved("ready.mdt", &file) == ReferenceStatus::Present && file == "textures/ready.mdt");
	TEST_EXPECT(resolved("field.nq8", &file) == ReferenceStatus::Present && file == "textures/field.nq8");
	TEST_EXPECT(!finding_for("ready.mdt") && !finding_for("field.nq8"));
	for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(!(d.code() == "asset.kind.unknown" && d.asset == "textures/ready.mdt"));
	// No other texture takes a material chunk: a particle naming field.nq8 misses it.
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "field.nq8") == ReferenceStatus::Missing);
	// A file of the name that holds no chunk is of no kind the game knows: it serves no row, and the
	// game opening it reads no chunk, which is its own finding (review F3: reference.wrong_kind), not a
	// name the project lacks; an error that gates where a texture's missing name does, nowhere (the
	// build follows retail, S14).
	TEST_EXPECT(editor_test::write_text(root + "/textures/field.nq8", "x"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.scan->find("field.nq8") && view.project.scan->find("field.nq8")->kind == AssetKind::Unknown);
	TEST_EXPECT(resolved("field.nq8") == ReferenceStatus::Missing && !finding_for("field.nq8"));
	const Diagnostic *wrong_kind = nullptr;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "reference.wrong_kind" && d.asset == model && subject_target(d) == "field.nq8") wrong_kind = &d;
	TEST_EXPECT(wrong_kind && wrong_kind->severity == DiagnosticSeverity::Error && !blocks_build(*wrong_kind) &&
	            wrong_kind->message.find("field.nq8 is ") != std::string::npos);
	textures({"wall.tga", "wall.dds", "plain.tga", "plain.dds", "bump.tga", "bump.dds", "ready.mdt", "field.nq8"});
	// The inspector's badge and Go to, and the edge's JSON, answer the same.
	editor_test::handle_to_end(session, request::open_document(model));
	const Document *document = session.document_for(model);
	const GraphEdge *wall = row_of("wall.tga");
	TEST_EXPECT(document && wall);
	if (!document || !wall) return 1;
	FieldUse name;
	for (const FieldSchema &schema : document->fields(wall->address.kind))
		if (schema.id == "name") name = document->field_on(wall->address, schema);
	TEST_EXPECT(name.reference == ReferenceKind::Texture && name.loader_arg == 0);
	TEST_EXPECT(reference_status(*view.findings.graph, name, std::string("wall.tga")) ==
			ReferenceStatus::Present);
	TEST_EXPECT(reference_target_file(*view.findings.graph, name, std::string("wall.tga")) ==
			"textures/wall.dds");
	const opennova::io::JsonValue json = graph_edge_to_json(*view.findings.graph, *wall);
	TEST_EXPECT(json.get("loader_arg") && json.get("loader_arg")->number == 0.0 && !json.get("material_type") &&
	            json.get_string("status", "") == "present" && json.get_string("file", "") == "textures/wall.dds");
	// A particle's graphic, as the particle manager's TGA loader reads it (ADR 0046 S18, its role on the
	// edge): the name alone, never its stem's .dds, its overlay twin or an extension appended to the
	// whole name [orig: CParticleTextureEntry_ProbeSizeFromDisk @ 0x5DFAA0].
	textures({"puff.tga"});
	const GraphEdge *puff =
			edge_to(*view.findings.graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(puff && puff->loader_arg == texture_role_arg(renderer::TextureRoleId::ParticleGraphic) &&
			view.findings.graph->resolve(*puff, &file) == ReferenceStatus::Present &&
			file == "textures/puff.tga" &&
			graph_edge_to_json(*view.findings.graph, *puff).get_string("texture_role", "") == "particle_graphic");
	for (const char *other : {"puff.dds", "puff_O.tga", "puff.tga.dds"}) {
		textures({other});
		puff = edge_to(*view.findings.graph, "fx.ptl", ReferenceKind::Texture, "puff.tga");
		TEST_EXPECT(puff && view.findings.graph->resolve(*puff) == ReferenceStatus::Missing);
	}
	return 0;
}

// A model material's shader names the tag an effect registers (ReferenceKind::Shader), compared
// without case [orig: HLSLEffect_FindByName @ 0x5ADE70]: with no effect it is a warning saying the game
// draws nothing (no registry entry holds a pass) and refuses no build; _ffp.fx registers the
// fixed-function tags, an object effect its tag and #UV twin, a skinned one no twin. A missing _ffp.fx
// is a required file the project lacks, which Create Missing makes.
static int test_model_shader_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_model_shaders");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Shaders"));
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/lit.o3d",
	                                    "o3d 2\nmodel LIT\nmaterial FF_ST_OP\ntexture wall.tga 1 0\nlod 0\npart 0 0 0 0\n"
	                                    "mesh 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	const ImportResult imported = import_assets({{scene + "/lit.o3d", {}}}, ProjectPaths::for_root(root), *view.project.document, false);
	TEST_EXPECT(imported.imported == std::vector<std::string>({"models/lit.3di"}));
	editor_test::handle_to_end(session, request::rescan());
	const std::string model = "models/lit.3di";
	const auto missing = [&]() -> const Diagnostic * {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "reference.missing" && d.asset == model && subject_target(d) == "FF_ST_OP") return &d;
		return nullptr;
	};
	const auto required = [&]() {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "requirement.missing" && d.message.find("_ffp.fx") != std::string::npos) return true;
		return false;
	};
	const GraphEdge *edge = edge_to(*view.findings.graph, model, ReferenceKind::Shader, "FF_ST_OP");
	TEST_EXPECT(edge && view.findings.graph->resolve(*edge) == ReferenceStatus::Missing);
	const Diagnostic *none = missing();
	TEST_EXPECT(none && none->severity == DiagnosticSeverity::Warning && !blocks_build(*none) &&
	            none->message.find("draws nothing") != std::string::npos);
	TEST_EXPECT(required());
	// The editor's _ffp.fx: the fixed-function tags and their twins, any case.
	const auto make = [&](const char *name, const char *tag) {
		BlankRequest request;
		request.logical_name = name;
		if (tag) request.values = {{"tag", tag}};
		std::vector<uint8_t> bytes;
		Diagnostic error;
		return make_blank(request, AssetKind::Shader, bytes, error) &&
		       editor_test::write_bytes(root + "/shaders/" + name, bytes);
	};
	TEST_EXPECT(make("_ffp.fx", nullptr));
	editor_test::handle_to_end(session, request::rescan());
	edge = edge_to(*view.findings.graph, model, ReferenceKind::Shader, "FF_ST_OP");
	std::string file;
	TEST_EXPECT(edge && view.findings.graph->resolve(*edge, &file) == ReferenceStatus::Present && !missing() && !required());
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(graph.resolve(ReferenceKind::Shader, "ff_mt_ab_lum#uv") == ReferenceStatus::Present &&
	            graph.resolve(ReferenceKind::Shader, "VS_PHONGT") == ReferenceStatus::Missing);
	// An object effect registers its tag and its #UV twin; a skinned one no twin.
	TEST_EXPECT(make("onphongt.fx", "VS_PHONGT") && make("onskphgt.fx", "VS_SKBUMPPHONGT"));
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &after = *view.findings.graph;
	TEST_EXPECT(after.resolve(ReferenceKind::Shader, "VS_PHONGT") == ReferenceStatus::Present &&
	            after.resolve(ReferenceKind::Shader, "vs_phongt#UV") == ReferenceStatus::Present &&
	            after.resolve(ReferenceKind::Shader, "VS_SKBUMPPHONGT") == ReferenceStatus::Present &&
	            after.resolve(ReferenceKind::Shader, "VS_SKBUMPPHONGT#UV") == ReferenceStatus::Missing);
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Spelling"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/relief.o3d",
	                                    "o3d 2\nmodel RELIEF\nmaterial FF_ST_OP\ntexture bump.tga 3 4\ntexture bump.tga 3 5\n"
	                                    "texture bump.tga 3 6\ntexture bump.tga 3 7\nlod 0\npart 0 0 0 0\nmesh 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	TEST_EXPECT(import_assets({{scene + "/relief.o3d", {}}}, paths, *view.project.document, false).imported ==
	            std::vector<std::string>({"models/relief.3di"}));
	TEST_EXPECT(editor_test::write_text(root + "/textures/bump.dds", "dds"));
	editor_test::handle_to_end(session, request::rescan());
	const RenamePlan plan = plan_rename(paths, *view.project.scan, *view.findings.graph, "textures/bump.dds", "stone.dds");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 4);
	for (const RenameSite &site : plan.sites) TEST_EXPECT(site.before == "bump.tga" && site.after == "stone.tga");
	editor_test::handle_to_end(session, request::rename_asset("textures/bump.dds", "stone.dds"));
	TEST_EXPECT(session.outcome().done() && view.project.scan->find("stone.dds") && !view.project.scan->find("bump.dds"));
	size_t rows = 0;
	for (const GraphEdge *edge : view.findings.graph->references_of("models/relief.3di")) {
		if (edge->kind != ReferenceKind::Texture) continue;
		++rows;
		std::string file;
		TEST_EXPECT(edge->value == "stone.tga" && view.findings.graph->resolve(*edge, &file) == ReferenceStatus::Present &&
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
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	if (!menu) return 1;
	EditorRequest set =
			request::edit_record(menu->path(), menu_test::image_edits(*menu, exit, "logo.tga"));
	editor_test::handle_to_end(session, set);
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(editor_test::write_text(root + "/textures/logo.dds", "dds") && editor_test::write_text(root + "/textures/shine.tga", "tga"));
	editor_test::handle_to_end(session, request::rescan());
	const RenamePlan kept = plan_rename(paths, *view.project.scan, *view.findings.graph, "textures/logo.dds", "glow.dds");
	TEST_EXPECT(kept.ok() && kept.sites.size() == 1 && kept.sites[0].after == "glow.tga");
	const RenamePlan taken = plan_rename(paths, *view.project.scan, *view.findings.graph, "textures/logo.dds", "shine.dds");
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
	TEST_EXPECT(assembled != fresh && !all_edges(graph).empty() && !all_symbols(graph).empty());
	const GraphEdge *edge = &all_edges(graph).front().get();
	const GraphSymbol *symbol = &all_symbols(graph).front().get();
	const auto kept = [&graph, assembled, edge, symbol] {
		return graph.generation() == assembled && &all_edges(graph).front().get() == edge &&
				&all_symbols(graph).front().get() == symbol;
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
	// A file the graph does not read (a mission text, S14): its row counts, so one added
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
	TEST_EXPECT(copy.edge_count() == graph.edge_count());
	graph.clear();
	TEST_EXPECT(seen.insert(graph.generation()).second);
	TEST_EXPECT(graph.edge_count() == 0 && graph.symbol_count() == 0);
	another.clear();
	TEST_EXPECT(seen.insert(another.generation()).second);
	// Emptied, the graph reads the files again as a new one would.
	graph.update(paths, doc, scan, {});
	TEST_EXPECT(seen.insert(graph.generation()).second);
	TEST_EXPECT(graph.symbols_of_kind(ReferenceKind::Item).size() == 2);
	return 0;
}

namespace {

// --- S13 D3: the incremental graph and the base layer ---------------------------------------

bool same_edge(const GraphEdge &a, const GraphEdge &b) {
	return a.source == b.source && a.record == b.record && a.locator == b.locator &&
			a.address == b.address && a.field == b.field && a.kind == b.kind &&
			a.value == b.value && a.target == b.target && a.scope == b.scope &&
			a.rewritable == b.rewritable && a.through == b.through && a.loader_arg == b.loader_arg &&
			a.span.line == b.span.line && a.span.column == b.span.column &&
			a.span.length == b.span.length && a.fallback == b.fallback;
}

bool same_symbol(const GraphSymbol &a, const GraphSymbol &b) {
	return a.kind == b.kind && a.name == b.name && a.display == b.display && a.value == b.value &&
			a.file == b.file && a.record == b.record && a.locator == b.locator &&
			a.address == b.address && a.field == b.field && a.scope == b.scope &&
			a.inert == b.inert && a.inert_reason == b.inert_reason && a.line == b.line;
}

bool same_edges(const std::vector<const GraphEdge *> &a, const std::vector<const GraphEdge *> &b) {
	return a.size() == b.size() &&
			std::equal(a.begin(), a.end(), b.begin(),
					[](const GraphEdge *x, const GraphEdge *y) { return same_edge(*x, *y); });
}

bool same_symbols(
		const std::vector<const GraphSymbol *> &a, const std::vector<const GraphSymbol *> &b) {
	return a.size() == b.size() &&
			std::equal(a.begin(), a.end(), b.begin(),
					[](const GraphSymbol *x, const GraphSymbol *y) { return same_symbol(*x, *y); });
}

bool same_choices(const std::vector<ReferenceChoice> &a, const std::vector<ReferenceChoice> &b) {
	return a.size() == b.size() &&
			std::equal(a.begin(), a.end(), b.begin(),
					[](const ReferenceChoice &x, const ReferenceChoice &y) {
						return x.name == y.name && x.kind == y.kind && x.file == y.file &&
								x.record == y.record && x.status == y.status &&
								x.inert == y.inert && x.reason == y.reason;
					});
}

// Where a symbol lookup lands: its file, place and field ("" for none).
std::string place_of(const GraphSymbol *symbol) {
	return symbol ? symbol->file + "#" + symbol->locator + "#" + symbol->field : std::string();
}

std::vector<const GraphEdge *> edges_in(const AssetGraph &graph) {
	std::vector<const GraphEdge *> out;
	graph.for_each_edge([&out](const GraphEdge &edge) { out.push_back(&edge); });
	return out;
}

std::vector<const GraphSymbol *> symbols_in(const AssetGraph &graph) {
	std::vector<const GraphSymbol *> out;
	graph.for_each_symbol([&out](const GraphSymbol &symbol) { out.push_back(&symbol); });
	return out;
}

// What a run of updates has met, which every later comparison asks both graphs about again, so an
// index entry left under a name, a file or a variable nothing holds any more shows: each edge's
// kind, target and scope and each symbol's kind, name and scope (the keys of the index's lists, a
// Record kind's in its file), each file's path, each style variable an edge's value names and each
// (kind, scope, loader argument) a reference was made with; and the texts to search for.
struct Seen {
	std::set<std::tuple<ReferenceKind, std::string, std::string>> names;
	std::set<std::string> paths;
	std::set<std::string> variables;
	std::set<std::tuple<ReferenceKind, std::string, int32_t>> lookups;
	std::vector<std::string> searches;

	explicit Seen(std::vector<std::string> texts) : searches(std::move(texts)) {}
	void add(const AssetGraph &graph, const AssetScan &scan) {
		graph.for_each_edge([this](const GraphEdge &edge) {
			names.insert({edge.kind, edge.target, edge.scope});
			// An edge with a fallback (S13 D9) is indexed under both its names (edges_naming).
			if (!edge.fallback.empty())
				for (const std::string *name : {&edge.value, &edge.fallback})
					names.insert({edge.kind, graph_names::symbol_name(edge.kind, *name), edge.scope});
			if (graph_names::is_style_reference(edge.value))
				variables.insert(graph_names::style_variable(edge.value));
			lookups.insert({edge.kind, edge.scope, edge.loader_arg});
		});
		graph.for_each_symbol([this](const GraphSymbol &symbol) {
			names.insert({symbol.kind, symbol.name, symbol.scope});
		});
		for (const AssetEntry &entry : scan.entries) paths.insert(entry.relative_path);
	}
};

std::vector<const GraphEdge *> edges_at(
		const GraphIndex &index, const std::vector<GraphIndex::Ref> &refs) {
	std::vector<const GraphEdge *> out;
	for (const GraphIndex::Ref ref : refs) out.push_back(&index.edge(ref));
	return out;
}

std::vector<const GraphSymbol *> symbols_at(
		const GraphIndex &index, const std::vector<GraphIndex::Ref> &refs) {
	std::vector<const GraphSymbol *> out;
	for (const GraphIndex::Ref ref : refs) out.push_back(&index.symbol(ref));
	return out;
}

bool same_hits(const std::vector<GraphSearchHit> &a, const std::vector<GraphSearchHit> &b) {
	return a.size() == b.size() &&
			std::equal(a.begin(), a.end(), b.begin(),
					[](const GraphSearchHit &x, const GraphSearchHit &y) {
						return x.name == y.name && x.file == y.file && x.usages == y.usages &&
								(x.symbol && y.symbol ? same_symbol(*x.symbol, *y.symbol)
													  : !x.symbol && !y.symbol);
					});
}

// Everything a graph holds and answers against a graph built fresh over the same files: every
// edge (its target) and every symbol (a style variable's inert as the game reads it) in order,
// the counts, the findings and the missing edges; each edge's resolution; each file's users,
// usages and references; each symbol's users, lookup, binding, and the symbols of its name, record
// and place; each kind's symbols and choices. Then, over everything `seen` met in the run (this
// step's included), the index's own lists: the edges into each name and the symbols of it, each
// file's users, the edges naming each style variable; the choices each reference's scope and
// loader argument get; and the searches. The first difference, "" for none.
std::string difference(
		const AssetGraph &graph, const AssetGraph &fresh, const AssetScan &scan, Seen &seen) {
	const std::vector<const GraphEdge *> edges = edges_in(graph), fresh_edges = edges_in(fresh);
	if (!same_edges(edges, fresh_edges)) return "the edges";
	const std::vector<const GraphSymbol *> symbols = symbols_in(graph),
										   fresh_symbols = symbols_in(fresh);
	if (!same_symbols(symbols, fresh_symbols)) return "the symbols";
	if (graph.edge_count() != edges.size() || graph.symbol_count() != symbols.size())
		return "the counts";
	if (graph.diagnostics() != fresh.diagnostics()) return "the findings";
	if (!same_edges(graph.missing(), fresh.missing()) ||
			graph.missing_count() != graph.missing().size())
		return "the missing edges";
	for (size_t i = 0; i < edges.size(); ++i) {
		std::string file, fresh_file;
		if (graph.resolve(*edges[i], &file) != fresh.resolve(*fresh_edges[i], &fresh_file) ||
				file != fresh_file)
			return "the resolution of " + edges[i]->source + " " + edges[i]->field;
	}
	for (const AssetEntry &entry : scan.entries) {
		const std::string &path = entry.relative_path;
		if (!same_edges(graph.referrers_of_file(path), fresh.referrers_of_file(path)))
			return "the users of " + path;
		if (!same_edges(graph.usages_of(path), fresh.usages_of(path)))
			return "the usages of " + path;
		if (!same_edges(graph.references_of(path), fresh.references_of(path)))
			return "the references of " + path;
	}
	for (size_t i = 0; i < symbols.size(); ++i) {
		const GraphSymbol &symbol = *symbols[i];
		if (!same_edges(graph.users_of(symbol), fresh.users_of(*fresh_symbols[i])))
			return "the users of " + symbol.display;
		if (place_of(graph.resolve_symbol(symbol.kind, symbol.display, symbol.scope)) !=
		    place_of(fresh.resolve_symbol(symbol.kind, symbol.display, symbol.scope)))
			return "the lookup of " + symbol.display;
		if (symbol.kind == ReferenceKind::StyleVar &&
				place_of(graph.style_binding(symbol.name)) !=
						place_of(fresh.style_binding(symbol.name)))
			return "the binding of " + symbol.name;
		if (!same_symbols(graph.symbols_named(symbol.kind, symbol.name, symbol.scope),
					fresh.symbols_named(symbol.kind, symbol.name, symbol.scope)) ||
				!same_symbols(graph.symbols_of(symbol.file, symbol.record),
						fresh.symbols_of(symbol.file, symbol.record)) ||
				place_of(graph.symbol_at(symbol.file, symbol.locator, symbol.field)) !=
						place_of(fresh.symbol_at(symbol.file, symbol.locator, symbol.field)))
			return "the definitions of " + symbol.display;
	}
	for (size_t k = 0; k < kReferenceKindCount; ++k) {
		const ReferenceKind kind = static_cast<ReferenceKind>(k);
		if (!same_symbols(graph.symbols_of_kind(kind), fresh.symbols_of_kind(kind)))
			return std::string("the symbols of the kind ") + reference_row(kind).token;
		if (!same_choices(graph.choices(kind), fresh.choices(kind)))
			return std::string("the choices of ") + reference_row(kind).token;
	}
	seen.add(graph, scan);
	seen.add(fresh, scan);
	const GraphIndex &index = graph.index(), &fresh_index = fresh.index();
	for (const auto &name : seen.names) {
		const ReferenceKind kind = std::get<0>(name);
		const std::string &target = std::get<1>(name);
		// A Record kind's name is its index in the file its scope names; any other kind's is asked
		// as before, everywhere.
		const std::string scope =
				reference_row(kind).resolution == ReferenceResolution::Record ? std::get<2>(name) : std::string();
		const std::string key = GraphIndex::key_of(kind, target, scope);
		if (!same_edges(edges_at(index, index.edges_targeting(key)),
					edges_at(fresh_index, fresh_index.edges_targeting(key))) ||
				!same_edges(graph.referrers_of(kind, target, scope), fresh.referrers_of(kind, target, scope)))
			return "the edges into " + target;
		if (!same_symbols(symbols_at(index, index.symbols_named(key)),
					symbols_at(fresh_index, fresh_index.symbols_named(key))))
			return "the symbols named " + target;
		if (!same_edges(edges_at(index, index.edges_naming(key)),
					edges_at(fresh_index, fresh_index.edges_naming(key))) ||
				!same_edges(graph.edges_naming(kind, target), fresh.edges_naming(kind, target)))
			return "the edges naming " + target + " as one of their two names";
	}
	for (const std::string &path : seen.paths) {
		if (!same_edges(edges_at(index, index.users_of(path)),
					edges_at(fresh_index, fresh_index.users_of(path))) ||
				!same_edges(graph.referrers_of_file(path), fresh.referrers_of_file(path)) ||
				!same_edges(graph.usages_of(path), fresh.usages_of(path)) ||
				!same_edges(graph.references_of(path), fresh.references_of(path)))
			return "the users of " + path + ", a file seen before";
	}
	for (const std::string &variable : seen.variables)
		if (!same_edges(edges_at(index, index.edges_through(variable)),
					edges_at(fresh_index, fresh_index.edges_through(variable))))
			return "the edges naming %" + variable + "%";
	for (const auto &lookup : seen.lookups) {
		const ReferenceKind kind = std::get<0>(lookup);
		const std::string &scope = std::get<1>(lookup);
		const int32_t loader_arg = std::get<2>(lookup);
		if (!same_choices(graph.choices(kind, scope, loader_arg),
					fresh.choices(kind, scope, loader_arg)))
			return std::string("the choices of ") + reference_row(kind).token + " in " + scope;
	}
	for (const std::string &text : seen.searches)
		if (!same_hits(graph.search(text), fresh.search(text))) return "the search for " + text;
	return std::string();
}

// A graph built fresh over the files (with the same base layer), against `graph`.
std::string fresh_difference(const AssetGraph &graph, const ProjectPaths &paths,
		const ProjectDocument &project, const AssetScan &scan,
		const std::vector<std::shared_ptr<const DocumentBase>> &open, Seen &seen,
		const std::shared_ptr<const GraphLayer> &base = nullptr) {
	AssetGraph fresh;
	if (base) fresh.set_base(base);
	fresh.update(paths, project, scan, open);
	return difference(graph, fresh, scan, seen);
}

// A file written, its last write moved on (so a scan sees it changed whatever the clock's tick).
bool rewrite(const std::string &path, const std::string &text) {
	if (!editor_test::write_text(path, text)) return false;
	std::error_code ec;
	static int bump = 0;
	fs::last_write_time(
			path, fs::last_write_time(path, ec) + std::chrono::seconds(10 + ++bump), ec);
	return !ec;
}

// An IMAGE row of a texture, a FONT of a name, an ACTION that selects a screen of a menu file.
std::string image(const std::string &texture) {
	return "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">" + texture + "</APPEARANCE>\r\n";
}
std::string font(const std::string &name) { return "<FONT><NAME>" + name + "</NAME></FONT>\r\n"; }
std::string go_screen(const std::string &file, const std::string &screen_name) {
	return "<ACTION TYPE=\"SCREEN\" FILE=\"" + file + "\">" + screen_name + "</ACTION>\r\n";
}

// A project of its own (no session), its files written by hand.
struct Project {
	std::string root;
	ProjectPaths paths;
	ProjectDocument document;
	AssetScan scan;
	bool made = false;
	explicit Project(const std::string &dir) : root(dir), paths(ProjectPaths::for_root(dir)) {
		Diagnostic error;
		made = create_project(root, "Graph", "jo", document, error);
	}
	std::string file(const std::string &relative) const { return root + "/" + relative; }
	const AssetScan &rescan() {
		scan = scan_project_assets(paths, document);
		return scan;
	}
};

size_t count_edges(const AssetGraph &graph, const std::function<bool(const GraphEdge &)> &which) {
	size_t n = 0;
	graph.for_each_edge([&](const GraphEdge &edge) { n += which(edge); });
	return n;
}

} // namespace

// An update patches the slots whose reading changed and equals a graph built fresh over the
// same files after every step (S13 D3): a closed file's reference edited, taken away and given
// back; an ammo renamed while other files name both its old and its new name; a string id added
// to an open table and moved to another section; a stylesheet the game reads after
// menu_style.mns added, its value changed, a variable of it holding a colour named as a font and
// then become a font, a variable no stylesheet the game reads defines given a definition in one
// it never reads, and the stylesheet taken away; a texture a menu names added; a menu moved to
// another folder and its name's case changed; a second file of a menu's name and the first one
// gone; a string table gone; an open document's window renamed while an ACTION names each name,
// undone, its case changed, and the document closed; a table whose bytes make it another kind; a
// mission that reads, then does not, then is gone; and a mission's .mis added and changed. The
// generation moves exactly when the graph changed.
static int test_incremental_equals_fresh() {
	editor_test::TempProjectDir dir("opennova_asset_graph_incremental");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Incremental"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const ProjectDocument project = *session.view().project.document;
	editor_test::handle_to_end(session, request::close_project());
	AssetScan scan = scan_project_assets(paths, project);
	const AssetEntry *style = scan.find("menu_style.mns");
	const AssetEntry *items = scan.find("items.def");
	const AssetEntry *menu = scan.find("main.mnu");
	const AssetEntry *weapons = scan.find("weapon.def");
	TEST_EXPECT(style && items && menu && weapons);
	if (!style || !items || !menu || !weapons) return 1;
	const std::string style_dir =
			fs::path(root + "/" + style->relative_path).parent_path().generic_string();
	// Each place copied: every step scans again, and the scan's entries with it.
	const std::string items_path = items->relative_path;
	const std::string items_file = root + "/" + items_path;
	const std::string menu_path = menu->relative_path;
	const std::string weapons_file = root + "/" + weapons->relative_path;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	AssetGraph graph;
	uint64_t generation = graph.generation();
	GraphUpdate update;
	Seen seen({"a", "e", "main", "font", "d3", "wep", "other", "ammo"});
	// The graph brought to the files: true when it equals a fresh one and its generation moved
	// exactly when `changes`; the update it made in `update`.
	const auto step = [&](const char *what, bool changes) {
		scan = scan_project_assets(paths, project);
		update = graph.update(paths, project, scan, open);
		const std::string different = fresh_difference(graph, paths, project, scan, open, seen);
		const bool moved = graph.generation() != generation;
		generation = graph.generation();
		if (different.empty() && update.changed == changes && moved == changes) return true;
		std::printf("  FAIL after %s: %s%s\n", what,
				different.empty() ? "" : ("it differs in " + different + "; ").c_str(),
				update.changed == changes && moved == changes
						? ""
						: "it changed where it should not, or the other way");
		return false;
	};
	TEST_EXPECT(step("the first update", true));
	TEST_EXPECT(graph.stats().files_patched == scan.entries.size() &&
			graph.stats().edges_resolved == graph.edge_count() &&
			graph.stats().findings_made == graph.missing_count());
	TEST_EXPECT(step("an update over the same files", false));
	TEST_EXPECT(graph.stats().files_patched == 0 && graph.stats().edges_resolved == 0 &&
			graph.stats().findings_made == 0);
	// A closed file's reference edited: a model the project lacks, then an item added naming it.
	std::string items_text;
	std::string problem;
	TEST_EXPECT(opennova::io::read_file_text(items_file, items_text, problem));
	TEST_EXPECT(rewrite(items_file,
			items_text + "\nbegin \"D3\"\nid 100399\ntype building\ngraphic d3_model\nend\n"));
	TEST_EXPECT(step("an item added", true));
	TEST_EXPECT(update.files == std::vector<std::string>{items_path} && !update.file_set);
	TEST_EXPECT(rewrite(items_file,
			items_text + "\nbegin \"D3\"\nid 100399\ntype building\ngraphic other\nend\n"));
	TEST_EXPECT(step("its graphic renamed", true));
	// The same bytes again: read again, and the graph as it was.
	TEST_EXPECT(rewrite(items_file,
			items_text + "\nbegin \"D3\"\nid 100399\ntype building\ngraphic other\nend\n"));
	TEST_EXPECT(step("the same bytes written again", false));
	TEST_EXPECT(graph.stats().files_extracted == 1 && graph.stats().files_patched == 0);
	// Its graphic taken away (the file's one finding gone, nothing else moving), then given back.
	TEST_EXPECT(
			rewrite(items_file, items_text + "\nbegin \"D3\"\nid 100399\ntype building\nend\n"));
	TEST_EXPECT(step("its graphic taken away", true));
	TEST_EXPECT(graph.stats().findings_made == 0);
	TEST_EXPECT(rewrite(items_file,
			items_text + "\nbegin \"D3\"\nid 100399\ntype building\ngraphic other\nend\n"));
	TEST_EXPECT(step("its graphic given back", true));
	TEST_EXPECT(graph.stats().findings_made == 1);
	// A name renamed while other files name both the old and the new one: two weapons' rounds, an
	// ammo the first names renamed to the name the second names; the weapons' labels, a string id
	// the table defines nowhere and one it will.
	std::string weapons_text;
	TEST_EXPECT(opennova::io::read_file_text(weapons_file, weapons_text, problem));
	TEST_EXPECT(rewrite(weapons_file,
			weapons_text +
					"\nweapon \"D3_A\"\nround_type AMMO_OLD\nloadout_menu_textid WEP_D3\nend\n"
					"weapon \"D3_B\"\nround_type AMMO_NEW\n"
					"loadout_menu_textid WEP_NONE_D3\nend\n"));
	TEST_EXPECT(rewrite(root + "/defs/ammo.def", "ammo AMMO_OLD\nend\n"));
	TEST_EXPECT(step("two weapons naming an ammo and a name none has", true));
	TEST_EXPECT(rewrite(root + "/defs/ammo.def", "ammo AMMO_NEW\nend\n"));
	TEST_EXPECT(step("the ammo renamed to the name the second weapon names", true));
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "AMMO_NEW") == ReferenceStatus::Present &&
			graph.resolve(ReferenceKind::Ammo, "AMMO_OLD") == ReferenceStatus::Missing);
	// A script naming an ammo directly and one through its fallback (S13 D9: spans and fallbacks);
	// the fallback's ammo defined, then renamed to the script's first name, which reaches it directly;
	// the script's span made longer; the script open and its text edited (standing in for its file),
	// then closed.
	const std::string script_file = root + "/scripts/d3.wac";
	TEST_EXPECT(rewrite(script_file,
			"If true(bluekills) then\r\n\tammoarea AMMO_NEW 1\r\n\tammo2tgt(ammo_D3, 2)\r\nendif\r\n"));
	TEST_EXPECT(step("a script naming two ammo, one through its fallback", true));
	TEST_EXPECT(rewrite(root + "/defs/ammo.def", "ammo AMMO_NEW\nend\nammo ammo_D3\nend\n"));
	TEST_EXPECT(step("the ammo its fallback names defined", true));
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "ammo_D3") == ReferenceStatus::Present);
	TEST_EXPECT(rewrite(root + "/defs/ammo.def", "ammo AMMO_NEW\nend\nammo D3\nend\n"));
	TEST_EXPECT(step("the ammo renamed to the script's first name", true));
	TEST_EXPECT(rewrite(script_file,
			"If true(bluekills) then\r\n\tammoarea AMMO_NEW 1\r\n\tammo2tgt(ammo_D3_LONGER, 2)\r\nendif\r\n"));
	TEST_EXPECT(step("the script's span made longer", true));
	{
		std::shared_ptr<DocumentBase> script = document_type_for(AssetKind::Script)->make();
		Diagnostic error;
		TEST_EXPECT(script->load(script_file, "scripts/d3.wac", AssetKind::Script, project.target_game, error));
		TextSpan longer;
		longer.line = 3;
		longer.column = 16;
		longer.length = 9;
		TEST_EXPECT(script->apply(TextDocument::replace(longer, "D3"), error));
		open = {script};
		TEST_EXPECT(step("the open script's span renamed back", true));
		open.clear();
		TEST_EXPECT(step("the script closed", true));
	}
	// A string id added to the open table's WepDes, which the first weapon's label reads, then
	// moved to another section; the table closed (the file on disk has neither).
	if (const AssetEntry *table = scan.find("gametext.bin")) {
		const std::string table_path = table->relative_path;
		auto strings = std::make_shared<StringsDocument>();
		Diagnostic error;
		TEST_EXPECT(strings->load(root + "/" + table_path, table_path, AssetKind::Strings,
				project.target_game, error));
		const NodeKind string_kind = strings->kind_from_name("string");
		NodeId wepdes = 0, overlays = 0;
		for (const auto &row : strings->rows()) {
			if (row->name() == "WepDes") wepdes = row->id;
			if (row->name() == "Overlays") overlays = row->id;
		}
		TEST_EXPECT(wepdes != 0 && overlays != 0);
		// A string of key WEP_D3 added to a section: its address, or none.
		const auto add_key = [&](NodeId section) {
			Edit add;
			add.operation = EditOperation::Add;
			add.address = {section, string_kind, 0};
			if (!strings->apply(add, error)) return NodeAddress{};
			Edit set;
			set.operation = EditOperation::Set;
			set.address = {section, string_kind, strings->last_added()};
			set.field = "key";
			set.value = std::string("WEP_D3");
			return strings->apply(set, error) ? set.address : NodeAddress{};
		};
		const NodeAddress added = add_key(wepdes);
		TEST_EXPECT(added.child != 0);
		open = {strings};
		TEST_EXPECT(step("a string id the weapon names added to the open table", true));
		TEST_EXPECT(graph.resolve(ReferenceKind::TextId, "WEP_D3", "GAMETEXT.BIN/WepDes") ==
				ReferenceStatus::Present);
		Edit remove;
		remove.operation = EditOperation::Remove;
		remove.address = added;
		TEST_EXPECT(strings->apply(remove, error) && add_key(overlays).child != 0);
		TEST_EXPECT(step("the string id moved to another section", true));
		TEST_EXPECT(graph.resolve(ReferenceKind::TextId, "WEP_D3", "GAMETEXT.BIN/WepDes") ==
				ReferenceStatus::Missing);
		open.clear();
		TEST_EXPECT(step("the table closed", true));
	}
	// brand.mns over menu_style.mns: a later definition wins; a variable of its own.
	TEST_EXPECT(
			rewrite(style_dir + "/brand.mns", "DEF_FONTNAME_LG Arial16n.fnt\r\nBRAND_ONLY 1\r\n"));
	TEST_EXPECT(step("brand.mns added", true));
	TEST_EXPECT(update.file_set &&
			std::find(update.bindings.begin(), update.bindings.end(), "DEF_FONTNAME_LG") !=
					update.bindings.end());
	TEST_EXPECT(
			rewrite(style_dir + "/brand.mns", "DEF_FONTNAME_LG nofont.fnt\r\nBRAND_ONLY 1\r\n"));
	TEST_EXPECT(step("its font a file the project lacks", true));
	TEST_EXPECT(update.bindings == std::vector<std::string>{"DEF_FONTNAME_LG"} && !update.file_set);
	TEST_EXPECT(rewrite(style_dir + "/other.mns", "DEF_FONTNAME_LG Arial16b.fnt\r\nSTRAY 2\r\n"));
	TEST_EXPECT(step("a stylesheet the game never reads", true));
	// A menu naming as its font a variable that holds a colour, and a variable no stylesheet
	// defines; the colour then a font the project lacks, which the stylesheet's own edge reports
	// in the menu's place.
	TEST_EXPECT(rewrite(root + "/d3.mnu",
			screen("D3",
					window("STATIC", "COLOURED", font("%D3_VAR%")) +
							window("STATIC", "UNDEFINED", font("%NOPE_D3%")))));
	TEST_EXPECT(rewrite(style_dir + "/brand.mns",
			"DEF_FONTNAME_LG nofont.fnt\r\nBRAND_ONLY 1\r\nD3_VAR FF00FF00\r\n"));
	TEST_EXPECT(step("a variable holding a colour named as a font", true));
	TEST_EXPECT(rewrite(style_dir + "/brand.mns",
			"DEF_FONTNAME_LG nofont.fnt\r\nBRAND_ONLY 1\r\nD3_VAR d3font.fnt\r\n"));
	TEST_EXPECT(step("the colour become a font", true));
	TEST_EXPECT(update.bindings == std::vector<std::string>{"D3_VAR"});
	// The undefined variable defined where the game never reads it: no binding moves, and only its
	// finding's words change.
	TEST_EXPECT(rewrite(
			style_dir + "/other.mns", "DEF_FONTNAME_LG Arial16b.fnt\r\nSTRAY 2\r\nNOPE_D3 1\r\n"));
	TEST_EXPECT(step("the undefined variable defined in a stylesheet the game never reads", true));
	TEST_EXPECT(update.bindings.empty() && graph.stats().findings_made == 1);
	fs::remove(style_dir + "/brand.mns");
	TEST_EXPECT(step("brand.mns gone", true));
	// A menu naming a texture the project lacks, then has; a screen of another menu.
	TEST_EXPECT(rewrite(root + "/extra.mnu",
			screen("EXTRA",
					window("STATIC", "PICTURE", image("logo.tga") + font("%DEF_FONTNAME_LG%")) +
							window("BUTTON", "JUMP", go_screen("main.mnu", "STARTUP")) +
							window("BUTTON", "NOWHERE", go_screen("gone.mnu", "LOST")))));
	TEST_EXPECT(step("a menu naming a texture the project lacks", true));
	TEST_EXPECT(editor_test::write_text(root + "/textures/logo.tga", "tga"));
	TEST_EXPECT(step("the texture added", true));
	TEST_EXPECT(update.file_set && update.files == std::vector<std::string>{"textures/logo.tga"});
	// A menu moved to another folder: another file at another path; then its name's case alone
	// changed (another path, the same name).
	fs::create_directories(root + "/moved");
	fs::rename(root + "/extra.mnu", root + "/moved/extra.mnu");
	TEST_EXPECT(step("a menu moved", true));
	fs::rename(root + "/moved/extra.mnu", root + "/moved/extra_d3.tmp");
	fs::rename(root + "/moved/extra_d3.tmp", root + "/moved/EXTRA.mnu");
	TEST_EXPECT(step("the menu's name, its case alone changed", true));
	TEST_EXPECT(update.files == std::vector<std::string>({"moved/EXTRA.mnu", "moved/extra.mnu"}));
	// A second file of the main menu's name, then the first one gone: the name resolves to the
	// second, whose screens the JUMP's lookup reads, and whose button's ACTIONs name a window of
	// its screen and a name none has yet.
	const std::string show = "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">";
	TEST_EXPECT(rewrite(root + "/zz/main.mnu",
			screen("STARTUP",
					window("STATIC", "OTHER") +
							window("BUTTON", "D3_GO",
									show + "OTHER</ACTION>\r\n" + show + "RENAMED</ACTION>\r\n"))));
	TEST_EXPECT(step("a second main.mnu", true));
	fs::remove(root + "/" + menu_path);
	TEST_EXPECT(step("the first main.mnu gone", true));
	// A string table gone: the ids it defined, and the table the menus and the weapons read, no
	// more (a string id it never defined now names a table the project lacks).
	if (const AssetEntry *table = scan.find("gametext.bin")) {
		fs::remove(root + "/" + table->relative_path);
		TEST_EXPECT(step("gametext.bin gone", true));
	}
	// An open document edited, undone and closed: its slot follows the document, then the file.
	const AssetEntry *main_menu = scan.find("main.mnu");
	TEST_EXPECT(main_menu != nullptr);
	if (main_menu) {
		const std::string menu_now = main_menu->relative_path;
		auto document = std::make_shared<MnuDocument>();
		Diagnostic error;
		TEST_EXPECT(document->load(
				root + "/" + menu_now, menu_now, AssetKind::Menu, project.target_game, error));
		open = {document};
		TEST_EXPECT(step("the menu opened", false));
		NodeAddress other;
		TEST_EXPECT(find_definition(graph, *document, "OTHER", other));
		Edit set;
		set.operation = EditOperation::Set;
		set.address = other;
		set.field = "name";
		set.value = std::string("RENAMED");
		TEST_EXPECT(document->apply(set, error));
		TEST_EXPECT(step("an open menu's window renamed to the name an ACTION names", true));
		TEST_EXPECT(update.files == std::vector<std::string>{menu_now});
		document->undo();
		TEST_EXPECT(step("the rename undone", true));
		set.value = std::string("Other");
		TEST_EXPECT(document->apply(set, error));
		TEST_EXPECT(step("the window's name, its case alone changed", true));
		document->undo();
		TEST_EXPECT(step("that undone", true));
		open.clear();
		TEST_EXPECT(step("the menu closed", false));
	}
	// A table whose bytes make it another kind (a string table becomes a raw table).
	if (const AssetEntry *table = scan.find("menutxt.bin")) {
		TEST_EXPECT(editor_test::write_bytes(root + "/" + table->relative_path, {1, 2, 3, 4}));
		TEST_EXPECT(step("a string table become a raw table", true));
	}
	// A mission that reads (an item the project has and one it lacks), then does not: its edges
	// gone (the finding is its type's validation's, S14, not the graph's), then the file.
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Item, 100399, {});
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Item, 100398, {});
		std::vector<uint8_t> bytes;
		std::string error;
		TEST_EXPECT(opennova::bms::write(mission, bytes, error));
		TEST_EXPECT(editor_test::write_bytes(root + "/missions/broken.bms", bytes));
	}
	TEST_EXPECT(step("a mission placing two items", true));
	TEST_EXPECT(rewrite(root + "/missions/broken.bms", "not a mission"));
	TEST_EXPECT(step("the mission no longer reads", true));
	TEST_EXPECT(count_code(graph.diagnostics(), "graph.unreadable") == 0 &&
			graph.references_of("missions/broken.bms").empty());
	fs::remove(root + "/missions/broken.bms");
	TEST_EXPECT(step("the mission gone", true));
	TEST_EXPECT(count_code(graph.diagnostics(), "graph.unreadable") == 0);
	// A mission text (a .mis): its row counts, what it holds is read by nothing.
	TEST_EXPECT(rewrite(root + "/missions/m1.mis", "; one\n"));
	TEST_EXPECT(step("a .mis added", true));
	TEST_EXPECT(rewrite(root + "/missions/m1.mis", "; two, a longer line\n"));
	TEST_EXPECT(step("the .mis changed", false));
	return 0;
}

// What an update resolves again (S13 D3, GraphStats): one file's edit its own edges alone; a
// binding's change its own stylesheet's edges and the edges naming the variable; a file added
// every file edge (and a screen's); a variable gone the edges naming it. missing_count follows
// each, and the findings worded are those of the missing edges resolved again: one edit to a
// missing reference words one finding.
static int test_patch_counts() {
	editor_test::TempProjectDir dir("opennova_asset_graph_counts");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	TEST_EXPECT(rewrite(project.file("menu_style.mns"),
			"FONT_A a.fnt\r\nFONT_B b.fnt\r\nCOLOR_C FF00FF00\r\n"));
	TEST_EXPECT(editor_test::write_text(project.file("fonts/a.fnt"), "a"));
	TEST_EXPECT(editor_test::write_text(project.file("fonts/b.fnt"), "b"));
	TEST_EXPECT(rewrite(project.file("m.mnu"),
			screen("S",
					window("STATIC", "W1", font("%FONT_A%")) +
							window("STATIC", "W2", font("%FONT_B%")) +
							window("STATIC", "W3", image("pic.tga")) +
							window("STATIC", "W4", font("a.fnt")) +
							window("BUTTON", "W5", go_screen("m.mnu", "S")))));
	TEST_EXPECT(rewrite(project.file("defs/items.def"),
			"begin \"A\"\nid 100301\ntype building\ngraphic gone\nend\n"));
	AssetGraph graph;
	graph.update(project.paths, project.document, project.rescan(), {});
	Seen seen({"a", "font", "s"});
	const auto fresh = [&]() {
		return fresh_difference(graph, project.paths, project.document, project.scan, {}, seen);
	};
	TEST_EXPECT(fresh().empty() && graph.missing_count() == graph.missing().size());
	const size_t missing_at_first = graph.missing_count();
	// One closed file edited: its own edges, and nothing else.
	TEST_EXPECT(rewrite(project.file("defs/items.def"),
			"begin \"A\"\nid 100301\ntype building\ngraphic away\nend\n"));
	GraphUpdate update = graph.update(project.paths, project.document, project.rescan(), {});
	const size_t item_edges = graph.references_of("defs/items.def").size();
	TEST_EXPECT(update.changed && update.files == std::vector<std::string>{ "defs/items.def" } &&
			update.bindings.empty() && !update.file_set);
	TEST_EXPECT(graph.stats().files_patched == 1 && item_edges > 0 &&
			graph.stats().edges_resolved == item_edges && graph.stats().findings_made == 1);
	TEST_EXPECT(fresh().empty() && graph.missing_count() == missing_at_first);
	// A binding's value changed: menu_style.mns's own edges, and the edges naming %FONT_A% (W1's
	// font and its variable), not W2's, W3's, W4's or W5's.
	TEST_EXPECT(rewrite(project.file("menu_style.mns"),
			"FONT_A b.fnt\r\nFONT_B b.fnt\r\nCOLOR_C FF00FF00\r\n"));
	update = graph.update(project.paths, project.document, project.rescan(), {});
	const size_t style_edges = graph.references_of("menu_style.mns").size();
	const size_t through_a =
			count_edges(graph, [](const GraphEdge &edge) { return edge.value == "%FONT_A%"; });
	TEST_EXPECT(update.bindings == std::vector<std::string>{ "FONT_A" } &&
			update.files == std::vector<std::string>{ "menu_style.mns" });
	TEST_EXPECT(through_a == 2 && graph.stats().files_patched == 1 &&
	            graph.stats().edges_resolved == style_edges + through_a &&
	            graph.stats().findings_made == 0);
	std::string loaded;
	TEST_EXPECT(graph.resolve(ReferenceKind::Font, "%FONT_A%", std::string(), &loaded) ==
					ReferenceStatus::Present &&
			loaded == "fonts/b.fnt");
	TEST_EXPECT(graph.referrers_of_file("b.fnt").size() == 4 &&
			graph.referrers_of_file("a.fnt").size() == 1);
	TEST_EXPECT(fresh().empty() && graph.missing_count() == missing_at_first);
	// A file added: every file edge (and the screen's, whose lookup asks for its menu file); the
	// missing texture found.
	TEST_EXPECT(editor_test::write_text(project.file("textures/pic.tga"), "tga"));
	update = graph.update(project.paths, project.document, project.rescan(), {});
	const size_t file_edges = count_edges(graph, [](const GraphEdge &edge) {
		return reference_row(edge.kind).resolution == ReferenceResolution::File ||
				edge.kind == ReferenceKind::MenuScreen;
	});
	TEST_EXPECT(update.file_set && update.files == std::vector<std::string>{ "textures/pic.tga" } &&
			update.bindings.empty());
	TEST_EXPECT(graph.stats().files_patched == 1 && file_edges > 0 &&
			file_edges < graph.edge_count() && graph.stats().edges_resolved == file_edges);
	TEST_EXPECT(fresh().empty() && graph.missing_count() == missing_at_first - 1);
	// The item's model, a file edge still missing, worded again; the texture's finding gone.
	TEST_EXPECT(graph.stats().findings_made == 1);
	// A variable gone: its binding's change reaches the edges naming it (the variable missing, its
	// font through it quiet), and the symbol's own key.
	TEST_EXPECT(rewrite(project.file("menu_style.mns"), "FONT_A b.fnt\r\nCOLOR_C FF00FF00\r\n"));
	update = graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(update.bindings == std::vector<std::string>{"FONT_B"});
	TEST_EXPECT(graph.stats().edges_resolved == graph.references_of("menu_style.mns").size() + 2 &&
			graph.stats().findings_made == 1);
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%FONT_B%") == ReferenceStatus::Missing);
	TEST_EXPECT(fresh().empty() && graph.missing_count() == missing_at_first);
	// Nothing changed: nothing resolved.
	const uint64_t generation = graph.generation();
	update = graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(!update.changed && update.files.empty() && graph.generation() == generation &&
			graph.stats().edges_resolved == 0 && graph.stats().findings_made == 0 &&
			graph.stats().files_reused == project.scan.entries.size() - 3);
	return 0;
}

// references_of reads a file's own slot: by its path, or by a logical name the file that name
// resolves to (the first of it by path), never another file of the name (the trunk's matched every
// file of the name); a path the project does not have names nothing. referrers_of_file and
// usages_of name a file the same way, the path first: a file at the project's root named by its
// path is itself, even where another file of its name comes first and takes the references.
static int test_references_of() {
	editor_test::TempProjectDir dir("opennova_asset_graph_references_of");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	TEST_EXPECT(rewrite(
			project.file("a/main.mnu"), screen("S", window("STATIC", "A", image("a.tga")))));
	TEST_EXPECT(rewrite(project.file("b/main.mnu"),
			screen("S",
					window("STATIC", "B", image("b.tga")) +
							window("STATIC", "C", image("c.tga")))));
	AssetGraph graph;
	graph.update(project.paths, project.document, project.rescan(), {});
	const auto values = [](const std::vector<const GraphEdge *> &edges) {
		std::vector<std::string> out;
		for (const GraphEdge *edge : edges) out.push_back(edge->source + ":" + edge->value);
		return out;
	};
	TEST_EXPECT(values(graph.references_of("main.mnu")) ==
			std::vector<std::string>{ "a/main.mnu:a.tga" });
	TEST_EXPECT(values(graph.references_of("MAIN.MNU")) ==
			std::vector<std::string>{ "a/main.mnu:a.tga" });
	TEST_EXPECT(values(graph.references_of("a/main.mnu")) ==
			std::vector<std::string>{ "a/main.mnu:a.tga" });
	TEST_EXPECT(values(graph.references_of("b/main.mnu")) ==
	            std::vector<std::string>({"b/main.mnu:b.tga", "b/main.mnu:c.tga"}));
	TEST_EXPECT(graph.references_of("c/main.mnu").empty() &&
			graph.references_of("nothing.mnu").empty());
	TEST_EXPECT(rewrite(
			project.file("main.mnu"), screen("R", window("STATIC", "R", image("r.tga")))));
	TEST_EXPECT(rewrite(project.file("c.mnu"),
			screen("C", window("BUTTON", "GO", go_screen("main.mnu", "S")))));
	graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(values(graph.references_of("main.mnu")) ==
			std::vector<std::string>{ "main.mnu:r.tga" });
	TEST_EXPECT(values(graph.references_of("MAIN.MNU")) ==
			std::vector<std::string>{ "a/main.mnu:a.tga" });
	TEST_EXPECT(graph.referrers_of_file("main.mnu").empty() && graph.usages_of("main.mnu").empty());
	const std::vector<std::string> referrers = values(graph.referrers_of_file("a/main.mnu"));
	TEST_EXPECT(referrers == std::vector<std::string>{ "c.mnu:main.mnu" } &&
			values(graph.referrers_of_file("MAIN.MNU")) == referrers);
	const std::vector<std::string> usages = values(graph.usages_of("a/main.mnu"));
	TEST_EXPECT(usages == std::vector<std::string>({"c.mnu:main.mnu", "c.mnu:S"}) &&
			values(graph.usages_of("MAIN.MNU")) == usages);
	TEST_EXPECT(graph.referrers_of_file("c/main.mnu").empty() &&
			graph.usages_of("c/main.mnu").empty());
	return 0;
}

namespace {

// A base layer's file of text, of a kind.
LayerFile layer_file(const std::string &name, AssetKind kind, const std::string &text) {
	LayerFile file;
	file.name = name;
	file.kind = kind;
	// As the file holds it (a .def with CR LF, editor_test::staged).
	file.read = [text = editor_test::staged(name, text)](std::vector<uint8_t> &bytes, std::string &) {
		bytes.assign(text.begin(), text.end());
		return true;
	};
	return file;
}

} // namespace

// The base layer (S13 D3, ADR 0046 d10: a read-only dependency mount; project assets win): a
// lookup tries the project, then the base, whose files the project has one of the name of are
// hidden (their names with them); the base resolves what the project lacks, a stylesheet's
// variables included; it makes no edge and no finding (a reference in it to nothing, a file of it
// that does not read); choices lists the base's names after the project's, each name once, the
// names a lookup finds before those none does: a variable the project defines only in a
// stylesheet the game never reads is the base's live one, and a base stylesheet the game never
// reads offers its variables inert. Set and taken away, and a file added under it, the graph
// equals a fresh one over the same base.
static int test_base_layer() {
	std::vector<LayerFile> files;
	files.push_back(layer_file("menu_style.mns", AssetKind::MenuStyle,
			"DEF_FONTNAME_LG basefont.fnt\r\nBASE_COLOR FF00FF00\r\nBIG FF0000FF\r\n"));
	files.push_back(layer_file("other.mns", AssetKind::MenuStyle, "STRAY 1\r\n"));
	files.push_back(layer_file("basefont.fnt", AssetKind::Font, "font"));
	files.push_back(layer_file("shared.tga", AssetKind::Texture, "tga"));
	files.push_back(layer_file("onlybase.tga", AssetKind::Texture, "tga"));
	files.push_back(layer_file("base.mnu", AssetKind::Menu,
			screen("BASESCREEN", window("STATIC", "BASEWIN", image("nothere.tga")))));
	files.push_back(layer_file("menus.mnu", AssetKind::Menu, screen("OLD", window("STATIC", "X"))));
	files.push_back(layer_file("broken.bms", AssetKind::Mission, "not a mission"));
	files.push_back(layer_file("SHARED.TGA", AssetKind::Texture, "a second file of the name"));
	GraphStats built;
	const std::shared_ptr<const GraphLayer> layer = GraphLayer::build(files, "jo", &built);
	TEST_EXPECT(layer->file_count() == 8 && built.files_extracted == 5 && built.files_failed == 1);
	TEST_EXPECT(layer->file_named("Base.mnu") && !layer->file_named("nothere.tga") &&
			layer->symbol_count() > 0);
	TEST_EXPECT(layer->index().edge_count() == 0);

	editor_test::TempProjectDir dir("opennova_asset_graph_base");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	TEST_EXPECT(rewrite(project.file("m.mnu"),
			screen("HOME",
					window("STATIC", "W1", font("%DEF_FONTNAME_LG%")) +
							window("STATIC", "W2", image("shared.tga")) +
							window("BUTTON", "W3", go_screen("base.mnu", "BASESCREEN")) +
							window("STATIC", "W4", image("onlybase.tga")) +
							window("BUTTON", "W5", go_screen("menus.mnu", "OLD")) +
							window("BUTTON", "W6", go_screen("menus.mnu", "NEW")))));
	TEST_EXPECT(rewrite(project.file("menus/menus.mnu"), screen("NEW", window("STATIC", "Y"))));
	TEST_EXPECT(editor_test::write_text(project.file("textures/shared.tga"), "tga"));
	TEST_EXPECT(editor_test::write_text(project.file("fonts/project.fnt"), "font"));
	TEST_EXPECT(rewrite(project.file("extra.mns"), "BIG FF00FF00\r\n"));
	AssetGraph graph;
	graph.update(project.paths, project.document, project.rescan(), {});
	const size_t missing_alone = graph.missing_count();
	Seen seen({"a", "base", "big", "shared"});
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%DEF_FONTNAME_LG%") ==
			ReferenceStatus::Missing);
	TEST_EXPECT(
			graph.resolve(ReferenceKind::MenuTexture, "onlybase.tga") == ReferenceStatus::Missing);
	const uint64_t before = graph.generation();
	GraphUpdate update = graph.set_base(layer);
	TEST_EXPECT(update.changed && graph.generation() != before && graph.base() == layer);
	TEST_EXPECT(graph.stats().edges_resolved == graph.edge_count());
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	// The base resolves what the project lacks: a variable of its stylesheet, the font its value
	// names, a texture, a screen of its menu.
	std::string file;
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%DEF_FONTNAME_LG%", std::string(), &file) ==
	                    ReferenceStatus::Present &&
	            file == "menu_style.mns");
	TEST_EXPECT(graph.style_binding("DEF_FONTNAME_LG") &&
			graph.style_binding("DEF_FONTNAME_LG")->value == "basefont.fnt");
	TEST_EXPECT(graph.resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &file) ==
					ReferenceStatus::Present &&
			file == "basefont.fnt");
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuTexture, "onlybase.tga", std::string(), &file) ==
	                    ReferenceStatus::Present &&
	            file == "onlybase.tga");
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuScreen, "BASESCREEN", "BASE.MNU", &file) ==
					ReferenceStatus::Present &&
			file == "base.mnu");
	TEST_EXPECT(graph.has_file("basefont.fnt") && graph.has_file("fonts/project.fnt"));
	// The project's file wins: its own texture of the name, and its menus.mnu, whose screens hide
	// the base's (OLD is none of the project's: missing; NEW is).
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuTexture, "shared.tga", std::string(), &file) ==
					ReferenceStatus::Present &&
			file == "textures/shared.tga");
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuScreen, "OLD", "MENUS.MNU") ==
			ReferenceStatus::Missing);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuScreen, "NEW", "MENUS.MNU", &file) ==
					ReferenceStatus::Present &&
			file == "menus/menus.mnu");
	TEST_EXPECT(graph.symbols_named(ReferenceKind::MenuScreen, "OLD").empty());
	TEST_EXPECT(graph.referrers_of_file("onlybase.tga").size() == 1 &&
			graph.usages_of("base.mnu").size() == 2);
	// The base makes no finding: its menu's missing texture, its file that does not read.
	TEST_EXPECT(graph.missing_count() + 3 == missing_alone);
	for (const Diagnostic &d : graph.diagnostics())
		TEST_EXPECT(d.asset == "m.mnu" && d.message.find("nothere") == std::string::npos);
	TEST_EXPECT(count_code(graph.diagnostics(), "graph.unreadable") == 0);
	TEST_EXPECT(graph.symbol_count() == symbols_in(graph).size() &&
			graph.symbols_of_kind(ReferenceKind::MenuScreen).size() == 2);
	// choices: the project's names, then the base's, each once.
	const std::vector<ReferenceChoice> textures = graph.choices(ReferenceKind::MenuTexture);
	TEST_EXPECT(textures.size() == 2 && textures[0].file == "textures/shared.tga" &&
			textures[1].file == "onlybase.tga");
	const std::vector<ReferenceChoice> fonts = graph.choices(ReferenceKind::Font);
	TEST_EXPECT(fonts.size() == 2 && fonts[0].file == "fonts/project.fnt" &&
			fonts[1].file == "basefont.fnt");
	const std::vector<ReferenceChoice> screens = graph.choices(ReferenceKind::MenuScreen);
	TEST_EXPECT(screens.size() == 3 && screens[0].name == "HOME" && screens[1].name == "NEW" &&
	            screens[2].name == "BASESCREEN");
	// The variables: the base's the game reads, BIG among them though the project's extra.mns
	// defines it (a stylesheet the game never reads), then the base's other.mns's, inert.
	const std::vector<ReferenceChoice> variables = graph.choices(ReferenceKind::StyleVar);
	TEST_EXPECT(variables.size() == 4 && variables[0].name == "%DEF_FONTNAME_LG%" &&
			variables[0].file == "menu_style.mns" && variables[1].name == "%BASE_COLOR%");
	TEST_EXPECT(variables.size() == 4 && variables[2].name == "%BIG%" &&
			variables[2].file == "menu_style.mns" && !variables[2].inert &&
			variables[2].status == ReferenceStatus::Present);
	TEST_EXPECT(variables.size() == 4 && variables[3].name == "%STRAY%" &&
			variables[3].file == "other.mns" && variables[3].inert &&
			variables[3].status == ReferenceStatus::Missing &&
			variables[3].reason == "the game reads no stylesheet but menu_style.mns and brand.mns");
	TEST_EXPECT(graph.usages_of("other.mns").empty());
	// A stylesheet of the project's hides the base's whole: its variables no longer bind.
	TEST_EXPECT(rewrite(project.file("menu_style.mns"), "PROJECT_ONLY 1\r\n"));
	update = graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(update.file_set &&
			std::find(update.bindings.begin(), update.bindings.end(), "DEF_FONTNAME_LG") !=
					update.bindings.end());
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%DEF_FONTNAME_LG%") ==
			ReferenceStatus::Missing);
	TEST_EXPECT(
			graph.resolve(ReferenceKind::StyleVar, "%PROJECT_ONLY%") == ReferenceStatus::Present);
	const std::vector<ReferenceChoice> hidden = graph.choices(ReferenceKind::StyleVar);
	TEST_EXPECT(hidden.size() == 3 && hidden[0].name == "%PROJECT_ONLY%" &&
			hidden[1].name == "%BIG%" && hidden[1].file == "extra.mns" && hidden[1].inert &&
			hidden[2].name == "%STRAY%" && hidden[2].inert);
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	fs::remove(project.file("menu_style.mns"));
	graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	// Taken away: the graph as it was without it.
	update = graph.set_base(nullptr);
	TEST_EXPECT(update.changed && !graph.base() && graph.missing_count() == missing_alone);
	TEST_EXPECT(fresh_difference(graph, project.paths, project.document, project.scan, {}, seen)
					.empty());
	TEST_EXPECT(!graph.set_base(nullptr).changed);
	return 0;
}

// A file added under a base layer (S13 D3) resolves again what its name reaches and no more than
// any file added does: every file edge (and a screen's) and the edges into the names of the base's
// file of its name, which it hides (the project's ammo.def hides the base's, whose ammo a weapon's
// round names); the findings worded are those of the missing edges resolved again and of the
// string ids, whose words read the file set. Gone again, the base's file shows.
static int test_base_layer_file_set() {
	const std::shared_ptr<const GraphLayer> layer = GraphLayer::build(
			{layer_file("ammo.def", AssetKind::AmmoDefs, "ammo BASE_AMMO\nend\n"),
					layer_file("weapon.def", AssetKind::WeaponDefs, "weapon \"BASE_GUN\"\nend\n")},
			"jo");
	editor_test::TempProjectDir dir("opennova_asset_graph_base_file_set");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	TEST_EXPECT(rewrite(project.file("defs/weapon.def"),
			"weapon \"GUN_A\"\nround_type BASE_AMMO\nloadout_menu_textid WEP_NONE_D3\nend\n"
			"weapon \"GUN_B\"\nround_type OTHER_AMMO\nend\n"));
	TEST_EXPECT(rewrite(project.file("m.mnu"),
			screen("S",
					window("STATIC", "W", image("pic.tga")) +
							window("BUTTON", "GO", go_screen("m.mnu", "S")))));
	AssetGraph graph;
	graph.set_base(layer);
	graph.update(project.paths, project.document, project.rescan(), {});
	Seen seen({"gun", "ammo"});
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "BASE_AMMO") == ReferenceStatus::Present);
	const size_t missing_before = graph.missing_count();
	const auto file_set_edges = [&graph]() {
		return count_edges(graph, [](const GraphEdge &edge) {
			return reference_row(edge.kind).resolution == ReferenceResolution::File ||
					edge.kind == ReferenceKind::MenuScreen || edge.source == "defs/ammo.def";
		});
	};
	// The project's ammo.def: the base's hidden, and the round naming its ammo missing.
	TEST_EXPECT(rewrite(project.file("defs/ammo.def"), "ammo PROJECT_AMMO\nend\n"));
	GraphUpdate update = graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(update.file_set && update.files == std::vector<std::string>{"defs/ammo.def"});
	TEST_EXPECT(graph.stats().edges_resolved == file_set_edges() + 1 &&
			graph.stats().edges_resolved < graph.edge_count());
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "BASE_AMMO") == ReferenceStatus::Missing &&
			graph.missing_count() == missing_before + 1);
	// The round, the menu's missing texture (a file edge) and the weapon's string id.
	TEST_EXPECT(graph.stats().findings_made == 3);
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	// Gone again: the base's ammo.def shows, and the round resolves through it.
	fs::remove(project.file("defs/ammo.def"));
	update = graph.update(project.paths, project.document, project.rescan(), {});
	TEST_EXPECT(update.file_set && graph.stats().edges_resolved == file_set_edges() + 1);
	TEST_EXPECT(graph.resolve(ReferenceKind::Ammo, "BASE_AMMO") == ReferenceStatus::Present &&
			graph.missing_count() == missing_before && graph.stats().findings_made == 2);
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	return 0;
}

// S13 D8: a model's registers and MTRX rows named by index (Record references) resolve within the
// model's own file. Its record sets (every register and row a symbol of its index, scoped to the
// file); each reference Present at an index its collection holds, Missing past it, never a graph
// finding (the model's validation reports it); a register's Referenced by (the symbols its record
// defines and their referrers, its own file's alone where two models hold the same registers), its
// users, the picker's rows (the file's registers by index); a file's usages and Find in project
// leaving the record sets out. And an open model's register removed in the middle, its references
// renumbered in the same step: the update patches that file alone, equal to a graph built fresh,
// the Referenced by following; its undo the same.
static int test_record_references() {
	editor_test::TempProjectDir dir("opennova_asset_graph_records");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	const std::vector<uint8_t> rig = rig_model::bytes();
	TEST_EXPECT(!rig.empty() && editor_test::write_bytes(project.file("models/rig.3di"), rig) &&
	            editor_test::write_bytes(project.file("models/twin.3di"), rig));
	AssetGraph graph;
	graph.update(project.paths, project.document, project.rescan(), {});
	const std::string file = "models/rig.3di";
	// A file's references of a kind, each in that file and found there (none counted otherwise).
	const auto record_edges = [&](const std::string &of, ReferenceKind kind) {
		size_t n = 0;
		for (const GraphEdge *edge : graph.references_of(of))
			if (edge->kind == kind)
				n += edge->scope == of && graph.resolve(*edge) == ReferenceStatus::Present ? 1 : 1000;
		return n;
	};
	TEST_EXPECT(record_edges(file, ReferenceKind::ModelRegister) == 4 &&
	            record_edges(file, ReferenceKind::ModelFrame) == 1);
	TEST_EXPECT(graph.symbols_of_kind(ReferenceKind::ModelRegister).size() == 8 &&
	            graph.symbols_of_kind(ReferenceKind::ModelFrame).size() == 6);
	for (const GraphEdge *edge : graph.missing())
		TEST_EXPECT(edge->kind != ReferenceKind::ModelRegister && edge->kind != ReferenceKind::ModelFrame);
	// Within its file: register 3 is FLICKER, 4 none (no finding), frame 2 a row.
	const GraphSymbol *flicker = graph.resolve_symbol(ReferenceKind::ModelRegister, "3", file);
	TEST_EXPECT(flicker && flicker->file == file && flicker->record == "rig/FLICKER" && flicker->field.empty());
	TEST_EXPECT(graph.resolve(ReferenceKind::ModelRegister, "3", file) == ReferenceStatus::Present &&
	            graph.resolve(ReferenceKind::ModelRegister, "4", file) == ReferenceStatus::Missing &&
	            graph.resolve(ReferenceKind::ModelFrame, "2", file) == ReferenceStatus::Present &&
	            graph.resolve(ReferenceKind::ModelRegister, "3", "models/none.3di") == ReferenceStatus::Missing);
	// Referenced by FLICKER: its record's symbols, their referrers (the light and the X track, this
	// file's alone), its users the same.
	const std::vector<const GraphSymbol *> defined = graph.symbols_of(file, flicker->record);
	TEST_EXPECT(defined.size() == 1 && defined[0] == flicker);
	std::vector<const GraphEdge *> users = graph.referrers_of(ReferenceKind::ModelRegister, "3", file);
	std::set<std::string> fields;
	for (const GraphEdge *edge : users) {
		TEST_EXPECT(edge->source == file);
		fields.insert(edge->field);
	}
	TEST_EXPECT(users.size() == 2 && fields == std::set<std::string>({"param", "rotx.param"}) &&
	            graph.users_of(*flicker).size() == 2);
	// The picker: the file's registers by index, each with its record.
	const std::vector<ReferenceChoice> registers = graph.choices(ReferenceKind::ModelRegister, file);
	TEST_EXPECT(registers.size() == 4 && registers[0].name == "0" && registers[3].name == "3" &&
	            registers[3].record == "rig/FLICKER" && registers[3].label == "FLICKER" &&
	            registers[3].file == file && registers[3].status == ReferenceStatus::Present);
	TEST_EXPECT(graph.choices(ReferenceKind::ModelRegister, "models/none.3di").empty());
	// A file's own records by index are no use of it, nor a name to find.
	for (const GraphEdge *edge : graph.usages_of(file))
		TEST_EXPECT(edge->kind != ReferenceKind::ModelRegister && edge->kind != ReferenceKind::ModelFrame);
	for (const GraphSearchHit &hit : graph.search("2"))
		TEST_EXPECT(!hit.symbol || hit.symbol->kind != ReferenceKind::ModelRegister);
	Seen seen({"rig", "2"});
	TEST_EXPECT(fresh_difference(graph, project.paths, project.document, project.scan, {}, seen).empty());

	// The model open, EWEAP_GUNYAW (1) removed: FLICKER is register 2 now, and so its references say.
	auto model = std::make_shared<ModelDocument>();
	Diagnostic error;
	TEST_EXPECT(model->load(project.file(file), file, AssetKind::Model, "jo", error));
	const std::vector<std::shared_ptr<const DocumentBase>> open = {model};
	graph.update(project.paths, project.document, project.scan, open);
	const NodeAddress gunyaw{model->model_row()->id, node_kind(ModelKind::Register),
	                         model->model_row()->ids.lists[4][1].id};
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = gunyaw;
	TEST_EXPECT(model->apply(remove, error));
	const GraphUpdate update = graph.update(project.paths, project.document, project.scan, open);
	TEST_EXPECT(update.changed && update.files == std::vector<std::string>{file} && graph.stats().files_patched == 1);
	TEST_EXPECT(fresh_difference(graph, project.paths, project.document, project.scan, open, seen).empty());
	const GraphSymbol *moved = graph.resolve_symbol(ReferenceKind::ModelRegister, "2", file);
	TEST_EXPECT(moved && moved->record == "rig/FLICKER" &&
	            graph.referrers_of(ReferenceKind::ModelRegister, "2", file).size() == 2 &&
	            graph.referrers_of(ReferenceKind::ModelRegister, "3", file).empty() &&
	            graph.choices(ReferenceKind::ModelRegister, file).size() == 3);
	// The other model keeps its four.
	TEST_EXPECT(graph.choices(ReferenceKind::ModelRegister, "models/twin.3di").size() == 4 &&
	            graph.referrers_of(ReferenceKind::ModelRegister, "3", "models/twin.3di").size() == 2);
	model->undo();
	graph.update(project.paths, project.document, project.scan, open);
	TEST_EXPECT(fresh_difference(graph, project.paths, project.document, project.scan, open, seen).empty());
	TEST_EXPECT(graph.referrers_of(ReferenceKind::ModelRegister, "3", file).size() == 2);
	return 0;
}

// The retail leg of the Record references (OPENNOVA_JO_DIR): the JO install's models (the base
// game's and each expansion's, each source's once, as editor_model_document's retail leg reads them)
// in a project, every register and MTRX row their records name by index an edge into the model's
// own record set that resolves; one model whose records name a register opened, a register added
// at its front (every reference to its registers renumbered up in the same step) and removed again
// (renumbered back down), the graph after each equal to one built fresh.
static int test_retail_record_references() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the Record references of the JO install's models)");
		return 0;
	}
	editor_test::TempProjectDir dir("opennova_asset_graph_retail_records");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	std::vector<std::string> expansions = opennova::vfs_list_expansions(install);
	expansions.insert(expansions.begin(), std::string()); // the base game first
	std::set<std::string> seen_models;
	size_t models = 0;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(
		        opennova::gameprofile::gameprofile_scr_policy_for_code(project.document.target_game.c_str()));
		TEST_EXPECT(game.mount_game(install, expansion) && game.has_mounted_archive());
		for (const opennova::VfsFileLocation &location : game.list_files()) {
			const std::string &name = location.logical_name;
			if (!opennova::strutil::ends_with_icase(name, ".3di") ||
			    !seen_models.insert(location.source_path + "|" + normalized_logical_name(name)).second)
				continue;
			std::vector<uint8_t> bytes;
			if (!game.read_file(name, bytes) || bytes.size() < 4 || std::memcmp(bytes.data(), "3DI3", 4) != 0)
				continue; // not a model the game's loader reads either
			const std::string folder = expansion.empty() ? std::string("base") : expansion;
			const std::string placed = "model/" + folder + "/" + std::to_string(models) + "/" + name;
			TEST_EXPECT(editor_test::write_bytes(project.file(placed), bytes));
			++models;
		}
	}
	AssetGraph graph;
	const auto clock = std::chrono::steady_clock::now();
	graph.update(project.paths, project.document, project.rescan(), {});
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock).count();
	size_t registers = 0, frames = 0, unresolved = 0;
	std::string named;
	graph.for_each_edge([&](const GraphEdge &edge) {
		if (edge.kind != ReferenceKind::ModelRegister && edge.kind != ReferenceKind::ModelFrame) return;
		(edge.kind == ReferenceKind::ModelRegister ? registers : frames) += 1;
		if (graph.resolve(edge) != ReferenceStatus::Present) ++unresolved;
		if (named.empty() && edge.kind == ReferenceKind::ModelRegister) named = edge.source;
	});
	std::printf("the JO install's models through the graph: %zu models, %zu edges (%zu registers and %zu MTRX rows "
	            "named by index, %zu not in their collection), %zu symbols (%zu registers, %zu rows), read in %.2f "
	            "s\n",
	            models, graph.edge_count(), registers, frames, unresolved, graph.symbol_count(),
	            graph.symbols_of_kind(ReferenceKind::ModelRegister).size(),
	            graph.symbols_of_kind(ReferenceKind::ModelFrame).size(), seconds);
	TEST_EXPECT(models > 900 && registers > 0 && frames > 0 && unresolved == 0 && !named.empty());
	if (named.empty()) return 0;
	auto model = std::make_shared<ModelDocument>();
	Diagnostic error;
	TEST_EXPECT(model->load(project.file(named), named, AssetKind::Model, "jo", error));
	const std::vector<std::shared_ptr<const DocumentBase>> open = {model};
	graph.update(project.paths, project.document, project.scan, open);
	Seen seen({"model"});
	// Each register reference of the model (its record and field) and the register it resolves to,
	// by the register's identity: what the renumbering keeps, whatever indexes the edits leave.
	using Resolved = std::map<std::pair<NodeId, std::string>, NodeAddress>;
	const auto resolved = [&] {
		Resolved out;
		for (const GraphEdge *edge : graph.references_of(named)) {
			if (edge->kind != ReferenceKind::ModelRegister) continue;
			const GraphSymbol *symbol = graph.resolve_symbol(edge->kind, edge->value, edge->scope);
			out[{edge->address.child, edge->field}] = symbol ? symbol->address : NodeAddress();
		}
		return out;
	};
	const Resolved before = resolved();
	bool all_found = !before.empty();
	for (const auto &reference : before) all_found = all_found && reference.second.child != 0;
	TEST_EXPECT(all_found);
	const auto step = [&](const char *what) {
		const GraphUpdate update = graph.update(project.paths, project.document, project.scan, open);
		const std::string different =
		        fresh_difference(graph, project.paths, project.document, project.scan, open, seen);
		const bool same = resolved() == before;
		std::printf("  %s: %zu files patched, %zu edges resolved of %zu, %zu register references naming the "
		            "registers they named%s%s%s\n",
		            what, graph.stats().files_patched, graph.stats().edges_resolved, graph.edge_count(), before.size(),
		            same ? "" : "; FAIL, a reference names another register",
		            different.empty() ? "" : "; FAIL, differs from a fresh graph in ", different.c_str());
		return same && different.empty() && update.changed && update.files == std::vector<std::string>{named};
	};
	const NodeId row = model->model_row()->id;
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {row, node_kind(ModelKind::Register), 0};
	add.position = 0;
	TEST_EXPECT(model->apply(add, error));
	TEST_EXPECT(step(("a register added at the front of " + named).c_str()));
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = {row, node_kind(ModelKind::Register), model->last_added()};
	TEST_EXPECT(model->apply(remove, error));
	TEST_EXPECT(step("the register removed again"));
	return 0;
}

// The retail leg of the incremental graph (OPENNOVA_JO_DIR): the install's menus, stylesheet,
// string tables, catalogs, animation tables, environments, avatars, particles and missions
// (every kind the graph reads but the models and clips) exported into a project, then edited: a
// stylesheet's font changed, brand.mns added, a string table gone, a model the items name added,
// a menu moved, an item added; after each, the graph equals one built fresh.
static int test_retail_incremental() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the incremental graph over the JO install's files)");
		return 0;
	}
	editor_test::TempProjectDir dir("opennova_asset_graph_retail_incremental");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	ImportOrigin origin;
	std::string error;
	TEST_EXPECT(origin.open(ImportOrigin::Kind::GameInstall, install, project.document, error));
	InstallView view;
	TEST_EXPECT(view.open(install_spec(install, project.document), error));
	const opennova::Vfs &mount = view.vfs();
	size_t exported = 0;
	std::string model_name;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		if (opennova::strutil::ends_with_icase(name, ".pff")) continue;
		const AssetKind kind = origin.file_kind(name);
		if (kind == AssetKind::Model && model_name.empty()) model_name = name;
		if (!graph_reads_kind(kind) || kind == AssetKind::Model ||
				kind == AssetKind::Animation)
			continue;
		std::vector<uint8_t> bytes;
		if (!origin.read(name, bytes)) continue;
		TEST_EXPECT(editor_test::write_bytes(
				project.file(std::string(asset_kind_token(kind)) + "/" + name), bytes));
		++exported;
	}
	TEST_EXPECT(exported > 0 && !model_name.empty());
	std::vector<std::shared_ptr<const DocumentBase>> open;
	AssetGraph graph;
	const auto clock = std::chrono::steady_clock::now();
	graph.update(project.paths, project.document, project.rescan(), open);
	const double seconds =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - clock).count();
	std::printf("the JO install's files through the incremental graph: %zu files exported, %zu "
				"edges, %zu symbols, %zu "
				"missing, read in %.2f s\n",
			exported, graph.edge_count(), graph.symbol_count(), graph.missing_count(), seconds);
	Seen seen({"main", "font", "d3"});
	const auto step = [&](const char *what) {
		const GraphUpdate update =
				graph.update(project.paths, project.document, project.rescan(), open);
		const auto compared = std::chrono::steady_clock::now();
		const std::string different =
				fresh_difference(graph, project.paths, project.document, project.scan, open, seen);
		std::printf("  %s: %zu files patched, %zu edges resolved of %zu, %zu findings worded, %zu "
					"missing (compared in %.1f s)%s%s\n",
				what, graph.stats().files_patched, graph.stats().edges_resolved,
				graph.edge_count(), graph.stats().findings_made, graph.missing_count(),
				std::chrono::duration<double>(std::chrono::steady_clock::now() - compared)
						.count(),
				different.empty() ? "" : "; FAIL, differs from a fresh graph in ",
				different.c_str());
		return different.empty() && update.changed;
	};
	const AssetEntry *style = project.scan.find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (style) {
		std::string text, problem;
		TEST_EXPECT(opennova::io::read_file_text(project.file(style->relative_path), text, problem));
		const size_t fnt = text.find(".fnt");
		if (fnt != std::string::npos) text.replace(fnt, 4, "_x.fnt");
		TEST_EXPECT(rewrite(project.file(style->relative_path), text));
		TEST_EXPECT(step("a stylesheet font renamed"));
		TEST_EXPECT(rewrite(project.file("menu_style/brand.mns"),
				"DEF_FONTNAME_LG arial.fnt\r\nBRAND_ONLY 1\r\n"));
		TEST_EXPECT(step("brand.mns added"));
	}
	if (const AssetEntry *table = project.scan.find("menutxt.bin")) {
		fs::remove(project.file(table->relative_path));
		TEST_EXPECT(step("menutxt.bin gone"));
	}
	std::vector<uint8_t> model;
	TEST_EXPECT(origin.read(model_name, model));
	TEST_EXPECT(editor_test::write_bytes(project.file("model/" + model_name), model));
	TEST_EXPECT(step("a model added"));
	if (const AssetEntry *menu = project.scan.find("main.mnu")) {
		fs::create_directories(project.file("moved"));
		fs::rename(project.file(menu->relative_path), project.file("moved/main.mnu"));
		TEST_EXPECT(step("main.mnu moved"));
	}
	if (const AssetEntry *items = project.scan.find("items.def")) {
		std::string text, problem;
		TEST_EXPECT(opennova::io::read_file_text(project.file(items->relative_path), text, problem));
		TEST_EXPECT(rewrite(project.file(items->relative_path),
				text +
						"\r\nbegin \"D3 RETAIL\"\r\nid 199999\r\ntype building\r\ngraphic "
						"nothing_here\r\nend\r\n"));
		TEST_EXPECT(step("an item added"));
	}
	return 0;
}

// The retail leg of the base layer (OPENNOVA_JO_DIR): a layer over the mounted JO install, every
// file the graph reads (models and clips included) read once for the names it defines, under a
// project of one menu naming what only the install has: its stylesheet's font variable, a
// texture, a screen of its main menu; each resolves through the base, and the base makes no
// finding.
static int test_retail_base_layer() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the base layer over the mounted JO install)");
		return 0;
	}
	editor_test::TempProjectDir dir("opennova_asset_graph_retail_base");
	Project project(dir.file("project"));
	TEST_EXPECT(project.made);
	ImportOrigin origin;
	std::string error;
	TEST_EXPECT(origin.open(ImportOrigin::Kind::GameInstall, install, project.document, error));
	InstallView view;
	TEST_EXPECT(view.open(install_spec(install, project.document), error));
	const opennova::Vfs &mount = view.vfs();
	std::vector<LayerFile> files;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		if (opennova::strutil::ends_with_icase(location.logical_name, ".pff")) continue;
		LayerFile file;
		file.name = location.logical_name;
		file.kind = origin.file_kind(location.logical_name);
		file.read = [&origin, name = location.logical_name](std::vector<uint8_t> &bytes,
							std::string &) { return origin.read(name, bytes); };
		files.push_back(std::move(file));
	}
	const auto clock = std::chrono::steady_clock::now();
	GraphStats built;
	const std::shared_ptr<const GraphLayer> layer =
			GraphLayer::build(files, project.document.target_game, &built);
	const double seconds =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - clock).count();
	std::map<std::string, size_t> kinds;
	for (size_t k = 0; k < kReferenceKindCount; ++k)
		if (const size_t n = layer->index().symbols_of_kind(static_cast<ReferenceKind>(k)).size())
			kinds[reference_row(static_cast<ReferenceKind>(k)).token] = n;
	std::printf("a base layer over the JO install: %zu files, %zu read, %zu did not read, %zu "
				"symbols, built in %.2f s\n",
			layer->file_count(), built.files_extracted, built.files_failed, layer->symbol_count(),
			seconds);
	for (const auto &entry : kinds)
		std::printf("  %-14s %6zu\n", entry.first.c_str(), entry.second);
	TEST_EXPECT(layer->index().edge_count() == 0 && layer->file_named("menu_style.mns") &&
			layer->file_named("main.mnu"));
	TEST_EXPECT(kinds["style_var"] > 0 && kinds["menu_screen"] > 0 && kinds["text_id"] > 0 &&
			kinds["weapon"] > 0 && kinds["item"] > 0);
	// A screen of the install's main menu, and a texture it has.
	std::string screen_name, texture;
	for (const GraphIndex::Ref ref : layer->index().symbols_of_kind(ReferenceKind::MenuScreen)) {
		const GraphSymbol &symbol = layer->index().symbol(ref);
		if (symbol.file == layer->file_named("main.mnu")->path && !symbol.inert) {
			screen_name = symbol.display;
			break;
		}
	}
	layer->index().for_each_slot([&](uint32_t id) {
		const GraphSlot &slot = layer->index().slot(id);
		if (texture.empty() && slot.kind == AssetKind::Texture &&
				opennova::strutil::ends_with_icase(slot.logical_name, ".tga"))
			texture = slot.logical_name;
	});
	TEST_EXPECT(!screen_name.empty() && !texture.empty());
	TEST_EXPECT(rewrite(project.file("mine.mnu"),
			screen("MINE",
					window("STATIC", "W1", font("%DEF_FONTNAME_LG%") + image(texture)) +
							window("BUTTON", "W2", go_screen("main.mnu", screen_name)) +
							window("STATIC", "W3", image("not_in_the_install.tga")))));
	AssetGraph graph;
	graph.set_base(layer);
	graph.update(project.paths, project.document, project.rescan(), {});
	std::string file;
	TEST_EXPECT(graph.resolve(ReferenceKind::StyleVar, "%DEF_FONTNAME_LG%") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::Font, "%DEF_FONTNAME_LG%", std::string(), &file) ==
					ReferenceStatus::Present &&
			layer->file_named(file));
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuTexture, texture) == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::MenuScreen, screen_name, "MAIN.MNU") ==
			ReferenceStatus::Present);
	TEST_EXPECT(graph.missing_count() == 1 && graph.diagnostics().size() == 1 &&
	            graph.diagnostics()[0].message.find("not_in_the_install.tga") != std::string::npos);
	Seen seen({"main", "font"});
	TEST_EXPECT(
			fresh_difference(graph, project.paths, project.document, project.scan, {}, seen, layer)
					.empty());
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_generation();
	failures += test_incremental_equals_fresh();
	failures += test_patch_counts();
	failures += test_references_of();
	failures += test_base_layer();
	failures += test_base_layer_file_set();
	failures += test_retail_incremental();
	failures += test_retail_base_layer();
	failures += test_record_references();
	failures += test_retail_record_references();
	failures += test_reference_kind_rows();
	failures += test_reference_file_candidates();
	failures += test_terrain_and_bank_extractors();
	failures += test_model_texture_references();
	failures += test_model_shader_references();
	failures += test_rename_keeps_loader_spelling();
	failures += test_user_point_references();
	failures += test_blank_project();
	failures += test_menu_names_and_targets();
	failures += test_retail_menu_graph();
	failures += test_menu_text_scope();
	failures += test_stylesheet_bindings();
	failures += test_native_extractors();
	failures += test_catalog_symbols();
	failures += test_symbol_locators();
	failures += test_rename();
	failures += test_rename_rewrites_planned_sites_only();
	failures += test_rename_by_locator();
	if (failures == 0) std::printf("editor_asset_graph: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
