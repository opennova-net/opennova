// Pins the project file (ADR 0046 d6): creation, the deterministic on-disk form, the
// round trip, and the refusals (unknown schema version, unknown game, missing file).
#include <cstdio>
#include <filesystem>
#include <string>

#include <base/io/json.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

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
	TEST_EXPECT(read_file_text((fs::path(paths.cache_dir) / ".gitignore").generic_string(), ignore, io_error));
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
	TEST_EXPECT(error.code == "project.exists");
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
	        "  \"schema_version\": 1,\n"
	        "  \"target_game\": \"jo\",\n"
	        "  \"title\": \"T\"\n"
	        "}\n";
	TEST_EXPECT(text == expected);
	return 0;
}

static int test_refusals() {
	editor_test::TempProjectDir dir("opennova_editor_project_document_refusals_test");
	Diagnostic error;
	ProjectDocument doc;

	TEST_EXPECT(!open_project(dir.file("nowhere"), doc, error));
	TEST_EXPECT(error.code == "project.file.missing");

	const std::string root = dir.file("bad");
	const ProjectPaths paths = ProjectPaths::for_root(root);
	std::error_code ec;
	fs::create_directories(root, ec);
	TEST_EXPECT(editor_test::write_text(paths.project_file,
	                                    "{\"schema_version\": 2, \"project_id\": \"x\", \"title\": \"t\"}\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code == "project.schema_version.unsupported");

	TEST_EXPECT(editor_test::write_text(
	        paths.project_file,
	        "{\"schema_version\": 1, \"project_id\": \"x\", \"title\": \"t\", \"target_game\": \"quake\"}\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code == "project.target_game.unknown");

	TEST_EXPECT(editor_test::write_text(paths.project_file, "{\"schema_version\": 1,\n"));
	TEST_EXPECT(!open_project(root, doc, error));
	TEST_EXPECT(error.code == "project.json");

	TEST_EXPECT(!create_project(dir.file("wrong-game"), "t", "quake", doc, error));
	TEST_EXPECT(error.code == "project.target_game.unknown");
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
	TEST_EXPECT(local.runtime_executable.empty() && local.retail_root.empty());
	local.runtime_executable = "C:/games/opennova.exe";
	// Saved into a project whose .opennova/ is gone: made again, ignored, before the file.
	std::error_code ec;
	fs::remove_all(paths.cache_dir, ec);
	TEST_EXPECT(!fs::exists(paths.cache_dir));
	TEST_EXPECT(save_local_settings(paths, local, error));
	std::string ignore, io_error;
	TEST_EXPECT(read_file_text((fs::path(paths.cache_dir) / ".gitignore").generic_string(), ignore, io_error) &&
	            ignore == "*\n");
	LocalSettings back;
	TEST_EXPECT(load_local_settings(paths, back, error));
	TEST_EXPECT(back.runtime_executable == "C:/games/opennova.exe");
	// Opened with a seed: a project naming no install takes it (written), one naming its own
	// keeps it; no seed (the command line) writes nothing. The seed is a path this platform
	// calls absolute ("C:/..." is relative on Linux), so it is kept as it is.
	const std::string install = dir.file("Joint Ops");
	TEST_EXPECT(open_local_settings(paths, std::string(), back, error) && back.retail_root.empty());
	TEST_EXPECT(open_local_settings(paths, install, back, error) && back.retail_root == install);
	TEST_EXPECT(load_local_settings(paths, back, error) && back.retail_root == install &&
	            back.runtime_executable == "C:/games/opennova.exe");
	TEST_EXPECT(open_local_settings(paths, dir.file("Other"), back, error) && back.retail_root == install);
	// The install kept absolute: a relative seed is taken from the working directory.
	TEST_EXPECT(absolute_install_path("").empty());
	TEST_EXPECT(absolute_install_path("games/../Joint Ops") ==
	            (fs::current_path() / "Joint Ops").lexically_normal().generic_string());
	LocalSettings bare;
	TEST_EXPECT(save_local_settings(paths, bare, error));
	TEST_EXPECT(open_local_settings(paths, "Joint Ops", back, error) &&
	            back.retail_root == (fs::current_path() / "Joint Ops").lexically_normal().generic_string());
	TEST_EXPECT(editor_test::write_text(paths.local_settings_file, "{\"schema_version\": 9}\n"));
	TEST_EXPECT(!load_local_settings(paths, back, error));
	TEST_EXPECT(error.code == "local_settings.schema_version.unsupported");
	TEST_EXPECT(!open_local_settings(paths, install, back, error) && back.retail_root.empty());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_create_then_open();
	failures += test_on_disk_form_is_deterministic();
	failures += test_refusals();
	failures += test_export_dir_and_local_settings();
	if (failures == 0) std::printf("editor_project_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
