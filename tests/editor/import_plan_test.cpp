// The plan of an import with the files it needs (ADR 0046 S11f, editor/import/import_plan):
// over fixtures made in temp folders, a menu with its font and texture beside it (found in
// the folder, spelled as the folder spells them, placed where import_assets puts them),
// what the project already reads or the selection brings taking no row, a font through a
// variable of the stylesheet the selection brings, a name the archives cannot store; an
// archive whose members name each other; a fake game install providing what the folder
// lacks, one found nowhere a row of its own, an install that does not mount; every other
// place with a file for a reference reported beside the file taken (the bytes differing or
// not), a later reference's and the selection's own file's included; an .o3d beside its
// textures (the model's texture rows found by their type's rule, a missing one listed,
// nothing the converter copies); an .mdt and a chunk file found, imported, resolved and
// packed; the stylesheets the shell reads once the import is in; two lookups of one name
// kept apart; a candidate taken for what it is, not its name; a PNG dependency copied as
// the game's own and resolving; a bare relative source; a cycle of menus ending; the cap
// stopping the walk and the selection, a converter's outputs whole or not at all; a symbol,
// a sound and a mission text (a .mis) listed as not followed; and nothing written
// anywhere. S13 D5: what goes unread is the kinds table's rule (a kind that names files the
// graph does not read), the kinds the hand list named before and a face and a map project.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <base/io/file_io.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/blank/blank_factory.h>
#include <editor/import/import_plan.h>
#include <editor/import/sidecar.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/lwf/lwf.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_mis.h>
#include <formats/pff/pff.h>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/import_test_support.h"
#include "common/png_test_support.h"

using namespace opennova::editor;
using opennova::pff::normalized_logical_name;
using namespace import_test;
namespace fs = std::filesystem;

namespace {

using State = ImportPlanRow::State;

// A file a model's chunk row reads (renderer::load_material_chunk): an 8-byte header, then
// an NQ8B chunk of a 2 x 2 image, its size at +12 and its width and height at +28 and +32.
std::vector<uint8_t> chunk_file() {
	std::vector<uint8_t> bytes(8 + 8 + 28 + 2 * 2 * 4, 0);
	const auto put = [&bytes](size_t at, uint32_t value) {
		for (size_t i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
	};
	const char tag[] = {'N', 'Q', '8', 'B'};
	for (size_t i = 0; i < 4; ++i) bytes[8 + i] = uint8_t(tag[i]);
	put(12, uint32_t(bytes.size() - 16));
	put(28, 2);
	put(32, 2);
	return bytes;
}

} // namespace

// A menu with its font and texture beside it: the menu selected, the two found in its
// folder (as the folder spells them, placed where import_assets puts them, the reference
// that wanted each named); a font the project has takes no row, nor one the selection
// brings; a font through a variable of the stylesheet the selection brings; a texture name
// the archives cannot store says so; without the dependencies, the selection alone.
static int test_plan_folder() {
	Project project("opennova_editor_plan_folder");
	const std::string root = project.root();
	TEST_EXPECT(editor_test::write_text(root + "/fonts/have.fnt", "fnt"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", font("arial99") + image("logo.tga")) +
	                                                                    window("STATIC", "KEEP", font("have")))));
	TEST_EXPECT(editor_test::write_text(art + "/arial99.fnt", "fnt") && editor_test::write_text(art + "/LOGO.TGA", "tga") &&
	            editor_test::write_text(art + "/unused.pcx", "pcx"));
	const auto before = snapshot(project.dir.path);
	const ImportPlan plan = project.plan({{art + "/a.mnu", {}}});
	TEST_EXPECT(snapshot(project.dir.path) == before);
	TEST_EXPECT(plan.rows.size() == 3 && plan.diagnostics.empty() && !plan.truncated);
	const ImportPlanRow *menu = row_named(plan, "a.mnu");
	TEST_EXPECT(menu && menu->state == State::Selected && menu->selected && menu->kind == AssetKind::Menu &&
	            menu->destination == "menus/a.mnu" && menu->found_in == "the folder " + art && menu->made_from.empty() &&
	            menu->needed_by.file.empty() && menu->problem.empty() && menu->source.path == art + "/a.mnu");
	const ImportPlanRow *arial = row_named(plan, "arial99.fnt");
	TEST_EXPECT(arial && arial->state == State::Found && arial->selected && arial->kind == AssetKind::Font &&
	            arial->destination == "fonts/arial99.fnt" && arial->found_in == "the folder " + art &&
	            arial->source.path == art + "/arial99.fnt" && arial->source.entry.empty() && !arial->source.install &&
	            arial->source.native && !menu->source.native);
	TEST_EXPECT(arial && arial->needed_by.file == "a.mnu" && arial->needed_by.record == "A/GO" &&
	            arial->needed_by.field == "font.name" && arial->needed_by.reference == ReferenceKind::Font &&
	            arial->needed_by.name == "arial99" && arial->rivals.empty() && arial->problem.empty());
	const ImportPlanRow *logo = row_named(plan, "LOGO.TGA");
	TEST_EXPECT(logo && logo->state == State::Found && logo->kind == AssetKind::Texture && logo->destination == "textures/LOGO.TGA" &&
	            logo->needed_by.reference == ReferenceKind::MenuTexture && logo->needed_by.name == "logo.tga");
	TEST_EXPECT(!row_named(plan, "have.fnt") && !row_named(plan, "unused.pcx"));
	// The font selected too: it is the selection's, no dependency row.
	const ImportPlan both = project.plan({{art + "/a.mnu", {}}, {art + "/arial99.fnt", {}}});
	const ImportPlanRow *selected_font = row_named(both, "arial99.fnt");
	TEST_EXPECT(both.rows.size() == 3 && selected_font && selected_font->state == State::Selected);
	// Without the dependencies: the selection alone.
	const ImportPlan alone = project.plan({{art + "/a.mnu", {}}}, false);
	TEST_EXPECT(alone.rows.size() == 1 && alone.rows[0].name == "a.mnu" && alone.not_followed.empty());

	// A font through a variable only the stylesheet the selection brings defines (the game
	// reads menu_style.mns), and a texture whose name the archives cannot store.
	TEST_EXPECT(editor_test::write_text(art + "/b.mnu", screen("B", window("STATIC", "LOGO", font("%FONT_X%") +
	                                                                                     image("a_long_texture_name.tga")))));
	TEST_EXPECT(editor_test::write_text(art + "/menu_style.mns", "FONT_X styled.fnt\r\n"));
	TEST_EXPECT(editor_test::write_text(art + "/styled.fnt", "fnt") && editor_test::write_text(art + "/a_long_texture_name.tga", "tga"));
	const ImportPlan styled = project.plan({{art + "/b.mnu", {}}, {art + "/menu_style.mns", {}}});
	const ImportPlanRow *font_row = row_named(styled, "styled.fnt");
	TEST_EXPECT(font_row && font_row->state == State::Found && font_row->needed_by.file == "b.mnu" &&
	            font_row->needed_by.name == "styled.fnt" && font_row->problem.empty());
	// Found, but the project cannot take it: listed with its problem, not taken.
	const ImportPlanRow *long_name = row_named(styled, "a_long_texture_name.tga");
	TEST_EXPECT(long_name && long_name->state == State::Found && !long_name->selected &&
	            long_name->problem.find("16 characters") != std::string::npos);
	// The variable's own reference is a symbol the selection's stylesheet defines (S14): followed
	// to nothing.
	TEST_EXPECT(!not_followed(styled, ReferenceKind::StyleVar) && styled.undefined.empty() && styled.rows.size() == 4);

	// A menu found that does not read: taken, its references not looked for, said once.
	TEST_EXPECT(editor_test::write_text(art + "/c.mnu", screen("C", window("BUTTON", "GO", go_to("broken.mnu", "X")))));
	TEST_EXPECT(editor_test::write_bytes(art + "/broken.mnu", {0xFF, 0xFE, 0x41}));
	const ImportPlan broken = project.plan({{art + "/c.mnu", {}}});
	size_t unreadable = 0;
	for (const Diagnostic &d : broken.diagnostics) unreadable += d.code() == "import.unreadable" && d.asset == "broken.mnu" ? 1 : 0;
	const ImportPlanRow *broken_row = row_named(broken, "broken.mnu");
	TEST_EXPECT(unreadable == 1 && broken_row && broken_row->state == State::Found && broken.rows.size() == 2);
	return 0;
}

// An archive whose members name each other: the member selected, the menu and the texture it
// names found in the archive, each a member import_assets takes.
static int test_plan_archive() {
	Project project("opennova_editor_plan_archive");
	const std::string archive = project.dir.file("mod/menus.pff");
	TEST_EXPECT(write_pff(archive, {{"first.mnu", screen("FIRST", window("BUTTON", "GO", image("tex.pcx") + go_to("second.mnu", "SECOND")))},
	                                {"second.mnu", screen("SECOND", window("STATIC", "BACK", image("tex.pcx")))},
	                                {"tex.pcx", "pcx"},
	                                {"other.txt", "text"}}));
	const auto before = snapshot(project.dir.path);
	ImportChoice member;
	member.path = archive;
	member.entry = "first.mnu";
	const ImportPlan plan = project.plan({member});
	TEST_EXPECT(snapshot(project.dir.path) == before);
	TEST_EXPECT(plan.rows.size() == 3 && plan.diagnostics.empty());
	const ImportPlanRow *first = row_named(plan, "first.mnu");
	TEST_EXPECT(first && first->state == State::Selected && first->found_in == "the archive " + archive &&
	            first->source.path == archive && first->source.entry == "first.mnu");
	for (const char *name : {"second.mnu", "tex.pcx"}) {
		const ImportPlanRow *row = row_named(plan, name);
		TEST_EXPECT(row && row->state == State::Found && row->selected && row->found_in == "the archive " + archive &&
		            row->source.path == archive && row->source.entry == name && !row->source.install);
	}
	const ImportPlanRow *second = row_named(plan, "second.mnu");
	const ImportPlanRow *tex = row_named(plan, "tex.pcx");
	TEST_EXPECT(second && second->kind == AssetKind::Menu && tex && tex->kind == AssetKind::Texture);
	TEST_EXPECT(second && second->needed_by.reference == ReferenceKind::Menu && second->needed_by.file == "first.mnu");
	// The target screen is a symbol the planned second.mnu defines (S14): neither not followed
	// nor undefined.
	TEST_EXPECT(!not_followed(plan, ReferenceKind::MenuScreen) && plan.undefined.empty());
	return 0;
}

// A game install providing what the folder lacks; a file both have: the folder's taken, the
// install's reported beside it with whether the bytes differ; one found nowhere, a row of its
// own (named once); a source of the install looks in the install; an install that does not
// mount is said, the folder still searched.
static int test_plan_game_install() {
	Project project("opennova_editor_plan_install");
	const std::string install = project.dir.file("install");
	TEST_EXPECT(write_pff(install + "/resource.pff",
	                      {{"arial99.fnt", "fnt"},
	                       {"logo.tga", "install tga"},
	                       {"shared.pcx", "same"},
	                       {"retail.mnu", screen("RETAIL", window("STATIC", "R", font("arial99") + image("logo.tga")))}}));
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu",
	                                    screen("A", window("BUTTON", "GO", font("arial99") + image("logo.tga")) +
	                                                    window("STATIC", "SHARED", image("shared.pcx")) +
	                                                    window("STATIC", "GONE", image("nowhere.tga")) +
	                                                    window("STATIC", "GONE2", image("nowhere.tga")))));
	TEST_EXPECT(editor_test::write_text(art + "/logo.tga", "folder tga") && editor_test::write_text(art + "/shared.pcx", "same"));
	// A loose logo.tga beside the install's archive, as the folder's: a stock launch reads
	// the archive's (the loose file wins only under /d) [orig: FileSystem_OpenFile @
	// 0x75b1c0, the gate @ 0x75b1e5], so the install's rival still differs.
	TEST_EXPECT(editor_test::write_text(install + "/logo.tga", "folder tga"));
	const auto before = snapshot(project.dir.path);
	const ImportPlan plan = project.plan({{art + "/a.mnu", {}}}, true, install);
	TEST_EXPECT(snapshot(project.dir.path) == before);
	TEST_EXPECT(plan.diagnostics.empty() && plan.rows.size() == 5);
	const ImportPlanRow *arial = row_named(plan, "arial99.fnt");
	TEST_EXPECT(arial && arial->state == State::Found && arial->found_in == "the game install" && arial->source.install &&
	            arial->source.path == install && arial->source.entry == "arial99.fnt" && arial->rivals.empty());
	const ImportPlanRow *logo = row_named(plan, "logo.tga");
	TEST_EXPECT(logo && logo->state == State::Found && logo->found_in == "the folder " + art && logo->rivals.size() == 1);
	TEST_EXPECT(logo && logo->rivals[0].found_in == "the game install" && logo->rivals[0].name == "logo.tga" &&
	            logo->rivals[0].source.install && logo->rivals[0].differs);
	const ImportPlanRow *shared = row_named(plan, "shared.pcx");
	TEST_EXPECT(shared && shared->found_in == "the folder " + art && shared->rivals.size() == 1 && !shared->rivals[0].differs);
	const ImportPlanRow *gone = row_named(plan, "nowhere.tga");
	TEST_EXPECT(gone && gone->state == State::NotFound && !gone->selected && gone->source.path.empty() &&
	            gone->destination.empty() && gone->kind == AssetKind::Texture &&
	            gone->needed_by.record == "A/GONE/Appearance 1" && gone->needed_by.field == "value" &&
	            gone->needed_by.reference == ReferenceKind::MenuTexture);
	// Without the install, its font is found nowhere (the row named as the menu names it).
	const ImportPlan folder_only = project.plan({{art + "/a.mnu", {}}});
	const ImportPlanRow *lost = row_named(folder_only, "arial99");
	const ImportPlanRow *alone = row_named(folder_only, "logo.tga");
	TEST_EXPECT(lost && lost->state == State::NotFound && lost->kind == AssetKind::Font && alone && alone->rivals.empty());
	// A source of the install: what it names is looked for in the install, no competition.
	ImportChoice retail;
	retail.path = install;
	retail.entry = "retail.mnu";
	retail.install = true;
	const ImportPlan from_install = project.plan({retail}, true, install);
	TEST_EXPECT(from_install.rows.size() == 3 && from_install.rows[0].found_in == "the game install");
	const ImportPlanRow *installed = row_named(from_install, "logo.tga");
	TEST_EXPECT(installed && installed->found_in == "the game install" && installed->rivals.empty());
	// An install that does not mount: said, the folder still searched.
	const ImportPlan unmounted = project.plan({{art + "/a.mnu", {}}}, true, project.dir.file("nowhere"));
	TEST_EXPECT(has_code(unmounted.diagnostics, "import.install") && !has_error(unmounted.diagnostics));
	const ImportPlanRow *folder_logo = row_named(unmounted, "logo.tga");
	const ImportPlanRow *no_font = row_named(unmounted, "arial99");
	TEST_EXPECT(folder_logo && folder_logo->state == State::Found && no_font && no_font->state == State::NotFound);
	return 0;
}

// An .o3d beside its textures: converted in memory, its model the selected row (made from the
// scene, placed with the models); the texture its model's diffuse row names found in the
// folder by the row's rule, one the folder lacks listed as not found (the converter copies
// none); the .dds beside a diffuse row's .tga is the file the loader opens.
static int test_plan_scene() {
	Project project("opennova_editor_plan_scene");
	const std::string scene = project.dir.file("scene");
	std::error_code ec;
	fs::create_directories(scene, ec);
	const fs::path fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "o3d" / "spinner.o3d";
	fs::copy_file(fixture, scene + "/spinner.o3d", ec);
	TEST_EXPECT(!ec && editor_test::write_text(scene + "/SPINNER.TGA", "tga")); // spinner.o3d names spinner.tga and glow.tga
	const auto before = snapshot(project.dir.path);
	const ImportPlan plan = project.plan({{scene + "/spinner.o3d", {}}});
	TEST_EXPECT(snapshot(project.dir.path) == before);
	TEST_EXPECT(!has_error(plan.diagnostics));
	TEST_EXPECT(plan.rows.size() == 3);
	const ImportPlanRow *model = row_named(plan, "spinner.3di");
	TEST_EXPECT(model && model->state == State::Selected && model->kind == AssetKind::Model &&
	            model->made_from == "spinner.o3d" && model->destination == "models/spinner.3di" &&
	            model->source.path == scene + "/spinner.o3d");
	const ImportPlanRow *skin = row_named(plan, "SPINNER.TGA");
	TEST_EXPECT(skin && skin->state == State::Found && skin->found_in == "the folder " + scene &&
	            skin->needed_by.file == "spinner.3di" && skin->needed_by.field == "name" &&
	            skin->needed_by.reference == ReferenceKind::Texture && skin->needed_by.loader_arg == 0);
	const ImportPlanRow *glow = row_named(plan, "glow.tga");
	TEST_EXPECT(glow && glow->state == State::NotFound && glow->needed_by.file == "spinner.3di");
	// The .dds beside it: a diffuse row loads it.
	TEST_EXPECT(editor_test::write_text(scene + "/glow.dds", "dds"));
	const ImportPlan with_dds = project.plan({{scene + "/spinner.o3d", {}}});
	const ImportPlanRow *dds = row_named(with_dds, "glow.dds");
	TEST_EXPECT(with_dds.rows.size() == 3 && !row_named(with_dds, "glow.tga") && dds && dds->state == State::Found);
	return 0;
}

// Menu A names menu B, which names A: the walk ends with each once. A cap on the files
// planned stops the walk and the selection, the plan saying it was cut short; the files one
// converter source makes are planned whole or not at all.
static int test_plan_cycle_and_cap() {
	Project project("opennova_editor_plan_cycle");
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", font("fa") + go_to("b.mnu", "B")))));
	TEST_EXPECT(editor_test::write_text(art + "/b.mnu", screen("B", window("BUTTON", "BACK", font("fb") + go_to("a.mnu", "A")))));
	TEST_EXPECT(editor_test::write_text(art + "/fa.fnt", "fnt") && editor_test::write_text(art + "/fb.fnt", "fnt"));
	const ImportPlan plan = project.plan({{art + "/a.mnu", {}}});
	TEST_EXPECT(!plan.truncated && plan.rows.size() == 4);
	size_t menus = 0;
	for (const ImportPlanRow &row : plan.rows) menus += row.kind == AssetKind::Menu ? 1 : 0;
	const ImportPlanRow *b = row_named(plan, "b.mnu");
	const ImportPlanRow *fb = row_named(plan, "fb.fnt");
	TEST_EXPECT(menus == 2 && b && b->state == State::Found && fb && fb->needed_by.file == "b.mnu");
	const ImportPlan capped = project.plan({{art + "/a.mnu", {}}}, true, std::string(), 2);
	size_t taken = 0;
	for (const ImportPlanRow &row : capped.rows) taken += row.selected ? 1 : 0;
	TEST_EXPECT(capped.truncated && taken == 2 && capped.rows.size() == 2);
	// The cap binds the selection too, followed or not: three fonts, room for two.
	TEST_EXPECT(editor_test::write_text(art + "/fc.fnt", "fnt"));
	const std::vector<ImportChoice> fonts = {{art + "/fa.fnt", {}}, {art + "/fb.fnt", {}}, {art + "/fc.fnt", {}}};
	for (const bool follow : {true, false}) {
		const ImportPlan roots = project.plan(fonts, follow, std::string(), 2);
		TEST_EXPECT(roots.truncated && roots.rows.size() == 2 && roots.rows[1].name == "fb.fnt");
		TEST_EXPECT(!project.plan(fonts, follow, std::string(), 3).truncated);
	}
	// And a converter's outputs, which come together: a clip set's table and clip with room
	// for one take neither; after a font, room for two takes the font alone.
	TEST_EXPECT(editor_test::write_text(art + "/walk.o3a",
	                                    "o3a 1\nadm CHECK.adm\nrow anim_reset \"walk\"\nclip walk\nfps 30\nflags 0x1\nframes 1\n"
	                                    "bone -1 0 0 0 0.5 \"BN01 Pelvis\"\n k 0 0 0 1\n k 0 0 0 1\n"
	                                    "event 0 0 0 0x0 0.9 1.7\nevent 0 0 0 0x0 0.9 1.7\n"));
	const ImportPlan clips = project.plan({{art + "/walk.o3a", {}}}, true, std::string(), 1);
	TEST_EXPECT(clips.truncated && clips.rows.empty());
	const ImportPlan after_font = project.plan({{art + "/fa.fnt", {}}, {art + "/walk.o3a", {}}}, false, std::string(), 2);
	TEST_EXPECT(after_font.truncated && after_font.rows.size() == 1 && after_font.rows[0].name == "fa.fnt");
	const ImportPlan both_clips = project.plan({{art + "/walk.o3a", {}}}, true, std::string(), 2);
	TEST_EXPECT(!both_clips.truncated && both_clips.rows.size() == 2);
	return 0;
}

// A terrain the game admits (load_trn's gate: a colour map, a detail map, height data, a sector
// grid of a power of two), its files named from `stem`, with one foliage block.
static std::string terrain_text(const std::string &stem) {
	return "terrain_name \"" + stem + "\"\r\npolytrn_colormap " + stem + "_c.tga\r\npolytrn_detailmap det.tga\r\n"
	       "polytrn_polydata " + stem + ".cpt\r\npolytrn_sectorcount 1\r\npolytrn_sectors 1\r\n"
	       "foliage\r\n  graphic palm\r\nend\r\n";
}

// What the walk does not follow, and what it follows to nothing: a def's sound, a set no bank of the
// folder defines (the sound lane: a set is a bank's symbol), undefined with where it was met first; a
// menu's SCREEN target its own menu defines is followed to nothing (S14: neither not
// followed nor undefined); a terrain a mission names found, taken and read (S14): its height data
// and its colour map found beside it, the detail map and the foliage model the folder lacks not
// found; the mission's dialog bank, a kind the graph does not read, listed once by its kind; a
// mission's .mis taken, what it names not looked for (the graph reads the .bms alone), listed by
// its kind, and no unreadable file.
static int test_plan_not_followed() {
	Project project("opennova_editor_plan_not_followed");
	const std::string art = project.dir.file("art");
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		std::string error;
		TEST_EXPECT(opennova::mission::set_header_string(mission, "terrain", "island", error));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(mission, bytes, error));
		TEST_EXPECT(editor_test::write_bytes(art + "/m.bms", bytes));
	}
	TEST_EXPECT(editor_test::write_text(art + "/island.trn", terrain_text("island")) &&
	            editor_test::write_text(art + "/island.cpt", "cpt") && editor_test::write_text(art + "/island_c.tga", "tga") &&
	            editor_test::write_text(art + "/m.dbf", "dbf"));
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", go_to("a.mnu", "A")))));
	TEST_EXPECT(editor_test::write_text(art + "/items.def", "begin \"Boom\"\nid 100100\ntype building\nsounddeath boom\nend\n"));
	const ImportPlan plan = project.plan({{art + "/m.bms", {}}, {art + "/a.mnu", {}}, {art + "/items.def", {}}});
	const ImportPlanRow *terrain = row_named(plan, "island.trn");
	TEST_EXPECT(terrain && terrain->state == State::Found && terrain->kind == AssetKind::Terrain &&
	            terrain->needed_by.file == "m.bms" && terrain->needed_by.reference == ReferenceKind::Terrain);
	const ImportPlanRow *heights = row_named(plan, "island.cpt"), *colour = row_named(plan, "island_c.tga");
	TEST_EXPECT(heights && heights->state == State::Found && heights->kind == AssetKind::TerrainPolyData &&
	            heights->needed_by.file == "island.trn" && heights->needed_by.field == "polytrn_polydata" &&
	            heights->needed_by.reference == ReferenceKind::TerrainData);
	TEST_EXPECT(colour && colour->state == State::Found && colour->needed_by.file == "island.trn" &&
	            colour->needed_by.reference == ReferenceKind::Texture);
	const ImportPlanRow *detail = row_named(plan, "det.tga"), *palm = row_named(plan, "palm");
	TEST_EXPECT(detail && detail->state == State::NotFound && palm && palm->state == State::NotFound &&
	            palm->kind == AssetKind::Model && palm->needed_by.record == "Terrain/Foliage 1");
	TEST_EXPECT(!not_followed(plan, ReferenceKind::None, AssetKind::Terrain));
	// The dialog bank is a document the graph reads (DI-32): its lines' waves followed, nothing left unfollowed.
	TEST_EXPECT(!not_followed(plan, ReferenceKind::None, AssetKind::DialogBank));
	TEST_EXPECT(!not_followed(plan, ReferenceKind::MenuScreen) && !not_followed(plan, ReferenceKind::Sound));
	// Needed by in a modder's words: the definition's record by its kind and name, its field by the label the
	// definitions give it (def_words), never the key as written.
	const std::string vehicles = project.dir.file("vehicles");
	TEST_EXPECT(editor_test::write_text(vehicles + "/items.def", "begin \"Dune Buggy\"\nid 100200\ntype building\ndefault_aip buggy.aip\nend\n") &&
	            editor_test::write_text(vehicles + "/buggy.aip", "aip"));
	const ImportPlan worded = project.plan({{vehicles + "/items.def", {}}});
	const ImportPlanRow *profile = row_named(worded, "buggy.aip");
	TEST_EXPECT(profile && profile->needed_by.file == "items.def" && profile->needed_by.field == "default_aip");
	TEST_EXPECT(profile && profile->needed_by.words.find("Default AI profile") != std::string::npos &&
	            profile->needed_by.words.find("default_aip") == std::string::npos);
	if (profile) std::printf("needed by: %s\n", import_need_text(profile->needed_by).c_str());
	// A model planned alone comes with its rig: the animation table the item that shows it pairs with it in the
	// place's items.def (its anim_def beside its graphic), as the model preview plays it.
	{
		const std::string rigs = project.dir.file("rigs");
		TEST_EXPECT(editor_test::write_text(rigs + "/items.def",
		                                    "begin \"Rifleman\"\nid 100300\ntype building\ngraphic onjo\nanim_def onjo.adm\nend\n") &&
		            editor_test::write_text(rigs + "/onjo.adm", "adm") && editor_test::write_text(rigs + "/other.adm", "adm"));
		std::vector<uint8_t> model;
		std::string unread;
		TEST_EXPECT(opennova::io::read_file_bytes(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/armory.3di",
		                                          model, unread));
		TEST_EXPECT(editor_test::write_bytes(rigs + "/onjo.3di", model));
		const ImportPlan rigged = project.plan({{rigs + "/onjo.3di", {}}});
		const ImportPlanRow *table = row_named(rigged, "onjo.adm");
		TEST_EXPECT(table && table->state == State::Found && table->kind == AssetKind::AnimationMap &&
		            table->needed_by.file == "items.def" && table->needed_by.field == "anim_def" &&
		            table->needed_by.reference == ReferenceKind::AnimationMap);
		TEST_EXPECT(!row_named(rigged, "other.adm") && !row_named(rigged, "items.def"));
		// Without the dependencies, the model alone.
		TEST_EXPECT(!row_named(project.plan({{rigs + "/onjo.3di", {}}}, false), "onjo.adm"));
	}
	TEST_EXPECT(plan.undefined.size() == 1 && plan.undefined[0].reference == ReferenceKind::Sound &&
	            plan.undefined[0].count == 1 && plan.undefined[0].first == "items.def");
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		std::string error, text;
		TEST_EXPECT(opennova::mission::set_header_string(mission, "terrain", "island", error));
		TEST_EXPECT(opennova::mission::set_header_string(mission, "environment", "day", error));
		TEST_EXPECT(opennova::mission::write_mis_text(mission, text, error));
		TEST_EXPECT(editor_test::write_text(art + "/m.mis", text));
	}
	TEST_EXPECT(editor_test::write_text(art + "/day.env", "env"));
	// A mission text (S14: its own kind, which the game never reads): taken, what it names listed
	// as not followed by its kind.
	const ImportPlan text_plan = project.plan({ { art + "/m.mis", {} } });
	const ImportNotFollowed *mis = not_followed(text_plan, ReferenceKind::None, AssetKind::MissionText);
	TEST_EXPECT(mis && mis->count == 1 && mis->first == "m.mis");
	TEST_EXPECT(text_plan.rows.size() == 1 && row_named(text_plan, "m.mis") &&
			row_named(text_plan, "m.mis")->kind == AssetKind::MissionText);
	for (const Diagnostic &d : text_plan.diagnostics)
		TEST_EXPECT(d.code() != "import.unreadable");
	// A face names its textures (S18: read by the graph), followed: its base texture found beside it; a wave
	// names nothing.
	TEST_EXPECT(editor_test::write_text(art + "/head.grm", "basetexture face.tga\r\n") &&
	            editor_test::write_text(art + "/face.tga", "tga") && editor_test::write_text(art + "/boom.wav", "RIFF"));
	const ImportPlan face_plan = project.plan({{art + "/head.grm", {}}, {art + "/boom.wav", {}}});
	TEST_EXPECT(!not_followed(face_plan, ReferenceKind::None, AssetKind::FaceAnimation) && row_named(face_plan, "head.grm"));
	const ImportPlanRow *base = row_named(face_plan, "face.tga");
	TEST_EXPECT(base && base->state == State::Found && base->needed_by.file == "head.grm");
	const ImportPlanRow *wave = row_named(face_plan, "boom.wav");
	TEST_EXPECT(wave && wave->kind == AssetKind::Wave && wave->problem.empty());
	// A script is followed whole (S14): the script its RUN names (by the name written, the kind's
	// extension reaching the file) and the wave it plays, each found beside it; nothing of it not
	// followed.
	TEST_EXPECT(editor_test::write_text(art + "/m.wac", "If true(bluekills) then\r\n\twave \"boom.wav\"\r\nendif\r\nRUN other\r\n") &&
	            editor_test::write_text(art + "/other.wac", "; the other\r\n"));
	const ImportPlan script_plan = project.plan({{art + "/m.wac", {}}});
	const ImportPlanRow *run = row_named(script_plan, "other.wac"), *played = row_named(script_plan, "boom.wav");
	TEST_EXPECT(run && run->state == State::Found && run->kind == AssetKind::Script && run->needed_by.file == "m.wac" &&
	            run->needed_by.reference == ReferenceKind::Script && run->needed_by.name == "other");
	TEST_EXPECT(played && played->state == State::Found && played->kind == AssetKind::Wave &&
	            played->needed_by.file == "m.wac" && played->needed_by.reference == ReferenceKind::Wave);
	TEST_EXPECT(script_plan.rows.size() == 3 && script_plan.not_followed.empty());
	return 0;
}

// What an import does not follow is the kinds table's rule (S13 D5): a file's references go
// unread when its kind names files (AssetKindRow::names_files) and the graph does not read the
// kind (graph_reads_kind). Those are the kinds the hand-written list named (the def tables beyond the
// catalogs and the avatar table; S14 reads a terrain and a sound bank, DI-32 a dialog bank, and a
// music bank holds its own audio and names no file) and the one S13 D5 added that names files (a map
// project); a mission text, the original editor's .mis, a kind of its own since S14
// (the graph reads the .bms the game loads). A script the graph reads since S13 D9 (its operands'
// names) and, since S14, its RUN and its waves: it left the list. powerup.def left the list when
// the catalog opened it (S13 D10): the graph reads it through the catalog's records. hudpos.def
// left it with its extractor (S14): the HUD's fonts and textures are followed. A face left it with
// its extractor (S18): its textures are followed. SndProf.def left it with its document (the sound lane):
// each slot's set is followed. charattr.def left it with its type (DI-09's charattr follow-up): each read
// class's camouflage items are followed.
static int test_references_unread() {
	const std::set<AssetKind> unread = {AssetKind::MissionText,
	        AssetKind::HudFxDefs, AssetKind::OtherDefs, AssetKind::MapProject};
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKind kind = AssetKind(i);
		const AssetKindRow &row = asset_kind_row(kind);
		const bool rule = row.names_files && !graph_reads_kind(kind);
		TEST_EXPECT(references_unread(kind) == rule);
		if (rule != (unread.count(kind) > 0))
			std::fprintf(stderr, "references_unread(%s) moved\n", asset_kind_token(kind));
		TEST_EXPECT(rule == (unread.count(kind) > 0));
	}
	TEST_EXPECT(references_unread(AssetKind::MissionText) && !references_unread(AssetKind::Mission));
	return 0;
}

// Every place with a file for a reference is reported beside the file taken: two selected
// menus of two folders naming logo.pcx, each folder with its own, the first menu's taken
// and the second's its rival; a logo.pcx the selection brings itself, a later reference's
// folder holding another, that one its rival; the same bytes said to be the same.
static int test_plan_competition() {
	Project project("opennova_editor_plan_competition");
	const std::string a = project.dir.file("a"), b = project.dir.file("b");
	TEST_EXPECT(editor_test::write_text(a + "/a.mnu", screen("A", window("STATIC", "LOGO", image("logo.pcx")))));
	TEST_EXPECT(editor_test::write_text(b + "/b.mnu", screen("B", window("STATIC", "LOGO", image("logo.pcx")))));
	TEST_EXPECT(editor_test::write_text(a + "/logo.pcx", "pcx of a") && editor_test::write_text(b + "/logo.pcx", "pcx of b"));
	const ImportPlan plan = project.plan({{a + "/a.mnu", {}}, {b + "/b.mnu", {}}});
	const ImportPlanRow *logo = row_named(plan, "logo.pcx");
	TEST_EXPECT(plan.rows.size() == 3 && logo && logo->state == State::Found && logo->found_in == "the folder " + a &&
	            logo->needed_by.file == "a.mnu");
	TEST_EXPECT(logo && logo->rivals.size() == 1 && logo->rivals[0].found_in == "the folder " + b && logo->rivals[0].differs &&
	            logo->rivals[0].source.path == b + "/logo.pcx" && logo->rivals[0].source.native);
	const ImportPlan chosen = project.plan({{a + "/logo.pcx", {}}, {b + "/b.mnu", {}}});
	const ImportPlanRow *picked = row_named(chosen, "logo.pcx");
	TEST_EXPECT(chosen.rows.size() == 2 && picked && picked->state == State::Selected && picked->rivals.size() == 1 &&
	            picked->rivals[0].found_in == "the folder " + b && picked->rivals[0].differs);
	TEST_EXPECT(editor_test::write_text(b + "/logo.pcx", "pcx of a"));
	const ImportPlan same = project.plan({{a + "/a.mnu", {}}, {b + "/b.mnu", {}}});
	const ImportPlanRow *twin = row_named(same, "logo.pcx");
	TEST_EXPECT(twin && twin->rivals.size() == 1 && !twin->rivals[0].differs);
	return 0;
}

// A model's .mdt normal map is a texture to the plan and its chunk row's file a material chunk
// (by its bytes, whatever its name: renderer::load_material_chunk; a texture before S13 A8): found
// beside the model, a file of the name that holds no chunk not found; from a game install they
// import, the scan types the chunk file by its chunk headers, the project's graph resolves both
// rows, and the build packs both (a file of no kind the game knows it would leave out).
static int test_plan_material_sources() {
	Project project("opennova_editor_plan_material_sources");
	const std::string relief = "o3d 2\nmodel RELIEF\nmaterial FF_ST_OP\ntexture ready.mdt 3 4\ntexture field.nq8 1 16\n"
	                           "texture other.nq8 1 17\nlod 0\npart 0 0 0 0\nmesh 0 0\n"
	                           "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n";
	const std::string scene = project.dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/relief.o3d", relief) && editor_test::write_text(scene + "/ready.mdt", "mdt") &&
	            editor_test::write_bytes(scene + "/field.nq8", chunk_file()) &&
	            editor_test::write_text(scene + "/other.nq8", "no chunk here"));
	const ImportPlan beside = project.plan({{scene + "/relief.o3d", {}}});
	const ImportPlanRow *ready = row_named(beside, "ready.mdt");
	const ImportPlanRow *field = row_named(beside, "field.nq8");
	const ImportPlanRow *other = row_named(beside, "other.nq8");
	TEST_EXPECT(ready && ready->state == State::Found && ready->kind == AssetKind::Texture && ready->problem.empty());
	TEST_EXPECT(field && field->state == State::Found && field->kind == AssetKind::MaterialChunk && field->problem.empty());
	TEST_EXPECT(other && other->state == State::NotFound && beside.rows.size() == 4);

	const std::string install = project.dir.file("install");
	const std::vector<uint8_t> chunk = chunk_file();
	TEST_EXPECT(write_pff(install + "/resource.pff", {{"ready.mdt", "mdt"}, {"field.nq8", std::string(chunk.begin(), chunk.end())}}));
	const std::string bare = project.dir.file("bare");
	TEST_EXPECT(editor_test::write_text(bare + "/relief.o3d", relief));
	const ImportPlan plan = project.plan({{bare + "/relief.o3d", {}}}, true, install);
	for (const char *name : {"ready.mdt", "field.nq8"}) {
		const ImportPlanRow *row = row_named(plan, name);
		TEST_EXPECT(row && row->state == State::Found && row->found_in == "the game install" &&
		            row->kind == (std::string(name) == "ready.mdt" ? AssetKind::Texture : AssetKind::MaterialChunk));
	}
	const std::string root = project.root();
	const ImportResult result = import_assets(selected_sources(plan), ProjectPaths::for_root(root), *project.view().project.document, false);
	TEST_EXPECT(!has_error(result.diagnostics) && result.imported.size() == 3);
	project.session.handle(request::rescan());
	project.session.run_operations();
	const SessionView &view = project.view();
	// Once imported, the scan types the chunk file by its chunk headers (S13 A8): a material chunk.
	TEST_EXPECT(view.project.scan->find("field.nq8") && view.project.scan->find("field.nq8")->kind == AssetKind::MaterialChunk);
	size_t resolved = 0;
	for (const GraphEdge *edge : view.findings.graph->references_of("models/relief.3di"))
		if ((edge->value == "ready.mdt" || edge->value == "field.nq8") && view.findings.graph->resolve(*edge) == ReferenceStatus::Present)
			++resolved;
	TEST_EXPECT(resolved == 2);
	const BuildPlan build = plan_build(ProjectPaths::for_root(root), *view.project.scan, *view.project.requirements, {});
	size_t packed = 0;
	for (const BuildArchive &archive : build.archives)
		for (const BuildEntry &entry : archive.entries)
			packed += archive.slot == ArchiveSlot::Resource && (entry.logical_name == "ready.mdt" || entry.logical_name == "field.nq8");
	TEST_EXPECT(packed == 2);
	return 0;
}

// A %NAME% resolves through the stylesheets the project reads once the import is in, as the
// shell loads them: the selection's menu_style.mns replaces the project's, and the project's
// brand.mns is still read over it, so its FONT_X wins (brand.fnt, not the selection's
// base.fnt, whose own value then loads nothing); a variable only the replaced menu_style.mns
// defined is gone, its font not looked for.
static int test_plan_stylesheets() {
	Project project("opennova_editor_plan_stylesheets");
	const std::string root = project.root();
	TEST_EXPECT(editor_test::write_text(root + "/menus/menu_style.mns", "FONT_X old.fnt\r\nVAR_Y gone.fnt\r\n") &&
	            editor_test::write_text(root + "/menus/brand.mns", "FONT_X brand.fnt\r\n"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/menu_style.mns", "FONT_X base.fnt\r\n"));
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("STATIC", "X", font("%FONT_X%")) +
	                                                                    window("STATIC", "Y", font("%VAR_Y%")))));
	for (const char *name : {"base.fnt", "brand.fnt", "gone.fnt", "old.fnt"})
		TEST_EXPECT(editor_test::write_text(art + "/" + name, "fnt"));
	const ImportPlan plan = project.plan({{art + "/menu_style.mns", {}}, {art + "/a.mnu", {}}});
	const ImportPlanRow *brand = row_named(plan, "brand.fnt");
	TEST_EXPECT(brand && brand->state == State::Found && brand->needed_by.file == "a.mnu" &&
	            brand->needed_by.name == "brand.fnt");
	TEST_EXPECT(!row_named(plan, "base.fnt") && !row_named(plan, "gone.fnt") && !row_named(plan, "old.fnt") &&
	            plan.rows.size() == 3);
	return 0;
}

// Two rows of a model naming glow.tga, a diffuse one and a plain one, both missing: one row
// holding both lookups. A glow.dds another selected menu brings meets the diffuse row's (its
// loader takes the .dds sibling) and not the plain row's: the row stays, naming the plain one.
static int test_plan_missing_lookups() {
	Project project("opennova_editor_plan_missing_lookups");
	const std::string a = project.dir.file("a"), b = project.dir.file("b");
	TEST_EXPECT(editor_test::write_text(a + "/glow.o3d",
	                                    "o3d 2\nmodel GLOW\nmaterial FF_ST_OP\ntexture glow.tga 1 0\ntexture glow.tga 1 1\n"
	                                    "lod 0\npart 0 0 0 0\nmesh 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\n"
	                                    "v 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	const ImportPlan alone = project.plan({{a + "/glow.o3d", {}}});
	const ImportPlanRow *missing = row_named(alone, "glow.tga");
	TEST_EXPECT(alone.rows.size() == 2 && missing && missing->state == State::NotFound &&
	            missing->needed_by.loader_arg == 0);
	TEST_EXPECT(editor_test::write_text(b + "/b.mnu", screen("B", window("STATIC", "GLOW", image("glow.dds")))) &&
	            editor_test::write_text(b + "/glow.dds", "dds"));
	const ImportPlan both = project.plan({{a + "/glow.o3d", {}}, {b + "/b.mnu", {}}});
	const ImportPlanRow *dds = row_named(both, "glow.dds");
	const ImportPlanRow *still = row_named(both, "glow.tga");
	TEST_EXPECT(dds && dds->state == State::Found && still && still->state == State::NotFound &&
	            still->needed_by.loader_arg == 1 && both.rows.size() == 4);
	return 0;
}

// A candidate counts by what it is, not by its name: a raw labels.bin the selection brings
// is no string table for the menu naming it (the menu's table is not found: the project holds
// one file of a name), and a folder's raw labels.bin gives way to the game install's string
// table of the name.
static int test_plan_kinds() {
	Project project("opennova_editor_plan_kinds");
	std::vector<uint8_t> table;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "labels.bin";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	const std::string install = project.dir.file("install");
	TEST_EXPECT(write_pff(install + "/resource.pff", {{"labels.bin", std::string(table.begin(), table.end())}}));
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("STATIC", "TEXT", "<TEXT_RSRC>labels.bin</TEXT_RSRC>\r\n"))));
	TEST_EXPECT(editor_test::write_text(art + "/labels.bin", "raw bytes"));
	const ImportPlan selected = project.plan({{art + "/a.mnu", {}}, {art + "/labels.bin", {}}});
	size_t raw = 0, lost = 0;
	for (const ImportPlanRow &row : selected.rows) {
		if (row.name != "labels.bin") continue;
		raw += row.state == State::Selected && row.kind == AssetKind::RawBin;
		lost += row.state == State::NotFound && row.kind == AssetKind::Strings;
	}
	TEST_EXPECT(raw == 1 && lost == 1);
	const ImportPlan from_install = project.plan({{art + "/a.mnu", {}}}, true, install);
	const ImportPlanRow *found = row_named(from_install, "labels.bin");
	TEST_EXPECT(found && found->state == State::Found && found->found_in == "the game install" &&
	            found->kind == AssetKind::Strings && found->rivals.empty());
	return 0;
}

// A PNG a menu names beside it is found as the game's own file (native): imported as it is,
// with no import record, the menu's reference resolves. A PNG picked from the disk as a
// selected source stays the author's, its record written.
static int test_plan_native_png() {
	Project project("opennova_editor_plan_native_png");
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("STATIC", "LOGO", image("logo.png")))) &&
	            editor_test::write_bytes(art + "/logo.png", test_png::gradient_png(4, 4)));
	const ImportPlan plan = project.plan({{art + "/a.mnu", {}}});
	const ImportPlanRow *logo = row_named(plan, "logo.png");
	TEST_EXPECT(logo && logo->state == State::Found && logo->kind == AssetKind::Texture && logo->source.native);
	const std::string root = project.root();
	const ImportResult result = import_assets(selected_sources(plan), ProjectPaths::for_root(root), *project.view().project.document, false);
	TEST_EXPECT(!has_error(result.diagnostics) && result.imported.size() == 2);
	project.session.handle(request::rescan());
	project.session.run_operations();
	const SessionView &view = project.view();
	const AssetEntry *png = view.project.scan->find("logo.png");
	TEST_EXPECT(png && png->kind == AssetKind::Texture && !fs::exists(fs::path(root) / (png->relative_path + kImportSidecarSuffix)));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.png") == ReferenceStatus::Present);
	TEST_EXPECT(editor_test::write_bytes(art + "/badge.png", test_png::gradient_png(4, 4, 7)));
	const ImportResult authored = import_assets({{art + "/badge.png", {}}}, ProjectPaths::for_root(root), *view.project.document, false);
	TEST_EXPECT(authored.imported.size() == 1 && fs::exists(fs::path(root) / (authored.imported[0] + kImportSidecarSuffix)));
	return 0;
}

// A source named by a bare name, from the folder the editor runs in: that is the folder the
// files it names are looked for in.
static int test_plan_relative_path() {
	Project project("opennova_editor_plan_relative");
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("STATIC", "GO", font("arial99")))) &&
	            editor_test::write_text(art + "/arial99.fnt", "fnt"));
	std::error_code ec;
	const fs::path was = fs::current_path(ec);
	fs::current_path(art, ec);
	TEST_EXPECT(!ec);
	const ImportPlan plan = project.plan({{"a.mnu", {}}});
	fs::current_path(was, ec);
	const ImportPlanRow *font_row = row_named(plan, "arial99.fnt");
	TEST_EXPECT(plan.diagnostics.empty() && font_row && font_row->state == State::Found);
	TEST_EXPECT(font_row && fs::equivalent(font_row->source.path, art + "/arial99.fnt", ec));
	return 0;
}

// ADR 0046 S14: the plan a step at a time (ImportPlanner) is the plan plan_import makes in one
// call, row for row. A chain of eight menus in an archive, each naming a texture and the next
// menu: stepped a byte a step, each step takes one source or one file, the counts of files known
// and done only rise, and both end at the plan's files. Each row carries its size as its origin
// stores it, a texture's without its bytes being read: a menu naming eight textures of a megabyte
// each plans within one 64 KiB step, where reading them would take eight.
static int test_plan_steps() {
	Project project("opennova_editor_plan_steps");
	const std::string archive = project.dir.file("mod/chain.pff");
	std::vector<std::pair<std::string, std::string>> files;
	uint64_t bytes = 0;
	for (int i = 0; i < 8; ++i) {
		const std::string n = std::to_string(i), next = std::to_string(i + 1);
		const std::string menu = screen(("S" + n).c_str(), window("BUTTON", "GO",
				image("t" + n + ".pcx") + (i < 7 ? go_to("m" + next + ".mnu", ("S" + next).c_str()) : std::string())));
		files.push_back({"m" + n + ".mnu", menu});
		files.push_back({"t" + n + ".pcx", std::string(size_t(100 + i), 'x')});
		bytes += menu.size() + size_t(100 + i);
	}
	files.push_back({"unused.txt", "unused"});
	TEST_EXPECT(write_pff(archive, files));
	ImportChoice first;
	first.path = archive;
	first.entry = "m0.mnu";
	const ImportPlan whole = project.plan({first});
	TEST_EXPECT(whole.rows.size() == 16 && !whole.truncated && whole.diagnostics.empty());
	TEST_EXPECT(whole.file_count() == 16 && whole.total_bytes() == bytes);
	const ImportPlanRow *texture = row_named(whole, "t3.pcx");
	const ImportPlanRow *menu = row_named(whole, "m3.mnu");
	TEST_EXPECT(texture && texture->size == 103 && menu && menu->size == files[6].second.size());

	const SessionView &v = project.view();
	const ProjectPaths paths = ProjectPaths::for_root(v.project.root);
	ImportPlanner planner({first}, true, paths, *v.project.document, *v.project.scan, *v.findings.graph, std::string());
	size_t steps = 0, known = 0, done = 0;
	TEST_EXPECT(!planner.done());
	while (!planner.step(1)) {
		++steps;
		TEST_EXPECT(planner.files_known() >= known && planner.files_done() >= done &&
				planner.files_done() <= planner.files_known());
		known = planner.files_known();
		done = planner.files_done();
		TEST_EXPECT(steps < 1000);
	}
	// The install's step, the source, the stylesheets and a step for each file followed.
	TEST_EXPECT(planner.done() && steps >= 16 + 2);
	TEST_EXPECT(planner.files_known() == 16 && planner.files_done() == 16);
	const ImportPlan stepped = planner.take();
	TEST_EXPECT(same_import(whole, stepped) && stepped.total_bytes() == bytes);
	for (size_t i = 0; i < whole.rows.size(); ++i)
		TEST_EXPECT(whole.rows[i].size == stepped.rows[i].size && whole.rows[i].needed_by.file == stepped.rows[i].needed_by.file);
	// A file of the selection the listing lacks is said without a read; one chosen with no
	// dependencies is a row by its listing alone.
	ImportChoice absent = first, alone = first;
	absent.entry = "nowhere.mnu";
	alone.entry = "t0.pcx";
	const ImportPlan missing = project.plan({absent});
	TEST_EXPECT(missing.rows.empty() && has_code(missing.diagnostics, "import.read"));
	const ImportPlan single = project.plan({alone}, false);
	TEST_EXPECT(single.rows.size() == 1 && single.rows[0].kind == AssetKind::Texture && single.rows[0].size == 100);

	// A texture is never read: eight of a megabyte each cost their rows alone.
	const std::string art = project.dir.file("art");
	std::string windows;
	for (int i = 0; i < 8; ++i) {
		const std::string name = "big" + std::to_string(i) + ".tga";
		windows += window("STATIC", ("W" + std::to_string(i)).c_str(), image(name));
		TEST_EXPECT(editor_test::write_text(art + "/" + name, std::string(size_t(1) << 20, 't')));
	}
	TEST_EXPECT(editor_test::write_text(art + "/big.mnu", screen("BIG", windows)));
	ImportPlanner big({{art + "/big.mnu", {}}}, true, paths, *v.project.document, *v.project.scan, *v.findings.graph,
			std::string());
	size_t big_steps = 1;
	while (!big.step(uint64_t(64) << 10)) ++big_steps;
	const ImportPlan big_plan = big.take();
	TEST_EXPECT(big_plan.rows.size() == 9 && big_steps <= 4);
	TEST_EXPECT(big_plan.total_bytes() > (uint64_t(8) << 20));
	return 0;
}

// A sound bank of the singles given (each its name and its wave's path), with no set: what the
// graph reads of one.
static std::string bank_text(const std::vector<std::pair<std::string, std::string>> &singles) {
	opennova::lwf::File bank;
	for (const auto &[name, path] : singles) {
		opennova::lwf::Single single;
		single.name = name;
		single.path = path;
		bank.singles.push_back(single);
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::lwf::encode_lwf(bank, bytes, error)) return std::string();
	return std::string(bytes.begin(), bytes.end());
}

// ADR 0046 S14: a mission imported from a game install with its dependencies brings its closure.
// A fake install of the three boot archives holds m.bms (its terrain island, its environment day,
// one item 100100), the files found by its name (m.bin, m.wac, m.pcx, m.til, m.dbf and, since the
// .dbf exists, m.lwf, a bank of two waves, one the install has under the path's file name; no
// m.pwf), the terrain with its height data and its maps, the environment naming cloud.pcx, items.def whose
// item names the weapon M4 and the effect BOOM, weapon.def defining M4 with a round of an ammo no
// place defines, fx.ptl defining BOOM, and a few of the game's manifest files. The plan: the
// mission's own set each needed by the mission, its role and its edge's kind (data commit 8: the
// set is the mission's edges); the file references; the symbols
// followed to their files (the effect to fx.ptl, the item to items.def, which the manifest brings
// first); the manifest's files the install has found, each needed by the mission for the game, a
// Required one the install lacks not found (vmacros.bin), an optional one no row (hiscore.txt);
// the undefined ammo counted; nothing not followed but the kinds the graph does not read; the
// same plan stepped a byte at a time. Without dependencies, the .bms alone. A second mission with
// almost none of its set: the optional files no row, the text table's fallback serving it.
static int test_plan_mission_closure() {
	Project project("opennova_editor_plan_mission_closure");
	const std::string install = project.dir.file("install");
	std::vector<uint8_t> mission_bytes;
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		std::string error;
		TEST_EXPECT(opennova::mission::set_header_string(mission, "terrain", "island", error));
		TEST_EXPECT(opennova::mission::set_header_string(mission, "environment", "day", error));
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Building, 100100, {});
		TEST_EXPECT(opennova::bms::write(mission, mission_bytes, error));
	}
	std::string env_text;
	{
		opennova::env::Config config;
		config.sky_map1 = "cloud.pcx";
		config.sky_map2 = "cloud.pcx";
		std::ostringstream out;
		std::string error;
		TEST_EXPECT(opennova::env::save_env(out, config, error));
		env_text = out.str();
	}
	std::vector<uint8_t> table;
	{
		Diagnostic error;
		BlankRequest blank;
		blank.logical_name = "m.bin";
		TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	}
	const std::string strings(table.begin(), table.end());
	TEST_EXPECT(write_pff(install + "/localres.pff",
	                      {{"m.bms", std::string(mission_bytes.begin(), mission_bytes.end())},
	                       // A second mission with none of its own set but its loading image.
	                       {"n.bms", std::string(mission_bytes.begin(), mission_bytes.end())},
	                       {"n.pcx", "pcx"},
                       // A bank of its name, which the game reads as its dialog's sounds only beside a .dbf it
                       // lacks (review F5).
                       {"n.lwf", bank_text({{"LINE9", "nine.wav"}})},
	                       {"m.wac", "// the mission's script\r\n"},
	                       {"m.dbf", "dbf"},
	                       {"m.lwf", bank_text({{"LINE1", "SFX\\VOICE\\line1.wav"}, {"LINE2", "gone.wav"}})},
	                       {"line1.wav", "RIFF"},
	                       {"items.def", "begin \"Box\"\nid 100100\ntype building\nprimary_weapon \"M4\"\nparticledeath BOOM\nend\n"},
	                       {"weapon.def", "weapon \"M4\"\nround_type NOWHERE\nend\n"},
	                       {"ammo.def", "ammo AMMO_X\nend\n"},
	                       // The main menu's font through a variable only the install's stylesheet defines
	                       // (the manifest brings the sheet; the menu is followed again once it came).
	                       {"main.mnu", screen("STARTUP", window("STATIC", "W", font("%FONT_X%")))},
	                       {"menu_style.mns", "FONT_X styled.fnt\r\n"},
	                       {"styled.fnt", "fnt"},
	                       // The HUD layout the game opens at mission start: its font and its textures (a
	                       // stance's last record, the last static frame) are its references.
	                       {"hudpos.def", "fonthud1_hi\thud.fnt\r\nHUDSTANCE 0\t0 0 old.tga STAND\r\nHUDSTANCE 0\t0 0 stand.tga STAND\r\n"
	                                      "StaticFrame\tfirst.tga 1,2\r\nStaticFrame\tframe.tga 2,570\r\n"},
	                       {"hud.fnt", "fnt"}}));
	TEST_EXPECT(write_pff(install + "/language.pff", {{"m.bin", strings}, {"gametext.bin", strings}, {"medmssn.bin", strings}}));
	TEST_EXPECT(write_pff(install + "/resource.pff",
	                      {{"island.trn", terrain_text("island")}, {"island.cpt", "cpt"}, {"island_c.tga", "tga"}, {"det.tga", "tga"},
	                       {"day.env", env_text}, {"cloud.pcx", "pcx"}, {"m.pcx", "pcx"},
	                       // Two of the fixed names a running mission opens that the manifest does not list.
	                       {"overcast.def", "; overcast\r\n"}, {"eraindrp.tga", "tga"},
                       // And the HUD's, a crosshair style's, a view effect's and an impact scar's (review F4).
                       {"compring.tga", "tga"}, {"cross03.tga", "tga"}, {"BNumbers.tga", "tga"}, {"scorch1.tga", "tga"},
	                       {"m.til", "til"},
	                       // An effect of two particles: a plain graphic, and a flipbook of two frames, which
	                       // loads a file a frame named from the graphic's and never the graphic's own name.
	                       {"fx.ptl", "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff;\n}\n\n[particledef]\n{\n\tid = puff;\n\tpdefs = puff;\n\tgraphic1 = puff.tga, additive;\n}\n\n"
	                                  "[particledef]\n{\n\tid = flame;\n\tgraphic1 = Flame.TGA, additive;\n\tg1_flip_frames = 2;\n}\n"},
	                       {"puff.tga", "tga"}, {"Flame.TGA", "never loaded"}, {"flame_01.tga", "tga"}, {"flame_02.tga", "tga"},
	                       {"stand.tga", "tga"}, {"old.tga", "tga"}, {"frame.tga", "tga"}, {"first.tga", "tga"},
	                       {"other.ptl", "[effectdef]\n{\n\tid = OTHER;\n\tpdefs = p;\n}\n\n[particledef]\n{\n\tid = p;\n}\n"}}));
	// The music banks the game streams loose from its root (the manifest's, found there), and a
	// save beside them, which is the player's, never the game's.
	TEST_EXPECT(editor_test::write_text(install + "/menumus.sbf", "menu music") &&
	            editor_test::write_text(install + "/GAMEMUS.SBF", "game music") && editor_test::write_text(install + "/player.sav", "save"));
	ImportChoice mission;
	mission.path = install;
	mission.entry = "m.bms";
	mission.install = true;
	const ImportPlan plan = project.plan({mission}, true, install);
	TEST_EXPECT(!plan.truncated && !has_error(plan.diagnostics));
	const auto found = [&plan](const char *name) {
		const ImportPlanRow *row = row_named(plan, name);
		return row && row->state == State::Found && row->selected && row->found_in == "the game install" ? row : nullptr;
	};
	// The mission's own set, each as the game finds it by the mission's name: the mission's edges
	// (documents/mission_file_set.h), each of its role and kind.
	struct Own {
		const char *name, *role;
		ReferenceKind kind;
	};
	for (const Own &own : {Own{"m.bin", "text", ReferenceKind::MissionStrings}, Own{"m.wac", "script", ReferenceKind::Script},
	                       Own{"m.pcx", "loading_image", ReferenceKind::LoadingImage},
	                       Own{"m.til", "tiles", ReferenceKind::TilePlacement}, Own{"m.dbf", "dialog", ReferenceKind::DialogBank},
	                       Own{"m.lwf", "dialog_sounds", ReferenceKind::SoundBank}}) {
		const ImportPlanRow *row = found(own.name);
		TEST_EXPECT(row && row->needed_by.file == "m.bms" && row->needed_by.field == own.role && row->needed_by.name == own.name &&
		            row->needed_by.reference == own.kind);
	}
	TEST_EXPECT(!row_named(plan, "m.pwf"));
	// The file references, and the environment's texture through it.
	const ImportPlanRow *terrain = found("island.trn"), *env = found("day.env"), *cloud = found("cloud.pcx");
	TEST_EXPECT(terrain && terrain->needed_by.reference == ReferenceKind::Terrain && env && cloud &&
	            cloud->needed_by.file == "day.env" && cloud->needed_by.reference == ReferenceKind::Texture);
	// The symbols: the effect to the particle file defining it (and its texture through it), the
	// other particle file not; the item to items.def, which the manifest brought first.
	const ImportPlanRow *fx = found("fx.ptl"), *puff = found("puff.tga"), *items = found("items.def");
	TEST_EXPECT(fx && fx->needed_by.file == "items.def" && fx->needed_by.reference == ReferenceKind::Particle &&
	            fx->needed_by.name == "BOOM" && puff && puff->needed_by.file == "fx.ptl" && !row_named(plan, "other.ptl"));
	TEST_EXPECT(items && items->needed_by.file == "m.bms" && items->needed_by.reference == ReferenceKind::None &&
	            items->needed_by.field.find("the game") == 0);
	// A flipbook's frames, each a file of its own; the graphic's own name is never loaded.
	const ImportPlanRow *frame_one = found("flame_01.tga"), *frame_two = found("flame_02.tga");
	TEST_EXPECT(frame_one && frame_two && frame_one->needed_by.file == "fx.ptl" && frame_one->needed_by.record == "flame" &&
	            frame_one->needed_by.field == "graphic1[1]" && frame_two->needed_by.field == "graphic1[2]" &&
	            !row_named(plan, "Flame.TGA"));
	// The HUD layout, which the manifest brings: its font, the stance's icon as read (the last
	// record of its id) and the last static frame; the ones the game does not read stay.
	const ImportPlanRow *hud = found("hudpos.def"), *hud_font = found("hud.fnt"), *stand = found("stand.tga"), *frame = found("frame.tga");
	TEST_EXPECT(hud && hud_font && hud_font->needed_by.file == "hudpos.def" && hud_font->needed_by.reference == ReferenceKind::Font &&
	            stand && stand->needed_by.record == "HUDSTANCE 0" && frame && frame->needed_by.record == "StaticFrame" &&
	            !row_named(plan, "old.tga") && !row_named(plan, "first.tga"));
	TEST_EXPECT(!not_followed(plan, ReferenceKind::None, AssetKind::HudPosDefs));
	// The terrain's files: its height data and its two maps found, the foliage model the install
	// lacks not found.
	const ImportPlanRow *heights = found("island.cpt"), *colour = found("island_c.tga"), *detail = found("det.tga");
	TEST_EXPECT(heights && heights->kind == AssetKind::TerrainPolyData && heights->needed_by.file == "island.trn" &&
	            heights->needed_by.reference == ReferenceKind::TerrainData && colour && detail &&
	            detail->needed_by.field == "polytrn_detailmap");
	const ImportPlanRow *palm = row_named(plan, "palm");
	TEST_EXPECT(palm && palm->state == State::NotFound && palm->needed_by.file == "island.trn");
	// The dialog bank's sounds: a wave by the file name of the path its single holds, one the
	// install lacks not found under the name the single gives.
	const ImportPlanRow *line = found("line1.wav"), *gone = row_named(plan, "gone.wav");
	TEST_EXPECT(line && line->kind == AssetKind::Wave && line->needed_by.file == "m.lwf" && line->needed_by.record == "LINE1" &&
	            line->needed_by.reference == ReferenceKind::Wave && line->needed_by.name == "SFX\\VOICE\\line1.wav");
	TEST_EXPECT(gone && gone->state == State::NotFound && gone->kind == AssetKind::Wave && gone->needed_by.record == "LINE2");
	// The manifest: found in the install, each for the game; a Required one the install lacks not
	// found; an optional one no row.
	for (const char *name : {"gametext.bin", "weapon.def", "ammo.def", "main.mnu", "menu_style.mns", "medmssn.bin"}) {
		const ImportPlanRow *row = found(name);
		TEST_EXPECT(row && row->needed_by.file == "m.bms" && row->needed_by.field.find("the game, ") == 0);
	}
	// The install's loose files of the kinds the game ships loose, as the disk spells them, their
	// sizes the disk's; never a save.
	for (const auto &[name, bytes] : {std::pair<const char *, uint64_t>{"menumus.sbf", 10}, {"GAMEMUS.SBF", 10}}) {
		const ImportPlanRow *row = found(name);
		TEST_EXPECT(row && row->kind == AssetKind::MusicBank && row->size == bytes && row->source.install &&
		            row->needed_by.field.find("the game, ") == 0);
	}
	TEST_EXPECT(!row_named(plan, "player.sav"));
	// The menu's font through the variable the stylesheet the walk brought defines: the font found
	// (the sheet's own value names it first, in the manifest's order; the menu followed again once
	// the sheet was read names it too), the variable neither undefined nor not followed.
	const ImportPlanRow *styled = found("styled.fnt");
	TEST_EXPECT(styled && (styled->needed_by.file == "main.mnu" || styled->needed_by.file == "menu_style.mns") &&
	            styled->needed_by.reference == ReferenceKind::Font && !not_followed(plan, ReferenceKind::StyleVar));
	for (const ImportNotFollowed &entry : plan.undefined) TEST_EXPECT(entry.reference != ReferenceKind::StyleVar);
	const ImportPlanRow *vmacros = row_named(plan, "vmacros.bin");
	TEST_EXPECT(vmacros && vmacros->state == State::NotFound && vmacros->kind == AssetKind::Strings &&
	            vmacros->needed_by.file == "m.bms");
	TEST_EXPECT(!row_named(plan, "hiscore.txt") && !row_named(plan, "loadscrn.pcx"));
	// The fixed names a running mission opens beyond the manifest: the ones the install has found,
	// each for the game; one it lacks no row.
	const ImportPlanRow *overcast = found("overcast.def"), *rain = found("eraindrp.tga");
	TEST_EXPECT(overcast && overcast->needed_by.file == "m.bms" && overcast->needed_by.field == "the game, for the overcast sky" &&
	            rain && rain->kind == AssetKind::Texture && rain->needed_by.field == "the game, for rain");
	TEST_EXPECT(!row_named(plan, "jsnwflk.tga") && !row_named(plan, "helo1.aip"));
	const ImportPlanRow *ring = found("compring.tga"), *cross = found("cross03.tga"), *digits = found("BNumbers.tga"),
	                    *scar = found("scorch1.tga");
	TEST_EXPECT(ring && ring->needed_by.field == "the game, for the HUD" && cross &&
	            cross->needed_by.field == "the game, for a crosshair style" && digits && scar &&
	            scar->needed_by.field == "the game, for an impact scar" && !row_named(plan, "cross01.tga"));
	// The ammo no place defines, counted; the weapon M4 defined by the planned weapon.def is not.
	TEST_EXPECT(plan.undefined.size() == 1 && plan.undefined[0].reference == ReferenceKind::Ammo &&
	            plan.undefined[0].count == 1 && plan.undefined[0].first == "weapon.def");
	// Not followed: only the kinds whose references the graph does not read (the script's RUN), never a
	// symbol kind, the terrain, a sound bank or a dialog bank (DI-32: its lines' waves are followed).
	for (const ImportNotFollowed &entry : plan.not_followed)
		TEST_EXPECT(entry.reference == ReferenceKind::None || reference_row(entry.reference).resolution == ReferenceResolution::Unchecked);
	TEST_EXPECT(!not_followed(plan, ReferenceKind::None, AssetKind::DialogBank) &&
	            !not_followed(plan, ReferenceKind::None, AssetKind::Terrain) &&
	            !not_followed(plan, ReferenceKind::None, AssetKind::SoundBank) &&
	            !not_followed(plan, ReferenceKind::None, AssetKind::MusicBank) && !not_followed(plan, ReferenceKind::Particle) &&
	            !not_followed(plan, ReferenceKind::Item) && !not_followed(plan, ReferenceKind::Weapon));
	TEST_EXPECT(plan.file_count() >= 18 && plan.total_bytes() > mission_bytes.size());
	// Stepped a byte at a time: the same plan.
	const SessionView &v = project.view();
	ImportPlanner planner({mission}, true, ProjectPaths::for_root(v.project.root), *v.project.document, *v.project.scan,
	                      *v.findings.graph, install);
	size_t steps = 0, done = 0;
	bool fell = false; // the files followed never fall, a menu followed again included (review F13)
	while (!planner.step(1)) {
		TEST_EXPECT(++steps < 2000);
		fell = fell || planner.files_done() < done;
		done = planner.files_done();
	}
	const ImportPlan stepped = planner.take();
	if (!same_import(plan, stepped))
		for (size_t i = 0; i < std::min(plan.rows.size(), stepped.rows.size()); ++i)
			if (plan.rows[i].name != stepped.rows[i].name || plan.rows[i].selected != stepped.rows[i].selected ||
			    plan.rows[i].problem != stepped.rows[i].problem)
				std::printf("row %zu: %s (%d, %s) | %s (%d, %s)\n", i, plan.rows[i].name.c_str(), int(plan.rows[i].selected),
				            plan.rows[i].problem.c_str(), stepped.rows[i].name.c_str(), int(stepped.rows[i].selected),
				            stepped.rows[i].problem.c_str());
	// A step takes one source, the manifest for the missions, or one file followed; the counts kept by
	// kind come out the same too (review F13).
	const auto same_counts = [](const std::vector<ImportNotFollowed> &a, const std::vector<ImportNotFollowed> &b) {
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (a[i].reference != b[i].reference || a[i].kind != b[i].kind || a[i].count != b[i].count || a[i].first != b[i].first)
				return false;
		return true;
	};
	TEST_EXPECT(steps > 20 && same_import(plan, stepped) && same_counts(plan.undefined, stepped.undefined) &&
	            same_counts(plan.not_followed, stepped.not_followed) && same_counts(plan.shadowed, stepped.shadowed) && !fell);
	// Without dependencies: the .bms alone.
	const ImportPlan alone = project.plan({mission}, false, install);
	TEST_EXPECT(alone.rows.size() == 1 && alone.rows[0].name == "m.bms" && alone.undefined.empty());
	// A mission with none of its own set but its loading image: the files the game runs without
	// (its script, its dialog bank and the bank's sounds) are no row, the sounds' bank of its name the
	// install has never read without the .dbf (review F5); its text table is served by
	// the name its lookup takes next (medmssn.bin, which the manifest brings), no row of its own;
	// the tile placement, which every shipped mission has, is not found.
	ImportChoice bare = mission;
	bare.entry = "n.bms";
	const ImportPlan lean = project.plan({bare}, true, install);
	TEST_EXPECT(!row_named(lean, "n.wac") && !row_named(lean, "n.dbf") && !row_named(lean, "n.lwf") &&
	            !row_named(lean, "n.pwf") && !row_named(lean, "n.bin"));
	const ImportPlanRow *image = row_named(lean, "n.pcx"), *tiles = row_named(lean, "n.til"),
	                    *fallback = row_named(lean, "medmssn.bin");
	TEST_EXPECT(image && image->state == State::Found && image->needed_by.reference == ReferenceKind::LoadingImage);
	TEST_EXPECT(tiles && tiles->state == State::NotFound && tiles->needed_by.file == "n.bms" &&
	            tiles->needed_by.reference == ReferenceKind::TilePlacement);
	TEST_EXPECT(fallback && fallback->state == State::Found);
	return 0;
}

// A symbol only the install's copy of a file the project holds defines (review F6): the project's
// edited items.def lacks the item the mission places; the import keeps the project's file, so the
// install's is not brought, and the plan says so (shadowed), neither silent nor undefined.
static int test_plan_shadowed() {
	Project project("opennova_editor_plan_shadowed");
	const std::string install = project.dir.file("install");
	std::vector<uint8_t> mission_bytes;
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Building, 100100, {});
		std::string error;
		TEST_EXPECT(opennova::bms::write(mission, mission_bytes, error));
	}
	TEST_EXPECT(editor_test::write_text(project.root() + "/items.def", "begin \"Mine\"\nid 100200\ntype building\nend\n"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	TEST_EXPECT(write_pff(install + "/localres.pff", {{"m.bms", std::string(mission_bytes.begin(), mission_bytes.end())},
	                                                   {"items.def", "begin \"Box\"\nid 100100\ntype building\nend\n"}}));
	ImportChoice mission;
	mission.path = install;
	mission.entry = "m.bms";
	mission.install = true;
	const ImportPlan plan = project.plan({mission}, true, install);
	TEST_EXPECT(!row_named(plan, "items.def"));
	TEST_EXPECT(plan.shadowed.size() == 1 && plan.shadowed[0].reference == ReferenceKind::Item && plan.shadowed[0].count == 1 &&
	            plan.shadowed[0].kind == AssetKind::ItemDefs && plan.shadowed[0].first == "m.bms");
	for (const ImportNotFollowed &entry : plan.undefined) TEST_EXPECT(entry.reference != ReferenceKind::Item);
	return 0;
}

int run_import_plan_tests() {
	int failures = 0;
	failures += test_plan_shadowed();
	failures += test_plan_mission_closure();
	failures += test_plan_steps();
	failures += test_plan_folder();
	failures += test_plan_archive();
	failures += test_plan_game_install();
	failures += test_plan_scene();
	failures += test_plan_cycle_and_cap();
	failures += test_plan_not_followed();
	failures += test_references_unread();
	failures += test_plan_competition();
	failures += test_plan_material_sources();
	failures += test_plan_stylesheets();
	failures += test_plan_missing_lookups();
	failures += test_plan_kinds();
	failures += test_plan_native_png();
	failures += test_plan_relative_path();
	return failures;
}
