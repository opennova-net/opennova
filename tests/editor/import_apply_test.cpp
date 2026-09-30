// An import with the files it needs, through the session (ADR 0046 S11g): the preview plans a
// folder's menu with the closure it names known (the font, the texture and the menu it names
// found beside it, the font that menu names, a texture found nowhere, a screen reference not
// followed), and Import with the rows the plan takes writes exactly those, after which every
// reference to a file resolves but the one not found; a row left unchecked is not written; a
// file changed since the preview (a new dependency, one gone, one the project had deleted
// outside the editor) writes nothing and the plan comes back; the write stages every file
// under the cache first, so a destination that cannot be written leaves the project as it
// was, a failure while publishing is said file by file, an import record goes in with its
// file, and a crash's staging folder is never scanned and goes with the next import; an
// .o3d's textures come through its plan, a direct import saying which it leaves; a plan cut
// by its cap holds a converter's files whole or not at all; the unsaved guard reads the plan the
// dialog shows (S13 A3); and, with a packed game install (OPENNOVA_JO_DIR), a retail menu and
// its stylesheet imported with the files they need, against what is known of them without the
// planner.
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/project/project_files.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/import_test_support.h"
#include "editor/png_test_support.h"

using namespace opennova::editor;
using namespace import_test;
namespace fs = std::filesystem;

namespace {

using State = ImportPlanRow::State;

// A folder with a menu, what it names and what that names in turn: a.mnu's GO names the font
// arial99, the texture logo.tga (LOGO.TGA beside it) and the screen B of b.mnu; its KEEP
// names gone.tga, which no place has; b.mnu's BACK names the font fb. The closure: a.mnu,
// arial99.fnt, LOGO.TGA, b.mnu and fb.fnt; gone.tga not found; the screen not followed (a
// screen is no file).
std::string closure_folder(const editor_test::TempProjectDir &dir) {
	const std::string art = dir.file("art");
	editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", font("arial99") + image("logo.tga") +
	                                                                              go_to("b.mnu", "B")) +
	                                                        window("STATIC", "KEEP", image("gone.tga"))));
	editor_test::write_text(art + "/b.mnu", screen("B", window("BUTTON", "BACK", font("fb"))));
	editor_test::write_text(art + "/arial99.fnt", "fnt");
	editor_test::write_text(art + "/fb.fnt", "fnt");
	editor_test::write_text(art + "/LOGO.TGA", "tga");
	return art;
}

// The preview of `paths` with the files they need: what the request and its plan came to.
ActionOutcome preview(ProjectSession &session, const std::vector<std::string> &paths) {
	EditorRequest request = request::of(EditorRequestKind::PreviewImport);
	request.paths = paths;
	request.with_dependencies = true;
	return editor_test::handle_to_end(session, request);
}

// Import of `imports` (the rows kept): what the request and its operation came to (S13 A3: the
// import's write refuses or fails once the request that started it is done).
ActionOutcome import(ProjectSession &session, const std::vector<ImportSource> &imports, bool replace = false) {
	EditorRequest request = request::of(EditorRequestKind::ImportFiles);
	request.imports = imports;
	request.replace = replace;
	return editor_test::handle_to_end(session, request);
}

// What a file references by the name it gives, each reference to a file resolved.
std::map<std::string, ReferenceStatus> file_references(const SessionView &view, const std::string &path) {
	std::map<std::string, ReferenceStatus> out;
	for (const GraphEdge *edge : view.findings.graph->references_of(path))
		if (reference_row(edge->kind).resolution == ReferenceResolution::File) out[edge->value] = view.findings.graph->resolve(*edge);
	return out;
}

bool finding(const ActionOutcome &outcome, const char *code, DiagnosticSeverity severity, const std::string &asset) {
	for (const Diagnostic &d : outcome.findings)
		if (d.code() == code && d.severity == severity && (asset.empty() || d.asset == asset)) return true;
	return false;
}

bool said(const SessionView &view, const std::string &line) {
	for (const std::string &output : view.activity.output)
		if (output == line) return true;
	return false;
}

// Whether an import left its staging folder behind (under the project's cache).
bool staged_left(const std::string &root) {
	std::error_code ec;
	return fs::exists(ProjectPaths::for_root(root).staging_dir, ec);
}

bool has_warning(const std::vector<Diagnostic> &findings, const char *code, const std::string &asset) {
	for (const Diagnostic &d : findings)
		if (d.code() == code && d.severity == DiagnosticSeverity::Warning && d.asset == asset) return true;
	return false;
}

} // namespace

// A folder's menu previewed with the files it needs: the plan holds the known closure, gone.tga
// not found (with what wants it) and the screen reference not followed; Import with the rows it
// takes writes those five files, the preview closes, and every reference of the two menus to a
// file resolves but gone.tga's, which stays missing.
static int test_apply_closure() {
	Project project("opennova_editor_apply_closure");
	const std::string art = closure_folder(project.dir);
	const SessionView &view = project.view();
	const ActionOutcome previewed = preview(project.session, {art + "/a.mnu"});
	const DialogsView::ImportPreview &shown = view.dialogs.import_preview;
	TEST_EXPECT(previewed.done() && shown.open && shown.with_dependencies && shown.choices.empty());
	TEST_EXPECT(shown.roots.size() == 1 && shown.roots[0].path == art + "/a.mnu" && !shown.changed);
	const ImportPlan &plan = *shown.plan;
	TEST_EXPECT(plan.rows.size() == 6 && !plan.truncated && plan.diagnostics.empty());
	const ImportPlanRow *menu = row_named(plan, "a.mnu");
	TEST_EXPECT(menu && menu->state == State::Selected && menu->selected && !menu->source.native);
	for (const char *name : {"arial99.fnt", "LOGO.TGA", "b.mnu", "fb.fnt"}) {
		const ImportPlanRow *row = row_named(plan, name);
		TEST_EXPECT(row && row->state == State::Found && row->selected && row->source.native && row->found_in == "the folder " + art);
	}
	const ImportPlanRow *fb = row_named(plan, "fb.fnt");
	TEST_EXPECT(fb && fb->needed_by.file == "b.mnu" && fb->needed_by.record == "B/BACK");
	const ImportPlanRow *gone = row_named(plan, "gone.tga");
	TEST_EXPECT(gone && gone->state == State::NotFound && !gone->selected && gone->needed_by.file == "a.mnu" &&
	            gone->needed_by.record == "A/KEEP/Appearance 1" && gone->needed_by.reference == ReferenceKind::MenuTexture);
	const ImportNotFollowed *screens = not_followed(plan, ReferenceKind::MenuScreen);
	TEST_EXPECT(screens && screens->count == 1 && screens->first == "a.mnu" && plan.not_followed.size() == 1);

	const std::vector<ImportSource> kept = selected_sources(plan);
	TEST_EXPECT(kept.size() == 5);
	const ActionOutcome imported = import(project.session, kept);
	TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open && view.dialogs.import_preview.plan->rows.empty());
	for (const char *name : {"a.mnu", "b.mnu", "arial99.fnt", "fb.fnt", "LOGO.TGA"}) TEST_EXPECT(view.project.scan->find(name));
	TEST_EXPECT(!view.project.scan->find("gone.tga") && said(view, "Imported menus/a.mnu") && said(view, "Imported LOGO.TGA"));
	// Each font and texture copied as the game's own: no import record beside it.
	TEST_EXPECT(!fs::exists(project.root() + "/LOGO.TGA" + kImportSidecarSuffix));
	const auto a = file_references(view, "menus/a.mnu");
	const auto b = file_references(view, "menus/b.mnu");
	TEST_EXPECT(a.size() == 4 && b.size() == 1);
	const auto status = [](const std::map<std::string, ReferenceStatus> &references, const char *name) {
		const auto found = references.find(name);
		return found == references.end() ? ReferenceStatus::NotAReference : found->second;
	};
	TEST_EXPECT(status(a, "arial99") == ReferenceStatus::Present && status(a, "logo.tga") == ReferenceStatus::Present &&
	            status(a, "b.mnu") == ReferenceStatus::Present && status(b, "fb") == ReferenceStatus::Present);
	TEST_EXPECT(status(a, "gone.tga") == ReferenceStatus::Missing);
	// The screen reference, not followed, resolves too: b.mnu has its screen.
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuScreen, "B", menu_screen_scope("b.mnu")) == ReferenceStatus::Present);
	return 0;
}

// A dependency row left unchecked is not imported, and its reference stays missing; the rows
// kept are.
static int test_apply_unchecked() {
	Project project("opennova_editor_apply_unchecked");
	const std::string art = closure_folder(project.dir);
	const SessionView &view = project.view();
	preview(project.session, {art + "/a.mnu"});
	std::vector<ImportSource> kept;
	for (const ImportSource &source : selected_sources(*view.dialogs.import_preview.plan))
		if (source.path != art + "/LOGO.TGA") kept.push_back(source);
	TEST_EXPECT(kept.size() == 4);
	const ActionOutcome imported = import(project.session, kept);
	TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open);
	TEST_EXPECT(view.project.scan->find("arial99.fnt") && view.project.scan->find("b.mnu") && view.project.scan->find("fb.fnt") && !view.project.scan->find("LOGO.TGA"));
	TEST_EXPECT(!fs::exists(project.root() + "/LOGO.TGA"));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.tga") == ReferenceStatus::Missing);
	return 0;
}

// The files change between the preview and Import: a.mnu names a new texture its folder has, so
// the plan is no longer the one shown. Nothing is written, the refusal says why, and the preview
// stays open on the new plan, marked changed; Import with its rows then writes them. A
// dependency gone since the preview (deleted from the folder) is refused the same way, and the
// new plan lists it as not found. A row the plan does not have is refused.
static int test_apply_changed() {
	Project project("opennova_editor_apply_changed");
	const std::string art = closure_folder(project.dir);
	const std::string root = project.root();
	const SessionView &view = project.view();
	preview(project.session, {art + "/a.mnu"});
	const std::vector<ImportSource> shown = selected_sources(*view.dialogs.import_preview.plan);
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", font("arial99") + image("logo.tga") +
	                                                                                     go_to("b.mnu", "B")) +
	                                                                    window("STATIC", "KEEP", image("new.tga")))));
	TEST_EXPECT(editor_test::write_text(art + "/new.tga", "tga"));
	const auto before = snapshot(root);
	const uint64_t seen = view.events.next_seq() - 1;
	const ActionOutcome stale = import(project.session, shown);
	TEST_EXPECT(!stale.done() &&
	            finding(stale, "import.changed", DiagnosticSeverity::Warning, std::string()));
	TEST_EXPECT(snapshot(root) == before && !view.project.scan->find("a.mnu"));
	// Planned again before writing: one ImportPlanned event, flagged (the files changed).
	const std::vector<ViewEvent> planned =
			editor_test::events_after(view, seen, ViewEventKind::ImportPlanned);
	TEST_EXPECT(view.dialogs.import_preview.open && view.dialogs.import_preview.changed &&
			planned.size() == 1 && planned[0].flag && planned[0].path.empty() &&
			planned[0].tag == 0);
	const ImportPlanRow *added = row_named(*view.dialogs.import_preview.plan, "new.tga");
	TEST_EXPECT(added && added->state == State::Found && added->selected &&
			!row_named(*view.dialogs.import_preview.plan, "gone.tga"));
	const ActionOutcome imported = import(project.session, selected_sources(*view.dialogs.import_preview.plan));
	TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open);
	TEST_EXPECT(view.project.scan->find("a.mnu") && view.project.scan->find("new.tga") &&
			view.project.scan->find("LOGO.TGA"));

	// A dependency gone since the preview.
	const std::string more = project.dir.file("more");
	TEST_EXPECT(editor_test::write_text(more + "/c.mnu", screen("C", window("STATIC", "GO", font("fc")))) &&
	            editor_test::write_text(more + "/fc.fnt", "fnt"));
	preview(project.session, {more + "/c.mnu"});
	const std::vector<ImportSource> with_font = selected_sources(*view.dialogs.import_preview.plan);
	TEST_EXPECT(with_font.size() == 2);
	std::error_code ec;
	fs::remove(more + "/fc.fnt", ec);
	const auto unchanged = snapshot(root);
	const ActionOutcome gone = import(project.session, with_font);
	TEST_EXPECT(!gone.done() && snapshot(root) == unchanged && view.dialogs.import_preview.changed);
	const ImportPlanRow *font_row = row_named(*view.dialogs.import_preview.plan, "fc");
	TEST_EXPECT(font_row && font_row->state == State::NotFound && font_row->needed_by.file == "c.mnu");
	// A row the plan does not have: refused, nothing written, the preview still open.
	const ActionOutcome unplanned = import(project.session, {{art + "/arial99.fnt", {}}});
	TEST_EXPECT(!unplanned.done() &&
	            finding(unplanned, "import.not_planned", DiagnosticSeverity::Error, "arial99.fnt"));
	TEST_EXPECT(snapshot(root) == unchanged && view.dialogs.import_preview.open);
	project.session.handle(request::cancel_import());
	TEST_EXPECT(!view.dialogs.import_preview.open);
	return 0;
}

// The write stages every file before it publishes any: where the font goes, fonts/, is a file,
// so its staging fails after the menu's, and the menu's staged file and the folder made for it
// are removed: the project is as it was, the failure a finding naming the file.
static int test_apply_staging() {
	Project project("opennova_editor_apply_staging");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/extra.mnu", screen("EXTRA", window("STATIC", "GO", ""))) &&
	            editor_test::write_text(art + "/Custom.fnt", "fnt"));
	TEST_EXPECT(editor_test::write_text(root + "/fonts", "a file where a folder goes"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const bool menus = fs::exists(root + "/menus");
	const auto before = snapshot(root);
	const ImportResult result = import_assets({{art + "/extra.mnu", {}}, {art + "/Custom.fnt", {}}},
	                                          ProjectPaths::for_root(root), *project.view().project.document, false);
	TEST_EXPECT(result.imported.empty() && result.not_imported.empty());
	bool write_failed = false;
	for (const Diagnostic &d : result.diagnostics)
		write_failed = write_failed || (d.code() == "import.write" && d.asset == "Custom.fnt" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(write_failed);
	TEST_EXPECT(snapshot(root) == before && !staged_left(root) && fs::exists(root + "/menus") == menus);
	TEST_EXPECT(!fs::exists(root + "/menus/extra.mnu"));
	// The whole selection or none of it: a file refused before the write refuses them all.
	TEST_EXPECT(editor_test::write_text(art + "/a_very_long_texture_name.tga", "tga"));
	const ImportResult refused = import_assets({{art + "/extra.mnu", {}}, {art + "/a_very_long_texture_name.tga", {}}},
	                                           ProjectPaths::for_root(root), *project.view().project.document, false);
	TEST_EXPECT(refused.imported.empty() && has_code(refused.diagnostics, "import.name") && snapshot(root) == before);
	return 0;
}

// A failure while publishing is said file by file: b.mnu's destination is a folder, so the
// rename over it fails after a.mnu was published; c.mnu is not published either. The session
// lists each in Output, the failure and what it stopped are findings, and the status says how
// far it got; nothing staged is left behind.
static int test_apply_partial_publish() {
	Project project("opennova_editor_apply_publish");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	for (const char *name : {"a", "b", "c"})
		TEST_EXPECT(editor_test::write_text(art + "/" + name + ".mnu", screen(name, window("STATIC", "GO", ""))));
	std::error_code ec;
	fs::create_directories(root + "/menus/b.mnu", ec);
	TEST_EXPECT(!ec);
	const SessionView &view = project.view();
	const ActionOutcome outcome = import(project.session, {{art + "/a.mnu", {}}, {art + "/b.mnu", {}}, {art + "/c.mnu", {}}});
	TEST_EXPECT(!outcome.done() && finding(outcome, "import.publish", DiagnosticSeverity::Error, "b.mnu") &&
	            finding(outcome, "import.not_published", DiagnosticSeverity::Warning, "c.mnu"));
	TEST_EXPECT(fs::is_regular_file(root + "/menus/a.mnu") && fs::is_directory(root + "/menus/b.mnu") &&
	            !fs::exists(root + "/menus/c.mnu") && !staged_left(root));
	TEST_EXPECT(said(view, "Imported menus/a.mnu") && said(view, "Not imported menus/b.mnu") &&
	            said(view, "Not imported menus/c.mnu"));
	TEST_EXPECT(
			view.activity.status == "1 of 3 files imported: the import stopped at menus/b.mnu.");
	TEST_EXPECT(view.project.scan->find("a.mnu") && !view.project.scan->find("c.mnu"));
	return 0;
}

// The plan an import applies reads the project as it is on the disk, not as the last refresh
// saw it: a menu previewed while the project has its font takes no font row; the font deleted
// outside the editor, Import plans again, finds the font the folder has, writes nothing and
// says the files changed; Import again writes the menu and its font.
static int test_apply_reads_the_disk() {
	Project project("opennova_editor_apply_disk");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("STATIC", "GO", font("arial99")))) &&
	            editor_test::write_text(art + "/arial99.fnt", "fnt") && editor_test::write_text(root + "/fonts/arial99.fnt", "fnt"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const SessionView &view = project.view();
	preview(project.session, {art + "/a.mnu"});
	TEST_EXPECT(view.dialogs.import_preview.plan->rows.size() == 1 && view.dialogs.import_preview.plan->rows[0].name == "a.mnu");
	const std::vector<ImportSource> shown = selected_sources(*view.dialogs.import_preview.plan);
	std::error_code ec;
	fs::remove(root + "/fonts/arial99.fnt", ec); // outside the editor: no rescan
	TEST_EXPECT(!ec && view.project.scan->find("arial99.fnt"));
	const auto before = snapshot(root);
	const ActionOutcome stale = import(project.session, shown);
	TEST_EXPECT(!stale.done() &&
	            finding(stale, "import.changed", DiagnosticSeverity::Warning, std::string()));
	TEST_EXPECT(snapshot(root) == before && view.dialogs.import_preview.changed);
	const ImportPlanRow *font_row = row_named(*view.dialogs.import_preview.plan, "arial99.fnt");
	TEST_EXPECT(font_row && font_row->state == State::Found && font_row->found_in == "the folder " + art);
	const ActionOutcome imported = import(project.session, selected_sources(*view.dialogs.import_preview.plan));
	TEST_EXPECT(imported.done() && fs::is_regular_file(root + "/menus/a.mnu") &&
	            fs::is_regular_file(root + "/fonts/arial99.fnt"));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Font, "arial99") ==
			ReferenceStatus::Present);
	return 0;
}

// The unsaved guard reads the plan the import dialog shows (S13 A3: the ImportPlan operation's),
// not one made again in the request: a catalog open with unsaved edits, the dialog planning a file
// of its name over it, the file to import then gone. Import with replace waits on the unsaved
// prompt, which lists the catalog as the shown plan puts the file there (a plan made in the
// request would find the file gone and list nothing); its Save writes the edits, and the import,
// planning again before it writes, finds its file gone and writes nothing (import.changed).
static int test_apply_guard_reads_the_shown_plan() {
	Project project("opennova_editor_apply_guard");
	const std::string root = project.root();
	const SessionView &view = project.view();
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("items.def"));
	const Document *items = project.session.document_for("items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	EditorRequest edit = request::edit_record("defs/items.def", Edit());
	edit.edits[0].address = {items->rows()[0]->id, node_kind(opennova::def::DefRecordKind::Item), 0};
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(20);
	project.session.handle(edit);
	TEST_EXPECT(items->dirty());
	const std::string loose = project.dir.file("art/items.def");
	TEST_EXPECT(editor_test::write_text(loose, "begin \"Marker\"\nid 100001\ntype marker\nhp 30\nend\n"));
	preview(project.session, {loose});
	const ImportPlanRow *row = row_named(*view.dialogs.import_preview.plan, "items.def");
	TEST_EXPECT(view.dialogs.import_preview.open && row && row->state == State::Selected &&
	            row->destination == "defs/items.def");
	const std::vector<ImportSource> shown = selected_sources(*view.dialogs.import_preview.plan);
	std::error_code ec;
	fs::remove(loose, ec);
	TEST_EXPECT(!ec);
	EditorRequest request = request::of(EditorRequestKind::ImportFiles);
	request.imports = shown;
	request.replace = true;
	project.session.handle(request);
	const DialogsView::UnsavedPrompt &prompt = view.dialogs.unsaved_prompt;
	TEST_EXPECT(project.session.outcome().unsaved_prompt && prompt.open && prompt.action == EditorRequestKind::ImportFiles &&
	            prompt.files == std::vector<std::string>({"defs/items.def"}) && !prompt.can_discard);
	project.session.handle(request::resolve_unsaved(UnsavedChoice::Save));
	project.session.run_operations();
	std::string text, error;
	TEST_EXPECT(!items->dirty() && read_file_text(root + "/defs/items.def", text, error) &&
	            text.find("hp 20") != std::string::npos);
	TEST_EXPECT(view.activity.last_operation.kind == OperationKind::ImportApply &&
	            view.activity.last_operation.end != OperationEnd::Done && view.dialogs.import_preview.changed);
	bool changed = false;
	for (const Diagnostic &d : view.activity.last_operation.findings) changed = changed || d.code() == "import.changed";
	TEST_EXPECT(changed);
	project.session.handle(request::cancel_import());
	return 0;
}

// An .o3d's textures come with it only through its plan. Imported with the files it needs,
// spinner.o3d beside SPINNER.TGA lands the model and the texture, and the model's texture row
// resolves to it; glow.tga, found nowhere, is the plan's not-found row and the import's
// warning. Imported alone (no preview: a direct import), the model lands and each texture it
// names that the import does not bring is a warning saying how to bring it.
static int test_apply_scene_textures() {
	Project project("opennova_editor_apply_scene");
	const std::string root = project.root();
	const std::string scene = project.dir.file("scene");
	std::error_code ec;
	fs::create_directories(scene, ec);
	const fs::path fixture =
	        fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "o3d" / "spinner.o3d";
	fs::copy_file(fixture, scene + "/spinner.o3d", ec);
	TEST_EXPECT(!ec && editor_test::write_text(scene + "/SPINNER.TGA", "tga")); // spinner.o3d names spinner.tga and glow.tga
	const SessionView &view = project.view();
	preview(project.session, {scene + "/spinner.o3d"});
	const ImportPlan plan = *view.dialogs.import_preview.plan;
	TEST_EXPECT(row_named(plan, "spinner.3di") && row_named(plan, "SPINNER.TGA") && row_named(plan, "glow.tga"));
	const ActionOutcome outcome = import(project.session, selected_sources(plan));
	TEST_EXPECT(outcome.done() && fs::is_regular_file(root + "/models/spinner.3di") && fs::is_regular_file(root + "/SPINNER.TGA"));
	TEST_EXPECT(has_warning(outcome.findings, "import.texture_not_imported", "spinner.3di"));
	size_t textures = 0, resolved = 0;
	for (const GraphEdge *edge : view.findings.graph->references_of("models/spinner.3di")) {
		if (edge->kind != ReferenceKind::Texture) continue;
		++textures;
		std::string file;
		if (view.findings.graph->resolve(*edge, &file) == ReferenceStatus::Present) {
			++resolved;
			TEST_EXPECT(file == "SPINNER.TGA");
		} else {
			TEST_EXPECT(edge->value == "glow.tga");
		}
	}
	TEST_EXPECT(textures == 2 && resolved == 1);
	// S11h: the warning names the texture as the graph's missing reference does, and while the
	// project lacks it offers that reference's fix, a placeholder; made, nothing is left to fix.
	Diagnostic glow;
	for (const Diagnostic &d : outcome.findings)
		if (d.code() == "import.texture_not_imported") glow = d;
	TEST_EXPECT(editor_test::reference_of(glow).kind == ReferenceKind::Texture && subject_target(glow) == "glow.tga" && editor_test::reference_of(glow).loader_arg >= 0);
	const std::vector<ProblemFix> fixes = fixes_for(glow, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Create a placeholder glow.tga" && fixes[0].bulk);
	if (fixes.empty()) return 1;
	project.session.handle(fixes[0].request);
	TEST_EXPECT(project.session.outcome().done() && view.project.scan->find("glow.tga") && fixes_for(glow, view).empty());
	// A direct import: the model alone, each texture it names a warning.
	Project direct("opennova_editor_apply_scene_direct");
	const ImportResult alone = import_assets({{scene + "/spinner.o3d", {}}}, ProjectPaths::for_root(direct.root()),
	                                         *direct.view().project.document, false);
	TEST_EXPECT(alone.imported == std::vector<std::string>({"models/spinner.3di"}));
	size_t told = 0;
	for (const Diagnostic &d : alone.diagnostics)
		if (d.code() == "import.texture_not_imported" && d.severity == DiagnosticSeverity::Warning) {
			++told;
			TEST_EXPECT(d.asset == "spinner.3di" && d.message.find("--with-dependencies") != std::string::npos &&
			            d.message.find("Include the files these need") != std::string::npos);
		}
	TEST_EXPECT(told == 2 && !fs::exists(direct.root() + "/SPINNER.TGA"));
	return 0;
}

// An import record goes in with its file: with a folder named logo.png.import where the record
// of an author's logo.png goes, the import is refused before anything is written (the other
// file of the selection too); with the way clear, the PNG and its record land together and
// the PNG is an import source.
static int test_apply_record_with_its_file() {
	Project project("opennova_editor_apply_record");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_bytes(art + "/logo.png", editor_test::gradient_png(4, 4)) &&
	            editor_test::write_text(art + "/extra.mnu", screen("EXTRA", window("STATIC", "GO", ""))));
	std::error_code ec;
	fs::create_directories(root + "/logo.png.import", ec);
	TEST_EXPECT(!ec);
	const auto before = snapshot(root);
	const ActionOutcome refused = import(project.session, {{art + "/extra.mnu", {}}, {art + "/logo.png", {}}});
	TEST_EXPECT(!refused.done() &&
	            finding(refused, "import.record", DiagnosticSeverity::Error, "logo.png"));
	TEST_EXPECT(snapshot(root) == before && !fs::exists(root + "/logo.png") && !fs::exists(root + "/menus/extra.mnu"));
	fs::remove(root + "/logo.png.import", ec);
	const ActionOutcome imported = import(project.session, {{art + "/extra.mnu", {}}, {art + "/logo.png", {}}});
	const SessionView &view = project.view();
	TEST_EXPECT(imported.done() && fs::is_regular_file(root + "/logo.png.import") &&
	            fs::is_regular_file(root + "/menus/extra.mnu") && !staged_left(root));
	TEST_EXPECT(
			view.project.imports->size() == 1 && (*view.project.imports)[0].source == "logo.png");
	return 0;
}

// A plan stopped by its cap holds a converter's files whole or not at all: 999 files chosen, then
// a clip set making two, leave no room for the clip set, so the plan does not hold it, and an
// Import naming it is refused with nothing written.
static int test_apply_cap_keeps_groups() {
	Project project("opennova_editor_apply_cap");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	std::vector<std::string> paths;
	for (int i = 0; i < 999; ++i) {
		char name[16];
		std::snprintf(name, sizeof(name), "/t%03d.txt", i);
		paths.push_back(art + name);
		TEST_EXPECT(editor_test::write_text(paths.back(), "x"));
	}
	TEST_EXPECT(editor_test::write_text(art + "/walk.o3a",
	                                    "o3a 1\nadm CHECK.adm\nrow anim_reset \"walk\"\nclip walk\nfps 30\nflags 0x1\nframes 1\n"
	                                    "bone -1 0 0 0 0.5 \"BN01 Pelvis\"\n k 0 0 0 1\n k 0 0 0 1\n"
	                                    "event 0 0 0 0x0 0.9 1.7\nevent 0 0 0 0x0 0.9 1.7\n"));
	paths.push_back(art + "/walk.o3a");
	const SessionView &view = project.view();
	EditorRequest request = request::of(EditorRequestKind::PreviewImport);
	request.paths = paths;
	project.session.handle(request);
	project.session.run_operations();
	const ImportPlan &plan = *view.dialogs.import_preview.plan;
	TEST_EXPECT(plan.truncated && plan.rows.size() == 999 && !row_named(plan, "CHECK.adm") && !row_named(plan, "walk.bad"));
	std::vector<ImportSource> every;
	for (const std::string &path : paths) every.push_back({path, {}});
	const auto before = snapshot(root);
	const ActionOutcome unplanned = import(project.session, every);
	TEST_EXPECT(!unplanned.done() &&
	            finding(unplanned, "import.not_planned", DiagnosticSeverity::Error, "walk.o3a"));
	TEST_EXPECT(snapshot(root) == before && !fs::exists(root + "/anims"));
	return 0;
}

// An import stages under the project's cache, where the scan never looks: a staging folder a
// crash left (a file whose name would break the build's name rule) is not scanned, and the
// next import removes it; an import leaves none of its own.
static int test_apply_staging_leftover() {
	Project project("opennova_editor_apply_leftover");
	const std::string root = project.root();
	const std::string staging = ProjectPaths::for_root(root).staging_dir;
	TEST_EXPECT(staging == root + "/.opennova/staging");
	TEST_EXPECT(editor_test::write_text(staging + "/crashed/menu_style.mns.staged", "left by a crash"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const SessionView &view = project.view();
	for (const AssetEntry &entry : view.project.scan->entries)
		TEST_EXPECT(entry.logical_name.find("staged") == std::string::npos && entry.relative_path.find("staging") == std::string::npos);
	for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(d.message.find("menu_style.mns.staged") == std::string::npos);
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/notes.txt", "notes"));
	const ActionOutcome imported = import(project.session, {{art + "/notes.txt", {}}});
	TEST_EXPECT(imported.done() && view.project.scan->find("notes.txt") && !fs::exists(staging));
	return 0;
}

// With a packed game install: its main.mnu and menu_style.mns imported with the files they need.
// What is known without the planner: the shipped game loads this menu and its stylesheet with
// every file they name, so the plan finds each of them (none not found, none the project cannot
// take) and, once imported, every reference of every file imported to a file resolves; and a
// shell menu's style variables, SCREEN and WINDOW targets and string ids name no file, and its
// sound bank (menu.lwf) is a file whose own references are not read: exactly those five kinds
// are not followed.
static int test_apply_retail_menu() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (a retail menu imported with the files it needs)");
	Project project("opennova_editor_apply_retail");
	editor_test::set_game_install(project.session, install);
	const SessionView &view = project.view();
	EditorRequest listed = request::preview_install_import();
	listed.names = {"main.mnu", "menu_style.mns"};
	listed.with_dependencies = true;
	project.session.handle(listed);
	project.session.run_operations();
	const ImportPlan plan = *view.dialogs.import_preview.plan;
	TEST_EXPECT(project.session.outcome().done() && view.dialogs.import_preview.open && view.dialogs.import_preview.roots.size() == 2);
	TEST_EXPECT(!plan.truncated && !has_error(plan.diagnostics));
	size_t found = 0;
	for (const ImportPlanRow &row : plan.rows) {
		if (row.state == State::NotFound)
			std::printf("editor_import retail: %s (%s) not found, needed by %s\n", row.name.c_str(),
			            asset_kind_token(row.kind), row.needed_by.file.c_str());
		TEST_EXPECT(row.state != State::NotFound && row.selected && row.problem.empty());
		found += row.state == State::Found ? 1 : 0;
	}
	TEST_EXPECT(found > 0);
	std::set<std::string> skipped;
	for (const ImportNotFollowed &entry : plan.not_followed)
		skipped.insert(entry.reference != ReferenceKind::None ? reference_row(entry.reference).token
		                                                      : asset_kind_token(entry.kind));
	TEST_EXPECT(skipped == std::set<std::string>({"style_var", "menu_screen", "menu_window", "text_id", "sound_bank"}));
	const ActionOutcome imported = import(project.session, selected_sources(plan));
	TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open);
	size_t references = 0;
	for (const ImportPlanRow &row : plan.rows) {
		TEST_EXPECT(view.project.scan->find(row.name) != nullptr);
		for (const GraphEdge *edge : view.findings.graph->references_of(row.destination)) {
			if (reference_row(edge->kind).resolution != ReferenceResolution::File) continue;
			++references;
			if (view.findings.graph->resolve(*edge) == ReferenceStatus::Present) continue;
			std::printf("editor_import retail: %s: %s %s resolves nowhere\n", row.name.c_str(), edge->record.c_str(),
			            edge->value.c_str());
			TEST_EXPECT(false);
		}
	}
	std::printf("editor_import retail: %zu files imported, %zu references to files resolved\n", plan.rows.size(), references);
	TEST_EXPECT(references > 0);
	return 0;
}

int run_import_apply_tests() {
	int failures = 0;
	failures += test_apply_closure();
	failures += test_apply_unchecked();
	failures += test_apply_changed();
	failures += test_apply_staging();
	failures += test_apply_partial_publish();
	failures += test_apply_reads_the_disk();
	failures += test_apply_scene_textures();
	failures += test_apply_record_with_its_file();
	failures += test_apply_cap_keeps_groups();
	failures += test_apply_staging_leftover();
	failures += test_apply_guard_reads_the_shown_plan();
	failures += test_apply_retail_menu();
	return failures;
}
