// The apply library (engine/formats/update/apply.h): a packed update's section installed over a
// product's tree as UPDATE.EXE's PACK installs the rows it fetched. The package
// opennova-update-tool builds from the synthetic trees (synthetic_trees.h) is applied to a copy
// of the baseline, which must then hold what the target tree holds: the loose rows written and
// stamped with their rows' times, the archive's entries replaced, added and removed with their
// stamps, the removed loose file gone, the unchanged and unshipped files untouched; then the
// expansion's section into its own directory, a whole archive removed, and what a bad package
// is refused for before anything is written.

#include "common/temp_dir.h"
#include "common/test_expect.h"
#include "update/synthetic_trees.h"

#include "package_builder.h"

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/os_path.h>
#include <formats/pff/pff.h>
#include <formats/update/apply.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/sfs.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;
using synthetic_trees::bytes;

namespace {

std::string read_text(const std::string &path) {
	std::vector<uint8_t> content;
	std::string error;
	if (!io::read_file_bytes(path, content, error))
		return "<missing>";
	return std::string(content.begin(), content.end());
}

// An archive's entries: name -> {content, timestamp}.
struct Entry {
	std::vector<uint8_t> content;
	uint32_t timestamp = 0;
};
std::map<std::string, Entry> archive_entries(const std::string &path) {
	std::map<std::string, Entry> out;
	pff::PffArchive archive{};
	if (pff::pff_open(&archive, path.c_str()) != 0)
		return out;
	for (uint32_t i = 0; i < archive.entry_count; ++i) {
		const pff::PffEntry &entry = archive.entries[i];
		Entry item;
		item.timestamp = entry.timestamp;
		item.content.resize(entry.size ? entry.size : 1);
		pff::pff_extract(&archive, &entry, item.content.data(), item.content.size());
		item.content.resize(entry.size);
		out[pff::normalized_logical_name(pff::pff_entry_stored_name(entry))] = std::move(item);
	}
	pff::pff_close(&archive);
	return out;
}

bool copy_tree(const std::string &from, const std::string &to) {
	std::error_code ec;
	fs::copy(io::os_path(from), io::os_path(to), fs::copy_options::recursive, ec);
	return !ec;
}

update::SfsPackage build(const test_temp::TempDir &dir, bool &ok) {
	update_tool::BuildSpec spec;
	spec.from = io::os_path(dir.file("from"));
	spec.to = io::os_path(dir.file("to"));
	spec.package_product = "Synthetic Product";
	spec.products.push_back({ "", "", "SYNPTR.CMD", "", "", "" });
	spec.products.push_back({ "expansion/jox01", "jox01", "SYNEPTR.CMD", "", "", "" });
	std::vector<update_tool::ProductReport> report;
	std::string error;
	update::SfsPackage package;
	ok = update_tool::build_update_package(spec, package, report, error);
	if (!ok)
		std::fprintf(stderr, "update_apply: build failed: %s\n", error.c_str());
	return package;
}

bool has(const std::vector<std::string> &list, const std::string &name) {
	return std::find(list.begin(), list.end(), name) != list.end();
}

int test_apply(const test_temp::TempDir &dir) {
	bool ok = false;
	const update::SfsPackage package = build(dir, ok);
	TEST_EXPECT(ok && package.sections.size() == 2);
	if (!ok)
		return 1;
	const std::string tree = dir.file("tree");
	TEST_EXPECT(copy_tree(dir.file("from"), tree));

	// The base section over the copy of the baseline.
	update::ApplyReport report;
	std::string error;
	TEST_EXPECT(update::apply_section(
			package.sections[0], "SYNPTR.CMD", io::os_path(tree), report, error));
	if (!error.empty())
		std::fprintf(stderr, "update_apply: %s\n", error.c_str());
	// The loose rows: written with their content, stamped with the row's time; the dropped
	// file gone; the unchanged and the unshipped file as they were.
	TEST_EXPECT(read_text(tree + "/version.txt") == "Synthetic Product, V1.0.0.10; English");
	TEST_EXPECT(read_text(tree + "/new.txt") == "added");
	TEST_EXPECT(read_text(tree + "/same.txt") == "unchanged");
	TEST_EXPECT(read_text(tree + "/update.log") == "the updater's own file");
	TEST_EXPECT(read_text(tree + "/old.txt") == "<missing>");
	TEST_EXPECT(has(report.files_written, "version.txt") && has(report.files_written, "new.txt"));
	TEST_EXPECT(has(report.files_deleted, "old.txt"));
	TEST_EXPECT(has(report.archives_rewritten, "res.pff"));
	// A loose row's mtime is its row's time (the builder took `to`'s mtime for it).
	const int64_t to_mtime = io::file_modified_unix_seconds(io::os_path(dir.file("to/new.txt")));
	const int64_t tree_mtime = io::file_modified_unix_seconds(io::os_path(tree + "/new.txt"));
	TEST_EXPECT(to_mtime != 0 && tree_mtime >= to_mtime - 2 && tree_mtime <= to_mtime + 2);
	// The archive: the entry replaced with its new stamp, the added one, the dropped one gone,
	// the kept one as it was.
	const std::map<std::string, Entry> entries = archive_entries(tree + "/res.pff");
	const std::map<std::string, Entry> expected = archive_entries(dir.file("to/res.pff"));
	TEST_EXPECT(entries.size() == 3 && expected.size() == 3);
	for (const auto &[name, want] : expected) {
		const auto got = entries.find(name);
		TEST_EXPECT(got != entries.end());
		if (got == entries.end())
			continue;
		TEST_EXPECT(got->second.content == want.content);
		TEST_EXPECT(got->second.timestamp == want.timestamp);
	}
	TEST_EXPECT(entries.count("DROP.DEF") == 0 && entries.count("drop.def") == 0);
	// version.txt, new.txt, old.txt (REMOVE), TARGET, wpn.def, add.def, drop.def (REMOVE).
	TEST_EXPECT(report.rows_applied == 7);

	// The expansion's section over its own directory.
	update::ApplyReport exp_report;
	TEST_EXPECT(update::apply_section(package.sections[1], "SYNEPTR.CMD",
			io::os_path(tree + "/expansion/jox01"), exp_report, error));
	TEST_EXPECT(read_text(tree + "/expansion/jox01/version.txt") ==
			"Synthetic Expansion, V1.0.0.11; English");

	// Applied again, nothing changes: the archive is not rewritten (pff_apply_edits writes
	// nothing for an identical add), the files are written again as they are.
	update::ApplyReport again;
	TEST_EXPECT(update::apply_section(
			package.sections[0], "SYNPTR.CMD", io::os_path(tree), again, error));
	TEST_EXPECT(!has(again.archives_rewritten, "res.pff"));
	TEST_EXPECT(archive_entries(tree + "/res.pff").size() == 3);
	return 0;
}

int test_remove_archive(const test_temp::TempDir &dir) {
	// A REMOVE of a whole archive at the root (an archive `to` lacks, B-25): the file goes.
	TEST_EXPECT(synthetic_trees::make_archive(
			dir.file("from") + "/old.pff", { { "gone.def", bytes("x"), 1100000000 } }));
	bool ok = false;
	const update::SfsPackage package = build(dir, ok);
	TEST_EXPECT(ok);
	if (!ok)
		return 1;
	const std::string tree = dir.file("tree2");
	TEST_EXPECT(copy_tree(dir.file("from"), tree));
	update::ApplyReport report;
	std::string error;
	TEST_EXPECT(update::apply_section(
			package.sections[0], "SYNPTR.CMD", io::os_path(tree), report, error));
	std::error_code ec;
	TEST_EXPECT(!fs::exists(io::os_path(tree + "/old.pff"), ec));
	TEST_EXPECT(has(report.files_deleted, "old.pff"));
	fs::remove(io::os_path(dir.file("from") + "/old.pff"), ec);
	return 0;
}

int test_refusals(const test_temp::TempDir &dir) {
	bool ok = false;
	update::SfsPackage package = build(dir, ok);
	TEST_EXPECT(ok);
	if (!ok)
		return 1;
	const std::string tree = dir.file("tree3");
	TEST_EXPECT(copy_tree(dir.file("from"), tree));
	update::ApplyReport report;
	std::string error;

	// No command file of that name.
	TEST_EXPECT(!update::apply_section(
			package.sections[0], "NOPE.CMD", io::os_path(tree), report, error));
	TEST_EXPECT(error.find("no command file") != std::string::npos);

	// A row's blob missing: nothing written (version.txt stays the baseline's).
	update::SfsSection missing = package.sections[0];
	missing.entries.erase(std::remove_if(missing.entries.begin(), missing.entries.end(),
									[](const update::SfsEntry &e) { return e.name == "new.txt"; }),
			missing.entries.end());
	TEST_EXPECT(!update::apply_section(missing, "SYNPTR.CMD", io::os_path(tree), report, error));
	TEST_EXPECT(error.find("no file for the row new.txt") != std::string::npos);
	TEST_EXPECT(read_text(tree + "/version.txt") == "Synthetic Product, V1.0.0.0; English");

	// A blob whose bytes do not match the row's crc.
	update::SfsSection wrong = package.sections[0];
	for (update::SfsEntry &e : wrong.entries)
		if (e.name == "new.txt")
			e.data = update::nlz_encode(reinterpret_cast<const uint8_t *>("ADDED"), 5);
	TEST_EXPECT(!update::apply_section(wrong, "SYNPTR.CMD", io::os_path(tree), report, error));
	TEST_EXPECT(error.find("crc") != std::string::npos);

	// A row whose name would leave the tree.
	update::SfsSection unsafe = package.sections[0];
	std::string text(unsafe.entries[0].data.begin(), unsafe.entries[0].data.end());
	std::optional<update::CommandFile> cmd = update::read_command_file(text);
	TEST_EXPECT(cmd.has_value());
	if (!cmd)
		return 1;
	cmd->rows.push_back({ "REMOVE", update::CommandType::Remove, "..", 0, 0, std::nullopt });
	// write_command_file refuses a separator but not "..": the apply library must.
	const std::string unsafe_text = update::write_command_file(*cmd);
	TEST_EXPECT(!unsafe_text.empty());
	unsafe.entries[0].data.assign(unsafe_text.begin(), unsafe_text.end());
	TEST_EXPECT(!update::apply_section(unsafe, "SYNPTR.CMD", io::os_path(tree), report, error));
	TEST_EXPECT(error.find("does not name a file") != std::string::npos);
	TEST_EXPECT(!update::is_safe_row_name("a/b") && !update::is_safe_row_name("a\\b") &&
			!update::is_safe_row_name(".") && !update::is_safe_row_name("") &&
			update::is_safe_row_name("version.txt") && update::is_safe_row_name("jox01L.pff"));
	return 0;
}

} // namespace

int main() {
	test_temp::TempDir dir("update_apply");
	if (!synthetic_trees::make_trees(dir)) {
		std::fprintf(stderr, "update_apply: cannot write the synthetic trees\n");
		return 1;
	}
	if (int rc = test_apply(dir))
		return rc;
	if (int rc = test_remove_archive(dir))
		return rc;
	if (int rc = test_refusals(dir))
		return rc;
	std::printf("update_apply: ok\n");
	return 0;
}
