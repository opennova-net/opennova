// Drives the real opennova-project commands (ADR 0046 d4) end to end on a temporary
// project: new, status, validate, and their exit codes.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/project/project_document.h>

#include "commands.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using opennova::project_cli::run_project_command;

static int run(std::initializer_list<std::string> args) {
	std::vector<const char *> argv;
	for (const std::string &a : args) argv.push_back(a.c_str());
	return run_project_command(static_cast<int>(argv.size()), argv.data(), stdout, stderr);
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

	// Create all missing: the project then validates clean, and a second run is a no-op.
	TEST_EXPECT(run({"create-missing", root, "--role"}) == 2);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	TEST_EXPECT(run({"validate", root}) == 0);
	TEST_EXPECT(run({"create-missing", root}) == 0);
	TEST_EXPECT(run({"create-missing", root, "--role", "no_such_role"}) == 0); // nothing to do

	// Build: a directory the runtime boots, and the same content is the same build.
	TEST_EXPECT(run({"build", root, "--out"}) == 2);
	TEST_EXPECT(run({"build", root}) == 0);
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

int main() {
	int failures = 0;
	failures += test_usage_errors();
	failures += test_new_status_validate();
	if (failures == 0) std::printf("project_cli: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
