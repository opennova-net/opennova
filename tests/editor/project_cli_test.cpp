// Drives the real opennova-project verbs (ADR 0046 d4) end to end on a temporary
// project: new, status, validate, create-missing, build, import and reimport, their
// exit codes, and the one refresh they share with the editor (S9c: every verb
// imports the changed sources first), an import with the files it needs (S11g: the plan,
// a dry run that writes nothing, an .o3d's textures, the cap), validate listing what the
// editor's Problems lists (S12), and the one game install the editor and the command line
// share (S12). Since S13 A7 each verb is the editor's session, headless
// (project_cli_session_test.cpp holds its table, its JSON and the request and query verbs).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <base/io/json.h>
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

#include "cli_verbs.h"
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
	TEST_EXPECT(text.find("--entry needs a file name") != std::string::npos);
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "import", "p", "--retail", "game" }, text) == 2);
	TEST_EXPECT(text.find("unknown option --retail") != std::string::npos);
	// An argument that starts with a dash is an option, the verb's or unknown: a mistyped one never
	// becomes a folder (new -x makes no ./-x).
	std::error_code ec;
	const fs::path stray = fs::current_path(ec) / "-x";
	TEST_EXPECT(!ec && !fs::exists(stray));
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "new", "-x" }, text) == 2);
	TEST_EXPECT(text.find("unknown option -x for new") != std::string::npos);
	TEST_EXPECT(!fs::exists(stray));
	// An option is given once (but --entry), and its value is never another option.
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "new", dir.file("twice"), "--title", "A", "--title", "B" }, text) == 2);
	TEST_EXPECT(text.find("--title is given twice") != std::string::npos && !fs::exists(dir.file("twice")));
	TEST_EXPECT(run_usage(dir.file("err.txt"), { "new", dir.file("twice"), "--title", "--game", "jo" }, text) == 2);
	TEST_EXPECT(text.find("--title needs a text") != std::string::npos && !fs::exists(dir.file("twice")));
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
	// Without --title the project is named after its folder, as on every path that leaves the title
	// empty (the editor's form, a new_project request).
	const std::string untitled = dir.file("Folder Named");
	TEST_EXPECT(run({"new", untitled}) == 0);
	TEST_EXPECT(opennova::editor::open_project(untitled, doc, error) && doc.title == "Folder Named");

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
	// S13 A8: the line says what the build read and linked as --json does; --rehash reads every file.
	{
		std::string built;
		TEST_EXPECT(run_capture(dir.file("built.txt"), {"build", root, "--out", dir.file("rehashed"), "--rehash"}, built) == 0);
		TEST_EXPECT(built.find("built ") != std::string::npos && built.find(" of them linked, ") != std::string::npos &&
		            built.find(" hashed)") != std::string::npos);
	}

	// Generic native file import uses the same core; a file the project holds already is kept as it
	// is, whatever its bytes, unless --replace writes it over (review F2).
	TEST_EXPECT(run({"import", root}) == 2);
	const std::string source = dir.file("source.txt");
	TEST_EXPECT(editor_test::write_text(source, "imported file"));
	TEST_EXPECT(run({"import", root, source}) == 0);
	TEST_EXPECT(std::filesystem::is_regular_file(root + "/source.txt"));
	TEST_EXPECT(run({"import", root, source}) == 0);
	TEST_EXPECT(editor_test::write_text(source, "changed file"));
	std::string held_text, held_error;
	TEST_EXPECT(run({"import", root, source}) == 0 && opennova::editor::read_file_text(root + "/source.txt", held_text, held_error) &&
	            held_text == "imported file");
	TEST_EXPECT(run({"import", root, source, "--replace"}) == 0 && opennova::editor::read_file_text(root + "/source.txt", held_text, held_error) &&
	            held_text == "changed file");

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
	// A project made in a folder that holds a source imports none of it: new opens it without the
	// import pass, and the first verb that reads it imports.
	const std::string holder = dir.file("Holder");
	TEST_EXPECT(editor_test::write_bytes(holder + "/art/logo.png", editor_test::gradient_png(8, 8)));
	TEST_EXPECT(mark_for_import(holder + "/art/logo.png"));
	TEST_EXPECT(run({"new", holder}) == 0);
	TEST_EXPECT(!fs::exists(holder + "/.opennova/imported"));
	TEST_EXPECT(run_capture(capture, {"status", holder}, text) == 0);
	TEST_EXPECT(text.find("imports: 1 source, 1 imported now, 0 failed") != std::string::npos);
	TEST_EXPECT(fs::exists(holder + "/.opennova/imported"));
	TEST_EXPECT(run({"new", root}) == 0);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	// A PNG with no record is a texture the build packs as it is.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/plain.png", editor_test::gradient_png(4, 4, 3)));
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", editor_test::gradient_png(8, 8)));
	TEST_EXPECT(mark_for_import(root + "/art/logo.png"));
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0);
	TEST_EXPECT(text.find("imports: 1 source, 1 imported now, 0 failed") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/art/logo.png.import"));
	// A PNG brought in by `import` is imported at once, as the editor's rescan does.
	TEST_EXPECT(editor_test::write_bytes(dir.file("splash.png"), editor_test::gradient_png(4, 4, 5)));
	TEST_EXPECT(run_capture(capture, {"import", root, dir.file("splash.png")}, text) == 0);
	TEST_EXPECT(text.find("-> 1 output") != std::string::npos);
	TEST_EXPECT(text.find("imported splash.png\n") != std::string::npos);
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0);
	TEST_EXPECT(text.find("imports: 2 sources, 0 imported now, 0 failed") != std::string::npos);
	// reimport: nothing changed, nothing imports; --force with a source imports that one.
	TEST_EXPECT(run_capture(capture, {"reimport", root}, text) == 0 && text.find("2 sources, 0 imported") != std::string::npos);
	TEST_EXPECT(run_capture(capture, {"reimport", root, "--force", "--source", "logo.png"}, text) == 0);
	TEST_EXPECT(text.find("imported art/logo.png") != std::string::npos && text.find("2 sources, 1 imported") != std::string::npos);
	// A fresh clone has no .opennova/ at all (its .gitignore ignores itself): status
	// imports into it and makes it again with its self-ignore file, so neither the cache
	// nor the outputs show up in the modder's repository.
	fs::remove_all(root + "/.opennova", ec);
	TEST_EXPECT(run_capture(capture, {"status", root}, text) == 0);
	TEST_EXPECT(text.find("imports: 2 sources, 2 imported now, 0 failed") != std::string::npos);
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
	// A PNG brought in that does not decode: it is written, the pass fails on it, and the Problems
	// row on it is said on the error stream (exit 1).
	TEST_EXPECT(editor_test::write_text(dir.file("broken.png"), "not a png"));
	TEST_EXPECT(run_usage(capture, {"import", root, dir.file("broken.png")}, text) == 1);
	TEST_EXPECT(text.find("error import.decode") != std::string::npos && text.find("broken.png") != std::string::npos);
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
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies", "--dry-run", "--rows"}, text) == 0);
	TEST_EXPECT(has("take a.mnu (menu) -> menus/a.mnu, chosen, from the folder " + art + "\n"));
	TEST_EXPECT(has("take arial99.fnt (font) -> fonts/arial99.fnt, needed by a.mnu: A/GO font.name, from the folder " + art + "\n"));
	TEST_EXPECT(has("take b.mnu (menu) -> menus/b.mnu, needed by a.mnu: A/GO"));
	TEST_EXPECT(has("not found gone.tga (texture), needed by a.mnu: A/GO/Appearance 1"));
	// S14: the screen B is a symbol the planned b.mnu defines, followed to nothing; the plan by kind.
	TEST_EXPECT(!has("not followed: menu_screen") && !has("undefined:"));
	TEST_EXPECT(has("  menu: 2 files, 0.0 MB\n") && has("  font: 1 file, 0.0 MB\n"));
	TEST_EXPECT(has("plan: 3 files to import, 1 not found, 0.0 MB"));
	TEST_EXPECT(!fs::exists(root + "/menus") && !fs::exists(root + "/fonts")); // written nowhere
	// Without --rows, the summary alone.
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies", "--dry-run"}, text) == 0);
	TEST_EXPECT(!has("take ") && has("not found gone.tga") && has("  menu: 2 files") && has("plan: 3 files to import"));
	// The source alone without --with-dependencies.
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--dry-run"}, text) == 0);
	TEST_EXPECT(has("plan: 1 file to import, 0 not found") && !has("arial99") && !fs::exists(root + "/menus"));
	// The import: the closure copied, the file found nowhere named.
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies"}, text) == 0);
	TEST_EXPECT(has("imported menus/a.mnu") && has("imported fonts/arial99.fnt") && has("imported menus/b.mnu"));
	TEST_EXPECT(has("not found gone.tga (texture)"));
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
	// Dated past the import, as a later edit is: the import cache vouches for a source's hash while
	// its size and last write hold, and a file system's clock can stand still for milliseconds
	// (some 4 ms on Linux), so a rewrite at the same size this soon after the import would read
	// as unchanged (ADR 0046 S13 A3).
	std::error_code ec;
	const fs::file_time_type rewritten = fs::last_write_time(root + "/art/logo.png", ec);
	TEST_EXPECT(!ec);
	fs::last_write_time(root + "/art/logo.png", rewritten + std::chrono::hours(1), ec);
	TEST_EXPECT(!ec);
	TEST_EXPECT(editor_test::write_text(dir.file("notes.txt"), "notes"));
	const std::string record = root + "/art/logo.png.import";
	std::string stale, now, error;
	TEST_EXPECT(opennova::editor::read_file_text(record, stale, error));
	const auto before = snapshot(root);
	std::string text;
	TEST_EXPECT(run_capture(dir.file("out.txt"), {"import", root, dir.file("notes.txt"), "--with-dependencies", "--dry-run", "--rows"},
	                        text) == 0);
	TEST_EXPECT(text.find("take notes.txt") != std::string::npos && text.find("imported ") == std::string::npos);
	TEST_EXPECT(snapshot(root) == before);
	// --install too: a dry run opens the project on it for that run alone, so no
	// .opennova/local.json is made or changed.
	fs::create_directories(dir.file("install"), ec);
	TEST_EXPECT(run_capture(dir.file("out.txt"),
	                        {"import", root, dir.file("notes.txt"), "--dry-run", "--rows", "--install", dir.file("install")}, text) == 0);
	TEST_EXPECT(text.find("take notes.txt") != std::string::npos && snapshot(root) == before);
	TEST_EXPECT(!fs::exists(root + "/.opennova/local.json"));
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
	// The same model again: the .3di the project holds with the same bytes is left as it is, so the
	// import names nothing it wrote (the outcome's imported list is import_assets' own); with
	// --replace it is written and named again.
	TEST_EXPECT(run_capture(dir.file("out.txt"), {"import", root, scene + "/spinner.o3d"}, text) == 0);
	TEST_EXPECT(text.find("imported models/spinner.3di") == std::string::npos);
	TEST_EXPECT(run_capture(dir.file("out.txt"), {"import", root, scene + "/spinner.o3d", "--replace"}, text) == 1);
	TEST_EXPECT(text.find("imported models/spinner.3di\n") != std::string::npos);
	return 0;
}

// ADR 0046 S14: the plan's cap is a guard (kImportPlanFileCap, fifty thousand), not a limit an
// import meets: 1001 members of an archive with --with-dependencies plan whole, and the command
// imports every one and says nothing of a stop.
static int test_many() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_many");
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
	TEST_EXPECT(run_list(dir.file("err.txt"), args, text) == 0);
	TEST_EXPECT(text.find("the plan stopped at") == std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/t0999.txt") && fs::is_regular_file(root + "/t1000.txt"));
	return 0;
}

namespace {

using editor_test::NoProcess;

// What a run printed, line by line.
std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> lines;
	for (size_t start = 0; start < text.size();) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		lines.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return lines;
}

} // namespace

// S12 C2, pinned (S13 A7): `validate` lists the editor's Problems rows, each as it prints a
// finding, in the Problems window's order (errors, then warnings, then notes), and fails exactly
// when a build would be refused (the build_gate query), which it says. Since the command line is
// the session, the rows are pinned here, not compared with a session: for a project with a
// texture whose name no archive can store and a menu whose colour the game draws transparent,
// the name's error (the scan's), the render check's warning, then only notes (the optional files
// the project lacks, the stylesheet's variables no menu names); the build's own check of the name,
// which no row shows, before the verdict; and exit 1, as the build is refused. The name gone, the
// same warning and notes and exit 0: a warning never refuses a build.
static int test_validate_pins_the_rows() {
	editor_test::TempProjectDir dir("opennova_editor_project_cli_problems");
	const std::string root = dir.file("Problems");
	TEST_EXPECT(run({"new", root, "--title", "Problems"}) == 0);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	TEST_EXPECT(editor_test::write_text(root + "/menus/extra.mnu",
	                                    "<SCREEN>\r\n<NAME>EXTRA</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"TITLE\">\r\n"
	                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></POSITION>\r\n"
	                                    "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">FF0000</APPEARANCE>\r\n"
	                                    "</WINDOW>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/art/a_texture_name_too_long.pcx", "pcx"));
	const std::string name_row =
	        "error asset.name.too_long: The file name a_texture_name_too_long.pcx is longer than 16 characters; the "
	        "game cannot store it in an archive. [art/a_texture_name_too_long.pcx]";
	const std::string render_row =
	        "warning menu.render.color_transparent: The game reads \"FF0000\" as AARRGGBB, so with fewer than eight "
	        "digits its alpha is 0 (the preview draws it transparent); write eight digits (FF, then RRGGBB) for an "
	        "opaque one. [menus/extra.mnu] record EXTRA/TITLE/Appearance 1 field value";
	const std::string gate_row =
	        "error build.name_unstorable: The game cannot store a_texture_name_too_long.pcx in an "
	        "archive (the name is too long). [art/a_texture_name_too_long.pcx]";
	// Only notes after the rows pinned above them, of the two kinds the project makes, then the
	// build gate's verdict. The build's own word on the files is a row among the errors.
	const auto notes_only = [](const std::vector<std::string> &lines, size_t from, size_t to, size_t &notes) {
		notes = 0;
		for (size_t i = from; i < to; ++i) {
			if (lines[i].rfind("info requirement.optional_missing: ", 0) != 0 && lines[i].rfind("info style.unused: ", 0) != 0)
				return false;
			++notes;
		}
		return true;
	};
	std::string text;
	TEST_EXPECT(run_capture(dir.file("validate.txt"), {"validate", root}, text) == 1);
	std::vector<std::string> lines = lines_of(text);
	size_t notes = 0;
	TEST_EXPECT(lines.size() > 4 && lines[0] == name_row && lines[1] == gate_row && lines[2] == render_row);
	TEST_EXPECT(notes_only(lines, 3, lines.size() - 1, notes) && notes > 0);
	TEST_EXPECT(lines.back() == "not ok: 2 findings block a build");
	TEST_EXPECT(run({"build", root}) == 1);
	std::error_code ec;
	fs::remove(root + "/art/a_texture_name_too_long.pcx", ec);
	TEST_EXPECT(run_capture(dir.file("validate.txt"), {"validate", root}, text) == 0);
	lines = lines_of(text);
	size_t notes_now = 0;
	TEST_EXPECT(lines.size() > 2 && lines[0] == render_row && notes_only(lines, 1, lines.size() - 1, notes_now));
	TEST_EXPECT(notes_now == notes && lines.back() == "ok: no errors");
	TEST_EXPECT(run({"build", root}) == 0);
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
	TEST_EXPECT(run_capture(capture, {"import", root, art + "/a.mnu", "--with-dependencies", "--dry-run", "--rows"}, text) == 0);
	TEST_EXPECT(has("take arial99.fnt (font) -> fonts/arial99.fnt, needed by a.mnu: A/GO font.name, from the game install"));
	// Everything the install has (ADR 0046 S14): its archives' files and the loose files the game
	// ships beside them, chosen at once, with no walk and nothing else named; imported as the plan
	// has them.
	TEST_EXPECT(editor_test::write_text(install + "/menumus.sbf", "music") && editor_test::write_text(install + "/game.cfg", "cfg"));
	TEST_EXPECT(run_usage(dir.file("err.txt"), {"import", root, "--all", "--entry", "arial99.fnt"}, text) == 2 &&
	            text.find("--all") != std::string::npos);
	// Every file and a walk at once: a usage error, never a flag left unread (review F14).
	TEST_EXPECT(run_usage(dir.file("err.txt"), {"import", root, "--all", "--with-dependencies"}, text) == 2 &&
	            text.find("--with-dependencies") != std::string::npos);
	TEST_EXPECT(run_capture(capture, {"import", root, "--all", "--dry-run", "--rows"}, text) == 0);
	TEST_EXPECT(has("take arial99.fnt (font) -> fonts/arial99.fnt, chosen, from the game install") &&
	            has("take menumus.sbf (music_bank) -> ") && !has("game.cfg") && has("plan: 2 files to import, 0 not found"));
	TEST_EXPECT(!fs::exists(root + "/fonts/arial99.fnt"));
	TEST_EXPECT(run_capture(capture, {"import", root, "--all"}, text) == 0);
	TEST_EXPECT(has("imported fonts/arial99.fnt") && has("imported ") && fs::is_regular_file(root + "/fonts/arial99.fnt"));
	std::string music, io_error;
	TEST_EXPECT(opennova::editor::read_file_text(root + "/menumus.sbf", music, io_error) && music == "music");

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
	// What the Open came to says what it set aside (S13 A3: the local settings are opened as it
	// finishes).
	const opennova::editor::ActionOutcome opened =
			editor_test::handle_to_end(session, opennova::editor::request::open_project(root));
	size_t warnings = 0;
	for (const opennova::editor::Diagnostic &d : opened.findings)
		warnings += d.code() == "local_settings.schema_version.unsupported" &&
				d.severity == opennova::editor::DiagnosticSeverity::Warning;
	TEST_EXPECT(session.project_open() && opened.done() && warnings == 1 &&
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
	failures += test_validate_pins_the_rows();
	failures += test_one_game_install();
	failures += test_older_local_settings();
	failures += test_imports();
	failures += test_import_with_dependencies();
	failures += test_dry_run_writes_nothing();
	failures += test_scene_textures();
	failures += test_many();
	if (failures == 0) std::printf("project_cli: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
