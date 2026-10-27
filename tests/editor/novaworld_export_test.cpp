// A project's export from its files alone (NovaWorld, ADR 0069): the server builds a
// release from a revision's files, which the store keeps without the machine's own folder
// (`.opennova/`: its game install, its caches, its import outputs) or the export folder, and with
// no game install beside them. A copy of a project holding only those files exports the very bytes
// the project does: one imported whole from a game install (a retail product's stock revision, a game
// you own: new --all --install), one of our own with an import source (its output made again from the
// source), and an expansion project over its base game's project in the folder beside it (the
// store's sibling layout: base_project "../Base"). Run on Linux too, whose file names are
// case-sensitive, as the data program's.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "cli_verbs.h"
#include "common/png_test_support.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/minted_install.h"

namespace fs = std::filesystem;

using opennova::project::run_project_command;

namespace {

// A run of the command: its exit code, what it printed and what it said on its error stream.
struct Ran {
	int code = -1;
	std::string out, err;
};

std::string read_back(std::FILE *file) {
	std::string text;
	std::rewind(file);
	char buffer[4096];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
		text.append(buffer, n);
	std::fclose(file);
	return text;
}

Ran run(const std::string &scratch, const std::vector<std::string> &args) {
	std::vector<const char *> argv;
	for (const std::string &a : args)
		argv.push_back(a.c_str());
	Ran ran;
	std::FILE *out = std::fopen((scratch + "/out.txt").c_str(), "w+b");
	std::FILE *err = std::fopen((scratch + "/err.txt").c_str(), "w+b");
	if (!out || !err)
		return ran;
	ran.code = run_project_command(static_cast<int>(argv.size()), argv.data(), out, err);
	ran.out = read_back(out);
	ran.err = read_back(err);
	if (!ran.err.empty())
		std::fputs(ran.err.c_str(), stderr);
	return ran;
}

// The project at `from` copied to `to` as NovaWorld keeps it: every file but the machine's
// own folder and the export folder (`build/export`, the default every project here keeps), each
// written afresh as a download writes it. False when a file is not copied.
bool store_copy(const std::string &from, const std::string &to) {
	const fs::path root = opennova::editor::system_path(from),
				   dest = opennova::editor::system_path(to);
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(root, ec);
			!ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		const std::string relative =
				opennova::io::utf8_generic_path(it->path().lexically_relative(root));
		if (relative == ".opennova" || relative == "build/export") {
			it.disable_recursion_pending();
			continue;
		}
		std::error_code kind;
		if (!it->is_regular_file(kind))
			continue;
		const fs::path target = dest / it->path().lexically_relative(root);
		std::error_code made;
		fs::create_directories(target.parent_path(), made);
		if (made || !fs::copy_file(it->path(), target, made)) {
			std::fprintf(stderr, "cannot copy %s: %s\n", relative.c_str(), made.message().c_str());
			return false;
		}
	}
	return !ec;
}

// A digest without its export record's line: the record's `sources` stamps the project's files on
// this disk (their sizes and times, which a copy does not share), and a release leaves the record out
// (release_build's --exclude export.json).
std::string without_record(const std::string &digest) {
	std::string out;
	size_t at = 0;
	while (at < digest.size()) {
		size_t end = digest.find('\n', at);
		end = end == std::string::npos ? digest.size() : end + 1;
		if (digest.compare(at, 12, "export.json ") != 0)
			out.append(digest, at, end - at);
		at = end;
	}
	return out;
}

// Two exports' digests the same but for their records; the difference said when not.
bool same_export(const std::string &original, const std::string &copied) {
	if (without_record(original) == without_record(copied))
		return true;
	std::fprintf(stderr, "the project exported:\n%.6000s\nits copy exported:\n%.6000s\n",
			original.c_str(), copied.c_str());
	return false;
}

} // namespace

// A project imported whole from a game install, as a retail product's stock revision is (a game you own:
// new --all, every file imported as it opens), and its copy, which names no game install: one export.
static int test_install_import() {
	test_temp::TempDir dir("opennova_content_export_install");
	const std::string scratch = dir.root(), install = dir.file("install"), root = dir.file("Stock"),
					  copy = dir.file("copy/Stock");
	std::string minted;
	TEST_EXPECT(minted_install::mint(install, minted));
	TEST_EXPECT(
			run(scratch, { "new", root, "--title", "Stock", "--game", "jo", "--all", "--install", install })
					.code == 0);
	TEST_EXPECT(run(scratch, { "export", root, "--out", dir.file("export") }).code == 0);
	TEST_EXPECT(store_copy(root, copy));
	TEST_EXPECT(run(scratch, { "export", copy, "--out", dir.file("copy-export") }).code == 0);
	TEST_EXPECT(!fs::exists(opennova::editor::system_path(copy + "/.opennova/local.json")));
	const std::string original = editor_test::tree_digest(dir.file("export"));
	TEST_EXPECT(original.find("resource.pff") != std::string::npos);
	TEST_EXPECT(same_export(original, editor_test::tree_digest(dir.file("copy-export"))));
	std::printf("install import: the copy exports the project's %zu bytes of digest\n",
			original.size());
	return 0;
}

// Our own project with an import source: the copy holds the source and its record, not the output
// the import made under `.opennova/`, which its export makes again.
static int test_import_source() {
	test_temp::TempDir dir("opennova_content_export_source");
	const std::string scratch = dir.root(), root = dir.file("Own"), copy = dir.file("copy/Own");
	TEST_EXPECT(run(scratch, { "new", root, "--title", "Own" }).code == 0);
	TEST_EXPECT(run(scratch, { "create-missing", root }).code == 0);
	TEST_EXPECT(test_io::write_file(dir.file("logo.png"), test_png::gradient_png(8, 8)));
	TEST_EXPECT(run(scratch, { "import", root, dir.file("logo.png") }).code == 0);
	TEST_EXPECT(run(scratch, { "export", root, "--out", dir.file("export") }).code == 0);
	TEST_EXPECT(store_copy(root, copy));
	TEST_EXPECT(run(scratch, { "export", copy, "--out", dir.file("copy-export") }).code == 0);
	const std::string original = editor_test::tree_digest(dir.file("export"));
	TEST_EXPECT(!original.empty());
	TEST_EXPECT(same_export(original, editor_test::tree_digest(dir.file("copy-export"))));
	std::printf("import source: the copy imports it again and exports the project's bytes\n");
	return 0;
}

// An expansion imported from the install's expansion over its base game's project (base_project
// "../Base"), as a retail expansion's stock revision is (a game you own: new --all --expansion, made before
// its base was exported: the session exports the base first, as the expansion opens, then imports the
// expansion's own files), and the copies of both side by side: one export. The copy of the base holds no
// export, so the expansion's exports its base first (the session's base export, session/base_export.h);
// exported again with the base unchanged, nothing of the base is exported.
static int test_expansion_over_base_project() {
	test_temp::TempDir dir("opennova_content_export_expansion");
	const std::string scratch = dir.root(), install = dir.file("install"),
					  base = dir.file("work/Base"), expansion = dir.file("work/Exp"),
					  base_copy = dir.file("copy/Base"), expansion_copy = dir.file("copy/Exp");
	std::string minted;
	TEST_EXPECT(minted_install::mint(install, minted));
	TEST_EXPECT(
			run(scratch, { "new", base, "--title", "Base", "--game", "jo", "--all", "--install", install })
					.code == 0);
	TEST_EXPECT(!fs::exists(opennova::editor::system_path(base + "/build/export/export.json")));
	TEST_EXPECT(run(scratch,
						{ "new", expansion, "--title", "Exp", "--game", "jo", "--expansion",
								minted_install::kExpansion, "--base-project", "../Base", "--all", "--install",
								install })
						.code == 0);
	TEST_EXPECT(fs::exists(opennova::editor::system_path(base + "/build/export/export.json")));
	TEST_EXPECT(run(scratch, { "export", expansion, "--out", dir.file("export") }).code == 0);
	TEST_EXPECT(store_copy(base, base_copy) && store_copy(expansion, expansion_copy));
	TEST_EXPECT(!fs::exists(opennova::editor::system_path(base_copy + "/build/export/export.json")));
	TEST_EXPECT(
			run(scratch, { "export", expansion_copy, "--out", dir.file("copy-export") }).code == 0);
	const std::string record = base_copy + "/build/export/export.json";
	std::string recorded, error;
	TEST_EXPECT(opennova::io::read_file_text(record, recorded, error) &&
			recorded.find("\"sources\"") != std::string::npos);
	TEST_EXPECT(
			run(scratch, { "export", expansion_copy, "--out", dir.file("copy-export") }).code == 0);
	std::string again;
	TEST_EXPECT(opennova::io::read_file_text(record, again, error) && again == recorded);
	const std::string original = editor_test::tree_digest(dir.file("export"));
	TEST_EXPECT(original.find(std::string("expansion/") + minted_install::kExpansion) !=
			std::string::npos);
	TEST_EXPECT(same_export(original, editor_test::tree_digest(dir.file("copy-export"))));
	std::printf("expansion: the copies beside each other export the project's bytes\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_install_import();
	failures += test_import_source();
	failures += test_expansion_over_base_project();
	if (failures == 0)
		std::printf("content_export: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
