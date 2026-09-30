// Drives the real opennova-project commands (ADR 0046 d4) end to end on a temporary
// project: new, status, validate, create-missing, build, import and reimport, their
// exit codes, and the one refresh they share with the editor (S9c: every command
// imports the changed sources first), an import with the files it needs (S11g: the plan,
// a dry run that writes nothing, an .o3d's textures, the cap), validate listing what the
// editor's Problems lists (S12), and the one game install the editor and the command line
// share (S12).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/pff/pff.h>

#include "commands.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/png_test_support.h"

using opennova::project_cli::run_project_command;
namespace fs = std::filesystem;

static int run(std::initializer_list<std::string> args) {
	std::vector<const char *> argv;
	for (const std::string &a : args) argv.push_back(a.c_str());
	return run_project_command(static_cast<int>(argv.size()), argv.data(), stdout, stderr);
}

// The same, with what the command printed on its output kept in `text` (and echoed).
static int run_capture(const std::string &capture_file, std::initializer_list<std::string> args, std::string &text) {
	std::vector<const char *> argv;
	for (const std::string &a : args) argv.push_back(a.c_str());
	text.clear();
	std::FILE *out = std::fopen(capture_file.c_str(), "w+b");
	if (!out) return -1;
	const int code = run_project_command(static_cast<int>(argv.size()), argv.data(), out, stderr);
	std::rewind(out);
	char buffer[4096];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), out)) > 0;) text.append(buffer, n);
	std::fclose(out);
	std::fputs(text.c_str(), stdout);
	return code;
}

// A usage error: the exit code, and what the command printed on its error stream.
static int run_usage(const std::string &capture_file, std::initializer_list<std::string> args, std::string &text) {
	std::vector<const char *> argv;
	for (const std::string &a : args) argv.push_back(a.c_str());
	text.clear();
	std::FILE *err = std::fopen(capture_file.c_str(), "w+b");
	if (!err) return -1;
	const int code = run_project_command(static_cast<int>(argv.size()), argv.data(), stdout, err);
	std::rewind(err);
	char buffer[4096];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), err)) > 0;) text.append(buffer, n);
	std::fclose(err);
	return code;
}

static bool archive_has(const std::string &archive_path, const char *name) {
	opennova::pff::PffArchive archive{};
	if (opennova::pff::pff_open(&archive, archive_path.c_str()) != 0) return false;
	const bool found = opennova::pff::pff_find(&archive, name) != nullptr;
	opennova::pff::pff_close(&archive);
	return found;
}

static int test_usage_errors() {
	TEST_EXPECT(run({}) == 2);
	TEST_EXPECT(run({"frobnicate"}) == 2);
	TEST_EXPECT(run({"new"}) == 2);
	TEST_EXPECT(run({"new", "a", "b"}) == 2);
	TEST_EXPECT(run({"new", "a", "--title"}) == 2);
	TEST_EXPECT(run({"status"}) == 2);
	TEST_EXPECT(run({"validate"}) == 2);
	TEST_EXPECT(run({"--help"}) == 2);
	// `--install` names the game install before or after its entries; an `--entry` is never
	// taken for it, and `--retail` (its name before S13 A4) is no option.
	editor_test::TempProjectDir dir("opennova_project_cli_usage");
	std::string text;
	TEST_EXPECT(run_usage(dir.file("err.txt"), {"import", "p", "--install", "--entry", "items.def"}, text) == 2);
	TEST_EXPECT(text.find("--install needs the game install folder") != std::string::npos);
	TEST_EXPECT(run_usage(dir.file("err.txt"), {"import", "p", "--install", "game", "--entry"}, text) == 2);
	TEST_EXPECT(text.find("incomplete import option --entry") != std::string::npos);
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "import", "p", "--retail", "game" }, text) == 2);
	TEST_EXPECT(text.find("unknown or incomplete import option game") != std::string::npos);
	return 0;
}

static int test_new_status_validate() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_test");
	const std::string root = dir.file("CliGame");
	TEST_EXPECT(run({"status", root}) == 2);                              // no project yet
	TEST_EXPECT(run({"new", root, "--title", "CLI Game", "--game", "jo"}) == 0);
	TEST_EXPECT(run({"new", root}) == 2);                                 // already a project
	TEST_EXPECT(run({"new", dir.file("bad"), "--game", "quake"}) == 2);   // unknown game
	TEST_EXPECT(run({"status", root}) == 0);
	TEST_EXPECT(run({"validate", root}) == 1);                            // the fatal set is missing

	opennova::editor::ProjectDocument doc;
	opennova::editor::Diagnostic error;
	TEST_EXPECT(opennova::editor::open_project(root, doc, error));
	TEST_EXPECT(doc.title == "CLI Game");

	// Create all missing: the project then validates clean, and a second run is a no-op
	// (nothing is unmet, so nothing is named). A role asked for by name whose file is there
	// is refused, never overwritten, and a role no requirement has is refused.
	TEST_EXPECT(run({"create-missing", root, "--role"}) == 2);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	TEST_EXPECT(run({"validate", root}) == 0);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	TEST_EXPECT(run({"create-missing", root, "--role", "main_menu"}) == 1);
	TEST_EXPECT(run({"create-missing", root, "--role", "no_such_role"}) == 1);

	// A clone has no .opennova/ (its .gitignore ignores itself): reading a project with
	// nothing to import writes nothing there, and the build that lands under it makes it
	// again with its self-ignore file.
	std::error_code ec;
	fs::remove_all(root + "/.opennova", ec);
	TEST_EXPECT(run({"status", root}) == 0 && run({"validate", root}) == 0);
	TEST_EXPECT(!fs::exists(root + "/.opennova"));

	// Build: a directory the runtime boots, and the same content is the same build.
	TEST_EXPECT(run({"build", root, "--out"}) == 2);
	TEST_EXPECT(run({"build", root}) == 0);
	TEST_EXPECT(fs::is_regular_file(root + "/.opennova/.gitignore"));
	TEST_EXPECT(run({"build", root}) == 0);
	TEST_EXPECT(run({"build", root, "--out", dir.file("elsewhere")}) == 0);
	TEST_EXPECT(std::filesystem::is_regular_file(dir.file("elsewhere") + "/last_good.json"));

	// Generic native file import uses the same core and requires explicit replacement.
	TEST_EXPECT(run({"import", root}) == 2);
	const std::string source = dir.file("source.txt");
	TEST_EXPECT(editor_test::write_text(source, "imported file"));
	TEST_EXPECT(run({"import", root, source}) == 0);
	TEST_EXPECT(std::filesystem::is_regular_file(root + "/source.txt"));
	TEST_EXPECT(run({"import", root, source}) == 1);
	TEST_EXPECT(run({"import", root, source, "--replace"}) == 0);

	// A file with the wrong content behind a required name is an error too.
	TEST_EXPECT(editor_test::write_text(root + "/strings/gametext.bin", "raw"));
	TEST_EXPECT(run({"validate", root}) == 1);
	TEST_EXPECT(run({"create-missing", root}) == 1); // refused, never overwritten
	TEST_EXPECT(run({"build", root}) == 1);          // blocked until fixed
	return 0;
}

// A PNG is an import source only with its `.import` record; the author writes the
// importer's defaults by hand here (importing a file writes it).
static bool mark_for_import(const std::string &source) {
	const opennova::editor::Importer *importer = opennova::editor::importer_for(source);
	if (importer == nullptr) return false;
	opennova::editor::ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	sidecar.options = importer->default_options;
	opennova::editor::Diagnostic error;
	return opennova::editor::save_import_sidecar(source + ".import", sidecar, error);
}

// Every command opens the project the way the editor does (ADR 0046 d4, d8): the import
// pass first, so a fresh clone (no outputs, no import cache) still builds with the
// imported texture, and validate lists the import findings.
static int test_imports() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_imports");
	const std::string root = dir.file("Imports");
	const std::string capture = dir.file("capture.txt");
	std::string text;
	std::error_code ec;
	TEST_EXPECT(run({"new", root}) == 0);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	// A PNG with no record is a texture the build packs as it is.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/plain.png", editor_test::gradient_png(4, 4, 3)));
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", editor_test::gradient_png(8, 8)));
	TEST_EXPECT(mark_for_import(root + "/art/logo.png"));
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0);
	TEST_EXPECT(text.find("imports: 1 source(s), 1 imported now, 0 failed") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/art/logo.png.import"));
	// A PNG brought in by `import` is imported at once, as the editor's rescan does.
	TEST_EXPECT(editor_test::write_bytes(dir.file("splash.png"), editor_test::gradient_png(4, 4, 5)));
	TEST_EXPECT(run_capture(capture, {"import", root, dir.file("splash.png")}, text) == 0);
	TEST_EXPECT(text.find("-> 1 output(s)") != std::string::npos);
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0);
	TEST_EXPECT(text.find("imports: 2 source(s), 0 imported now, 0 failed") != std::string::npos);
	// reimport: nothing changed, nothing imports; --force with a source imports that one.
	TEST_EXPECT(run_capture(capture, {"reimport", root}, text) == 0 && text.find("2 source(s), 0 imported") != std::string::npos);
	TEST_EXPECT(run_capture(capture, {"reimport", root, "--force", "--source", "logo.png"}, text) == 0);
	TEST_EXPECT(text.find("imported art/logo.png") != std::string::npos && text.find("2 source(s), 1 imported") != std::string::npos);
	// A fresh clone has no .opennova/ at all (its .gitignore ignores itself): status
	// imports into it and makes it again with its self-ignore file, so neither the cache
	// nor the outputs show up in the modder's repository.
	fs::remove_all(root + "/.opennova", ec);
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0);
	TEST_EXPECT(text.find("imports: 2 source(s), 2 imported now, 0 failed") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/.opennova/.gitignore") && fs::is_regular_file(root + "/.opennova/import_cache.json"));
	// The same clone built: the build imports first and packs the texture; the source
	// itself never ships.
	fs::remove_all(root + "/.opennova", ec);
	TEST_EXPECT(run_capture(capture, {"build", root, "--out", dir.file("out")}, text) == 0);
	TEST_EXPECT(fs::is_regular_file(root + "/.opennova/.gitignore"));
	TEST_EXPECT(text.find("imported art/logo.png") != std::string::npos);
	const std::string build = opennova::editor::last_good_build_dir(dir.file("out"));
	TEST_EXPECT(!build.empty());
	TEST_EXPECT(archive_has(build + "/resource.pff", "LOGO.PCX") && archive_has(build + "/resource.pff", "SPLASH.PCX"));
	TEST_EXPECT(!archive_has(build + "/resource.pff", "LOGO.PNG"));
	TEST_EXPECT(archive_has(build + "/resource.pff", "PLAIN.PNG"));
	TEST_EXPECT(run({"validate", root}) == 0);
	// A source that no longer decodes, with its output gone: validate lists the import
	// failure and the output it could not make, and the build is blocked.
	TEST_EXPECT(editor_test::write_text(root + "/art/logo.png", "not a png"));
	fs::remove_all(root + "/.opennova/imported", ec);
	TEST_EXPECT(run_capture(capture, {"validate", root}, text) == 1);
	TEST_EXPECT(text.find("import.decode") != std::string::npos && text.find("import.output_missing") != std::string::npos);
	TEST_EXPECT(run({"build", root}) == 1);
	// The source deleted: its record lists nothing and says so, and the project is clean.
	fs::remove(root + "/art/logo.png", ec);
	TEST_EXPECT(run_capture(capture, {"validate", root}, text) == 0);
	TEST_EXPECT(text.find("import.orphan_record") != std::string::npos && text.find("import.decode") == std::string::npos);
	return 0;
}

// import --with-dependencies copies the files a source needs, found beside it, and names the
// ones found nowhere; --dry-run prints the plan (each file taken, where it goes, what wanted
// it, where it comes from; what is not found; what is not followed) and writes nothing;
// without --with-dependencies the plan is the source alone (S11g).
static int test_import_with_dependencies() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_dependencies");
	const std::string root = dir.file("game");
	TEST_EXPECT(run({"new", root, "--title", "Dependencies"}) == 0);
	const std::string art = dir.file("art");
	const std::string position = "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM></POSITION>\r\n";
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", "<SCREEN>\r\n<NAME>A</NAME>\r\n<WINDOW TYPE=\"BUTTON\" NAME=\"GO\">\r\n" +
	                                                        position + "<FONT><NAME>arial99</NAME></FONT>\r\n"
	                                                        "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">gone.tga</APPEARANCE>\r\n"
	                                                        "<ACTION TYPE=\"SCREEN\" FILE=\"b.mnu\">B</ACTION>\r\n"
	                                                        "</WINDOW>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(editor_test::write_text(art + "/b.mnu", "<SCREEN>\r\n<NAME>B</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"BACK\">\r\n" +
	                                                        position + "</WINDOW>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(editor_test::write_text(art + "/arial99.fnt", "fnt"));
	const std::string capture = dir.file("out.txt");
	std::string text;
	const auto has = [&text](const std::string &line) { return text.find(line) != std::string::npos; };
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies", "--dry-run"}, text) == 0);
	TEST_EXPECT(has("take a.mnu (Menu) -> menus/a.mnu, chosen, from the folder " + art + "\n"));
	TEST_EXPECT(has("take arial99.fnt (Font) -> fonts/arial99.fnt, needed by a.mnu: A/GO font.name, from the folder " + art + "\n"));
	TEST_EXPECT(has("take b.mnu (Menu) -> menus/b.mnu, needed by a.mnu: A/GO"));
	TEST_EXPECT(has("not found gone.tga (Texture), needed by a.mnu: A/GO/Appearance 1"));
	TEST_EXPECT(has("not followed: menu_screen references, which name no file (1, the first in a.mnu)"));
	TEST_EXPECT(has("plan: 3 file(s) to import, 1 not found"));
	TEST_EXPECT(!fs::exists(root + "/menus") && !fs::exists(root + "/fonts")); // written nowhere
	// The source alone without --with-dependencies.
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--dry-run"}, text) == 0);
	TEST_EXPECT(has("plan: 1 file(s) to import, 0 not found") && !has("arial99") && !fs::exists(root + "/menus"));
	// The import: the closure copied, the file found nowhere named.
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies"}, text) == 0);
	TEST_EXPECT(has("imported menus/a.mnu") && has("imported fonts/arial99.fnt") && has("imported menus/b.mnu"));
	TEST_EXPECT(has("not found gone.tga (Texture)"));
	TEST_EXPECT(fs::is_regular_file(root + "/menus/a.mnu") && fs::is_regular_file(root + "/fonts/arial99.fnt") &&
	            fs::is_regular_file(root + "/menus/b.mnu") && !fs::exists(root + "/gone.tga"));
	return 0;
}

// Every file under `root` with its size and last write: what "nothing written" compares.
static std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> snapshot(const fs::path &root) {
	std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> files;
	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_regular_file(ec)) files[it->path().generic_string()] = {it->file_size(ec), it->last_write_time(ec)};
	return files;
}

// The command with its arguments from a list (more than an initializer list holds), its
// error stream kept in `text`.
static int run_list(const std::string &capture_file, const std::vector<std::string> &args, std::string &text) {
	std::vector<const char *> argv;
	for (const std::string &a : args) argv.push_back(a.c_str());
	text.clear();
	std::FILE *err = std::fopen(capture_file.c_str(), "w+b");
	if (!err) return -1;
	const int code = run_project_command(static_cast<int>(argv.size()), argv.data(), stdout, err);
	std::rewind(err);
	char buffer[4096];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), err)) > 0;) text.append(buffer, n);
	std::fclose(err);
	return code;
}

// A dry run writes nothing, the import pass included: with an import source changed since it
// was last imported (its outputs, its record and the import cache stale), nothing on the disk
// changes. A real import runs the pass as every command does.
static int test_dry_run_writes_nothing() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_dry_run");
	const std::string root = dir.file("game");
	TEST_EXPECT(run({"new", root}) == 0);
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", editor_test::gradient_png(8, 8)));
	TEST_EXPECT(mark_for_import(root + "/art/logo.png"));
	TEST_EXPECT(run({"status", root}) == 0);
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", editor_test::gradient_png(8, 8, 9))); // changed since
	TEST_EXPECT(editor_test::write_text(dir.file("notes.txt"), "notes"));
	const std::string record = root + "/art/logo.png.import";
	std::string stale, now, error;
	TEST_EXPECT(opennova::editor::read_file_text(record, stale, error));
	const auto before = snapshot(root);
	std::string text;
	TEST_EXPECT(run_capture(dir.file("out.txt"), {"import", root, dir.file("notes.txt"), "--with-dependencies", "--dry-run"},
	                        text) == 0);
	TEST_EXPECT(text.find("take notes.txt") != std::string::npos && text.find("imported ") == std::string::npos);
	TEST_EXPECT(snapshot(root) == before);
	// The import itself: the pass first (the changed source's record written again), then the file.
	TEST_EXPECT(run_capture(dir.file("out.txt"), {"import", root, dir.file("notes.txt")}, text) == 0);
	TEST_EXPECT(text.find("imported notes.txt") != std::string::npos && opennova::editor::read_file_text(record, now, error) &&
	            now != stale);
	return 0;
}

// An .o3d's textures come with it only through its plan: imported alone, the model lands and
// the command says, for each texture the model names that the import does not bring, how to
// bring it; with --with-dependencies the texture beside it comes too, and only the one found
// nowhere is said.
static int test_scene_textures() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_scene");
	const std::string root = dir.file("game");
	TEST_EXPECT(run({"new", root}) == 0);
	const std::string scene = dir.file("scene");
	std::error_code ec;
	fs::create_directories(scene, ec);
	fs::copy_file(fs::path(test_paths_repo_root(__FILE__)) / "fixtures" / "threedi" / "o3d" / "spinner.o3d",
	              scene + "/spinner.o3d", ec);
	TEST_EXPECT(!ec && editor_test::write_text(scene + "/SPINNER.TGA", "tga")); // it names spinner.tga and glow.tga
	std::string text;
	TEST_EXPECT(run_usage(dir.file("err.txt"), {"import", root, scene + "/spinner.o3d"}, text) == 1);
	TEST_EXPECT(fs::is_regular_file(root + "/models/spinner.3di") && !fs::exists(root + "/SPINNER.TGA"));
	TEST_EXPECT(text.find("import.texture_not_imported") != std::string::npos && text.find("spinner.tga") != std::string::npos &&
	            text.find("glow.tga") != std::string::npos && text.find("--with-dependencies") != std::string::npos);
	TEST_EXPECT(run_usage(dir.file("err.txt"), {"import", root, scene + "/spinner.o3d", "--replace", "--with-dependencies"},
	                      text) == 1);
	TEST_EXPECT(fs::is_regular_file(root + "/SPINNER.TGA"));
	TEST_EXPECT(text.find("glow.tga") != std::string::npos && text.find("names the texture spinner.tga") == std::string::npos);
	return 0;
}

// The cap binds the command line as it binds the dialog: 1001 members of an archive with
// --with-dependencies make a plan cut at 1000, and the command imports those the plan holds, not
// the one past it, and says so.
static int test_cap() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_cap");
	const std::string root = dir.file("game");
	TEST_EXPECT(run({"new", root}) == 0);
	std::vector<std::string> names;
	for (int i = 0; i < 1001; ++i) {
		char name[16];
		std::snprintf(name, sizeof(name), "t%04d.txt", i);
		names.push_back(name);
	}
	const uint8_t data[] = {'x'};
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const std::string &name : names) entries.push_back({name.c_str(), data, sizeof(data), 0, 0, 0});
	const std::string archive = dir.file("many.pff");
	TEST_EXPECT(opennova::pff::pff_write_archive(archive.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(),
	                                             uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK);
	std::vector<std::string> args = {"import", root, archive, "--with-dependencies"};
	for (const std::string &name : names) {
		args.push_back("--entry");
		args.push_back(name);
	}
	std::string text;
	TEST_EXPECT(run_list(dir.file("err.txt"), args, text) == 1);
	TEST_EXPECT(text.find("the plan stopped at 1000 files") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/t0999.txt") && !fs::exists(root + "/t1000.txt"));
	return 0;
}

namespace {

using editor_test::NoProcess;

// A finding as `validate` prints it.
std::string printed(const opennova::editor::Diagnostic &d) {
	std::string line = std::string(opennova::editor::diagnostic_severity_label(d.severity)) + " " + d.code() + ": " + d.message;
	if (!d.asset.empty()) line += " [" + d.asset + "]";
	if (d.line) line += " line " + std::to_string(d.line);
	if (!d.record.empty()) line += " record " + d.record;
	if (!d.field.empty()) line += " field " + d.field;
	return line;
}

} // namespace

// S12 C2: `validate` lists what the editor's Problems lists for the same project, in the same
// order, from the one composer: the scan's, the requirements', the documents' and the graph's,
// and the menu render check's notes (a menu whose colour the game draws transparent).
static int test_validate_matches_the_editor() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_problems");
	const std::string root = dir.file("Problems");
	TEST_EXPECT(run({"new", root, "--title", "Problems"}) == 0);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	TEST_EXPECT(editor_test::write_text(root + "/menus/extra.mnu",
	                                    "<SCREEN>\r\n<NAME>EXTRA</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"TITLE\">\r\n"
	                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></POSITION>\r\n"
	                                    "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">FF0000</APPEARANCE>\r\n"
	                                    "</WINDOW>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/a_name_too_long_for_archives.txt", "x"));
	std::string text;
	run_capture(dir.file("validate.txt"), {"validate", root}, text);
	std::vector<std::string> listed;
	for (size_t start = 0; start < text.size();) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		const std::string line = text.substr(start, end - start);
		for (const char *label : {"error ", "warning ", "info "})
			if (line.rfind(label, 0) == 0) listed.push_back(line);
		start = end + 1;
	}
	NoProcess platform;
	opennova::editor::MemoryPreferencesStore preferences;
	opennova::editor::ProjectSession session(platform, preferences);
	session.handle(opennova::editor::request::open_project(root));
	session.run_operations();
	std::vector<std::string> shown;
	bool render_note = false;
	for (const opennova::editor::Diagnostic &d : session.view().findings.diagnostics) {
		shown.push_back(printed(d));
		render_note = render_note || d.code().rfind("menu.render.", 0) == 0;
	}
	TEST_EXPECT(session.project_open() && render_note && !shown.empty());
	TEST_EXPECT(listed == shown);
	return 0;
}

// S12 C6: one game install. The editor sets the project's (the settings' Apply writes its
// local.json) and opennova-project reads the same one: status names it, and an import with
// the files it needs finds the font it lacks there. A project naming none takes the install
// the editor last chose when the editor opens it, and the command line then reads that. An
// install given as a relative path is kept absolute, from the editor's working directory, so
// the command line run from another directory finds the same one.
static int test_one_game_install() {
	struct WorkingDirectory {
		fs::path saved = fs::current_path();
		~WorkingDirectory() {
			std::error_code ignored;
			fs::current_path(saved, ignored);
		}
	} const restore;
	editor_test::TempProjectDir dir("opennova_editor_project_cli_install");
	const std::string root = dir.file("Install");
	TEST_EXPECT(run({"new", root, "--title", "Install"}) == 0);
	const std::string install = dir.file("Joint Ops");
	std::error_code ec;
	fs::create_directories(install, ec);
	const std::string font = "fnt";
	const opennova::pff::PffWriteEntry entries[] = {
	        {"arial99.fnt", reinterpret_cast<const uint8_t *>(font.data()), uint32_t(font.size()), 0, 0, 0}};
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3,
	                                             entries, 1) == opennova::pff::PFF_WRITE_OK);
	const std::string art = dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", "<SCREEN>\r\n<NAME>A</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"GO\">\r\n"
	                                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM>"
	                                                    "</POSITION>\r\n<FONT><NAME>arial99</NAME></FONT>\r\n</WINDOW>\r\n"
	                                                    "</SCREEN>\r\n"));
	NoProcess platform;
	{
		opennova::editor::FilePreferencesStore preferences(dir.file("settings.json"));
		opennova::editor::ProjectSession session(platform, preferences);
		session.handle(opennova::editor::request::open_project(root));
		session.run_operations();
		TEST_EXPECT(session.project_open() && session.view().project.retail_directory.empty());
		opennova::editor::EditorRequest apply =
		        opennova::editor::request::of(opennova::editor::EditorRequestKind::ApplyProjectSettings);
		fs::current_path(dir.path, ec);
		TEST_EXPECT(!ec);
		apply.settings.game_install = "art/../Joint Ops"; // from the editor's working directory
		session.handle(apply);
		TEST_EXPECT(session.view().project.retail_directory == install && session.view().project.settings_result.failures.empty());
	}
	// The command line elsewhere.
	fs::current_path(root, ec);
	TEST_EXPECT(!ec);
	const std::string capture = dir.file("out.txt");
	std::string text;
	const auto has = [&text](const std::string &line) { return text.find(line) != std::string::npos; };
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0 && has("game install: " + install + "\n"));
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies", "--dry-run"}, text) == 0);
	TEST_EXPECT(has("take arial99.fnt (Font) -> fonts/arial99.fnt, needed by a.mnu: A/GO font.name, from the game install"));

	// Another project names none: the editor opens it on the install it last chose.
	const std::string other = dir.file("Other");
	TEST_EXPECT(run({"new", other, "--title", "Other"}) == 0);
	TEST_EXPECT(run_capture(capture, {"status", other}, text) == 0 && !has("game install:"));
	{
		opennova::editor::FilePreferencesStore preferences(dir.file("settings.json"));
		opennova::editor::ProjectSession session(platform, preferences);
		session.handle(opennova::editor::request::open_project(other));
		session.run_operations();
		TEST_EXPECT(session.project_open() && session.view().project.retail_directory == install);
	}
	TEST_EXPECT(run_capture(capture, {"status", other}, text) == 0 && has("game install: " + install + "\n"));
	return 0;
}

// S13 A4: a project's local.json an older editor wrote (schema 1, the install under retail_root)
// is set aside, never refused: opennova-project goes on with the warning naming what it held,
// the editor opens the project (the outcome done) with that one warning, and the next write (the
// install chosen again in the project settings) makes a new file, which the command line then
// reads with nothing to say.
static int test_older_local_settings() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_older_local");
	const std::string root = dir.file("Older");
	TEST_EXPECT(run({ "new", root, "--title", "Older" }) == 0);
	const std::string install = dir.file("Joint Ops");
	const std::string local = opennova::editor::ProjectPaths::for_root(root).local_settings_file;
	TEST_EXPECT(editor_test::write_text(local,
			"{\"retail_root\": \"" + install +
					"\", \"runtime_executable\": \"\", \"schema_version\": 1}\n"));
	std::string text;
	const auto warned = [&text, &install]() {
		return text.find("local_settings.schema_version.unsupported") != std::string::npos &&
				text.find("retail_root \"" + install + "\"") != std::string::npos;
	};
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "status", root }, text) == 0 && warned());
	NoProcess platform;
	opennova::editor::MemoryPreferencesStore preferences;
	opennova::editor::ProjectSession session(platform, preferences);
	session.handle(opennova::editor::request::open_project(root));
	session.run_operations();
	size_t warnings = 0;
	for (const opennova::editor::Diagnostic &d : session.outcome().findings)
		warnings += d.code() == "local_settings.schema_version.unsupported" &&
				d.severity == opennova::editor::DiagnosticSeverity::Warning;
	TEST_EXPECT(session.project_open() && session.outcome().done() && warnings == 1 &&
			session.view().project.retail_directory.empty());
	opennova::editor::ProjectSettingsChange change;
	change.game_install = install;
	session.handle(opennova::editor::request::apply_project_settings(change));
	std::string written, io_error;
	TEST_EXPECT(opennova::editor::read_file_text(local, written, io_error) &&
			written.find("\"schema_version\": 2") != std::string::npos &&
			written.find("\"game_install\": \"" + install + "\"") != std::string::npos &&
			written.find("retail") == std::string::npos);
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "status", root }, text) == 0 && !warned() &&
			text.find("local_settings") == std::string::npos);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_usage_errors();
	failures += test_new_status_validate();
	failures += test_validate_matches_the_editor();
	failures += test_one_game_install();
	failures += test_older_local_settings();
	failures += test_imports();
	failures += test_import_with_dependencies();
	failures += test_dry_run_writes_nothing();
	failures += test_scene_textures();
	failures += test_cap();
	if (failures == 0) std::printf("project_cli: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
