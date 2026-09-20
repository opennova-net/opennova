// Pins "Create all missing" (ADR 0046 d7): a new project filled from the factories
// validates clean, the run is idempotent, one row can be created alone, a wrong-kind
// file is never overwritten, and every Required row of the default project has a
// factory (the mission rows without one are reported, not skipped silently).
#include <cstdio>
#include <string>

#include <editor/assets/asset_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/blank/create_missing.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

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

	const CreateMissingResult result = create_missing_requirements(paths, doc, before.report);
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
	TEST_EXPECT(after.report.diagnostics.empty());
	for (const RequirementRow &row : after.report.rows) {
		if (row.required) TEST_EXPECT(row.state == RequirementState::Present);
	}

	// Idempotent: nothing is created or touched on a second run.
	const CreateMissingResult again = create_missing_requirements(paths, doc, after.report);
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
	const CreateMissingResult one = create_missing_requirements(paths, doc, before.report, "main_menu");
	TEST_EXPECT(one.created.size() == 1 && one.created[0] == "menus/main.mnu");
	// An optional row asked for by name is created too.
	const CreateMissingResult optional = create_missing_requirements(paths, doc, before.report, "brand_style");
	TEST_EXPECT(optional.created.empty() && optional.unavailable.size() == 1); // no factory yet
	TEST_EXPECT(optional.unavailable[0] == "brand.mns");

	// A present-but-wrong file is refused, never overwritten.
	TEST_EXPECT(editor_test::write_text(root + "/gametext.bin", "not a string table"));
	Evaluated wrong = evaluate(paths, doc);
	const CreateMissingResult refused = create_missing_requirements(paths, doc, wrong.report, "gametext");
	TEST_EXPECT(refused.created.empty());
	TEST_EXPECT(refused.diagnostics.size() == 1 && refused.diagnostics[0].code == "create_missing.wrong_kind");
	std::string text;
	std::string io_error;
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
	const CreateMissingResult result = create_missing_requirements(paths, doc, before.report);
	TEST_EXPECT(result.diagnostics.empty());
	bool ammo_created = false;
	for (const std::string &path : result.created) ammo_created = ammo_created || path == "defs/ammo.def";
	TEST_EXPECT(ammo_created);
	bool failsafe_unavailable = false, menus_unavailable = false;
	for (const std::string &name : result.unavailable) {
		if (name == "failsafe.bad") failsafe_unavailable = true;
		if (name == "cmap.mnu") menus_unavailable = true;
	}
	TEST_EXPECT(failsafe_unavailable && menus_unavailable);
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
