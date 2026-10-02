// S13 V1 (ADR 0046 S13): the Problems window's model (ui/problems_list), with no ImGui. The
// lines of each grouping, a header before each group's findings and a group of notes alone
// folded the first time it shows; each finding known by what it is about, so a press on a fix
// survives a validation that reorders the findings (the one before it gone, the release on its
// fix still counts, one on the finding that slid into its place does not) and the finding
// selected stays selected; a fix known by its request too, so a release on a fix that changed
// since the press (the game data gained the file: a Create become an Import) counts for
// nothing; the fold rules (notes alone fold on first sight and open again when
// a warning joins, unless the user folded them; another project starts over); what a Fix all,
// the summary's Fix alls and a Use fix say and raise, a missing texture's placeholders in one
// line, no Rewrite for a file that does not serialize; a confirmation waiting for Apply
// proposed again as the findings go, its version moving (an Apply pressed on the old one counts
// for nothing) and another project closing it; and the fixes planned only for the findings
// asked, never for every one a refresh lists.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/requirements/requirements.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/problems_list.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

namespace {

using Words = std::vector<std::string>;

RequirementRow missing_row(const char *role, const char *name, AssetKind kind) {
	RequirementRow row;
	row.role = role;
	row.name = name;
	row.required = true;
	row.expected_kind = kind;
	row.state = RequirementState::Missing;
	return row;
}

Diagnostic missing_finding(const char *role, const char *name) {
	Diagnostic d = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.missing",
	                                       std::string("Missing required file ") + name + ".");
	d.subject = RequirementSubject{role, name};
	return d;
}

Diagnostic finding(DiagnosticSeverity severity, const char *code, const char *message,
                   const char *file = "", const char *field = "") {
	return editor_test::finding_of(severity, code, message, file, field);
}

AssetEntry file_entry(const std::string &name, const std::string &path, AssetKind kind) {
	AssetEntry entry;
	entry.logical_name = name;
	entry.relative_path = path;
	entry.kind = kind;
	return entry;
}

// Two required files the project lacks, each made by a factory (the game data has
// gametext.bin; a spare string table and two menus could stand in for them), a catalog error
// on a record's field, a warning in a menu and a stylesheet's note in another.
SessionView problems_view() {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Problems";
	editor_test::own(v.project.scan).entries = { file_entry("items.def", "defs/items.def",
														 AssetKind::ItemDefs),
		file_entry("a.mnu", "menus/a.mnu", AssetKind::Menu),
		file_entry("b.mnu", "menus/b.mnu", AssetKind::Menu),
		file_entry("spare.bin", "strings/spare.bin", AssetKind::Strings) };
	editor_test::own(v.project.scan).index(); // a scan made by hand is indexed, as the session's is
	editor_test::own(v.project.requirements).rows = { missing_row("gametext", "gametext.bin",
															  AssetKind::Strings),
		missing_row("main_menu", "main.mnu", AssetKind::Menu) };
	editor_test::own(v.project.requirements).required_total = 2;
	editor_test::own(v.project.requirements).required_missing = 2;
	v.project.retail_files = {"gametext.bin"};
	Diagnostic type = finding(DiagnosticSeverity::Error, "catalog.item_type",
	                          "Alpha: choose an item type.", "defs/items.def", "type");
	type.record = "Marker";
	type.line = 12;
	type.row_id = 4;
	type.record_kind = 2;
	v.findings.diagnostics = {missing_finding("gametext", "gametext.bin"),
	                 missing_finding("main_menu", "main.mnu"), type,
	                 finding(DiagnosticSeverity::Warning, "menu.duplicate_window",
	                         "Bravo: two windows are named GO.", "menus/a.mnu"),
	                 finding(DiagnosticSeverity::Info, "style.unused", "Charlie: nothing uses it.",
	                         "menus/b.mnu")};
	return v;
}

// The list's lines in words: "#<title>" for a group's header, the finding's message's first
// word ("Alpha:") for a finding.
Words shown(const ProblemsList &list, const ProblemAnswer &answer, const SessionView &v) {
	Words out;
	for (const ProblemsList::Line &line : list.lines()) {
		const std::string &message = v.findings.diagnostics[line.finding].message;
		out.push_back(line.header ? "#" + answer.groups[line.group].title
		                          : message.substr(0, message.find(' ')));
	}
	return out;
}

// Grouped by kind at first, a header before each group's findings, the stylesheets' group of
// notes alone folded; as one list, every finding worst first; by file, the project's own
// findings first, b.mnu's notes folded.
int test_lines() {
	SessionView v = problems_view();
	ProblemsList list;
	const ProblemAnswer *answer = &list.refresh(v);
	TEST_EXPECT(shown(list, *answer, v) == Words({"#Required files", "Missing", "Missing",
	                                              "#Catalogs", "Alpha:", "#Menus", "Bravo:",
	                                              "#Stylesheets"}));
	TEST_EXPECT(list.folded("style") && !list.folded("requirement"));
	const ProblemGroup &required = answer->groups[0];
	TEST_EXPECT(ProblemsList::severity_counts(required.errors, required.warnings, required.infos) ==
	            "2 errors");
	TEST_EXPECT(ProblemsList::severity_counts(2, 1, 3) == "2 errors, 1 warning, 3 info");
	list.query().grouping = ProblemGrouping::None;
	answer = &list.refresh(v);
	TEST_EXPECT(shown(list, *answer, v) ==
	            Words({"Missing", "Missing", "Alpha:", "Bravo:", "Charlie:"}));
	TEST_EXPECT(answer->errors == 3 && answer->warnings == 1 && answer->infos == 1);
	list.query().grouping = ProblemGrouping::File;
	answer = &list.refresh(v);
	TEST_EXPECT(shown(list, *answer, v) == Words({"#Project", "Missing", "Missing",
	                                              "#defs/items.def", "Alpha:", "#menus/a.mnu",
	                                              "Bravo:", "#menus/b.mnu"}));
	// A header toggled: folded away with its rows, and open again.
	list.toggle_fold("defs/items.def");
	answer = &list.refresh(v);
	TEST_EXPECT(shown(list, *answer, v) == Words({"#Project", "Missing", "Missing",
	                                              "#defs/items.def", "#menus/a.mnu", "Bravo:",
	                                              "#menus/b.mnu"}));
	list.toggle_fold("defs/items.def");
	list.toggle_fold("menus/b.mnu");
	answer = &list.refresh(v);
	TEST_EXPECT(shown(list, *answer, v).size() == 9);
	// Where a finding is, and the summary.
	TEST_EXPECT(ProblemsList::location_of(v.findings.diagnostics[2], false) ==
	            "items.def:12 - Marker - type");
	TEST_EXPECT(ProblemsList::location_of(v.findings.diagnostics[2], true) ==
	            "defs/items.def:12 - Marker - type");
	TEST_EXPECT(ProblemsList::location_of(v.findings.diagnostics[0], false) == "gametext.bin");
	TEST_EXPECT(ProblemsList::summary(*v.project.requirements) ==
	            "The game cannot start: 2 required files are missing.");
	RequirementReport met;
	TEST_EXPECT(ProblemsList::summary(met).empty());
	return 0;
}

// A finding is known by what it is about: a press on main.mnu's Create survives the
// validation that takes gametext.bin's finding away (main.mnu's slides into its place, its
// fix keeps its id), while a release on the finding that slid under the mouse (menu_style's)
// counts for nothing; a key's press and release in one frame counts. Findings alike but for
// their record keep apart: the one selected stays selected when one before it goes.
int test_keys() {
	SessionView v = problems_view();
	editor_test::own(v.project.requirements).rows.push_back(
	        missing_row("menu_style", "menu_style.mns", AssetKind::MenuStyle));
	editor_test::own(v.project.requirements).required_missing = 3;
	Diagnostic unnamed = finding(DiagnosticSeverity::Error, "catalog.name_empty",
	                             "Lima: a record has no name.", "defs/items.def", "name");
	unnamed.row_id = 10;
	unnamed.record_kind = 2;
	Diagnostic unnamed_too = unnamed;
	unnamed_too.row_id = 11;
	Diagnostic unnamed_three = unnamed;
	unnamed_three.row_id = 12;
	const std::vector<Diagnostic> findings = {missing_finding("gametext", "gametext.bin"),
	                                          missing_finding("main_menu", "main.mnu"),
	                                          missing_finding("menu_style", "menu_style.mns"),
	                                          unnamed, unnamed_too, unnamed_three};
	v.findings.diagnostics = findings;
	ProblemsList list;
	list.query().grouping = ProblemGrouping::None;
	list.refresh(v);
	const std::string main_key = list.key(1);
	const ProblemFix create_main = list.fixes(v, 1).front();
	TEST_EXPECT(create_main.request.kind == EditorRequestKind::CreateMissing &&
	            create_main.request.roles == Words({"main_menu"}));
	ProblemsList::PressLatch latch;
	TEST_EXPECT(!latch.released_on(list.fix_id(v, 1, create_main), true, false, false)); // pressed

	v.findings.diagnostics.erase(v.findings.diagnostics.begin());
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	TEST_EXPECT(list.key(0) == main_key);
	const ProblemFix after = list.fixes(v, 0).front();
	const ProblemFix slid = list.fixes(v, 1).front();
	// Released over menu_style's (slid under the mouse), then over main.mnu's where it went;
	// a key's press and release in one frame.
	TEST_EXPECT(!latch.released_on(list.fix_id(v, 1, slid), false, true, true));
	TEST_EXPECT(latch.released_on(list.fix_id(v, 0, after), false, true, true));
	TEST_EXPECT(latch.released_on(list.fix_id(v, 1, slid), true, true, false));
	TEST_EXPECT(list.resolve(v, {v.project.root, main_key}) == 0);
	TEST_EXPECT(list.resolve(v, {"C:/mods/Another", main_key}) == SIZE_MAX);

	// The second of three findings alike but for their record selected; the first goes: the
	// same finding, now the first of two, is still the selected one.
	v.findings.diagnostics = findings;
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	list.toggle_selected(v, 4);
	TEST_EXPECT(list.selected() == 4 && v.findings.diagnostics[list.selected()].row_id == 11);
	v.findings.diagnostics.erase(v.findings.diagnostics.begin() + 3);
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	TEST_EXPECT(list.selected() == 3 && v.findings.diagnostics[list.selected()].row_id == 11);
	list.toggle_selected(v, 3); // clicked again: folded back
	TEST_EXPECT(list.selected() == SIZE_MAX);
	return 0;
}

// A fix that changes while the findings stand: the game data gains the font a finding names,
// so its first fix, a Create, becomes an Import. A fix's id holds its request, so it moves
// with it: a release on the Import of a press on the Create counts for nothing, while a new
// click on the Import takes it.
int test_fix_changes() {
	SessionView v = problems_view();
	Diagnostic font = finding(DiagnosticSeverity::Error, "reference.missing",
	                          "Kilo: the font is missing.", "menus/a.mnu", "font.name");
	font.subject = ReferenceSubject{ReferenceKind::Font, "Custom.fnt"};
	v.findings.diagnostics.push_back(font);
	const size_t kilo = v.findings.diagnostics.size() - 1;
	ProblemsList list;
	list.refresh(v);
	const std::string key = list.key(kilo);
	const ProblemFix create = list.fixes(v, kilo).front();
	TEST_EXPECT(create.label == "Create Custom.fnt" &&
	            create.request.kind == EditorRequestKind::CreateFile);
	const std::string pressed = list.fix_id(v, kilo, create);
	ProblemsList::PressLatch latch;
	TEST_EXPECT(!latch.released_on(pressed, true, false, false)); // pressed on the Create

	v.project.retail_files = {"Custom.fnt", "gametext.bin"};
	v.revisions.touch(ViewConcern::Files); // the findings stand
	list.refresh(v);
	const ProblemFix import = list.fixes(v, kilo).front();
	TEST_EXPECT(import.request.kind == EditorRequestKind::PreviewInstallImport &&
	            import.request.names == Words({"Custom.fnt"}));
	// The same finding, another request: another id (the Create's own id stands).
	const std::string now = list.fix_id(v, kilo, import);
	TEST_EXPECT(list.key(kilo) == key && list.fix_id(v, kilo, create) == pressed && now != pressed);
	TEST_EXPECT(!latch.released_on(now, false, true, true)); // released on the Import: nothing
	TEST_EXPECT(!latch.released_on(now, true, false, false)); // a new press on it
	TEST_EXPECT(latch.released_on(now, false, true, true));   // and its release: applied
	return 0;
}

// A group of notes alone (the stylesheets') folds the first time it shows; a warning joining
// it opens it, and it stays open when it holds notes alone again. A group the user folded (the
// menus') stays folded when a warning joins it. Another project's groups start over.
int test_folding() {
	SessionView v = problems_view();
	ProblemsList list;
	list.refresh(v);
	TEST_EXPECT(list.folded("style"));
	v.findings.diagnostics.push_back(finding(DiagnosticSeverity::Warning, "style.line_ending",
	                                "Delta: its line ends changed.", "menus/b.mns"));
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	TEST_EXPECT(!list.folded("style"));
	v.findings.diagnostics.pop_back();
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	TEST_EXPECT(!list.folded("style"));
	list.toggle_fold("menu");
	list.refresh(v);
	TEST_EXPECT(list.folded("menu"));
	v.findings.diagnostics.push_back(finding(DiagnosticSeverity::Warning, "menu.duplicate_screen",
	                                "Echo: another warning.", "menus/a.mnu"));
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	TEST_EXPECT(list.folded("menu"));
	// Another project: its own groups, the notes folded again.
	SessionView other = problems_view();
	other.project.root = "C:/mods/Another";
	list.refresh(other);
	TEST_EXPECT(!list.folded("menu") && list.folded("style"));
	return 0;
}

// What the Fix alls say and raise: the required files' group one Create naming every role; the
// summary's one Create for what factories make and one import list for what only the game data
// has, each of one kind; a Use fix its rename; two missing textures' placeholders in one line;
// no Rewrite for a file that does not serialize, and Only fixable lists what has a fix.
int test_proposals() {
	SessionView v = problems_view();
	ProblemsList list;
	list.refresh(v);
	const ProblemsList::Proposal &required = list.group_fixes(0);
	TEST_EXPECT(required.findings == 2 && required.requests.size() == 1 &&
	            required.requests[0].kind == EditorRequestKind::CreateMissing &&
	            required.requests[0].roles == Words({"gametext", "main_menu"}));
	TEST_EXPECT(required.lines ==
	            Words({"Create 2 files: gametext.bin, main.mnu. They start as placeholder "
	                   "content, to replace with your own.",
	                   "What this does to the files cannot be undone with Undo."}));
	// The catalog's and the menus' findings have no fix: no Fix all.
	TEST_EXPECT(list.group_fixes(1).requests.empty() && list.group_fixes(2).requests.empty());

	// The summary's Fix alls: a Create and an import list, each confirmed by kind.
	editor_test::own(v.project.requirements)
			.rows.push_back(missing_row("cmap_menu", "cmap.mnu", AssetKind::Menu));
	editor_test::own(v.project.requirements).required_missing = 3;
	v.project.retail_files = {"cmap.mnu", "gametext.bin"};
	v.findings.diagnostics.push_back(missing_finding("cmap_menu", "cmap.mnu"));
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	const std::vector<EditorRequest> &summary = list.required_fixes().requests;
	TEST_EXPECT(summary.size() == 2 && ProblemsList::fix_all_label(summary[0]) == "Create 2" &&
	            ProblemsList::fix_all_label(summary[1]) == "Import 1 from the game data...");
	TEST_EXPECT(ProblemsList::summary(*v.project.requirements) ==
	            "The game cannot start: 3 required files are missing.");
	const ProblemsList::Proposal create =
	        list.propose(v, list.required_fix(v, EditorRequestKind::CreateMissing));
	TEST_EXPECT(create.requests.size() == 1 &&
	            create.requests[0].roles == Words({"gametext", "main_menu"}));
	const ProblemsList::Proposal import =
	        list.propose(v, list.required_fix(v, EditorRequestKind::PreviewInstallImport));
	TEST_EXPECT(import.requests.size() == 1 && import.requests[0].names == Words({"cmap.mnu"}) &&
	            import.lines.front().rfind("Import cmap.mnu from the game data", 0) == 0);

	// A Use fix: gametext.bin's, spare.bin renamed to it, asked first.
	const std::vector<ProblemFix> &fixes = list.fixes(v, 0);
	const ProblemFix *use = nullptr;
	for (const ProblemFix &fix : fixes)
		if (fix.label == "Use spare.bin as gametext.bin") use = &fix;
	TEST_EXPECT(use && ProblemsList::asks_first(*use) && !ProblemsList::asks_first(fixes.front()));
	const ProblemsList::Proposal rename = list.propose(v, list.use_fix(v, 0, *use));
	TEST_EXPECT(rename.findings == 1 && rename.lines.size() == 2 &&
	            rename.lines[0] == "Use spare.bin as gametext.bin" &&
	            rename.lines[1].find("Renames spare.bin to gametext.bin.") != std::string::npos);
	TEST_EXPECT(rename.requests.size() == 1 &&
	            rename.requests[0].kind == EditorRequestKind::AssignRequirement &&
	            rename.requests[0].path == "strings/spare.bin" &&
	            rename.requests[0].role == "gametext");

	// Two textures the project lacks: their placeholders in one line, a CreateFile each.
	SessionView textures;
	textures.project.open = true;
	textures.project.root = "C:/mods/Placeholders";
	Diagnostic skin = finding(DiagnosticSeverity::Error, "reference.missing",
	                          "Golf: the texture 'skin.tga'.", "models/tank.3di", "name");
	skin.subject = ReferenceSubject{ReferenceKind::Texture, "skin.tga", std::string(), 0};
	Diagnostic puff = finding(DiagnosticSeverity::Error, "reference.missing",
	                          "Hotel: the texture 'puff.tga'.", "fx.ptl", "graphic1");
	puff.subject = ReferenceSubject{ReferenceKind::Texture, "puff.tga"};
	textures.findings.diagnostics = {skin, puff};
	ProblemsList placeholders;
	placeholders.refresh(textures);
	const ProblemsList::Proposal &made = placeholders.group_fixes(0);
	TEST_EXPECT(made.lines.size() == 2 &&
	            made.lines[0] == "Create 2 placeholder textures: skin.tga, puff.tga. Each is the "
	                             "checkerboard the game draws for a missing texture, to replace "
	                             "with your own art.");
	TEST_EXPECT(made.requests.size() == 2 &&
	            made.requests[0].kind == EditorRequestKind::CreateFile &&
	            made.requests[0].path == "skin.tga" && made.requests[0].file_kind == "texture" &&
	            made.requests[1].path == "puff.tga");

	// No Rewrite of a file that does not serialize; Only fixable: the finding a fix is offered for.
	SessionView rewrite;
	rewrite.project.open = true;
	rewrite.project.root = "C:/mods/Rewrite";
	rewrite.findings.diagnostics = {finding(DiagnosticSeverity::Warning, "catalog.ignored_input",
	                               "Delta: a key the game ignores.", "defs/weapon.def"),
	                       finding(DiagnosticSeverity::Error, "catalog.unserializable",
	                               "Echo: this cannot be written.", "defs/weapon.def"),
	                       finding(DiagnosticSeverity::Warning, "catalog.ignored_input",
	                               "Foxtrot: a key the game ignores.", "defs/ammo.def")};
	ProblemsList fixable;
	fixable.query().grouping = ProblemGrouping::None;
	fixable.refresh(rewrite);
	TEST_EXPECT(fixable.fixes(rewrite, 0).empty() && fixable.fixes(rewrite, 2).size() == 1 &&
	            fixable.fixes(rewrite, 2).front().label == "Rewrite ammo.def");
	fixable.query().fixable = true;
	const ProblemAnswer &only = fixable.refresh(rewrite);
	TEST_EXPECT(only.rows == std::vector<size_t>({2}) && only.total() == 3);
	return 0;
}

// A confirmation waits for Apply, proposed again as the view moves: a finding gone (main.mnu
// made elsewhere) changes what it says and moves its version, so an Apply pressed on the old
// version counts for nothing on its release while a key's does; its project closed, it closes.
int test_confirmation() {
	SessionView v = problems_view();
	ProblemsList list;
	list.ask(v, list.fix_all_of(v, list.refresh(v).groups[0].rows));
	const uint64_t asked = list.version();
	TEST_EXPECT(!list.changed() && list.shown().requests.size() == 1 &&
	            list.shown().lines.front().rfind("Create 2 files: gametext.bin, main.mnu.", 0) ==
	                    0);
	TEST_EXPECT(list.follow(v) && list.version() == asked); // the view as it was: nothing moves
	ProblemsList::PressLatch apply;
	TEST_EXPECT(!apply.released_on(std::to_string(list.version()), true, false, false)); // pressed

	v.findings.diagnostics.erase(v.findings.diagnostics.begin() + 1);
	editor_test::own(v.project.requirements).rows[1].state = RequirementState::Present;
	editor_test::own(v.project.requirements).required_missing = 1;
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	list.refresh(v);
	TEST_EXPECT(list.follow(v) && list.changed() && list.version() == asked + 1);
	TEST_EXPECT(list.shown().lines.front() ==
	                    "Create gametext.bin. It starts as placeholder content, to replace with "
	                    "your own." &&
	            list.shown().requests.size() == 1 &&
	            list.shown().requests[0].roles == Words({"gametext"}));
	// Released on the new version, pressed on the old: nothing; a key's Enter: applied.
	TEST_EXPECT(!apply.released_on(std::to_string(list.version()), false, true, true));
	TEST_EXPECT(apply.released_on(std::to_string(list.version()), true, true, false));
	// Nothing changes while the view stands; applied, it shows nothing.
	TEST_EXPECT(list.follow(v) && list.version() == asked + 1);
	list.close();
	TEST_EXPECT(list.shown().requests.empty());

	// Another project (or none): it closes.
	list.ask(v, list.fix_all_of(v, list.refresh(v).groups[0].rows));
	SessionView other = v;
	other.project.root = "C:/mods/Another";
	TEST_EXPECT(!list.follow(other));
	SessionView closed;
	TEST_EXPECT(!list.follow(closed));
	return 0;
}

// A thousand findings in fifty catalogs: a refresh, grouped or not, plans no finding's fixes
// (a group's Fix all reads the bulk ones, unkept); the fixes are planned for the findings asked,
// each once while what they read stands.
int test_fixes_lazy() {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Many";
	for (int file = 0; file < 50; ++file) {
		const std::string name = "f" + std::to_string(file) + ".def";
		editor_test::own(v.project.scan)
				.entries.push_back(file_entry(name, "defs/" + name, AssetKind::ItemDefs));
	}
	for (size_t i = 0; i < 1000; ++i) {
		const std::string message = "Finding " + std::to_string(i) + ": a line the game ignores.";
		const std::string &file = v.project.scan->entries[i / 20].relative_path;
		Diagnostic d = finding(DiagnosticSeverity::Warning, "catalog.ignored_input",
		                       message.c_str(), file.c_str(), "name");
		d.row_id = i + 1;
		d.record_kind = 2;
		d.line = i + 1;
		v.findings.diagnostics.push_back(d);
	}
	editor_test::own(v.project.scan).index();
	ProblemsList list;
	list.query().grouping = ProblemGrouping::None;
	TEST_EXPECT(list.refresh(v).rows.size() == 1000 && list.lines().size() == 1000);
	TEST_EXPECT(list.fixes_asked() == 0);
	list.query().grouping = ProblemGrouping::File;
	TEST_EXPECT(list.refresh(v).groups.size() == 50 && list.lines().size() == 1050);
	TEST_EXPECT(list.fixes_asked() == 0);
	// Each file's twenty Rewrites merged into its one.
	TEST_EXPECT(list.group_fixes(0).findings == 20 && list.group_fixes(0).requests.size() == 1);
	list.fixes(v, 500);
	list.fixes(v, 501);
	list.fixes(v, 500);
	TEST_EXPECT(list.fixes_asked() == 2);
	return 0;
}

} // namespace

int main() {
	if (test_lines() != 0) return 1;
	if (test_keys() != 0) return 1;
	if (test_fix_changes() != 0) return 1;
	if (test_folding() != 0) return 1;
	if (test_proposals() != 0) return 1;
	if (test_confirmation() != 0) return 1;
	if (test_fixes_lazy() != 0) return 1;
	std::printf("editor_problems_list: all tests passed\n");
	return 0;
}
