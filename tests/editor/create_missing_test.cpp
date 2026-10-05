// Pins create missing (ADR 0046 d7, S11b): a new project filled from the factories
// through the roles of its unmet rows validates clean (the optional files it lacks stay
// notes), the run is idempotent, one row can be created alone, the roles are explicit
// (none makes nothing, one named twice is made once, an unknown one is refused), a file
// that is there, of the right kind or the wrong one, is never overwritten (and one the
// report missed is found on disk), and every Required row has a factory of its own kind, the
// mission-start rows included (an optional row without one is reported, not skipped silently).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/blank/create_missing.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <formats/mnu/mnu.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

static bool window_names_a_string_id(const opennova::mnu::Window &window) {
	if (opennova::strutil::iequals(window.string_data.type, "id")) return true;
	for (const opennova::mnu::Window &child : window.children) {
		if (window_names_a_string_id(child)) return true;
	}
	return false;
}

struct Evaluated {
	AssetScan scan;
	RequirementReport report;
};

static Evaluated evaluate(const ProjectPaths &paths, const ProjectDocument &doc) {
	Evaluated e;
	e.scan = scan_project_assets(paths, doc);
	e.report = evaluate_requirements(doc, e.scan);
	return e;
}

static int test_default_project_fills_and_validates() {
	editor_test::TempProjectDir dir("opennova_editor_create_missing_test");
	const std::string root = dir.file("Blank");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Blank Game", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);

	Evaluated before = evaluate(paths, doc);
	TEST_EXPECT(before.report.required_missing == before.report.required_total);
	for (const RequirementRow &row : before.report.rows) {
		if (row.required) TEST_EXPECT(find_blank_factory_for_role(row.role) != nullptr);
	}

	const CreateMissingResult result =
	        create_missing_requirements(paths, doc, before.scan, before.report, unmet_required_roles(before.report));
	TEST_EXPECT(result.unavailable.empty());
	TEST_EXPECT(result.diagnostics.empty());
	// Every required file, and the pointer the startup screen names, made with it.
	TEST_EXPECT(static_cast<int>(result.created.size()) == before.report.required_total + 1);
	bool menu_in_menus = false, font_in_fonts = false, defs_in_defs = false, pointer_in_textures = false;
	for (const std::string &path : result.created) {
		if (path == "menus/main.mnu") menu_in_menus = true;
		if (path == "fonts/Impac38b.fnt") font_in_fonts = true;
		if (path == "defs/items.def") defs_in_defs = true;
		if (path == "textures/newarow1.tga") pointer_in_textures = true;
	}
	TEST_EXPECT(menu_in_menus && font_in_fonts && defs_in_defs && pointer_in_textures);

	Evaluated after = evaluate(paths, doc);
	TEST_EXPECT(!diagnostics_have_errors(after.scan.diagnostics));
	TEST_EXPECT(after.report.required_missing == 0);
	TEST_EXPECT(after.report.required_wrong_kind == 0);
	// What is left are the optional files the project lacks: notes, never errors.
	TEST_EXPECT(!after.report.diagnostics.empty() && !diagnostics_have_errors(after.report.diagnostics));
	for (const Diagnostic &d : after.report.diagnostics)
		TEST_EXPECT(d.code() == "requirement.optional_missing" && d.severity == DiagnosticSeverity::Info);
	for (const RequirementRow &row : after.report.rows) {
		if (row.required) TEST_EXPECT(row.state == RequirementState::Present);
	}

	// The filled project is closed over itself: the startup screen names no text table
	// and no string id that this run did not create (an optional row such as menutxt.bin
	// is never made here, and a label looked up in a missing table draws its raw key).
	std::vector<uint8_t> menu_bytes;
	std::string io_error;
	TEST_EXPECT(read_file_bytes(root + "/menus/main.mnu", menu_bytes, io_error));
	opennova::mnu::Document menu;
	std::string parse_error;
	TEST_EXPECT(opennova::mnu::parse(menu_bytes.data(), menu_bytes.size(), menu, parse_error));
	const opennova::mnu::Screen *startup = menu.find_screen("STARTUP");
	TEST_EXPECT(startup != nullptr && startup->roots.size() == 1);
	if (!startup || startup->roots.size() != 1) return 1;
	const std::string &text_table = startup->roots.front().text_rsrc;
	TEST_EXPECT(text_table.empty() || after.scan.find(text_table) != nullptr);
	TEST_EXPECT(text_table.empty() ? !window_names_a_string_id(startup->roots.front()) : true);
	// ... and it has a pointer: its root names the game's pointer, which the run made (the
	// original game shows no system pointer, docs/mnu/menu-re.md).
	const opennova::mnu::Cursor &cursor = startup->roots.front().cursor;
	TEST_EXPECT(cursor.file == blank_pointer_name() && cursor.flags == "STANDARD_TRANSPARENT");
	const AssetEntry *pointer = after.scan.find(cursor.file);
	TEST_EXPECT(pointer != nullptr && pointer->kind == AssetKind::Texture);

	// Idempotent: nothing is unmet, so a second run names nothing and touches nothing.
	TEST_EXPECT(unmet_required_roles(after.report).empty());
	const CreateMissingResult again =
	        create_missing_requirements(paths, doc, after.scan, after.report, unmet_required_roles(after.report));
	TEST_EXPECT(again.created.empty() && again.unavailable.empty() && again.diagnostics.empty());
	return 0;
}

static int test_single_role_and_wrong_kind() {
	editor_test::TempProjectDir dir("opennova_editor_create_missing_role_test");
	const std::string root = dir.file("One");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "One", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);

	Evaluated before = evaluate(paths, doc);
	// No role names nothing.
	const CreateMissingResult none = create_missing_requirements(paths, doc, before.scan, before.report, {});
	TEST_EXPECT(none.created.empty() && none.unavailable.empty() && none.diagnostics.empty());
	// One role named twice is made once.
	const CreateMissingResult one = create_missing_requirements(paths, doc, before.scan, before.report, {"main_menu", "main_menu"});
	TEST_EXPECT(one.created.size() == 2 && one.created[0] == "menus/main.mnu" && one.created[1] == "textures/newarow1.tga" &&
	            one.diagnostics.empty());
	std::vector<uint8_t> made, kept;
	std::string text, io_error;
	TEST_EXPECT(read_file_bytes(root + "/menus/main.mnu", made, io_error));
	// The report it was made from, asked again: the file is on disk now, refused, untouched.
	TEST_EXPECT(editor_test::write_text(root + "/menus/main.mnu", "edited since"));
	const CreateMissingResult stale = create_missing_requirements(paths, doc, before.scan, before.report, {"main_menu"});
	TEST_EXPECT(stale.created.empty() && stale.diagnostics.size() == 1 &&
	            stale.diagnostics[0].code() == "create_missing.exists");
	TEST_EXPECT(read_file_text(root + "/menus/main.mnu", text, io_error) && text == "edited since");
	// A report that lists it, in another folder: refused the same way, nothing made beside it.
	TEST_EXPECT(editor_test::write_bytes(root + "/elsewhere/main.mnu", made));
	fs::remove(root + "/menus/main.mnu");
	const Evaluated moved = evaluate(paths, doc);
	const CreateMissingResult present = create_missing_requirements(paths, doc, moved.scan, moved.report, {"main_menu"});
	TEST_EXPECT(present.created.empty() && present.diagnostics.size() == 1 &&
	            present.diagnostics[0].code() == "create_missing.exists");
	TEST_EXPECT(!fs::exists(root + "/menus/main.mnu"));
	TEST_EXPECT(read_file_bytes(root + "/elsewhere/main.mnu", kept, io_error) && kept == made);
	// An optional row asked for by name is created too.
	const CreateMissingResult optional = create_missing_requirements(paths, doc, before.scan, before.report, {"brand_style"});
	TEST_EXPECT(optional.created.size() == 1 && optional.unavailable.empty());
	// ... unless no factory can make it.
	const CreateMissingResult no_factory = create_missing_requirements(paths, doc, before.scan, before.report, {"prolog_bik"});
	TEST_EXPECT(no_factory.created.empty() && no_factory.unavailable.size() == 1);
	TEST_EXPECT(!no_factory.unavailable.empty() && no_factory.unavailable[0] == "prolog.BIK");
	// A role no row has (a mission row with missions off, a role no row has at all) is
	// refused, each once.
	const CreateMissingResult unknown = create_missing_requirements(paths, doc, before.scan, before.report, {"ammo_def", "nope", "nope"});
	TEST_EXPECT(unknown.created.empty() && unknown.diagnostics.size() == 2);
	for (const Diagnostic &d : unknown.diagnostics) TEST_EXPECT(d.code() == "create_missing.unknown");

	// A present-but-wrong file is refused, never overwritten.
	TEST_EXPECT(editor_test::write_text(root + "/gametext.bin", "not a string table"));
	Evaluated wrong = evaluate(paths, doc);
	const CreateMissingResult refused = create_missing_requirements(paths, doc, wrong.scan, wrong.report, {"gametext"});
	TEST_EXPECT(refused.created.empty());
	TEST_EXPECT(refused.diagnostics.size() == 1 && refused.diagnostics[0].code() == "create_missing.wrong_kind");
	TEST_EXPECT(read_file_text(root + "/gametext.bin", text, io_error) && text == "not a string table");
	return 0;
}

// With missions on, every Required row has a factory of the row's own kind: Create missing fills
// the boot, menu and mission-start rows alike (ammo.def, powerup.def, the seven menus a mission
// opens by name), and the project then lacks no required file. The optional mission rows a factory
// can fill are made when named; the ones that need real content (the sound banks, the music, the
// mission-text fallback, the HUD layouts, the celestial model, the fallback animation) have none.
static int test_mission_rows_are_all_filled() {
	editor_test::TempProjectDir dir("opennova_editor_create_missing_mission_test");
	const std::string root = dir.file("Mission");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Mission", "jo", doc, error));
	doc.features.mission = true;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	Evaluated before = evaluate(paths, doc);
	for (const RequirementRow &row : before.report.rows) {
		const BlankFactory *factory = find_blank_factory_for_role(row.role);
		if (row.required) TEST_EXPECT(factory != nullptr);
		if (factory && factory->kind != row.expected_kind)
			std::fprintf(stderr, "  %s: the factory makes another kind of file\n", row.role.c_str());
		TEST_EXPECT(factory == nullptr || factory->kind == row.expected_kind);
	}
	const CreateMissingResult result =
	        create_missing_requirements(paths, doc, before.scan, before.report, unmet_required_roles(before.report));
	for (const std::string &name : result.unavailable) std::fprintf(stderr, "  unavailable: %s\n", name.c_str());
	TEST_EXPECT(result.diagnostics.empty() && result.unavailable.empty());
	TEST_EXPECT(static_cast<int>(result.created.size()) == before.report.required_total);
	for (const char *path : {"defs/ammo.def", "defs/powerup.def", "menus/cmap.mnu", "menus/game.mnu", "menus/weapon.mnu",
	                         "menus/vehicle.mnu", "menus/stat.mnu", "menus/death.mnu", "menus/mp.mnu"}) {
		bool created = false;
		for (const std::string &made : result.created) created = created || made == path;
		if (!created) std::fprintf(stderr, "  not created: %s\n", path);
		TEST_EXPECT(created);
	}
	Evaluated after = evaluate(paths, doc);
	TEST_EXPECT(after.report.required_missing == 0 && after.report.required_wrong_kind == 0);
	TEST_EXPECT(!diagnostics_have_errors(after.scan.diagnostics) && !diagnostics_have_errors(after.report.diagnostics));

	const std::vector<std::string> optional = {"font_arials18", "font_arial22", "font_couri20b", "game_wac", "server_wac",
	                                           "loadscrn_pcx", "monogram_tga", "boxtile_tga", "border_tga"};
	const CreateMissingResult extras = create_missing_requirements(paths, doc, after.scan, after.report, optional);
	TEST_EXPECT(extras.diagnostics.empty() && extras.unavailable.empty() && extras.created.size() == optional.size());
	for (const char *path : {"fonts/Arials18.fnt", "missions/game.wac", "textures/loadscrn.pcx", "textures/border.tga"}) {
		bool created = false;
		for (const std::string &made : extras.created) created = created || made == path;
		TEST_EXPECT(created);
	}
	const CreateMissingResult content = create_missing_requirements(
	        paths, doc, after.scan, after.report,
	        {"failsafe_bad", "game_lwf", "gamemus_sbf", "medmssn_bin", "hudpos_def", "hudfx_def", "upl_3di"});
	TEST_EXPECT(content.created.empty() && content.diagnostics.empty() && content.unavailable.size() == 7);
	return 0;
}

// The pointer a made menu names is made with it only where the project has no file of its name
// (anywhere: a pointer in another folder is the one the menu takes) and its target is free; an
// expansion makes none (its base game serves the pointer).
static int test_the_pointer_comes_with_the_menu_once() {
	editor_test::TempProjectDir dir("opennova_editor_create_missing_pointer_test");
	const std::string root = dir.file("Pointer");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Pointer", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);

	// A pointer of the project's own, in its own folder: the menu takes it, nothing is made beside it.
	std::vector<uint8_t> own{1, 2, 3};
	TEST_EXPECT(editor_test::write_bytes(root + "/art/newarow1.tga", own));
	const Evaluated has_one = evaluate(paths, doc);
	const CreateMissingResult kept = create_missing_requirements(paths, doc, has_one.scan, has_one.report, {"main_menu"});
	TEST_EXPECT(kept.created.size() == 1 && kept.created[0] == "menus/main.mnu" && kept.diagnostics.empty());
	TEST_EXPECT(!fs::exists(root + "/textures/newarow1.tga"));
	std::vector<uint8_t> read;
	std::string io_error;
	TEST_EXPECT(read_file_bytes(root + "/art/newarow1.tga", read, io_error) && read == own);

	// None in the project and none on disk: made under textures/, the game's pointer form.
	fs::remove(root + "/menus/main.mnu");
	fs::remove(root + "/art/newarow1.tga");
	const Evaluated bare = evaluate(paths, doc);
	const CreateMissingResult made = create_missing_requirements(paths, doc, bare.scan, bare.report, {"main_menu"});
	TEST_EXPECT(made.created.size() == 2 && made.created[1] == "textures/newarow1.tga" && made.diagnostics.empty());
	TEST_EXPECT(read_file_bytes(root + "/textures/newarow1.tga", read, io_error) && read.size() == 18u + 32u * 32u * 4u &&
	            read[2] == 2 && read[16] == 32 && read[17] == 8);

	// The scan older than the tree (the pointer on disk since): left as it is, no finding.
	fs::remove(root + "/menus/main.mnu");
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/newarow1.tga", own));
	const CreateMissingResult stale = create_missing_requirements(paths, doc, bare.scan, bare.report, {"main_menu"});
	TEST_EXPECT(stale.created.size() == 1 && stale.diagnostics.empty());
	TEST_EXPECT(read_file_bytes(root + "/textures/newarow1.tga", read, io_error) && read == own);

	// An expansion's menu takes its base game's pointer.
	fs::remove(root + "/menus/main.mnu");
	fs::remove(root + "/textures/newarow1.tga");
	doc.expansion = ProjectExpansion{ "ptr", "" };
	const Evaluated expansion = evaluate(paths, doc);
	const RequirementRow *menu_row = nullptr;
	for (const RequirementRow &row : expansion.report.rows)
		if (row.role == "main_menu") menu_row = &row;
	TEST_EXPECT(menu_row != nullptr);
	const CreateMissingResult on_base = create_missing_requirements(paths, doc, expansion.scan, expansion.report, {"main_menu"});
	TEST_EXPECT(on_base.created.size() == 1 && on_base.created[0] == "menus/main.mnu");
	TEST_EXPECT(!fs::exists(root + "/textures/newarow1.tga"));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_default_project_fills_and_validates();
	failures += test_single_role_and_wrong_kind();
	failures += test_mission_rows_are_all_filled();
	failures += test_the_pointer_comes_with_the_menu_once();
	if (failures == 0) std::printf("editor_create_missing: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
