// Pins Files' chores (DI-25, the deep-integration plan): a file deleted to the project's trash (who names it
// asked first, its uses left naming nothing only when asked, an import source with its outputs or alone), a
// file duplicated under a name the project's rules give (graph/file_plans.h duplicate_name: the stem's number
// counted on, 15 characters where the build packs the kind, a model's stem of 8, no name taken in any case), a
// new file made in a folder, a folder made, renamed (each file through the rename transaction) and deleted,
// and each of them one step of the file history that undo_file and redo_file take back and do again.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/file_plans.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_files.h>
#include <editor/project/project_trash.h>
#include <editor/session/file_card.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "common/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

AssetEntry entry(const std::string &path, AssetKind kind) {
	AssetEntry out;
	out.relative_path = path;
	out.logical_name = basename_of(path);
	out.kind = kind;
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
	return fs::exists(system_path(join_path(root, relative)), ec);
}

std::vector<uint8_t> bytes_of(const std::string &root, const std::string &relative) {
	std::vector<uint8_t> out;
	std::string error;
	opennova::io::read_file_bytes(join_path(root, relative), out, error);
	return out;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [code](const Diagnostic &d) { return d.code() == code; });
}

// A Problems row of `code` whose words name `name`.
bool row_naming(const SessionView &view, const char *code, const std::string &name) {
	return std::any_of(view.findings.diagnostics.begin(), view.findings.diagnostics.end(), [&](const Diagnostic &d) {
		return d.code() == code && d.message.find(name) != std::string::npos;
	});
}

// A new project with its required files made, validated.
struct Project {
	editor_test::TempProjectDir dir{"opennova_editor_file_chores"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	bool made = false;
	Project() {
		if (!session.handle(request::new_project(dir.file("project"), "Chores"))) return;
		session.run_operations();
		root = session.view().project.root;
		editor_test::create_missing_files(session);
		session.run_operations();
		made = !session.view().project.scan->entries.empty();
	}
	const SessionView &view() const { return session.view(); }
	// The first file of `kind` something names, by its path ("" none).
	std::string named_file(AssetKind kind) const {
		for (const AssetEntry &file : view().project.scan->entries)
			if (file.kind == kind && file_use_count(view(), file.relative_path) > 0) return file.relative_path;
		return std::string();
	}
	std::string output_of(const std::string &source) const {
		for (const AssetEntry &file : view().project.scan->entries)
			if (file.imported_from == source) return file.relative_path;
		return std::string();
	}
};

// The name a copy is given: the stem's number counted on, cut to the rules, never one taken in any case.
int test_copy_names() {
	const AssetScan scan = scan_of({entry("oncrate1.3di", AssetKind::Model), entry("oncrate2.3di", AssetKind::Model),
	                                entry("barrel_wood.3di", AssetKind::Model), entry("items.def", AssetKind::ItemDefs),
	                                entry("ITEMS2.DEF", AssetKind::OtherDefs), entry("tex01.tga", AssetKind::Texture),
	                                entry("verylongname12.tga", AssetKind::Texture), entry("first.bms", AssetKind::Mission),
	                                entry("first.wac", AssetKind::Script), entry("first2.wac", AssetKind::Script)});
	const auto copy = [&scan](const std::string &name) { return duplicate_name(scan, *scan.find(name), false); };
	TEST_EXPECT(copy("oncrate1.3di") == "oncrate3.3di"); // oncrate2 is taken
	TEST_EXPECT(copy("barrel_wood.3di") == "barrel_2.3di"); // a model's stem: 8 characters
	TEST_EXPECT(copy("items.def") == "items3.def");         // ITEMS2.DEF is items2.def, as the game compares names
	TEST_EXPECT(copy("tex01.tga") == "tex02.tga");          // the number's zeros kept
	TEST_EXPECT(copy("verylongname12.tga") == "verylongn13.tga" && copy("verylongname12.tga").size() == kCopyNameChars);
	// A mission's copy takes no name a file of its set would collide with: first2.wac is another file.
	TEST_EXPECT(copy("first.bms") == "first3.bms");
	// The base game's names are taken too (an expansion's copy would stand in for the base's file).
	const std::vector<std::string> base_files{"OnCrate3.3di"};
	const BaseNames base{&base_files};
	TEST_EXPECT(duplicate_name(scan, *scan.find("oncrate1.3di"), false, &base) == "oncrate4.3di");
	return 0;
}

// A texture a menu names: deleting it is refused with who names it; asked anyway, it goes to the trash, its
// uses are Problems rows, and Undo file brings it back; Redo file takes it again.
int test_delete_named() {
	Project p;
	TEST_EXPECT(p.made);
	if (!p.made) return 1;
	ProjectSession &session = p.session;
	const SessionView &v = p.view();
	const std::string texture = p.named_file(AssetKind::Texture);
	TEST_EXPECT(!texture.empty());
	if (texture.empty()) return 1;
	const std::string name = basename_of(texture);
	const std::vector<uint8_t> before = bytes_of(p.root, texture);

	const ActionOutcome refused = editor_test::handle_to_end(session, request::delete_asset(texture));
	TEST_EXPECT(!refused.done() && has_code(refused.findings, "file.named") && on_disk(p.root, texture));
	TEST_EXPECT(v.activity.file_history.undo.empty());

	TEST_EXPECT(editor_test::handle_to_end(session, request::delete_asset(texture, true)).done());
	TEST_EXPECT(!on_disk(p.root, texture) && !v.project.scan->at_path(texture));
	TEST_EXPECT(on_disk(p.root, ".opennova/trash/1/" + texture));
	TEST_EXPECT(row_naming(v, "reference.missing", name));
	TEST_EXPECT(v.activity.file_history.undo == "Delete " + name && v.activity.file_history.undo_steps == 1);

	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(on_disk(p.root, texture) && bytes_of(p.root, texture) == before && v.project.scan->at_path(texture));
	TEST_EXPECT(!row_naming(v, "reference.missing", name));
	TEST_EXPECT(!fs::exists(system_path(join_path(p.root, ".opennova/trash/1"))));
	TEST_EXPECT(v.activity.file_history.undo.empty() && v.activity.file_history.redo == "Delete " + name);

	TEST_EXPECT(editor_test::handle_to_end(session, request::redo_file()).done());
	TEST_EXPECT(!on_disk(p.root, texture) && row_naming(v, "reference.missing", name));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(on_disk(p.root, texture) && bytes_of(p.root, texture) == before);
	// Nothing left to take back past the first step.
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::undo_file()).findings, "file.history"));
	return 0;
}

// A file duplicated beside itself under the name the rules give, a mission with its companions; a name taken or
// of another extension refused; Undo file takes the copy to the trash.
int test_duplicate() {
	Project p;
	TEST_EXPECT(p.made);
	if (!p.made) return 1;
	ProjectSession &session = p.session;
	const SessionView &v = p.view();
	const AssetEntry *menu = v.project.scan->find("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string menu_path = menu->relative_path; // the scan is replaced as files change
	const std::string folder = folder_of_path(menu_path);
	TEST_EXPECT(editor_test::handle_to_end(session, request::duplicate_asset(menu_path)).done());
	const std::string copy = join_path(folder, "main2.mnu");
	TEST_EXPECT(on_disk(p.root, copy) && bytes_of(p.root, copy) == bytes_of(p.root, menu_path));
	TEST_EXPECT(v.documents.file_selected.path == copy);
	TEST_EXPECT(v.activity.file_history.undo == "Duplicate main.mnu as main2.mnu");
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::duplicate_asset("main.mnu", "MAIN2.MNU")).findings, "file.exists"));
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::duplicate_asset("main.mnu", "main3.mns")).findings, "file.name"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(!on_disk(p.root, copy) && !v.project.scan->at_path(copy));
	TEST_EXPECT(editor_test::handle_to_end(session, request::redo_file()).done() && on_disk(p.root, copy));

	// A mission and the script the game finds by its name, copied together.
	const std::vector<uint8_t> bms = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
	TEST_EXPECT(!bms.empty());
	TEST_EXPECT(editor_test::write_bytes(join_path(p.root, "missions/logic1.bms"), bms));
	TEST_EXPECT(editor_test::write_text(join_path(p.root, "missions/logic1.wac"), "// the mission's script\n"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::rescan()).done());
	TEST_EXPECT(editor_test::handle_to_end(session, request::duplicate_asset("logic1.bms")).done());
	TEST_EXPECT(on_disk(p.root, "missions/logic2.bms") && on_disk(p.root, "missions/logic2.wac"));
	TEST_EXPECT(bytes_of(p.root, "missions/logic2.wac") == bytes_of(p.root, "missions/logic1.wac"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(!on_disk(p.root, "missions/logic2.bms") && !on_disk(p.root, "missions/logic2.wac"));
	return 0;
}

// An import source: duplicated with its record (its import makes the copy's outputs), deleted alone (its
// outputs kept as files of the project) and with its outputs; each taken back.
int test_import_source() {
	Project p;
	TEST_EXPECT(p.made);
	if (!p.made) return 1;
	ProjectSession &session = p.session;
	const SessionView &v = p.view();
	TEST_EXPECT(editor_test::write_bytes(join_path(p.root, "art/logo.png"), test_png::gradient_png(8, 8)));
	const Importer *importer = importer_for("logo.png");
	TEST_EXPECT(importer != nullptr);
	if (!importer) return 1;
	ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	Diagnostic error;
	TEST_EXPECT(save_import_sidecar(join_path(p.root, "art/logo.png.import"), sidecar, error));
	TEST_EXPECT(editor_test::handle_to_end(session, request::rescan()).done());
	const std::string output = p.output_of("art/logo.png");
	TEST_EXPECT(!output.empty());

	TEST_EXPECT(editor_test::handle_to_end(session, request::duplicate_asset("art/logo.png")).done());
	TEST_EXPECT(on_disk(p.root, "art/logo2.png") && on_disk(p.root, "art/logo2.png.import"));
	TEST_EXPECT(!p.output_of("art/logo2.png").empty());
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(!on_disk(p.root, "art/logo2.png") && !on_disk(p.root, "art/logo2.png.import") && p.output_of("art/logo2.png").empty());
	// Alone: no record, so no import.
	TEST_EXPECT(editor_test::handle_to_end(session, request::duplicate_asset("art/logo.png", std::string(), true)).done());
	TEST_EXPECT(on_disk(p.root, "art/logo2.png") && !on_disk(p.root, "art/logo2.png.import"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done() && !on_disk(p.root, "art/logo2.png"));

	// Deleted alone: the source and its record go, its output stays a file of the project.
	const std::string kept_name = basename_of(output);
	TEST_EXPECT(editor_test::handle_to_end(session, request::delete_asset("art/logo.png", false, true)).done());
	TEST_EXPECT(!on_disk(p.root, "art/logo.png") && !on_disk(p.root, "art/logo.png.import"));
	const AssetEntry *kept = v.project.scan->find(kept_name);
	TEST_EXPECT(kept && kept->imported_from.empty() && on_disk(p.root, kept->relative_path));
	TEST_EXPECT(v.activity.file_history.undo == "Delete logo.png alone");
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(on_disk(p.root, "art/logo.png") && on_disk(p.root, "art/logo.png.import") && p.output_of("art/logo.png") == output);
	const AssetEntry *back = v.project.scan->find(kept_name);
	TEST_EXPECT(back && back->imported_from == "art/logo.png");

	// With its outputs: none left in the scan.
	TEST_EXPECT(editor_test::handle_to_end(session, request::delete_asset("art/logo.png")).done());
	TEST_EXPECT(!v.project.scan->find(kept_name) && !on_disk(p.root, output));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(p.output_of("art/logo.png") == output && on_disk(p.root, output));
	// An import's output is no file to delete.
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::delete_asset(output)).findings, "file.imported"));

	// A record that names its output whatever the source is called (S18's name option): a copy with it would
	// make a second file of that name, so it is refused, saying so; alone, the source copies.
	TEST_EXPECT(editor_test::write_bytes(join_path(p.root, "art/sign_src.png"), test_png::gradient_png(8, 8, 3)));
	ImportSidecar named = sidecar;
	named.options["name"] = "fixedsign.tga";
	TEST_EXPECT(save_import_sidecar(join_path(p.root, "art/sign_src.png.import"), named, error));
	TEST_EXPECT(editor_test::handle_to_end(session, request::rescan()).done());
	TEST_EXPECT(basename_of(p.output_of("art/sign_src.png")) == "fixedsign.tga");
	const ActionOutcome fixed = editor_test::handle_to_end(session, request::duplicate_asset("art/sign_src.png"));
	TEST_EXPECT(!fixed.done() && has_code(fixed.findings, "file.name") && !on_disk(p.root, "art/sign_src2.png"));
	TEST_EXPECT(std::any_of(fixed.findings.begin(), fixed.findings.end(),
	                        [](const Diagnostic &d) { return d.message.find("fixedsign.tga") != std::string::npos; }));
	TEST_EXPECT(editor_test::handle_to_end(session, request::duplicate_asset("art/sign_src.png", std::string(), true)).done());
	TEST_EXPECT(on_disk(p.root, "art/sign_src2.png") && !on_disk(p.root, "art/sign_src2.png.import"));
	return 0;
}

// New here, and the folders: one made (listed though empty), a file made in it, the folder renamed with its
// files moved through the rename transaction (the open document following), an empty folder deleted; each a
// step Undo file takes back.
int test_folders() {
	Project p;
	TEST_EXPECT(p.made);
	if (!p.made) return 1;
	ProjectSession &session = p.session;
	const SessionView &v = p.view();
	TEST_EXPECT(editor_test::handle_to_end(session, request::new_folder("maps/")).done());
	TEST_EXPECT(fs::is_directory(system_path(join_path(p.root, "maps"))) && v.project.scan->folders().count("maps"));
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::new_folder("maps")).findings, "file.exists"));
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::new_folder("../out")).findings, "file.folder"));
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::new_folder(".opennova/x")).findings, "file.folder"));

	// New here: a menu made in maps/, where the placement rule would not put it.
	TEST_EXPECT(editor_test::handle_to_end(session, request::create_file_in("maps", "extra.mnu", asset_kind_token(AssetKind::Menu))).done());
	TEST_EXPECT(on_disk(p.root, "maps/extra.mnu") && v.documents.active == "maps/extra.mnu");
	TEST_EXPECT(v.activity.file_history.undo == "New file maps/extra.mnu");
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::create_file_in("../x", "x.mnu", asset_kind_token(AssetKind::Menu))).findings,
	                     "document.path"));

	// The folder renamed: its file moved, its document open at the new path, nothing rewritten.
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::rename_folder("maps", "a/b")).findings, "file.folder"));
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::rename_folder("nowhere", "x")).findings, "file.unknown"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::new_folder("maps/empty")).done());
	TEST_EXPECT(editor_test::handle_to_end(session, request::rename_folder("maps", "levels")).done());
	TEST_EXPECT(on_disk(p.root, "levels/extra.mnu") && !on_disk(p.root, "maps"));
	TEST_EXPECT(fs::is_directory(system_path(join_path(p.root, "levels/empty"))));
	TEST_EXPECT(v.documents.active == "levels/extra.mnu" && v.project.scan->at_path("levels/extra.mnu"));
	TEST_EXPECT(v.activity.file_history.undo == "Rename folder maps to levels");
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(on_disk(p.root, "maps/extra.mnu") && !on_disk(p.root, "levels") && v.documents.active == "maps/extra.mnu");

	// A folder that holds a file is not deleted; an empty one is, and comes back.
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::delete_folder("maps")).findings, "file.folder"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::delete_folder("maps/empty")).done());
	TEST_EXPECT(!on_disk(p.root, "maps/empty") && !v.project.scan->folders().count("maps/empty"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done() && on_disk(p.root, "maps/empty"));

	// The new file's step taken back: its file to the trash, its document closed.
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done()); // the folder made in maps/
	TEST_EXPECT(!on_disk(p.root, "maps/empty"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done()); // the new file
	TEST_EXPECT(!on_disk(p.root, "maps/extra.mnu") && !session.document_for("maps/extra.mnu"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done()); // maps/ itself
	TEST_EXPECT(!on_disk(p.root, "maps"));
	TEST_EXPECT(has_code(editor_test::handle_to_end(session, request::undo_file()).findings, "file.history"));

	// New here in folders the project has not: they are made with the file, and go with it once empty.
	TEST_EXPECT(editor_test::handle_to_end(session, request::create_file_in("ui/extra", "x.mnu", asset_kind_token(AssetKind::Menu))).done());
	TEST_EXPECT(on_disk(p.root, "ui/extra/x.mnu") && v.project.scan->folders().count("ui/extra"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::undo_file()).done());
	TEST_EXPECT(!on_disk(p.root, "ui") && !v.project.scan->folders().count("ui"));
	TEST_EXPECT(editor_test::handle_to_end(session, request::redo_file()).done() && on_disk(p.root, "ui/extra/x.mnu"));
	return 0;
}

// The trash alone: a batch moved in and out whole, refused over a file that sits where one comes back.
int test_trash() {
	editor_test::TempProjectDir dir("opennova_editor_trash");
	const ProjectPaths paths = ProjectPaths::for_root(dir.file("project"));
	TEST_EXPECT(editor_test::write_text(join_path(paths.root, "a.txt"), "a"));
	TEST_EXPECT(editor_test::write_text(join_path(paths.root, "sub/b.txt"), "b"));
	TrashBatch batch;
	std::string error;
	TEST_EXPECT(!trash_paths(paths, {"a.txt", "missing.txt"}, batch, error) && !error.empty() && on_disk(paths.root, "a.txt"));
	TEST_EXPECT(trash_paths(paths, {"a.txt", "sub/b.txt"}, batch, error) && batch.id == 1 && batch.paths.size() == 2);
	TEST_EXPECT(!on_disk(paths.root, "a.txt") && on_disk(paths.root, ".opennova/trash/1/sub/b.txt"));
	TEST_EXPECT(editor_test::write_text(join_path(paths.root, "a.txt"), "another"));
	TEST_EXPECT(!restore_trash(paths, batch, error) && error.find("a.txt") != std::string::npos);
	std::error_code ec;
	fs::remove(system_path(join_path(paths.root, "a.txt")), ec);
	TEST_EXPECT(restore_trash(paths, batch, error) && on_disk(paths.root, "a.txt") && on_disk(paths.root, "sub/b.txt"));
	TEST_EXPECT(!fs::exists(system_path(join_path(paths.root, ".opennova/trash/1"))));
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_copy_names();
	failed += test_trash();
	failed += test_delete_named();
	failed += test_duplicate();
	failed += test_import_source();
	failed += test_folders();
	if (failed == 0) std::printf("editor_file_chores: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
