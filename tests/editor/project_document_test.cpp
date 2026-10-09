// Pins the project file (ADR 0046 d6): creation, the deterministic on-disk form, the
// round trip, and the refusals (unknown schema version, unknown game, missing file); and the project's
// local settings (local.json), how it plays among them.
#include <cstdio>
#include <filesystem>
#include <string>

#include <base/io/json.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/run/run_directory.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

static int test_create_then_open() {
	editor_test::TempProjectDir dir("opennova_editor_project_document_test");
	const std::string root = dir.file("MyGame");
	ProjectDocument created;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "My Game", "JO", created, error));
	TEST_EXPECT(created.title == "My Game");
	TEST_EXPECT(created.target_game == "jo");
	TEST_EXPECT(created.project_id.size() == 36);
	TEST_EXPECT(created.features.menu && !created.features.mission);
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(fs::is_regular_file(paths.project_file));
	TEST_EXPECT(fs::is_directory(paths.cache_dir));
	std::string ignore;
	std::string io_error;
	TEST_EXPECT(opennova::io::read_file_text((fs::path(paths.cache_dir) / ".gitignore").generic_string(), ignore, io_error));
	TEST_EXPECT(ignore == "*\n");

	ProjectDocument opened;
	TEST_EXPECT(open_project(root, opened, error));
	TEST_EXPECT(opened.project_id == created.project_id);
	TEST_EXPECT(opened.title == created.title);
	TEST_EXPECT(opened.target_game == "jo");
	TEST_EXPECT(opened.export_settings.output == kDefaultExportOutput);

	// A second create in the same place is refused; the project is untouched.
	ProjectDocument again;
	TEST_EXPECT(!create_project(root, "Other", "jo", again, error));
	TEST_EXPECT(error.code() == "project.exists");
	TEST_EXPECT(open_project(root, opened, error) && opened.title == "My Game");

	// A title defaults to the directory name.
	ProjectDocument untitled;
	TEST_EXPECT(create_project(dir.file("Untitled Thing"), "", "jo", untitled, error));
	TEST_EXPECT(untitled.title == "Untitled Thing");
	return 0;
}

static int test_on_disk_form_is_deterministic() {
	ProjectDocument doc;
	doc.project_id = "00000000-0000-4000-8000-000000000000";
	doc.title = "T";
	doc.features.mission = true;
	const std::string text = opennova::io::json_write(project_document_to_json(doc));
	const std::string expected =
	        "{\n"
	        "  \"export\": {\n"
	        "    \"include_runtime\": false,\n"
	        "    \"output\": \"build/export\"\n"
	        "  },\n"
	        "  \"features\": {\n"
	        "    \"menu\": true,\n"
	        "    \"mission\": true,\n"
	        "    \"multiplayer\": false\n"
	        "  },\n"
	        "  \"project_id\": \"00000000-0000-4000-8000-000000000000\",\n"
	        "  \"schema_version\": 2,\n"
	        "  \"target_game\": \"jo\",\n"
	        "  \"title\": \"T\"\n"
	        "}\n";
	TEST_EXPECT(text == expected);
	// An expansion project's object, both keys written; a standalone one has none (above).
	doc.expansion = { "jxm", "jox01" };
	const std::string with = opennova::io::json_write(project_document_to_json(doc));
	TEST_EXPECT(with.find("  \"expansion\": {\n"
	                      "    \"builds_on\": \"jox01\",\n"
	                      "    \"name\": \"jxm\"\n"
	                      "  },\n") != std::string::npos);
	doc.expansion = { "jxm", "" };
	TEST_EXPECT(opennova::io::json_write(project_document_to_json(doc)).find("\"builds_on\": \"\"") != std::string::npos);
	return 0;
}

// The expansion object (ADR 0046 S16): read back as written, absent for a standalone project, and
// refused where the game could not take it (the name rule, a builds_on with no name, another game).
static int test_expansion_object() {
	editor_test::TempProjectDir dir("opennova_editor_project_document_expansion_test");
	Diagnostic error;
	ProjectDocument doc;
	const std::string root = dir.file("Mod");
	TEST_EXPECT(create_project(root, "Mod", "jo", doc, error, ProjectExpansion{ "jxm", "jox01" }));
	TEST_EXPECT(doc.expansion.name == "jxm" && doc.expansion.builds_on == "jox01");
	ProjectDocument opened;
	TEST_EXPECT(open_project(root, opened, error) && opened.expansion == doc.expansion);
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const auto refused = [&](const std::string &object, const char *code, const char *words) {
		const std::string text = "{\"schema_version\": 2, \"project_id\": \"x\", \"title\": \"t\", \"target_game\": "
		                         "\"jo\", \"expansion\": " + object + "}\n";
		if (!editor_test::write_text(paths.project_file, text)) return false;
		ProjectDocument out;
		Diagnostic why;
		return !open_project(root, out, why) && why.code() == code && why.message.find(words) != std::string::npos;
	};
	TEST_EXPECT(refused("[]", "project.field.invalid", "must be an object"));
	TEST_EXPECT(refused("{}", "project.field.invalid", "names no expansion"));
	TEST_EXPECT(refused("{\"name\": 3}", "project.field.invalid", "must be a string"));
	TEST_EXPECT(refused("{\"builds_on\": \"jox01\"}", "project.field.invalid", "only as an expansion of its own"));
	TEST_EXPECT(refused("{\"name\": \"my mod\"}", "project.field.invalid", "one word"));
	TEST_EXPECT(refused("{\"name\": \"twelve_chars\"}", "project.field.invalid", "holds 11"));
	TEST_EXPECT(refused("{\"name\": \"jxm\", \"builds_on\": \"a/b\"}", "project.field.invalid", "'/'"));
	// An installed expansion's name binds no archive name: only the mount's 31 characters.
	TEST_EXPECT(editor_test::write_text(paths.project_file,
	        "{\"schema_version\": 2, \"project_id\": \"x\", \"title\": \"t\", \"expansion\": {\"name\": \"jxm\", "
	        "\"builds_on\": \"a_long_expansion_name\"}}\n"));
	TEST_EXPECT(open_project(root, opened, error) && opened.expansion.builds_on == "a_long_expansion_name");
	// Only Joint Operations' expansions are witnessed.
	TEST_EXPECT(editor_test::write_text(paths.project_file,
	        "{\"schema_version\": 2, \"project_id\": \"x\", \"title\": \"t\", \"target_game\": \"dfx\", "
	        "\"expansion\": {\"name\": \"jxm\"}}\n"));
	TEST_EXPECT(!open_project(root, opened, error) && error.code() == "project.expansion.unsupported");
	TEST_EXPECT(!create_project(dir.file("Dfx"), "t", "dfx", doc, error, ProjectExpansion{ "jxm", "" }));
	TEST_EXPECT(error.code() == "project.expansion.unsupported" && !fs::exists(dir.file("Dfx")));
	TEST_EXPECT(!create_project(dir.file("Long"), "t", "jo", doc, error, ProjectExpansion{ "twelve_chars", "" }));
	TEST_EXPECT(error.code() == "project.field.invalid" && !fs::exists(dir.file("Long")));
	return 0;
}

static int test_refusals() {
	editor_test::TempProjectDir dir("opennova_editor_project_document_refusals_test");
	Diagnostic error;
	ProjectDocument doc;

	TEST_EXPECT(!open_project(dir.file("nowhere"), doc, error));
	TEST_EXPECT(error.code() == "project.file.missing");

	const std::string root = dir.file("bad");
	const ProjectPaths paths = ProjectPaths::for_root(root);
	std::error_code ec;
	fs::create_directories(root, ec);
	TEST_EXPECT(editor_test::write_text(paths.project_file,
	                                    "{\"schema_version\": 3, \"project_id\": \"x\", \"title\": \"t\"}\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code() == "project.schema_version.unsupported");
	// Schema 1, which S16's expansion object superseded: refused (no reader pre-1.0), saying what
	// changed so its author can bring it over.
	TEST_EXPECT(editor_test::write_text(paths.project_file,
	                                    "{\"schema_version\": 1, \"project_id\": \"x\", \"title\": \"t\"}\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code() == "project.schema_version.unsupported" &&
	            error.message.find("set \"schema_version\" to 2") != std::string::npos);

	TEST_EXPECT(editor_test::write_text(
	        paths.project_file,
	        "{\"schema_version\": 2, \"project_id\": \"x\", \"title\": \"t\", \"target_game\": \"quake\"}\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code() == "project.target_game.unknown");

	TEST_EXPECT(editor_test::write_text(paths.project_file, "{\"schema_version\": 2,\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code() == "project.json");

	TEST_EXPECT(!create_project(dir.file("wrong-game"), "t", "quake", doc, error));
	TEST_EXPECT(error.code() == "project.target_game.unknown");
	return 0;
}

static int test_export_dir_and_local_settings() {
	editor_test::TempProjectDir dir("opennova_editor_project_paths_test");
	const std::string root = dir.file("P");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "P", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(paths.export_dir(doc) == (fs::path(root) / "build" / "export").lexically_normal().generic_string());
	TEST_EXPECT(paths.build_dir == (fs::path(root) / ".opennova" / "build").generic_string());

	LocalSettings local;
	TEST_EXPECT(load_local_settings(paths, local, error)); // absent = defaults
	TEST_EXPECT(local.runtime_executable.empty() && local.game_install.empty());
	local.runtime_executable = "C:/games/opennova.exe";
	// Saved into a project whose .opennova/ is gone: made again, ignored, before the file.
	std::error_code ec;
	fs::remove_all(paths.cache_dir, ec);
	TEST_EXPECT(!fs::exists(paths.cache_dir));
	TEST_EXPECT(save_local_settings(paths, local, error));
	std::string ignore, io_error;
	TEST_EXPECT(opennova::io::read_file_text((fs::path(paths.cache_dir) / ".gitignore").generic_string(), ignore, io_error) &&
	            ignore == "*\n");
	LocalSettings back;
	TEST_EXPECT(load_local_settings(paths, back, error));
	TEST_EXPECT(back.runtime_executable == "C:/games/opennova.exe");
	// Opened with a seed: a project naming no install takes it (written), one naming its own
	// keeps it; no seed (the command line) writes nothing. The seed is a path this platform
	// calls absolute ("C:/..." is relative on Linux), so it is kept as it is.
	const std::string install = dir.file("Joint Ops");
	TEST_EXPECT(open_local_settings(paths, std::string(), back, error) && back.game_install.empty());
	TEST_EXPECT(open_local_settings(paths, install, back, error) && back.game_install == install);
	TEST_EXPECT(load_local_settings(paths, back, error) && back.game_install == install &&
	            back.runtime_executable == "C:/games/opennova.exe");
	TEST_EXPECT(open_local_settings(paths, dir.file("Other"), back, error) && back.game_install == install);
	// The install kept absolute: a relative seed is taken from the working directory.
	TEST_EXPECT(absolute_install_path("").empty());
	TEST_EXPECT(absolute_install_path("games/../Joint Ops") ==
	            (fs::current_path() / "Joint Ops").lexically_normal().generic_string());
	LocalSettings bare;
	TEST_EXPECT(save_local_settings(paths, bare, error));
	TEST_EXPECT(open_local_settings(paths, "Joint Ops", back, error) &&
	            back.game_install == (fs::current_path() / "Joint Ops").lexically_normal().generic_string());
	// Another schema is set aside (S13 A4, pre-1.0: no reader for it): read as absent, the warning
	// naming the file, its schema and what it held; a seed then writes a new file over it.
	TEST_EXPECT(editor_test::write_text(paths.local_settings_file, "{\"schema_version\": 9}\n"));
	Diagnostic finding;
	TEST_EXPECT(load_local_settings(paths, back, finding) && back.game_install.empty() &&
			back.runtime_executable.empty());
	TEST_EXPECT(finding.severity == DiagnosticSeverity::Warning &&
			finding.code() == "local_settings.schema_version.unsupported" &&
			finding.message.find(paths.local_settings_file + " is set aside") == 0 &&
			finding.message.find("(schema 9; this editor reads schema 2)") != std::string::npos);
	finding = Diagnostic();
	TEST_EXPECT(open_local_settings(paths, install, back, finding) &&
			back.game_install == install &&
			finding.code() == "local_settings.schema_version.unsupported");
	std::string text;
	TEST_EXPECT(opennova::io::read_file_text(paths.local_settings_file, text, io_error) &&
			text.find("\"schema_version\": 2") != std::string::npos &&
			text.find("\"game_install\": \"" + install + "\"") != std::string::npos);
	// The game install's key as S13 A4 named it, schema 2; the file an older editor wrote (schema
	// 1, the install under retail_root) set aside, the warning naming each thing it held, now
	// gone: with no seed (the command line) nothing writes over it, and with one a new file.
	LocalSettings named;
	named.game_install = install;
	TEST_EXPECT(save_local_settings(paths, named, error));
	TEST_EXPECT(opennova::io::read_file_text(paths.local_settings_file, text, io_error) &&
	            text.find("\"game_install\": \"" + install + "\"") != std::string::npos &&
	            text.find("\"schema_version\": 2") != std::string::npos && text.find("retail") == std::string::npos);
	const std::string older = "{\"retail_root\": \"" + install +
			"\", \"runtime_executable\": \"C:/games/opennova.exe\", \"schema_version\": 1}\n";
	TEST_EXPECT(editor_test::write_text(paths.local_settings_file, older));
	finding = Diagnostic();
	TEST_EXPECT(open_local_settings(paths, std::string(), back, finding) &&
			back.game_install.empty() && back.runtime_executable.empty());
	TEST_EXPECT(finding.severity == DiagnosticSeverity::Warning &&
			finding.code() == "local_settings.schema_version.unsupported" &&
			finding.message.find("retail_root \"" + install + "\"") != std::string::npos &&
			finding.message.find("runtime_executable \"C:/games/opennova.exe\"") !=
					std::string::npos);
	TEST_EXPECT(opennova::io::read_file_text(paths.local_settings_file, text, io_error) && text == older);
	finding = Diagnostic();
	TEST_EXPECT(open_local_settings(paths, install, back, finding) &&
			back.game_install == install && back.runtime_executable.empty() &&
			finding.code() == "local_settings.schema_version.unsupported");
	TEST_EXPECT(opennova::io::read_file_text(paths.local_settings_file, text, io_error) &&
			text.find("\"schema_version\": 2") != std::string::npos &&
			text.find("retail") == std::string::npos);
	return 0;
}

// Play's settings are the project's own, in its local.json (never the editor's): a project that never said
// plays in the OpenNova runtime and saves first; each mode written by its token and read back, saving first
// off too; a file without them (one an earlier editor wrote) reads as the defaults; a mode no PlayMode has
// reads as the runtime, never the game install; the tokens are the run directory's mode names.
static int test_local_play_settings() {
	editor_test::TempProjectDir dir("opennova_editor_local_play_settings");
	const std::string root = dir.file("P");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "P", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	LocalSettings local;
	TEST_EXPECT(load_local_settings(paths, local, error) && local.play_mode == PlayMode::Runtime && local.save_before_play);
	for (const PlayMode mode : kPlayModes) {
		local.play_mode = mode;
		local.save_before_play = mode != PlayMode::Strict;
		TEST_EXPECT(save_local_settings(paths, local, error));
		std::string text, io_error;
		TEST_EXPECT(opennova::io::read_file_text(paths.local_settings_file, text, io_error) &&
		            text.find(std::string("\"play_mode\": \"") + play_mode_token(mode) + "\"") != std::string::npos &&
		            text.find(std::string("\"save_before_play\": ") + (mode != PlayMode::Strict ? "true" : "false")) !=
		                    std::string::npos);
		LocalSettings back;
		TEST_EXPECT(load_local_settings(paths, back, error) && back.play_mode == mode &&
		            back.save_before_play == (mode != PlayMode::Strict));
	}
	TEST_EXPECT(editor_test::write_text(paths.local_settings_file, "{\"schema_version\": 2, \"game_install\": \"\"}\n"));
	TEST_EXPECT(load_local_settings(paths, local, error) && local.play_mode == PlayMode::Runtime && local.save_before_play);
	TEST_EXPECT(editor_test::write_text(paths.local_settings_file, "{\"schema_version\": 2, \"play_mode\": \"retail\"}\n"));
	TEST_EXPECT(load_local_settings(paths, local, error) && local.play_mode == PlayMode::Runtime);
	TEST_EXPECT(editor_test::write_text(paths.local_settings_file, "{\"schema_version\": 2, \"play_mode\": true}\n"));
	TEST_EXPECT(load_local_settings(paths, local, error) && local.play_mode == PlayMode::Runtime);
	PlayMode read = PlayMode::Strict;
	TEST_EXPECT(!play_mode_from_token("Strict", read) && read == PlayMode::Strict && play_mode_from_token("install", read) &&
	            read == PlayMode::Install);
	TEST_EXPECT(std::string(play_mode_token(PlayMode::Runtime)) == kRunModeRuntime &&
	            std::string(play_mode_token(PlayMode::Install)) == kRunModeInstall &&
	            std::string(play_mode_token(PlayMode::Strict)) == kRunModeStrict);
	TEST_EXPECT(!plays_in_install(PlayMode::Runtime) && plays_in_install(PlayMode::Install) && plays_in_install(PlayMode::Strict));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_create_then_open();
	failures += test_on_disk_form_is_deterministic();
	failures += test_refusals();
	failures += test_expansion_object();
	failures += test_export_dir_and_local_settings();
	failures += test_local_play_settings();
	if (failures == 0) std::printf("editor_project_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
