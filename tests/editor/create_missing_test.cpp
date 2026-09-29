// Pins create missing (ADR 0046 d7, S11b): a new project filled from the factories
// through the roles of its unmet rows validates clean (the optional files it lacks stay
// notes), the run is idempotent, one row can be created alone, the roles are explicit
// (none makes nothing, one named twice is made once, an unknown one is refused), a file
// that is there, of the right kind or the wrong one, is never overwritten (and one the
// report missed is found on disk), and every Required row of the default project has a
// factory (the mission rows without one are reported, not skipped silently).
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
	        create_missing_requirements(paths, doc, before.report, unmet_required_roles(before.report));
	TEST_EXPECT(result.unavailable.empty());
	TEST_EXPECT(result.diagnostics.empty());
	TEST_EXPECT(static_cast<int>(result.created.size()) == before.report.required_total);
	bool menu_in_menus = false, font_in_fonts = false, defs_in_defs = false;
	for (const std::string &path : result.created) {
		if (path == "menus/main.mnu") menu_in_menus = true;
		if (path == "fonts/Impac38b.fnt") font_in_fonts = true;
		if (path == "defs/items.def") defs_in_defs = true;
	}
	TEST_EXPECT(menu_in_menus && font_in_fonts && defs_in_defs);

	Evaluated after = evaluate(paths, doc);
	TEST_EXPECT(!diagnostics_have_errors(after.scan.diagnostics));
	TEST_EXPECT(after.report.required_missing == 0);
	TEST_EXPECT(after.report.required_wrong_kind == 0);
	// What is left are the optional files the project lacks: notes, never errors.
	TEST_EXPECT(!after.report.diagnostics.empty() && !diagnostics_have_errors(after.report.diagnostics));
	for (const Diagnostic &d : after.report.diagnostics)
		TEST_EXPECT(d.code == "requirement.optional_missing" && d.severity == DiagnosticSeverity::Info);
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

	// Idempotent: nothing is unmet, so a second run names nothing and touches nothing.
	TEST_EXPECT(unmet_required_roles(after.report).empty());
	const CreateMissingResult again =
	        create_missing_requirements(paths, doc, after.report, unmet_required_roles(after.report));
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
	const CreateMissingResult none = create_missing_requirements(paths, doc, before.report, {});
	TEST_EXPECT(none.created.empty() && none.unavailable.empty() && none.diagnostics.empty());
	// One role named twice is made once.
	const CreateMissingResult one = create_missing_requirements(paths, doc, before.report, {"main_menu", "main_menu"});
	TEST_EXPECT(one.created.size() == 1 && one.created[0] == "menus/main.mnu" && one.diagnostics.empty());
	std::vector<uint8_t> made, kept;
	std::string text, io_error;
	TEST_EXPECT(read_file_bytes(root + "/menus/main.mnu", made, io_error));
	// The report it was made from, asked again: the file is on disk now, refused, untouched.
	TEST_EXPECT(editor_test::write_text(root + "/menus/main.mnu", "edited since"));
	const CreateMissingResult stale = create_missing_requirements(paths, doc, before.report, {"main_menu"});
	TEST_EXPECT(stale.created.empty() && stale.diagnostics.size() == 1 &&
	            stale.diagnostics[0].code == "create_missing.exists");
	TEST_EXPECT(read_file_text(root + "/menus/main.mnu", text, io_error) && text == "edited since");
	// A report that lists it, in another folder: refused the same way, nothing made beside it.
	TEST_EXPECT(editor_test::write_bytes(root + "/elsewhere/main.mnu", made));
	fs::remove(root + "/menus/main.mnu");
	const Evaluated moved = evaluate(paths, doc);
	const CreateMissingResult present = create_missing_requirements(paths, doc, moved.report, {"main_menu"});
	TEST_EXPECT(present.created.empty() && present.diagnostics.size() == 1 &&
	            present.diagnostics[0].code == "create_missing.exists");
	TEST_EXPECT(!fs::exists(root + "/menus/main.mnu"));
	TEST_EXPECT(read_file_bytes(root + "/elsewhere/main.mnu", kept, io_error) && kept == made);
	// An optional row asked for by name is created too.
	const CreateMissingResult optional = create_missing_requirements(paths, doc, before.report, {"brand_style"});
	TEST_EXPECT(optional.created.size() == 1 && optional.unavailable.empty());
	// ... unless no factory can make it.
	const CreateMissingResult no_factory = create_missing_requirements(paths, doc, before.report, {"prolog_bik"});
	TEST_EXPECT(no_factory.created.empty() && no_factory.unavailable.size() == 1);
	TEST_EXPECT(!no_factory.unavailable.empty() && no_factory.unavailable[0] == "prolog.BIK");
	// A role no row has (a mission row with missions off, a role no row has at all) is
	// refused, each once.
	const CreateMissingResult unknown = create_missing_requirements(paths, doc, before.report, {"ammo_def", "nope", "nope"});
	TEST_EXPECT(unknown.created.empty() && unknown.diagnostics.size() == 2);
	for (const Diagnostic &d : unknown.diagnostics) TEST_EXPECT(d.code == "create_missing.unknown");

	// A present-but-wrong file is refused, never overwritten.
	TEST_EXPECT(editor_test::write_text(root + "/gametext.bin", "not a string table"));
	Evaluated wrong = evaluate(paths, doc);
	const CreateMissingResult refused = create_missing_requirements(paths, doc, wrong.report, {"gametext"});
	TEST_EXPECT(refused.created.empty());
	TEST_EXPECT(refused.diagnostics.size() == 1 && refused.diagnostics[0].code == "create_missing.wrong_kind");
	TEST_EXPECT(read_file_text(root + "/gametext.bin", text, io_error) && text == "not a string table");
	return 0;
}

static int test_mission_rows_report_what_has_no_writer_yet() {
	editor_test::TempProjectDir dir("opennova_editor_create_missing_mission_test");
	const std::string root = dir.file("Mission");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Mission", "jo", doc, error));
	doc.features.mission = true;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	Evaluated before = evaluate(paths, doc);
	const CreateMissingResult result =
	        create_missing_requirements(paths, doc, before.report, unmet_required_roles(before.report));
	TEST_EXPECT(result.diagnostics.empty());
	bool ammo_created = false;
	for (const std::string &path : result.created) ammo_created = ammo_created || path == "defs/ammo.def";
	TEST_EXPECT(ammo_created);
	// failsafe.bad is optional (retail JO ships none), so nothing asks for it.
	bool failsafe_listed = false, menus_unavailable = false;
	for (const std::string &name : result.unavailable) {
		if (name == "failsafe.bad") failsafe_listed = true;
		if (name == "cmap.mnu") menus_unavailable = true;
	}
	TEST_EXPECT(!failsafe_listed && menus_unavailable);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_default_project_fills_and_validates();
	failures += test_single_role_and_wrong_kind();
	failures += test_mission_rows_report_what_has_no_writer_yet();
	if (failures == 0) std::printf("editor_create_missing: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
