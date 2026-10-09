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
// planner, then built, one texture changed and built again (S13 A8: one file read, one archive
// written).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/install_view.h>
#include <editor/assets/player_files.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/requirements/requirements.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/pff/pff.h>

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
ActionOutcome import(ProjectSession &session, const std::vector<ImportChoice> &imports, bool replace = false) {
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

// A line Output says, or one folded under a line (an import's files fold under its one line).
bool said(const SessionView &view, const std::string &line) {
	const OutputLog &output = view.activity.output;
	for (size_t i = 0; i < output.size(); ++i) {
		if (output[i] == line) return true;
		for (const std::string &folded : output.folded(i))
			if (folded == line) return true;
	}
	return false;
}

// Whether an import left its staging folder behind (under the project's cache).
bool staged_left(const std::string &root) {
	std::error_code ec;
	return fs::exists(ProjectPaths::for_root(root).staging_dir, ec);
}

// What wanted a file, in a line.
std::string need_words_of(const ImportNeed &need) {
	std::string out = need.file;
	if (!need.record.empty()) out += ": " + need.record;
	if (!need.field.empty()) out += (need.record.empty() ? ": " : " ") + need.field;
	return out;
}

bool has_warning(const std::vector<Diagnostic> &findings, const char *code, const std::string &asset) {
	for (const Diagnostic &d : findings)
		if (d.code() == code && d.severity == DiagnosticSeverity::Warning && d.asset == asset) return true;
	return false;
}

} // namespace

// A folder's menu previewed with the files it needs: the plan holds the known closure, gone.tga
// not found (with what wants it) and the screen reference followed to b.mnu; Import with the rows it
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
	// The screen B of b.mnu, a symbol the planned b.mnu defines (S14): followed to nothing.
	TEST_EXPECT(plan.not_followed.empty() && plan.undefined.empty());

	const std::vector<ImportChoice> kept = selected_sources(plan);
	TEST_EXPECT(kept.size() == 5);
	const ActionOutcome imported = import(project.session, kept);
	TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open && view.dialogs.import_preview.plan->rows.empty());
	for (const char *name : {"a.mnu", "b.mnu", "arial99.fnt", "fb.fnt", "LOGO.TGA"}) TEST_EXPECT(view.project.scan->find(name));
	TEST_EXPECT(!view.project.scan->find("gone.tga") && said(view, "Imported menus/a.mnu") && said(view, "Imported textures/LOGO.TGA"));
	// One line for the import (the UX round's problems lane), its files folded under it: the import's
	// lines no longer push everything else out of Output.
	{
		const OutputLog &output = view.activity.output;
		size_t lines = 0;
		for (size_t i = 0; i < output.size(); ++i) {
			if (output[i].rfind("Imported 5 files (", 0) != 0) continue;
			++lines;
			TEST_EXPECT(output.folded(i).size() == 5 && output[i].find("): ") != std::string::npos &&
			            output[i].find("Menu 2") != std::string::npos);
		}
		bool loose = false;
		for (const std::string &line : output) loose = loose || line == "Imported menus/a.mnu";
		TEST_EXPECT(lines == 1 && !loose);
	}
	// Each font and texture copied as the game's own: no import record beside it.
	TEST_EXPECT(!fs::exists(project.root() + "/textures/LOGO.TGA" + kImportSidecarSuffix));
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
	std::vector<ImportChoice> kept;
	for (const ImportChoice &source : selected_sources(*view.dialogs.import_preview.plan))
		if (source.path != art + "/LOGO.TGA") kept.push_back(source);
	TEST_EXPECT(kept.size() == 4);
	const ActionOutcome imported = import(project.session, kept);
	TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open);
	TEST_EXPECT(view.project.scan->find("arial99.fnt") && view.project.scan->find("b.mnu") && view.project.scan->find("fb.fnt") && !view.project.scan->find("LOGO.TGA"));
	TEST_EXPECT(!fs::exists(project.root() + "/textures/LOGO.TGA"));
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
	const std::vector<ImportChoice> shown = selected_sources(*view.dialogs.import_preview.plan);
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
	const std::vector<ImportChoice> with_font = selected_sources(*view.dialogs.import_preview.plan);
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
	const std::vector<ImportChoice> shown = selected_sources(*view.dialogs.import_preview.plan);
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
	// The project holds items.def: the row is held, the import asked to replace it.
	TEST_EXPECT(view.dialogs.import_preview.open && row && row->state == State::Selected && row->held && !row->selected &&
	            row->destination == "defs/items.def");
	if (!row) return 1;
	const std::vector<ImportChoice> shown = {row->source};
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
	TEST_EXPECT(!items->dirty() && opennova::io::read_file_text(root + "/defs/items.def", text, error) &&
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
	TEST_EXPECT(outcome.done() && fs::is_regular_file(root + "/models/spinner.3di") && fs::is_regular_file(root + "/textures/SPINNER.TGA"));
	TEST_EXPECT(has_warning(outcome.findings, "import.texture_not_imported", "spinner.3di"));
	size_t textures = 0, resolved = 0;
	for (const GraphEdge *edge : view.findings.graph->references_of("models/spinner.3di")) {
		if (edge->kind != ReferenceKind::Texture) continue;
		++textures;
		std::string file;
		if (view.findings.graph->resolve(*edge, &file) == ReferenceStatus::Present) {
			++resolved;
			TEST_EXPECT(file == "textures/SPINNER.TGA");
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
	TEST_EXPECT(told == 2 && !fs::exists(direct.root() + "/textures/SPINNER.TGA"));
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
	fs::create_directories(root + "/textures/logo.png.import", ec);
	TEST_EXPECT(!ec);
	const auto before = snapshot(root);
	const ActionOutcome refused = import(project.session, {{art + "/extra.mnu", {}}, {art + "/logo.png", {}}});
	TEST_EXPECT(!refused.done() &&
	            finding(refused, "import.record", DiagnosticSeverity::Error, "logo.png"));
	TEST_EXPECT(snapshot(root) == before && !fs::exists(root + "/textures/logo.png") && !fs::exists(root + "/menus/extra.mnu"));
	fs::remove(root + "/textures/logo.png.import", ec);
	const ActionOutcome imported = import(project.session, {{art + "/extra.mnu", {}}, {art + "/logo.png", {}}});
	const SessionView &view = project.view();
	TEST_EXPECT(imported.done() && fs::is_regular_file(root + "/textures/logo.png.import") &&
	            fs::is_regular_file(root + "/menus/extra.mnu") && !staged_left(root));
	TEST_EXPECT(
			view.project.imports->size() == 1 && (*view.project.imports)[0].source == "textures/logo.png");
	return 0;
}

// An Import names only what the open preview plans: two files chosen and planned, an Import of
// those and of a clip set the preview never held is refused whole (import.not_planned names the
// clip set), nothing written. (A plan stopped by its cap leaves a converter's files out whole the
// same way: import_plan_test's test_plan_cycle_and_cap, the plan's own cap.)
static int test_apply_refuses_unplanned() {
	Project project("opennova_editor_apply_unplanned");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	std::vector<std::string> paths = {art + "/t0.txt", art + "/t1.txt"};
	for (const std::string &path : paths) TEST_EXPECT(editor_test::write_text(path, "x"));
	TEST_EXPECT(editor_test::write_text(art + "/walk.o3a",
	                                    "o3a 1\nadm CHECK.adm\nrow anim_reset \"walk\"\nclip walk\nfps 30\nflags 0x1\nframes 1\n"
	                                    "bone -1 0 0 0 0.5 \"BN01 Pelvis\"\n k 0 0 0 1\n k 0 0 0 1\n"
	                                    "event 0 0 0 0x0 0.9 1.7\nevent 0 0 0 0x0 0.9 1.7\n"));
	const SessionView &view = project.view();
	EditorRequest request = request::of(EditorRequestKind::PreviewImport);
	request.paths = paths;
	project.session.handle(request);
	project.session.run_operations();
	const ImportPlan &plan = *view.dialogs.import_preview.plan;
	TEST_EXPECT(!plan.truncated && plan.rows.size() == 2 && !row_named(plan, "CHECK.adm") && !row_named(plan, "walk.bad"));
	std::vector<ImportChoice> every;
	for (const std::string &path : paths) every.push_back({path, {}});
	every.push_back({art + "/walk.o3a", {}});
	const auto before = snapshot(root);
	const ActionOutcome unplanned = import(project.session, every);
	TEST_EXPECT(!unplanned.done() &&
	            finding(unplanned, "import.not_planned", DiagnosticSeverity::Error, "walk.o3a"));
	TEST_EXPECT(snapshot(root) == before && !fs::exists(root + "/anims"));
	return 0;
}

// ADR 0046 S14: the preview's plan steps with the polls (ImportPlanOperation's Plan phase over an
// ImportPlanner). A menu chain of twelve in a folder, a poll a step: the plan takes more polls
// than it has files, the operation's progress only rises and ends whole, the dialog shows no row
// until the plan is made, and the plan made is the one a single call makes.
static int test_apply_plan_steps() {
	Project project("opennova_editor_apply_plan_steps");
	const std::string art = project.dir.file("art");
	for (int i = 0; i < 12; ++i) {
		const std::string n = std::to_string(i), next = std::to_string(i + 1);
		TEST_EXPECT(editor_test::write_text(art + "/m" + n + ".mnu",
				screen(("S" + n).c_str(), window("BUTTON", "GO", image("t" + n + ".pcx") +
						(i < 11 ? go_to("m" + next + ".mnu", ("S" + next).c_str()) : std::string())))));
		TEST_EXPECT(editor_test::write_text(art + "/t" + n + ".pcx", "pcx"));
	}
	const SessionView &view = project.view();
	const ImportPlan whole = project.plan({{art + "/m0.mnu", {}}});
	TEST_EXPECT(whole.rows.size() == 24);
	project.session.set_poll_budget({0, 1});
	EditorRequest request = request::of(EditorRequestKind::PreviewImport);
	request.paths = {art + "/m0.mnu"};
	request.with_dependencies = true;
	project.session.handle(request);
	TEST_EXPECT(view.activity.operation.running() && view.activity.operation.kind == OperationKind::ImportPlan);
	TEST_EXPECT(view.dialogs.import_preview.open && view.dialogs.import_preview.plan->rows.empty());
	size_t polls = 0;
	uint64_t done = 0, total = 0;
	bool planning = false;
	while (view.activity.operation.running() && polls < 2000) {
		project.session.poll();
		++polls;
		const OperationStatus &status = view.activity.operation;
		if (!status.running()) break;
		TEST_EXPECT(status.done >= done && status.done <= status.total);
		if (status.total != 0) TEST_EXPECT(status.total >= total);
		done = status.done;
		total = status.total;
		planning = planning || status.label.find("Planning the import") == 0;
		if (planning && done < total) TEST_EXPECT(view.dialogs.import_preview.plan->rows.empty());
	}
	TEST_EXPECT(!view.activity.operation.running() && planning && polls > 24);
	TEST_EXPECT(view.activity.last_operation.end == OperationEnd::Done);
	TEST_EXPECT(same_import(whole, *view.dialogs.import_preview.plan));
	project.session.set_poll_budget(kDefaultPollBudget);
	return 0;
}

// ADR 0046 S14: the write a step at a time (AssetImport). Five files of a megabyte in total, a
// byte a step: the project is scanned, each source is read, checked and staged in a step of its
// own (nothing of the project written meanwhile, the staging folder holding what was staged), then
// each file published in a step; the progress only rises and ends whole. A refusal at the last
// source removes what the earlier ones staged and the folders made for them: the project is as it
// was. An import abandoned while it stages leaves nothing; one that has published cannot be.
static int test_apply_write_steps() {
	Project project("opennova_editor_apply_write_steps");
	const std::string root = project.root();
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string art = project.dir.file("art");
	std::vector<ImportChoice> sources;
	for (int i = 0; i < 5; ++i) {
		const std::string path = art + "/f" + std::to_string(i) + ".fnt";
		TEST_EXPECT(editor_test::write_text(path, std::string(size_t(200000), char('a' + i))));
		sources.push_back({path, {}});
	}
	const SessionView &view = project.view();
	const auto before = snapshot(root);
	{
		AssetImport run(sources, paths, *view.project.document, false);
		size_t steps = 0, done = 0, total = 0;
		bool staged_seen = false;
		while (!run.step(1)) {
			++steps;
			TEST_EXPECT(run.files_done() >= done && run.files_total() >= total && run.files_done() <= run.files_total());
			done = run.files_done();
			total = run.files_total();
			// While it stages, the project holds none of the files; the stage holds them.
			if (!run.publishing()) {
				TEST_EXPECT(!fs::exists(root + "/fonts/f0.fnt"));
				staged_seen = staged_seen || staged_left(root);
			}
			TEST_EXPECT(steps < 200);
		}
		TEST_EXPECT(staged_seen && steps >= 10 && run.done() && run.publishing());
		TEST_EXPECT(run.files_done() == 10 && run.files_total() == 10);
		const ImportResult result = run.take();
		TEST_EXPECT(!has_error(result.diagnostics) && result.imported.size() == 5 && result.not_imported.empty());
		TEST_EXPECT(result.imported.front() == "fonts/f0.fnt" && result.imported.back() == "fonts/f4.fnt");
		for (int i = 0; i < 5; ++i) TEST_EXPECT(fs::file_size(root + "/fonts/f" + std::to_string(i) + ".fnt") == 200000);
		TEST_EXPECT(!staged_left(root));
		// Published: abandoning it changes nothing.
		run.abandon();
		TEST_EXPECT(fs::exists(root + "/fonts/f0.fnt"));
	}
	// A refusal at the last source (a name the archives cannot store): what was staged goes.
	const std::string art2 = project.dir.file("art2");
	std::vector<ImportChoice> refused;
	for (int i = 0; i < 3; ++i) {
		const std::string path = art2 + "/m" + std::to_string(i) + ".mnu";
		TEST_EXPECT(editor_test::write_text(path, screen("S", window("STATIC", "W", std::string()))));
		refused.push_back({path, {}});
	}
	TEST_EXPECT(editor_test::write_text(art2 + "/a_name_far_too_long_for_an_archive.mnu", "x"));
	refused.push_back({art2 + "/a_name_far_too_long_for_an_archive.mnu", {}});
	const auto held = snapshot(root);
	{
		AssetImport run(refused, paths, *view.project.document, false);
		bool staged_seen = false;
		while (!run.step(1)) staged_seen = staged_seen || staged_left(root);
		const ImportResult result = run.take();
		TEST_EXPECT(staged_seen && !run.publishing());
		TEST_EXPECT(has_code(result.diagnostics, "import.name") && result.imported.empty() && result.not_imported.empty());
		TEST_EXPECT(snapshot(root) == held && !staged_left(root) && !fs::exists(root + "/menus"));
	}
	// Abandoned while it stages: nothing left, nothing written.
	{
		refused.pop_back();
		AssetImport run(refused, paths, *view.project.document, false);
		while (!staged_left(root) && !run.step(1)) {
		}
		TEST_EXPECT(staged_left(root) && !run.done() && !run.publishing());
		run.abandon();
		TEST_EXPECT(run.done() && snapshot(root) == held && !staged_left(root) && !fs::exists(root + "/menus"));
		TEST_EXPECT(run.take().imported.empty());
	}
	// The one call is the stepped import run to its end.
	const ImportResult whole = import_assets(refused, paths, *view.project.document, false);
	TEST_EXPECT(whole.imported.size() == 3 && fs::exists(root + "/menus/m2.mnu"));
	(void)before;
	return 0;
}

// The session's Import steps its write with the polls (ImportOperation over AssetImport): a poll a
// step, it can be cancelled while it stages (nothing written, the stage gone, the preview still
// open), and once it publishes it cannot; left to run, it imports every file.
static int test_apply_write_cancel() {
	Project project("opennova_editor_apply_write_cancel");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	std::vector<ImportChoice> sources;
	for (int i = 0; i < 6; ++i) {
		const std::string path = art + "/n" + std::to_string(i) + ".txt";
		TEST_EXPECT(editor_test::write_text(path, "notes"));
		sources.push_back({path, {}});
	}
	const SessionView &view = project.view();
	const auto before = snapshot(root);
	project.session.set_poll_budget({0, 1});
	EditorRequest request = request::of(EditorRequestKind::ImportFiles);
	request.imports = sources;
	project.session.handle(request);
	TEST_EXPECT(view.activity.operation.running() && view.activity.operation.kind == OperationKind::ImportApply);
	// Polled until it has staged a file: cancellable, and cancelled.
	for (int i = 0; i < 100 && !staged_left(root); ++i) project.session.poll();
	TEST_EXPECT(staged_left(root) && view.activity.operation.running() && view.activity.operation.cancellable);
	project.session.handle(request::cancel_operation());
	TEST_EXPECT(!view.activity.operation.running() && view.activity.last_operation.end == OperationEnd::Cancelled);
	TEST_EXPECT(snapshot(root) == before && !staged_left(root));
	// Again, to its first published file: no longer cancellable; then to its end.
	project.session.handle(request);
	for (int i = 0; i < 200 && view.activity.operation.running() && !fs::exists(root + "/n0.txt"); ++i) project.session.poll();
	TEST_EXPECT(fs::exists(root + "/n0.txt") && view.activity.operation.running() && !view.activity.operation.cancellable);
	project.session.handle(request::cancel_operation());
	TEST_EXPECT(view.activity.operation.running());
	project.session.set_poll_budget(kDefaultPollBudget);
	project.session.run_operations();
	TEST_EXPECT(view.activity.last_operation.end == OperationEnd::Done && view.activity.last_operation.imported.size() == 6);
	for (int i = 0; i < 6; ++i) TEST_EXPECT(view.project.scan->find("n" + std::to_string(i) + ".txt"));
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
// shell menu's style variables, SCREEN and WINDOW targets and string ids are followed to the
// files defining them and its sound bank (menu.lwf) to its waves (S14): nothing is not followed.
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
	// S14: the symbols are followed to their files (the string ids to the tables their windows
	// name, the screens and windows to their menus, the variables to the stylesheet) and the menu's
	// sound bank to its waves (the graph reads a bank's singles), so nothing is not followed; what
	// no place defines is counted apart and printed.
	std::set<std::string> skipped;
	for (const ImportNotFollowed &entry : plan.not_followed)
		skipped.insert(entry.reference != ReferenceKind::None ? reference_row(entry.reference).token
		                                                      : asset_kind_token(entry.kind));
	TEST_EXPECT(skipped.empty());
	size_t waves = 0;
	for (const ImportPlanRow &row : plan.rows)
		waves += row.kind == AssetKind::Wave && normalized_logical_name(row.needed_by.file) == normalized_logical_name("menu.lwf") ? 1 : 0;
	TEST_EXPECT(waves > 0);
	for (const ImportNotFollowed &entry : plan.undefined)
		std::printf("editor_import retail: %zu %s reference(s) no place defines, the first in %s\n", entry.count,
		            reference_row(entry.reference).token, entry.first.c_str());
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

	// S13 A8: the imported project built (the required files the import did not bring made), every
	// file read once; built again with one imported texture's bytes changed, that file alone read,
	// resource.pff written and the two other archives linked from the last build; a third time with
	// nothing changed, the same build and no file read. The project's files are dated a while ago (a
	// file written within the settle window is read by every build until it settles).
	editor_test::create_missing_files(project.session);
	TEST_EXPECT(editor_test::backdate_tree(project.root(), std::chrono::hours(1)));
	const ProjectPaths paths = ProjectPaths::for_root(project.root());
	const std::string out = project.dir.file("builds");
	const auto build = [&]() {
		return run_build(plan_build(paths, *view.project.scan, *view.project.requirements, {}), out);
	};
	const BuildReport first = build();
	for (const Diagnostic &d : first.diagnostics) std::printf("editor_import retail build: %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(first.ok && first.files_hashed > plan.rows.size());
	std::string texture;
	for (const ImportPlanRow &row : plan.rows)
		if (texture.empty() && row.kind == AssetKind::Texture) texture = project.root() + "/" + row.destination;
	std::vector<uint8_t> bytes;
	std::string error;
	TEST_EXPECT(!texture.empty() && opennova::io::read_file_bytes(texture, bytes, error) && bytes.size() > 64);
	if (bytes.size() > 64) bytes.back() ^= 0x01; // a pixel's byte: the file reads as it did
	TEST_EXPECT(opennova::io::write_file_atomic(texture, bytes.data(), bytes.size(), error) &&
	            editor_test::backdate(texture, std::chrono::minutes(50)));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const BuildReport second = build();
	TEST_EXPECT(second.ok && second.build_id != first.build_id && second.files_hashed == 1);
	TEST_EXPECT(second.archives_written == std::vector<std::string>{"resource.pff"} &&
	            second.archives_linked == std::vector<std::string>({"language.pff", "localres.pff"}));
	const BuildReport third = build();
	TEST_EXPECT(third.ok && third.reused_existing && third.files_hashed == 0);
	std::printf("editor_import retail: built %zu files (%llu bytes); one texture changed: %zu file read (%llu bytes), "
	            "%zu archive written, %zu linked; unchanged: %zu read\n",
	            first.files_hashed, static_cast<unsigned long long>(first.bytes_hashed), second.files_hashed,
	            static_cast<unsigned long long>(second.bytes_hashed), second.archives_written.size(),
	            second.archives_linked.size(), third.files_hashed);
	return 0;
}

// ADR 0046 S14, the retail leg: the smallest and the largest shipped JO mission (04TR.bms, 140 KB;
// ASH_I1gA.bms, 398 KB) each planned from the install with their dependencies into a fresh project.
// The plan is the mission's closure: every file found by its name the install has, its terrain,
// its environment, the catalogs and the game's manifest, nothing of a kind the graph reads left
// unfollowed, no Required manifest file not found; the rows and the bytes printed (the numbers
// the design estimated at 8,700 files and 515 MB from the archives' listing). The smaller one is
// then imported, checked (every file reference resolves or names what the install itself lacks;
// the checklist is met; the only errors are the shipped files' own unresolved references, which
// gate nothing) and built by the session's own build.
static int test_apply_retail_mission_closure() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (a shipped mission's closure planned)");
	for (const char *name : {"04TR.bms", "ASH_I1gA.bms"}) {
		Project project("opennova_editor_apply_retail_closure");
		editor_test::set_game_install(project.session, install);
		const SessionView &view = project.view();
		EditorRequest listed = request::preview_install_import();
		listed.names = {name};
		listed.with_dependencies = true;
		const auto started = std::chrono::steady_clock::now();
		project.session.handle(listed);
		project.session.run_operations();
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		const ImportPlan &plan = *view.dialogs.import_preview.plan;
		TEST_EXPECT(project.session.outcome().done() && view.dialogs.import_preview.open);
		TEST_EXPECT(!plan.truncated && !has_error(plan.diagnostics));
		size_t found = 0, missing = 0, problems = 0;
		std::map<std::string, std::pair<size_t, uint64_t>> by_kind;
		for (const ImportPlanRow &row : plan.rows) {
			if (row.state == State::NotFound) {
				++missing;
				std::printf("editor_import retail closure %s: %s (%s) not found, needed by %s\n", name, row.name.c_str(),
				            asset_kind_token(row.kind), need_words_of(row.needed_by).c_str());
				continue;
			}
			found += row.state == State::Found ? 1 : 0;
			if (!row.problem.empty()) {
				++problems;
				std::printf("editor_import retail closure %s: %s cannot be taken: %s\n", name, row.name.c_str(), row.problem.c_str());
			}
			auto &kind = by_kind[asset_kind_token(row.kind)];
			++kind.first;
			kind.second += row.size;
		}
		std::printf("editor_import retail closure %s: %zu files (%zu found), %.1f MB, %zu not found, %zu the project cannot "
		            "take, planned in %.1f s\n",
		            name, plan.file_count(), found, plan.total_bytes() / 1e6, missing, problems, seconds);
		for (const auto &entry : by_kind)
			std::printf("  %-20s %5zu %8.1f MB\n", entry.first.c_str(), entry.second.first, entry.second.second / 1e6);
		for (const ImportNotFollowed &entry : plan.not_followed)
			std::printf("  not followed: %s (%zu, the first %s)\n",
			            entry.reference != ReferenceKind::None ? reference_row(entry.reference).token : asset_kind_token(entry.kind),
			            entry.count, entry.first.c_str());
		for (const ImportNotFollowed &entry : plan.undefined)
			std::printf("  undefined: %s (%zu, the first in %s)\n", reference_row(entry.reference).token, entry.count,
			            entry.first.c_str());
		// The mission's own set (as the install spells each): the install has a .bin, a .til and a
		// .pcx for every shipped mission, each needed by the mission.
		const auto row_called = [&plan](const std::string &wanted) -> const ImportPlanRow * {
			for (const ImportPlanRow &row : plan.rows)
				if (normalized_logical_name(row.name) == normalized_logical_name(wanted)) return &row;
			return nullptr;
		};
		for (const char *extension : {".bin", ".til", ".pcx"}) {
			const std::string own = std::string(name).substr(0, std::string(name).size() - 4) + extension;
			const ImportPlanRow *row = row_called(own);
			TEST_EXPECT(row && row->state == State::Found &&
			            normalized_logical_name(row->needed_by.file) == normalized_logical_name(name));
		}
		// The catalogs and the terrain, through the mission; no symbol kind left unfollowed.
		TEST_EXPECT(row_called("items.def") && row_called("weapon.def") && row_called("ammo.def"));
		// The HUD's fixed names a running mission opens (review F4): the compass ring, the map's
		// icons and the first crosshair style, each for the game.
		for (const char *fixed : {"compring.tga", "TSDicon.tga", "cross01.tga"}) {
			const ImportPlanRow *row = row_called(fixed);
			TEST_EXPECT(row && row->state == State::Found && row->needed_by.field.find("the game, for ") == 0);
		}
		for (const ImportNotFollowed &entry : plan.not_followed)
			TEST_EXPECT(entry.reference == ReferenceKind::None ||
			            reference_row(entry.reference).resolution == ReferenceResolution::Unchecked);
		TEST_EXPECT(found > 100 && plan.total_bytes() > (uint64_t(100) << 20) && problems == 0);
		// The terrain and the banks are read (S14): the height data and the waves come.
		TEST_EXPECT(by_kind.count("terrain_polydata") && by_kind["wave"].first > 1000 &&
		            !not_followed(plan, ReferenceKind::None, AssetKind::Terrain) &&
		            !not_followed(plan, ReferenceKind::None, AssetKind::SoundBank));
		if (std::string(name) != "04TR.bms") continue;

		// The smaller mission's closure imported (the menu's videos and the music banks left
		// unchecked: 236 MB a mission starts without). What the plan did not find is what the
		// shipped game itself lacks: once imported, every reference to a file, of every file the
		// project holds, resolves or names one of those.
		std::set<std::string> lacking;
		std::vector<ImportChoice> sources;
		for (const ImportPlanRow &row : plan.rows) {
			if (row.state == State::NotFound) lacking.insert(normalized_logical_name(row.name));
			else if (row.selected && row.problem.empty() && row.kind != AssetKind::Video && row.kind != AssetKind::MusicBank)
				sources.push_back(row.source);
		}
		const size_t planned = sources.size();
		const auto import_started = std::chrono::steady_clock::now();
		const ActionOutcome imported = import(project.session, sources);
		const double import_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - import_started).count();
		TEST_EXPECT(imported.done() && !view.dialogs.import_preview.open);
		size_t references = 0, unresolved = 0, unplanned = 0;
		for (const AssetEntry &entry : view.project.scan->entries)
			for (const GraphEdge *edge : view.findings.graph->references_of(entry.relative_path)) {
				if (reference_row(edge->kind).resolution != ReferenceResolution::File) continue;
				++references;
				if (view.findings.graph->resolve(*edge) == ReferenceStatus::Present) continue;
				++unresolved;
				if (lacking.count(normalized_logical_name(edge->value))) continue;
				++unplanned;
				std::printf("editor_import retail closure %s: %s: %s %s names %s, which the plan neither brought nor listed\n", name,
				            entry.relative_path.c_str(), edge->record.c_str(), edge->field.c_str(), edge->value.c_str());
			}
		TEST_EXPECT(references > 1000 && unplanned == 0);
		// With the Missions feature on, the checklist: every Required file of the three phases the
		// install has is in; one the project lacks is one the plan listed as not found (an install
		// without its NovaWorld table), which "create every missing file" then makes.
		editor_test::set_missions(project.session, true);
		project.session.run_operations();
		for (const RequirementRow &row : view.project.requirements->rows) {
			if (!row.required || row.state == RequirementState::Present) continue;
			std::printf("editor_import retail closure %s: the required %s is not in the project\n", name, row.name.c_str());
			TEST_EXPECT(row.state == RequirementState::Missing && lacking.count(normalized_logical_name(row.name)));
		}
		editor_test::create_missing_files(project.session);
		project.session.run_operations();
		TEST_EXPECT(view.project.requirements->required_missing + view.project.requirements->required_wrong_kind == 0);
		// The project's errors: each a reference the shipped game's own files leave unresolved (a
		// name the install lacks, an effect no particle file of it defines), never a file the import
		// left behind. They are listed and gate nothing (S14: the game ships them and runs), so no
		// finding blocks a build.
		std::map<std::string, size_t> errors;
		size_t errors_listed = 0, gating = 0;
		for (const Diagnostic &d : view.findings.diagnostics) {
			if (d.severity != DiagnosticSeverity::Error) continue;
			++errors[d.code()];
			++errors_listed;
			gating += blocks_build(d) ? 1 : 0;
		}
		for (const auto &entry : errors)
			std::printf("editor_import retail closure %s: %zu error(s) %s\n", name, entry.second, entry.first.c_str());
		TEST_EXPECT(errors.size() <= 1 && (errors.empty() || errors.begin()->first == "reference.missing") && gating == 0);
		// The session's own build, gated on the Problems rows as they are, packs it.
		const auto build_started = std::chrono::steady_clock::now();
		editor_test::handle_to_end(project.session, request::build(project.dir.file("builds")));
		const double build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - build_started).count();
		TEST_EXPECT(view.activity.has_build);
		const BuildReport &built = *view.activity.last_build;
		for (const Diagnostic &d : built.diagnostics)
			std::printf("editor_import retail closure build: %s: %s\n", d.code().c_str(), d.message.c_str());
		TEST_EXPECT(view.activity.last_operation.end == OperationEnd::Done && built.ok && built.files_hashed >= planned);
		std::printf("editor_import retail closure %s: %zu files imported in %.1f s; %zu references to files, %zu unresolved (each a "
		            "name the install lacks); %zu error(s) listed, none gating; built %zu files (%.1f MB) in %.1f s\n",
		            name, planned, import_seconds, references, unresolved, errors_listed, built.files_hashed, built.bytes_hashed / 1e6,
		            build_seconds);
	}
	return 0;
}

// ADR 0046 S14 (review F1): a mission found loose in a game's folder sits beside the player's and
// this machine's own files. Planned with what it needs, a resource the manifest names comes from
// that folder and none of the player's files does (every row of player_files(), a save, a
// screenshot); each picked by hand is a row the project cannot take, the import refuses it, and the
// game install's listing never holds one. A project that holds them anyway builds without packing
// one, each said.
static int test_apply_never_player_files() {
	Project project("opennova_editor_apply_player_files");
	const std::string game = project.dir.file("game");
	std::vector<uint8_t> bms;
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		std::string error;
		TEST_EXPECT(opennova::bms::write(mission, bms, error));
	}
	TEST_EXPECT(editor_test::write_bytes(game + "/m.bms", bms) && editor_test::write_text(game + "/menu_style.mns", "X 1\r\n"));
	std::vector<std::string> players;
	for (const PlayerFile &file : player_files()) players.push_back(file.name);
	players.push_back("extra.sav");
	players.push_back("SS00001.tga");
	for (const std::string &name : players) TEST_EXPECT(editor_test::write_text(game + "/" + name, "the player's"));
	TEST_EXPECT(players.size() >= 18 && is_player_file("PLAYER.SAV") && is_player_file("dir/SS12345.bmp") &&
	            !is_player_file("SS1234.tga") && !is_player_file("items.def") && !is_player_file("cc.bin"));
	const ImportPlan plan = project.plan({{game + "/m.bms", {}}});
	const ImportPlanRow *sheet = row_named(plan, "menu_style.mns");
	TEST_EXPECT(sheet && sheet->state == State::Found && sheet->selected);
	for (const std::string &name : players) {
		if (row_named(plan, name)) std::printf("player file %s planned\n", name.c_str());
		TEST_EXPECT(!row_named(plan, name));
	}
	std::vector<ImportChoice> picked;
	for (const std::string &name : players) picked.push_back({game + "/" + name, {}});
	const ImportPlan chosen = project.plan(picked, false);
	for (const std::string &name : players) {
		const ImportPlanRow *row = row_named(chosen, name);
		TEST_EXPECT(row && row->problem.find("never takes the player's own files") != std::string::npos);
	}
	const ActionOutcome refused = import(project.session, {{game + "/player.sav", {}}, {game + "/epass.bin", {}}});
	TEST_EXPECT(!refused.done() && !project.view().project.scan->find("player.sav") && !project.view().project.scan->find("epass.bin"));
	bool said = false;
	for (const Diagnostic &d : project.view().activity.last_operation.findings) said = said || d.code() == "import.player_file";
	TEST_EXPECT(said);
	// The game install's listing: its loose files the game ships, never one of the player's.
	TEST_EXPECT(editor_test::write_text(game + "/menumus.sbf", "music"));
	const uint8_t member[] = {'x'};
	const opennova::pff::PffWriteEntry entries[] = {{"note.txt", member, sizeof(member), 0, 0, 0}};
	TEST_EXPECT(opennova::pff::pff_write_archive((game + "/localres.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	std::vector<Diagnostic> listing_findings;
	const std::vector<ImportChoice> listed = list_retail_import_choices(game, *project.view().project.document, listing_findings);
	bool music = false;
	for (const ImportChoice &choice : listed) {
		music = music || choice.entry == "menumus.sbf";
		TEST_EXPECT(!is_player_file(choice.entry));
	}
	TEST_EXPECT(music && listed.size() == 2);
	// Held by the project anyway (copied by hand): the build leaves each out and says so.
	editor_test::create_missing_files(project.session);
	for (const std::string &name : players) TEST_EXPECT(editor_test::write_text(project.root() + "/" + name, "the player's"));
	editor_test::handle_to_end(project.session, request::rescan());
	const SessionView &view = project.view();
	const BuildPlan build = plan_build(ProjectPaths::for_root(project.root()), *view.project.scan, *view.project.requirements, {});
	size_t told = 0;
	for (const Diagnostic &d : build.diagnostics) told += d.code() == "build.player_file" && d.severity == DiagnosticSeverity::Warning;
	TEST_EXPECT(build.ok && told == players.size());
	for (const BuildEntry &entry : build.loose) TEST_EXPECT(!is_player_file(entry.logical_name));
	for (const BuildArchive &archive : build.archives)
		for (const BuildEntry &entry : archive.entries) TEST_EXPECT(!is_player_file(entry.logical_name));
	return 0;
}

// ADR 0046 S16: a project that builds as an expansion (jxm) on the installed one (jox01) imports the
// install as `/exp jox01` serves it: the closure of jox01's smallest mission planned with what it needs,
// the expansion's own files under the project's names (jox01.bin as jxm.bin), each byte for byte what the
// game is served; and the whole install listed, each name once.
static int test_apply_retail_expansion() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (an expansion's mission closure planned)");
	if (!fs::is_regular_file(fs::path(install) / "expansion" / "jox01" / "jox01.pff"))
		return retail::skip_leg("OPENNOVA_JO_DIR with the jox01 expansion");
	editor_test::TempProjectDir dir("opennova_editor_apply_retail_expansion");
	const std::string root = dir.file("project");
	ProjectDocument created;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Escalation Mod", "jo", created, error, ProjectExpansion{ "jxm", "jox01" }));
	// The whole install as the project imports it, against the base game's.
	std::vector<Diagnostic> diagnostics;
	const std::vector<ImportChoice> whole = list_retail_import_choices(install, created, diagnostics);
	ProjectDocument standalone = created;
	standalone.expansion = ProjectExpansion();
	const std::vector<ImportChoice> base = list_retail_import_choices(install, standalone, diagnostics);
	TEST_EXPECT(diagnostics.empty() && whole.size() > base.size());
	std::set<std::string> names;
	size_t renamed = 0;
	for (const ImportChoice &choice : whole) {
		TEST_EXPECT(names.insert(normalized_logical_name(choice.name())).second);
		renamed += choice.as.empty() ? 0 : 1;
	}
	TEST_EXPECT(names.count("JXM.BIN") && names.count("MJXM.SBF") && !names.count("JOX01.BIN") &&
	            !names.count("MENUMUS.SBF") && !names.count("VERSION.TXT"));
	std::printf("editor_import retail expansion: the whole install with jox01 as jxm lists %zu names (%zu under the "
	            "project's own names), the base game %zu\n",
	            whole.size(), renamed, base.size());
	// The smallest mission of jox01's own.
	InstallView origin;
	std::string why;
	TEST_EXPECT(origin.open(install_spec(install, created), why));
	const InstallFile *smallest = nullptr;
	for (const InstallFile &file : origin.files())
		if (file.layer == InstallFile::Layer::Expansion && opennova::strutil::ends_with_icase(file.name, ".bms") &&
		    (!smallest || origin.size(file) < origin.size(*smallest)))
			smallest = &file;
	TEST_EXPECT(smallest != nullptr);
	if (!smallest) return 1;
	// Planned through the session, the project opened on the install.
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::open_project(root));
	editor_test::set_game_install(session, install);
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.open && view.project.document->expansion == created.expansion);
	EditorRequest listed = request::preview_install_import();
	listed.names = { smallest->name };
	listed.with_dependencies = true;
	const auto started = std::chrono::steady_clock::now();
	session.handle(listed);
	session.run_operations();
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	const ImportPlan &plan = *view.dialogs.import_preview.plan;
	TEST_EXPECT(session.outcome().done() && !plan.truncated && !has_error(plan.diagnostics));
	// What /exp jox01 serves, read the game's way, against each planned file of the install's archives.
	opennova::Vfs game;
	opennova::LaunchFlags flags;
	flags.expansion = "jox01";
	TEST_EXPECT(opennova::mount_install(game, install, flags) && game.mounted_expansion() == "jox01");
	size_t checked = 0, own = 0, expansion_layer = 0;
	for (const ImportPlanRow &row : plan.rows) {
		if (row.state == State::NotFound || !row.source.install) continue;
		const InstallFile *file = origin.find(row.name);
		TEST_EXPECT(file && file->member == row.source.entry &&
		            (row.source.as.empty() ? row.name == file->member : row.source.as == row.name));
		if (!file || !file->loose_path.empty()) continue;
		std::vector<uint8_t> planned, served;
		TEST_EXPECT(origin.read(*file, planned) && read_served(game, file->member, served) && planned == served);
		++checked;
		own += row.source.as.empty() ? 0 : 1;
		expansion_layer += file->layer == InstallFile::Layer::Expansion ? 1 : 0;
	}
	// The same, independently of the view's mount (the mount above is the code path the view reads
	// through): jox01's own archives opened directly. A row of the expansion's layer is a member of
	// jox01L.pff or jox01.pff, served as stored there; a row of the base's is a member of neither (the
	// expansion would serve it); a loose row is the file on the disk, the expansion's in its folder.
	opennova::pff::PffArchive language{}, resources{};
	const bool language_open = opennova::pff::pff_open(&language, (install + "/expansion/jox01/jox01L.pff").c_str()) == 0;
	const bool resources_open = opennova::pff::pff_open(&resources, (install + "/expansion/jox01/jox01.pff").c_str()) == 0;
	TEST_EXPECT(language_open && resources_open);
	const auto stored = [](const opennova::pff::PffArchive &archive, const opennova::pff::PffEntry *entry) {
		std::vector<uint8_t> bytes(entry ? entry->size : 0);
		if (entry && opennova::pff::pff_extract_raw(&archive, entry, bytes.data(), bytes.size()) != 0) bytes.clear();
		return bytes;
	};
	size_t independent = 0, loose_rows = 0;
	for (const ImportPlanRow &row : plan.rows) {
		if (row.state == State::NotFound || !row.source.install) continue;
		const InstallFile *file = origin.find(row.name);
		if (!file) continue;
		if (!file->loose_path.empty()) {
			std::vector<uint8_t> planned, on_disk;
			std::string io_error;
			TEST_EXPECT(origin.read(*file, planned) && opennova::io::read_file_bytes(file->loose_path, on_disk, io_error));
			std::vector<uint8_t> decoded = on_disk; // as the game's loaders take it (a scrambled text decoded)
			opennova::vfs_decode_payload(decoded, origin.vfs().scr_policy());
			TEST_EXPECT(planned == on_disk || planned == decoded);
			if (file->layer == InstallFile::Layer::Expansion)
				TEST_EXPECT(opennova::strutil::to_lower(file->loose_path).find("jox01") != std::string::npos);
			++loose_rows;
			continue;
		}
		const opennova::pff::PffEntry *in_language = language_open ? opennova::pff::pff_find(&language, file->member.c_str()) : nullptr;
		const opennova::pff::PffEntry *in_resources =
				resources_open ? opennova::pff::pff_find(&resources, file->member.c_str()) : nullptr;
		if (file->layer == InstallFile::Layer::Base) {
			TEST_EXPECT(!in_language && !in_resources);
			continue;
		}
		TEST_EXPECT(in_language || in_resources);
		std::vector<uint8_t> served;
		TEST_EXPECT(origin.vfs().read_file_raw(file->member, served) &&
		            ((in_language && served == stored(language, in_language)) ||
		             (in_resources && served == stored(resources, in_resources))));
		++independent;
	}
	if (language_open) opennova::pff::pff_close(&language);
	if (resources_open) opennova::pff::pff_close(&resources);
	TEST_EXPECT(independent > 0);
	const auto row_called = [&plan](const char *wanted) -> const ImportPlanRow * {
		for (const ImportPlanRow &row : plan.rows)
			if (normalized_logical_name(row.name) == normalized_logical_name(wanted)) return &row;
		return nullptr;
	};
	// The expansion's own files come with a mission as the game's fixed names do, under the project's.
	const ImportPlanRow *table = row_called("jxm.bin");
	TEST_EXPECT(table && table->state == State::Found && table->source.entry == "jox01.bin" &&
	            table->needed_by.field.find("the game, ") == 0);
	TEST_EXPECT(!row_called("jox01.bin") && !row_called("menumus.bin"));
	std::printf("editor_import retail expansion: %s's closure %zu files (%.1f MB), %zu of the install's archives' checked "
	            "byte for byte against /exp jox01 (%zu from the expansion's, %zu under the project's names; %zu against "
	            "jox01's archives opened directly, %zu loose against the disk), planned in %.1f s\n",
	            smallest->name.c_str(), plan.file_count(), plan.total_bytes() / 1e6, checked, expansion_layer, own, independent,
	            loose_rows, seconds);
	TEST_EXPECT(checked > 100 && expansion_layer > 0 && own > 0);
	return 0;
}

int run_import_apply_tests() {
	int failures = 0;
	failures += test_apply_never_player_files();
	failures += test_apply_retail_expansion();
	failures += test_apply_retail_mission_closure();
	failures += test_apply_closure();
	failures += test_apply_unchecked();
	failures += test_apply_changed();
	failures += test_apply_staging();
	failures += test_apply_partial_publish();
	failures += test_apply_reads_the_disk();
	failures += test_apply_scene_textures();
	failures += test_apply_record_with_its_file();
	failures += test_apply_refuses_unplanned();
	failures += test_apply_plan_steps();
	failures += test_apply_write_steps();
	failures += test_apply_write_cancel();
	failures += test_apply_staging_leftover();
	failures += test_apply_guard_reads_the_shown_plan();
	failures += test_apply_retail_menu();
	return failures;
}
