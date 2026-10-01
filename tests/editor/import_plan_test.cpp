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
// a sound, a terrain and a mission's .mis listed as not followed; and nothing written
// anywhere. S13 D5: what goes unread is the kinds table's rule (a kind that names files the
// graph does not read), the kinds the hand list named before and a face and a map project.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/blank/blank_factory.h>
#include <editor/import/import_plan.h>
#include <editor/import/sidecar.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_mis.h>
#include <formats/pff/pff.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/import_test_support.h"
#include "editor/png_test_support.h"

using namespace opennova::editor;
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
	TEST_EXPECT(logo && logo->state == State::Found && logo->kind == AssetKind::Texture && logo->destination == "LOGO.TGA" &&
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
	TEST_EXPECT(not_followed(styled, ReferenceKind::StyleVar) && styled.rows.size() == 4);

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
	ImportSource member;
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
	// The target screen is a symbol: not followed.
	const ImportNotFollowed *target = not_followed(plan, ReferenceKind::MenuScreen);
	TEST_EXPECT(target && target->first == "first.mnu");
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
	ImportSource retail;
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
	const std::vector<ImportSource> fonts = {{art + "/fa.fnt", {}}, {art + "/fb.fnt", {}}, {art + "/fc.fnt", {}}};
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

// What the walk does not follow: a menu's SCREEN target (a symbol) and a def's sound, each
// kind listed with where it was met first; a terrain a mission names found and taken, its
// own references not read (listed once by kind); a mission's .mis taken, what it names not
// looked for (the graph reads the .bms alone), listed by its kind, and no unreadable file.
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
	TEST_EXPECT(editor_test::write_text(art + "/island.trn", "trn"));
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", go_to("a.mnu", "A")))));
	TEST_EXPECT(editor_test::write_text(art + "/items.def", "begin \"Boom\"\nid 100100\ntype building\nsounddeath boom\nend\n"));
	const ImportPlan plan = project.plan({{art + "/m.bms", {}}, {art + "/a.mnu", {}}, {art + "/items.def", {}}});
	const ImportPlanRow *terrain = row_named(plan, "island.trn");
	TEST_EXPECT(terrain && terrain->state == State::Found && terrain->kind == AssetKind::Terrain &&
	            terrain->needed_by.file == "m.bms" && terrain->needed_by.reference == ReferenceKind::Terrain);
	const ImportNotFollowed *trn = not_followed(plan, ReferenceKind::None, AssetKind::Terrain);
	TEST_EXPECT(trn && trn->count == 1 && trn->first == "island.trn");
	const ImportNotFollowed *target = not_followed(plan, ReferenceKind::MenuScreen);
	TEST_EXPECT(target && target->count == 1 && target->first == "a.mnu");
	const ImportNotFollowed *sound = not_followed(plan, ReferenceKind::Sound);
	TEST_EXPECT(sound && sound->count == 1 && sound->first == "items.def");
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
	const ImportPlan text_plan = project.plan({ { art + "/m.mis", {} } });
	const ImportNotFollowed *mis = not_followed(text_plan, ReferenceKind::None, AssetKind::Mission);
	TEST_EXPECT(mis && mis->count == 1 && mis->first == "m.mis");
	TEST_EXPECT(text_plan.rows.size() == 1 && row_named(text_plan, "m.mis") &&
			row_named(text_plan, "m.mis")->kind == AssetKind::Mission);
	for (const Diagnostic &d : text_plan.diagnostics)
		TEST_EXPECT(d.code() != "import.unreadable");
	// A face names its textures, which nothing reads yet: taken, listed; a wave names nothing.
	TEST_EXPECT(editor_test::write_text(art + "/head.grm", "BASE_TEXTURE face.tga\r\n") &&
	            editor_test::write_text(art + "/boom.wav", "RIFF"));
	const ImportPlan face_plan = project.plan({{art + "/head.grm", {}}, {art + "/boom.wav", {}}});
	const ImportNotFollowed *face =
	        not_followed(face_plan, ReferenceKind::None, AssetKind::FaceAnimation);
	TEST_EXPECT(face && face->count == 1 && face->first == "head.grm");
	TEST_EXPECT(face_plan.not_followed.size() == 1 && row_named(face_plan, "head.grm"));
	const ImportPlanRow *wave = row_named(face_plan, "boom.wav");
	TEST_EXPECT(wave && wave->kind == AssetKind::Wave && wave->problem.empty());
	return 0;
}

// What an import does not follow is the kinds table's rule (S13 D5): a file's references go
// unread when its kind names files (AssetKindRow::names_files) and the graph does not read the
// file (graph_reads_file). Those are the kinds the hand-written list named (a terrain, a script,
// the two sound banks, a dialog bank, the def tables beyond the catalogs and the avatar table)
// and the ones S13 D5 added that name files (a face, a map project); a mission's .mis, which the
// graph does not read, where its .bms is read.
static int test_references_unread() {
	const std::set<AssetKind> unread = {AssetKind::Terrain, AssetKind::Script, AssetKind::MusicBank,
	        AssetKind::SoundBank, AssetKind::DialogBank, AssetKind::HudPosDefs,
	        AssetKind::HudFxDefs, AssetKind::SoundProfileDefs, AssetKind::CharAttrDefs,
	        AssetKind::PowerupDefs, AssetKind::OtherDefs, AssetKind::FaceAnimation,
	        AssetKind::MapProject};
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKind kind = AssetKind(i);
		const std::string file = kind == AssetKind::Mission ? "m.bms" : "x";
		const bool rule = asset_kind_row(kind).names_files && !graph_reads_file(kind, file);
		TEST_EXPECT(references_unread(kind, file) == rule);
		if (rule != (unread.count(kind) > 0))
			std::fprintf(stderr, "references_unread(%s) moved\n", asset_kind_token(kind));
		TEST_EXPECT(rule == (unread.count(kind) > 0));
	}
	TEST_EXPECT(references_unread(AssetKind::Mission, "m.mis"));
	TEST_EXPECT(!references_unread(AssetKind::Mission, "M.BMS"));
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

// A model's .mdt normal map and its chunk row's file are textures to the plan (the chunk
// file by its bytes, whatever its name: renderer::load_material_chunk): found beside the
// model, a file of the name that holds no chunk not found; from a game install they import
// as textures, the project's graph resolves both rows, and the build packs both.
static int test_plan_material_sources() {
	Project project("opennova_editor_plan_material_sources");
	const std::string relief = "o3d 1\nmodel RELIEF\nmaterial FF_ST_OP\ntexture ready.mdt 3 4\ntexture field.nq8 1 16\n"
	                           "texture other.nq8 1 17\nlod 0\npart 0 0 0 0\nstrip 0 0\n"
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
	TEST_EXPECT(field && field->state == State::Found && field->kind == AssetKind::Texture && field->problem.empty());
	TEST_EXPECT(other && other->state == State::NotFound && beside.rows.size() == 4);

	const std::string install = project.dir.file("install");
	const std::vector<uint8_t> chunk = chunk_file();
	TEST_EXPECT(write_pff(install + "/resource.pff", {{"ready.mdt", "mdt"}, {"field.nq8", std::string(chunk.begin(), chunk.end())}}));
	const std::string bare = project.dir.file("bare");
	TEST_EXPECT(editor_test::write_text(bare + "/relief.o3d", relief));
	const ImportPlan plan = project.plan({{bare + "/relief.o3d", {}}}, true, install);
	for (const char *name : {"ready.mdt", "field.nq8"}) {
		const ImportPlanRow *row = row_named(plan, name);
		TEST_EXPECT(row && row->state == State::Found && row->found_in == "the game install" && row->kind == AssetKind::Texture);
	}
	const std::string root = project.root();
	const ImportResult result = import_assets(selected_sources(plan), ProjectPaths::for_root(root), *project.view().project.document, false);
	TEST_EXPECT(!has_error(result.diagnostics) && result.imported.size() == 3);
	project.session.handle(request::rescan());
	project.session.run_operations();
	const SessionView &view = project.view();
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
	                                    "o3d 1\nmodel GLOW\nmaterial FF_ST_OP\ntexture glow.tga 1 0\ntexture glow.tga 1 1\n"
	                                    "lod 0\npart 0 0 0 0\nstrip 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\n"
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
	            editor_test::write_bytes(art + "/logo.png", editor_test::gradient_png(4, 4)));
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
	TEST_EXPECT(editor_test::write_bytes(art + "/badge.png", editor_test::gradient_png(4, 4, 7)));
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

int run_import_plan_tests() {
	int failures = 0;
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
