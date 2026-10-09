// Pins where the editor puts a file and how one moves (DI-03, the deep-integration plan): one placement
// rule for Create missing, New file, an import's destinations and a converter's outputs
// (assets/project_layout.h: beside the project's files of the kind, else the top level of a flat project or
// the kind's folder of one laid out by kind), so a flat project stays flat; and a move to another folder
// under the file's own name (graph/rename_transaction.h plan_move, the session's move_asset, the command
// line's mv), which rewrites no reference (the game finds a file by its name alone), takes an import source's
// record along, follows the open document and the card, and goes back with Edit > Move back.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_layout.h>
#include <editor/blank/create_missing.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "cli_verbs.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "common/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

// A scan made by hand: each path a file of the kind its name gives, an output of `from` when named so.
AssetEntry entry(const std::string &path, AssetKind kind, const std::string &from = std::string()) {
	AssetEntry out;
	out.relative_path = path;
	out.logical_name = basename_of(path);
	out.kind = kind;
	out.imported_from = from;
	return out;
}
AssetScan scan_of(std::vector<AssetEntry> entries) {
	AssetScan scan;
	scan.entries = std::move(entries);
	scan.index();
	return scan;
}

bool on_disk(const std::string &root, const std::string &relative) {
	std::error_code ec;
	return fs::is_regular_file(system_path(join_path(root, relative)), ec);
}

std::vector<uint8_t> bytes_of(const std::string &root, const std::string &relative) {
	std::vector<uint8_t> out;
	std::string error;
	read_file_bytes(join_path(root, relative), out, error);
	return out;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [code](const Diagnostic &d) { return d.code() == code; });
}

// The rule: a project laid out by kind keeps a new file in the kind's folder, a flat one at the top level;
// a file goes beside the project's files of its kind first; an import's output counts nowhere, an import
// source as the kind its name gives; a kind kept at the top level says nothing of the layout.
int test_rule() {
	const AssetScan empty;
	TEST_EXPECT(project_layout(empty) == ProjectLayout::ByKind);
	TEST_EXPECT(placement_folder(empty, AssetKind::SoundProfileDefs) == "defs");
	TEST_EXPECT(placement_path(empty, "brand.mns", AssetKind::MenuStyle) == "menus/brand.mns");

	// The base game: every file at the top level, its terrain set's sources in a folder of the author's own.
	const AssetScan flat = scan_of({entry("main.mnu", AssetKind::Menu), entry("items.def", AssetKind::ItemDefs),
	                                entry("Arial12b.fnt", AssetKind::Font), entry("oncrate1.3di", AssetKind::Model),
	                                entry("oncrate1_0.tga", AssetKind::Texture), entry("game.cfg", AssetKind::Config),
	                                entry("art/terrain/onisle1_heightmap.png", AssetKind::Texture),
	                                entry(".opennova/imported/0123/onisle1.trn", AssetKind::Terrain, "art/terrain/onisle1.tset")});
	TEST_EXPECT(project_layout(flat) == ProjectLayout::Flat && std::string(project_layout_token(ProjectLayout::Flat)) == "flat");
	// SndProf.def, the walk's hit: no file of its kind, so where the project keeps most of its files.
	TEST_EXPECT(placement_path(flat, "SndProf.def", AssetKind::SoundProfileDefs) == "SndProf.def");
	TEST_EXPECT(placement_path(flat, "joAmmoHit.ptl", AssetKind::Particles) == "joAmmoHit.ptl");
	// Beside the files of its kind: the top level holds more textures than art/terrain/.
	TEST_EXPECT(placement_path(flat, "boxtile.tga", AssetKind::Texture) == "boxtile.tga");
	// A terrain's only file is an import's output under the cache: it counts nowhere.
	TEST_EXPECT(placement_path(flat, "other.trn", AssetKind::Terrain) == "other.trn");
	// An import source sits with the files of the kind its name gives.
	TEST_EXPECT(placement_path(flat, "logo.png", AssetKind::ImportSource) == "logo.png");

	const AssetScan by_kind = scan_of({entry("menus/main.mnu", AssetKind::Menu), entry("defs/items.def", AssetKind::ItemDefs),
	                                   entry("fonts/Arial12b.fnt", AssetKind::Font), entry("game.cfg", AssetKind::Config),
	                                   entry("score.ini", AssetKind::Score), entry("notes.txt", AssetKind::Text)});
	TEST_EXPECT(project_layout(by_kind) == ProjectLayout::ByKind);
	TEST_EXPECT(placement_path(by_kind, "SndProf.def", AssetKind::SoundProfileDefs) == "defs/SndProf.def");
	TEST_EXPECT(placement_path(by_kind, "x.ptl", AssetKind::Particles) == "particles/x.ptl");
	TEST_EXPECT(placement_path(by_kind, "my.cfg", AssetKind::Config) == "my.cfg");

	// Beside the project's files of the kind, wherever it keeps them: the folder holding the most, of two
	// holding as many the kind's own, then the top level, then the first by its path.
	const AssetScan own = scan_of({entry("ui/main.mnu", AssetKind::Menu), entry("ui/sp.mnu", AssetKind::Menu),
	                               entry("Menus/mp.mnu", AssetKind::Menu), entry("defs/items.def", AssetKind::ItemDefs)});
	TEST_EXPECT(placement_folder(own, AssetKind::Menu) == "ui");
	const AssetScan tie = scan_of({entry("ui/main.mnu", AssetKind::Menu), entry("Menus/mp.mnu", AssetKind::Menu)});
	TEST_EXPECT(placement_folder(tie, AssetKind::Menu) == "Menus"); // the kind's own, as the disk spells it
	const AssetScan top_tie = scan_of({entry("ui/main.mnu", AssetKind::Menu), entry("mp.mnu", AssetKind::Menu)});
	TEST_EXPECT(placement_folder(top_tie, AssetKind::Menu).empty());
	const AssetScan first = scan_of({entry("zz/main.mnu", AssetKind::Menu), entry("aa/mp.mnu", AssetKind::Menu)});
	TEST_EXPECT(placement_folder(first, AssetKind::Menu) == "aa");

	// An import writes where the rule says, over the project's file of the name when it has one.
	TEST_EXPECT(import_destination(flat, "ammo.def", AssetKind::AmmoDefs) == "ammo.def");
	TEST_EXPECT(import_destination(by_kind, "ammo.def", AssetKind::AmmoDefs) == "defs/ammo.def");
	TEST_EXPECT(import_destination(flat, "onisle1_heightmap.png", AssetKind::Texture) == "art/terrain/onisle1_heightmap.png");
	return 0;
}

// A folder as a request names it: '/'-separated from the top level; never outside the project nor a
// dot-folder.
int test_folder_words() {
	std::string out, why;
	TEST_EXPECT(normalize_project_folder("", out, why) && out.empty());
	TEST_EXPECT(normalize_project_folder("/", out, why) && out.empty());
	TEST_EXPECT(normalize_project_folder("defs/", out, why) && out == "defs");
	TEST_EXPECT(normalize_project_folder("\\art\\terrain\\", out, why) && out == "art/terrain");
	TEST_EXPECT(normalize_project_folder("./a/./b", out, why) && out == "a/b");
	TEST_EXPECT(!normalize_project_folder("../elsewhere", out, why) && !why.empty());
	TEST_EXPECT(!normalize_project_folder("a/../../b", out, why));
	TEST_EXPECT(!normalize_project_folder("C:/games", out, why));
	TEST_EXPECT(!normalize_project_folder("//server/share", out, why));
	TEST_EXPECT(!normalize_project_folder(".opennova/imported", out, why) && why.find("dot") != std::string::npos);
	TEST_EXPECT(!normalize_project_folder("a/.git", out, why));
	TEST_EXPECT(!normalize_project_folder("what?", out, why));
	return 0;
}

// A project made, every required file created (laid out by kind, as an empty project is), then flattened
// on disk as the base game is: what the session reads afterwards is a flat project.
struct FlatProject {
	editor_test::TempProjectDir dir{"opennova_editor_file_placement"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	bool made = false;
	FlatProject() {
		if (!session.handle(request::new_project(dir.file("project"), "Flat"))) return;
		session.run_operations();
		root = session.view().project.root;
		editor_test::create_missing_files(session);
		const std::vector<AssetEntry> entries = session.view().project.scan->entries;
		if (entries.empty()) return;
		for (const AssetEntry &file : entries) {
			if (file.relative_path.find('/') == std::string::npos) continue;
			std::error_code ec;
			fs::rename(system_path(join_path(root, file.relative_path)), system_path(join_path(root, file.logical_name)), ec);
			if (ec) return;
		}
		for (const AssetEntry &file : entries) {
			const std::string folder = folder_of_path(file.relative_path);
			std::error_code ec;
			if (!folder.empty()) fs::remove(system_path(join_path(root, folder)), ec); // emptied now
		}
		session.handle(request::rescan());
		session.run_operations();
		made = project_layout(*session.view().project.scan) == ProjectLayout::Flat;
	}
	const SessionView &view() const { return session.view(); }
};

// The live hits on a flat project: Create missing, New file and a mission's text table land at the top
// level, the file and the session's line saying so; create_missing_requirements does the same over a scan.
int test_flat_project_stays_flat() {
	FlatProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	ProjectSession &session = project.session;
	TEST_EXPECT(session.view().project.scan->at_path("main.mnu") && session.view().project.scan->at_path("items.def"));
	// An optional row with a blank, made on request.
	TEST_EXPECT(editor_test::handle_to_end(session, request::create_missing({"brand_style"})).done());
	std::string brand_path;
	for (const RequirementRow &row : session.view().project.requirements->rows)
		if (row.role == "brand_style" && row.state == RequirementState::Present) brand_path = row.asset_path;
	TEST_EXPECT(!brand_path.empty() && brand_path.find('/') == std::string::npos);
	TEST_EXPECT(on_disk(project.root, brand_path) && !fs::exists(system_path(join_path(project.root, "menus"))));
	// New file: a menu at the top level, opened there.
	TEST_EXPECT(editor_test::handle_to_end(session, request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu))).done());
	TEST_EXPECT(on_disk(project.root, "extra.mnu") && session.document_for("extra.mnu") &&
	            session.view().documents.active == "extra.mnu");
	TEST_EXPECT(session.view().activity.status == "Created extra.mnu.");
	// The function over a scan, as the session calls it: the scan decides the place (one laid out by kind,
	// the kind's folder).
	const ProjectPaths paths = ProjectPaths::for_root(project.root);
	const ProjectDocument &doc = *session.view().project.document;
	std::error_code ec;
	TEST_EXPECT(fs::remove(system_path(join_path(project.root, brand_path)), ec));
	const RequirementReport now = evaluate_requirements(doc, scan_project_assets(paths, doc));
	const AssetScan by_kind = scan_of({entry("menus/main.mnu", AssetKind::Menu), entry("defs/items.def", AssetKind::ItemDefs)});
	const CreateMissingResult made = create_missing_requirements(paths, doc, by_kind, now, {"brand_style"});
	TEST_EXPECT(made.created.size() == 1 && made.created[0] == "menus/" + brand_path);
	return 0;
}

// A move: refused with the reasons, nothing touched; else the file in its new folder under its own name,
// nothing that names it rewritten, the requirement still met, its open document, card and selection with
// it; Edit > Move back takes it back and the folder it made goes once empty.
int test_move() {
	FlatProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = project.view();
	const ProjectPaths paths = ProjectPaths::for_root(project.root);
	const ProjectDocument &doc = *v.project.document;
	// main.mnu names its fonts; one of them moves.
	const AssetEntry *font = nullptr;
	for (const AssetEntry &file : v.project.scan->entries)
		if (file.kind == AssetKind::Font && !font) font = &file;
	TEST_EXPECT(font != nullptr);
	if (!font) return 1;
	const std::string font_name = font->logical_name;
	const std::vector<uint8_t> menu_before = bytes_of(project.root, "main.mnu");

	// Refusals, nothing written.
	const AssetScan &scan = *v.project.scan;
	TEST_EXPECT(has_code(plan_move(paths, doc, scan, "nothing.fnt", "fonts").refusals, "rename.unknown_file"));
	TEST_EXPECT(has_code(plan_move(paths, doc, scan, font_name, "").refusals, "rename.unchanged"));
	TEST_EXPECT(has_code(plan_move(paths, doc, scan, font_name, "../out").refusals, "rename.path"));
	TEST_EXPECT(has_code(plan_move(paths, doc, scan, font_name, ".opennova").refusals, "rename.path"));
	const std::string export_dir = shown_path(paths.export_dir(doc), project.root);
	TEST_EXPECT(has_code(plan_move(paths, doc, scan, font_name, export_dir + "/x").refusals, "rename.path"));
	TEST_EXPECT(editor_test::write_text(join_path(project.root, "taken/" + font_name), "not a font"));
	TEST_EXPECT(has_code(plan_move(paths, doc, scan, font_name, "taken").refusals, "rename.exists"));
	// An import's output moves only with its source; a source whose import reads files beside it does not
	// move alone; a file another source's import reads from its place does not move.
	const AssetScan outputs = scan_of({entry(".opennova/imported/01/logo.pcx", AssetKind::Texture, "logo.png")});
	TEST_EXPECT(has_code(plan_move(paths, doc, outputs, "logo.pcx", "art").refusals, "rename.imported"));
	TEST_EXPECT(editor_test::write_bytes(join_path(project.root, "set.png"), test_png::gradient_png(4, 4)));
	ImportSidecar reads;
	reads.importer = importer_for("set.png")->id;
	reads.version = importer_for("set.png")->version;
	reads.inputs = {"set_detail.png"};
	Diagnostic unsaved;
	TEST_EXPECT(save_import_sidecar(join_path(project.root, "set.png.import"), reads, unsaved));
	const AssetScan with_set = scan_of({entry("set.png", AssetKind::ImportSource), entry("set_detail.png", AssetKind::Texture)});
	TEST_EXPECT(has_code(plan_move(paths, doc, with_set, "set.png", "art").refusals, "rename.imported"));
	ImportedSource reader;
	reader.source = "set.png";
	reader.inputs = {"set_detail.png"};
	const std::vector<ImportedSource> imports{reader};
	TEST_EXPECT(has_code(plan_move(paths, doc, with_set, "set_detail.png", "art", &imports).refusals, "rename.imported"));
	std::error_code ec;
	fs::remove(system_path(join_path(project.root, "set.png")), ec);
	fs::remove(system_path(join_path(project.root, "set.png.import")), ec);
	fs::remove_all(system_path(join_path(project.root, "taken")), ec);
	TEST_EXPECT(editor_test::handle_to_end(session, request::rescan()).done());

	// Over the wire's request: a refusal is the outcome's, nothing moved.
	const ActionOutcome refused = editor_test::handle_to_end(session, request::move_asset(font_name, "../out"));
	TEST_EXPECT(!refused.done() && has_code(refused.findings, "rename.path") && on_disk(project.root, font_name));
	TEST_EXPECT(v.activity.status == "The move was refused.");

	// The font into fonts/, a folder the project did not have: nothing naming it rewritten, its requirement
	// still met at its new path, its card following.
	TEST_EXPECT(session.handle(request::about_file(font_name)) && v.workspace.card.path == font_name);
	const ActionOutcome moved = editor_test::handle_to_end(session, request::move_asset(font_name, "fonts/"));
	TEST_EXPECT(moved.done());
	TEST_EXPECT(!on_disk(project.root, font_name) && on_disk(project.root, "fonts/" + font_name));
	TEST_EXPECT(bytes_of(project.root, "main.mnu") == menu_before);
	TEST_EXPECT(v.project.scan->at_path("fonts/" + font_name) && !v.project.scan->at_path(font_name));
	TEST_EXPECT(v.workspace.card.path == "fonts/" + font_name);
	TEST_EXPECT(v.activity.status == "Moved " + font_name + " to fonts/.");
	for (const RequirementRow &row : v.project.requirements->rows)
		if (row.name == font_name) TEST_EXPECT(row.state == RequirementState::Present && row.asset_path == "fonts/" + font_name);
	// Edit > Move back: planned with no site, then done; the folder the move made goes, emptied.
	TEST_EXPECT(v.activity.last_rename.made && v.activity.last_rename.move && v.activity.last_rename.from.empty() &&
	            v.activity.last_rename.to == "fonts");
	TEST_EXPECT(session.handle(request::preview_rename_back(true)));
	const DialogsView::RenamePreview &back = v.dialogs.rename_preview;
	TEST_EXPECT(back.back && back.move && back.folder.empty() && back.refusals.empty() && back.sites && back.sites->empty());
	TEST_EXPECT(v.workspace.rename_back.open);
	TEST_EXPECT(editor_test::handle_to_end(session, request::rename_back()).done());
	TEST_EXPECT(on_disk(project.root, font_name) && !fs::exists(system_path(join_path(project.root, "fonts"))));
	TEST_EXPECT(v.activity.status == "Moved " + font_name + " back to the top level.");
	TEST_EXPECT(bytes_of(project.root, "main.mnu") == menu_before);

	// An open document follows its file, active still.
	TEST_EXPECT(editor_test::handle_to_end(session, request::open_document("main.mnu")).done() && v.documents.active == "main.mnu");
	TEST_EXPECT(editor_test::handle_to_end(session, request::move_asset("main.mnu", "ui")).done());
	TEST_EXPECT(v.documents.active == "ui/main.mnu" && v.documents.open.size() == 1 &&
	            v.documents.open.front()->path() == "ui/main.mnu");
	TEST_EXPECT(bytes_of(project.root, "ui/main.mnu") == menu_before);
	return 0;
}

// An import source moves with its record; the import pass that follows makes its outputs under its new
// place, and the scan lists them from there.
int test_move_import_source() {
	FlatProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = project.view();
	TEST_EXPECT(editor_test::write_bytes(join_path(project.root, "art/logo.png"), test_png::gradient_png(8, 8)));
	const Importer *importer = importer_for("logo.png");
	TEST_EXPECT(importer != nullptr);
	if (!importer) return 1;
	ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	Diagnostic error;
	TEST_EXPECT(save_import_sidecar(join_path(project.root, "art/logo.png.import"), sidecar, error));
	TEST_EXPECT(editor_test::handle_to_end(session, request::rescan()).done());
	const auto output_of = [&v](const std::string &source) {
		for (const AssetEntry &file : v.project.scan->entries)
			if (file.imported_from == source) return file.relative_path;
		return std::string();
	};
	const std::string before = output_of("art/logo.png");
	TEST_EXPECT(!before.empty());
	TEST_EXPECT(editor_test::handle_to_end(session, request::move_asset("art/logo.png", "")).done());
	TEST_EXPECT(on_disk(project.root, "logo.png") && on_disk(project.root, "logo.png.import"));
	TEST_EXPECT(!on_disk(project.root, "art/logo.png.import") && !fs::exists(system_path(join_path(project.root, "art"))));
	const std::string after = output_of("logo.png");
	TEST_EXPECT(!after.empty() && after != before && on_disk(project.root, after) && output_of("art/logo.png").empty());
	return 0;
}

// The command line's mv moves as the editor does; a refusal exits 1, nothing moved.
int test_command_line() {
	FlatProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	const std::string root = project.root;
	project.session.handle(request::close_project());
	const auto run = [](std::initializer_list<std::string> args) {
		std::vector<const char *> argv;
		for (const std::string &a : args) argv.push_back(a.c_str());
		return opennova::project::run_project_command(static_cast<int>(argv.size()), argv.data(), stdout, stderr);
	};
	TEST_EXPECT(run({"mv", root, "items.def", "defs"}) == 0);
	TEST_EXPECT(on_disk(root, "defs/items.def") && !on_disk(root, "items.def"));
	TEST_EXPECT(run({"mv", root, "defs/items.def", "/"}) == 0);
	TEST_EXPECT(on_disk(root, "items.def") && !fs::exists(system_path(join_path(root, "defs"))));
	TEST_EXPECT(run({"mv", root, "items.def", ".."}) == 1 && on_disk(root, "items.def"));
	TEST_EXPECT(run({"mv", root, "items.def"}) == 2); // a folder named, always
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_rule();
	failed += test_folder_words();
	failed += test_flat_project_stays_flat();
	failed += test_move();
	failed += test_move_import_source();
	failed += test_command_line();
	if (failed == 0) std::printf("editor_file_placement: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
