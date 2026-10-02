// Pins the requirements checklist (ADR 0046 d7) over the witnessed manifest: which rows
// apply, how a project file satisfies one, the diagnostics an unmet row raises (with the
// role and the file they are about; a missing file is the project's finding) and the note
// an optional file the project lacks is, and the roles "create every missing file" names.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
using namespace opennova::gameprofile;

static const RequirementRow *row_named(const RequirementReport &report, const char *name) {
	for (const RequirementRow &row : report.rows) {
		if (row.name == name) return &row;
	}
	return nullptr;
}

static int test_row_set_follows_the_manifest_and_features() {
	editor_test::TempProjectDir dir("opennova_editor_requirements_rows_test");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("R"), "R", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(dir.file("R"));
	const AssetScan empty = scan_project_assets(paths, doc);

	const RequirementReport menu_only = evaluate_requirements(doc, empty);
	int expected_rows = 0, expected_required = 0;
	for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
		const RequiredResource *r = gameprofile_required_resource_at(i);
		if (r->flags & (RES_F_PATTERN | RES_F_PFF_TABLE_ANY)) continue;
		if (r->phase == BOOT_PHASE_MISSION) continue;
		++expected_rows;
		if (r->severity != RES_OPTIONAL) ++expected_required;
	}
	TEST_EXPECT(static_cast<int>(menu_only.rows.size()) == expected_rows);
	TEST_EXPECT(menu_only.required_total == expected_required);
	TEST_EXPECT(menu_only.required_missing == expected_required);
	TEST_EXPECT(menu_only.required_wrong_kind == 0);
	// An error per Required row, a note per optional one: every row is missing.
	int errors = 0, notes = 0;
	for (const Diagnostic &d : menu_only.diagnostics) {
		if (d.code() == "requirement.missing" && d.severity == DiagnosticSeverity::Error) ++errors;
		if (d.code() == "requirement.optional_missing" && d.severity == DiagnosticSeverity::Info) ++notes;
		// What each is about is the row's, and no file of the project is at fault.
		const RequirementRow *row = row_named(menu_only, subject_target(d).c_str());
		TEST_EXPECT(row && row->role == editor_test::requirement_of(d).role && d.asset.empty());
		TEST_EXPECT(d.message.find("Without it: ") != std::string::npos);
	}
	TEST_EXPECT(errors == expected_required && notes == expected_rows - expected_required);
	TEST_EXPECT(static_cast<int>(menu_only.diagnostics.size()) == expected_rows);
	TEST_EXPECT(unmet_required_roles(menu_only).size() == static_cast<size_t>(expected_required));
	TEST_EXPECT(row_named(menu_only, "resource.pff") == nullptr);   // a build output
	TEST_EXPECT(row_named(menu_only, "*.npj/*.npz") == nullptr);    // a pattern
	TEST_EXPECT(row_named(menu_only, "failsafe.bad") == nullptr);   // mission phase, off
	TEST_EXPECT(row_named(menu_only, "main.mnu") != nullptr);
	TEST_EXPECT(row_named(menu_only, "main.mnu")->role == "main_menu");
	TEST_EXPECT(row_named(menu_only, "main.mnu")->required);
	TEST_EXPECT(row_named(menu_only, "main.mnu")->expected_kind == AssetKind::Menu);
	TEST_EXPECT(row_named(menu_only, "main.mnu")->state == RequirementState::Missing);
	TEST_EXPECT(row_named(menu_only, "hiscore.txt") != nullptr);
	TEST_EXPECT(!row_named(menu_only, "hiscore.txt")->required); // optional rows are listed, not demanded
	// Rows keep the manifest's phase-major order.
	int last_phase = BOOT_PHASE_BOOT;
	for (const RequirementRow &row : menu_only.rows) {
		TEST_EXPECT(row.phase >= last_phase);
		last_phase = row.phase;
	}

	doc.features.mission = true;
	const RequirementReport with_missions = evaluate_requirements(doc, empty);
	TEST_EXPECT(with_missions.rows.size() > menu_only.rows.size());
	TEST_EXPECT(row_named(with_missions, "failsafe.bad") != nullptr && !row_named(with_missions, "failsafe.bad")->required);
	TEST_EXPECT(row_named(with_missions, "ammo.def")->required);
	TEST_EXPECT(requirement_phase_enabled(doc, BOOT_PHASE_MISSION));
	doc.features.mission = false;
	TEST_EXPECT(!requirement_phase_enabled(doc, BOOT_PHASE_MISSION));
	TEST_EXPECT(requirement_phase_enabled(doc, BOOT_PHASE_BOOT));

	// S14: a mission in a project whose Missions feature is off is a warning on the project, said
	// once whatever the missions' number (the files a mission needs at its start are not checked);
	// none with the feature on, none with no mission.
	const auto feature_off = [](const RequirementReport &report) {
		std::vector<const Diagnostic *> found;
		for (const Diagnostic &d : report.diagnostics)
			if (d.code() == "project.mission.feature_off") found.push_back(&d);
		return found;
	};
	TEST_EXPECT(feature_off(menu_only).empty());
	TEST_EXPECT(editor_test::write_text(paths.root + "/missions/a.bms", "x") &&
	            editor_test::write_text(paths.root + "/missions/b.bms", "x"));
	const AssetScan with_missions_in = scan_project_assets(paths, doc);
	const RequirementReport off = evaluate_requirements(doc, with_missions_in);
	TEST_EXPECT(feature_off(off).size() == 1 && feature_off(off)[0]->severity == DiagnosticSeverity::Warning &&
	            feature_off(off)[0]->asset.empty() && feature_off(off)[0]->message.find("a.bms") != std::string::npos);
	TEST_EXPECT(off.rows.size() == menu_only.rows.size() && off.required_missing == menu_only.required_missing);
	doc.features.mission = true;
	TEST_EXPECT(feature_off(evaluate_requirements(doc, with_missions_in)).empty());
	return 0;
}

static int test_files_satisfy_rows_by_name_and_kind() {
	editor_test::TempProjectDir dir("opennova_editor_requirements_files_test");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("F"), "F", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(dir.file("F"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/ui/Main.MNU", "<MENU/>"));       // case differs: still it
	TEST_EXPECT(editor_test::write_text(paths.root + "/text/gametext.bin", "RTXT...."));
	TEST_EXPECT(editor_test::write_text(paths.root + "/text/vmacros.bin", "not a string table"));
	TEST_EXPECT(editor_test::write_text(paths.root + "/defs/items.def", "begin\nend\n"));

	const RequirementReport report = evaluate_requirements(doc, scan_project_assets(paths, doc));
	const RequirementRow *menu = row_named(report, "main.mnu");
	TEST_EXPECT(menu && menu->state == RequirementState::Present);
	TEST_EXPECT(menu->asset_path == "ui/Main.MNU");
	TEST_EXPECT(menu->found_kind == AssetKind::Menu);
	TEST_EXPECT(row_named(report, "gametext.bin")->state == RequirementState::Present);
	TEST_EXPECT(row_named(report, "items.def")->state == RequirementState::Present);
	const RequirementRow *vmacros = row_named(report, "vmacros.bin");
	TEST_EXPECT(vmacros && vmacros->state == RequirementState::WrongKind);
	TEST_EXPECT(vmacros->found_kind == AssetKind::RawBin);
	TEST_EXPECT(report.required_wrong_kind == 1);

	int wrong = 0, missing = 0;
	bool keyhelp_reported = false, vmacros_reported = false;
	for (const Diagnostic &d : report.diagnostics) {
		if (d.code() == "requirement.wrong_kind") ++wrong;
		if (d.code() == "requirement.missing") ++missing;
		// A missing file names no file of the project (Problems opens nothing for it).
		if (d.code() == "requirement.missing" && subject_target(d) == "keyhelp.bin" && editor_test::requirement_of(d).role == "keyhelp" && d.asset.empty())
			keyhelp_reported = true;
		// A file of the wrong kind is the offending file's finding.
		if (d.code() == "requirement.wrong_kind" && d.asset == "text/vmacros.bin" && editor_test::requirement_of(d).role == "vmacros" &&
		    subject_target(d) == "vmacros.bin")
			vmacros_reported = true;
	}
	TEST_EXPECT(wrong == 1 && keyhelp_reported && vmacros_reported);
	TEST_EXPECT(missing == report.required_missing);
	TEST_EXPECT(report.required_missing == report.required_total - 3 - 1); // three present, one wrong
	// Create every missing file names the unmet rows, the wrong-kind one included (it is
	// refused there), never a present one, in manifest order.
	const std::vector<std::string> roles = unmet_required_roles(report);
	TEST_EXPECT(static_cast<int>(roles.size()) == report.required_missing + report.required_wrong_kind);
	TEST_EXPECT(std::find(roles.begin(), roles.end(), "vmacros") != roles.end());
	TEST_EXPECT(std::find(roles.begin(), roles.end(), "main_menu") == roles.end());
	TEST_EXPECT(std::find(roles.begin(), roles.end(), "gametext") == roles.end());
	TEST_EXPECT(std::find(roles.begin(), roles.end(), "keyhelp") > std::find(roles.begin(), roles.end(), "vmacros"));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_row_set_follows_the_manifest_and_features();
	failures += test_files_satisfy_rows_by_name_and_kind();
	if (failures == 0) std::printf("editor_requirements: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
