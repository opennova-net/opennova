// opennova-project validate over the deepest menus the game reads (formats/mnu/mnu.cpp: windows nest as
// deep as the game's 1 MiB creation stack takes them): 2017 nested generic windows and 2015 LAN_LISTs
// nested through their JOIN_BUTTONs, 4031 windows deep. The editor core reads, validates and indexes a
// menu without a call a window level (its walks keep explicit stacks), on this test's own main-thread
// stack, and in time linear in the windows: a record's place, its owners' context and its values are
// read once, not walked down from the screen at every field. What it reports is what it did before:
// every window named W after the first is a duplicate_window warning.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/project/project_document.h>
#include <editor/model/diagnostic.h>
#include "cli_verbs.h"
#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

using opennova::project::run_project_command;

namespace {

// A validate run's exit code, its output kept in `text`, and its time in seconds.
int validate(const std::string &root, const std::string &capture, std::string &text, double &seconds) {
	const char *argv[] = {"validate", root.c_str()};
	std::FILE *out = std::fopen(capture.c_str(), "w+b");
	if (!out) return -1;
	const auto start = std::chrono::steady_clock::now();
	const int code = run_project_command(2, argv, out, stderr);
	seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::rewind(out);
	text.clear();
	char buffer[65536];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), out)) > 0;) text.append(buffer, n);
	std::fclose(out);
	return code;
}

size_t count_of(const std::string &text, const std::string &word) {
	size_t found = 0;
	for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + word.size())) ++found;
	return found;
}

// `levels` nested generic windows, each named W, the deepest DEEP (mnu_unit's builder).
std::string generic_chain(int levels) {
	std::string xml = "<SCREEN><NAME>S</NAME>";
	for (int i = 0; i < levels; ++i)
		xml += std::string("<WINDOW TYPE=\"X\" NAME=\"") + (i + 1 == levels ? "DEEP" : "W") +
		       "\"><POSITION><LEFT>1</LEFT></POSITION>";
	for (int i = 0; i < levels; ++i) xml += "</WINDOW>";
	return xml + "</SCREEN>";
}

// `levels` LAN_LISTs named L, each in the JOIN_BUTTON of the one above, a STATIC DEEP in the last.
std::string lan_chain(int levels) {
	std::string xml = "<SCREEN><NAME>S</NAME>";
	for (int i = 0; i < levels; ++i) xml += "<WINDOW TYPE=\"LAN_LIST\" NAME=\"L\"><JOIN_BUTTON>";
	xml += "<WINDOW TYPE=\"STATIC\" NAME=\"DEEP\"><STRING>t</STRING></WINDOW>";
	for (int i = 0; i < levels; ++i) xml += "</JOIN_BUTTON></WINDOW>";
	return xml + "</SCREEN>";
}

int test_deep_menu(const char *name, const std::string &menu, size_t duplicates) {
	test_temp::TempDir dir(name);
	const std::string root = dir.file("project");
	opennova::editor::ProjectDocument project;
	opennova::editor::Diagnostic error;
	TEST_EXPECT(opennova::editor::create_project(root, "Deep", "jo", project, error));
	TEST_EXPECT(test_io::write_text(root + "/deep.mnu", menu));
	std::string text;
	double seconds = 0;
	const int code = validate(root, dir.file("validate.txt"), text, seconds);
	// The project has none of the files the game needs to start, which block a build: exit 1.
	TEST_EXPECT(code == 1 && text.find("not ok:") != std::string::npos);
	TEST_EXPECT(count_of(text, "warning menu.duplicate_window:") == duplicates);
	std::printf("%s: validate in %.1f s, %zu duplicate windows\n", name, seconds, duplicates);
#ifdef NDEBUG
	// Linear in the windows: well inside half a minute (it took 382 s and 46 minutes when a record was
	// walked down to at every field).
	TEST_EXPECT(seconds < 30.0);
#endif
	return 0;
}

} // namespace

int main() {
	// 2017 nested windows, all but the deepest named W: the first W is the one the lookup finds.
	if (test_deep_menu("opennova_menu_depth_generic", generic_chain(2017), 2015) != 0) return 1;
	// 2015 LAN_LISTs named L through their JOIN_BUTTONs (a part is never found by its own NAME).
	if (test_deep_menu("opennova_menu_depth_lan", lan_chain(2015), 2014) != 0) return 1;
	return 0;
}
