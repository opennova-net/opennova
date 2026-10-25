// A project folder as a working copy of its content project (engine/editor/content_store, ADR
// 0069): the files no store keeps (a machine's beside a project's); a folder scanned as the store
// keeps it (the project file in, the machine's own folder, the export folder, other dot-folders and
// a Blender backup out, each file's SHA-256 the bytes'), stepped at the smallest budget the same as
// in one call, and a warm scan taking a settled file's SHA-256 from the cache by its size and last
// write alone; the changes since the base; a pull planned over the base, the folder and the
// revision for every case (only theirs changed, only mine, both alike, both otherwise, a delete
// against a change either way, a file the scan could not read) and each conflict settled either
// way; and a pull applied into the folder (the revision's files in place, the replaced and removed
// ones in the trash, an emptied folder gone, the base and binding written), refused while a
// conflict stands, a download is missing or a file changed since the plan, and finished by a pull
// planned again after one cut short.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/sha256.h>
#include <editor/content_store/content_apply.h>
#include <editor/content_store/content_plan.h>
#include <editor/content_store/content_scan.h>
#include <editor/content_store/content_state.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project/project_trash.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

namespace fs = std::filesystem;
namespace editor = opennova::editor;
using opennova::content::ContentFile;

namespace {

std::string sha_of(const std::string &bytes) {
	return opennova::io::sha256_hex(bytes.data(), bytes.size());
}

ContentFile file(const std::string &path, const std::string &bytes) {
	return { path, sha_of(bytes), bytes.size() };
}

std::string read(const std::string &path) {
	std::string text, error;
	return opennova::io::read_file_text(path, text, error) ? text : std::string("<missing>");
}

bool exists(const std::string &path) {
	std::error_code ec;
	return fs::exists(editor::system_path(path), ec);
}

std::vector<std::string> paths_of(const std::vector<ContentFile> &files) {
	std::vector<std::string> out;
	for (const ContentFile &f : files)
		out.push_back(f.path);
	return out;
}

// A small project folder: its project file, its own files, and what a machine keeps beside them.
bool make_folder(const std::string &root) {
	return test_io::write_text(root + "/project.opennova", "{}\n") &&
			test_io::write_text(root + "/defs/weapon.def", "weapon\n") &&
			test_io::write_text(root + "/art/gun.blend", "blend scene") &&
			test_io::write_text(root + "/art/gun.blend1", "blender's backup") &&
			test_io::write_text(root + "/art/Thumbs.db", "windows' thumbnails") &&
			test_io::write_text(root + "/.opennova/local.json", "{\"game_install\": \"C:/JO\"}") &&
			test_io::write_text(root + "/.git/config", "[core]") &&
			test_io::write_text(root + "/build/export/resource.pff", "an export") &&
			test_io::write_text(root + "/build/export.tmp/resource.pff", "an export cut short") &&
			test_io::write_text(root + "/build/notes.txt", "a file of the project's under build/");
}

} // namespace

static int test_skipped_files() {
	TEST_EXPECT(editor::content_skips_file("Thumbs.db") &&
			editor::content_skips_file("DESKTOP.INI") && editor::content_skips_file(".DS_Store"));
	TEST_EXPECT(editor::content_skips_file("gun.blend1") &&
			editor::content_skips_file("gun.blend12") &&
			editor::content_skips_file("weapon.def.tmp"));
	TEST_EXPECT(!editor::content_skips_file("gun.blend") &&
			!editor::content_skips_file("gun.blendx") &&
			!editor::content_skips_file("thumbs.dbx") && !editor::content_skips_file("weapon.def"));
	std::printf("skipped: a machine's files beside a project's, Blender's backups, an atomic "
				"save's leftover\n");
	return 0;
}

static int test_scan() {
	test_temp::TempDir dir("opennova_content_scan");
	const std::string root = dir.file("Project");
	TEST_EXPECT(make_folder(root));
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const std::string export_dir = root + "/build/export";

	editor::ContentScan whole = editor::scan_content(paths, export_dir);
	TEST_EXPECT(whole.done() && whole.problems().empty());
	const std::vector<std::string> expected = { "art/gun.blend", "build/notes.txt",
		"defs/weapon.def", "project.opennova" };
	TEST_EXPECT(paths_of(whole.files()) == expected);
	TEST_EXPECT(whole.files()[2].sha256 == sha_of("weapon\n") && whole.files()[2].size == 7);

	// Stepped at the smallest budget: the same files.
	editor::ContentScan stepped(paths, export_dir);
	int steps = 0;
	while (!stepped.step(1))
		++steps;
	TEST_EXPECT(steps > 4 && stepped.files().size() == whole.files().size());
	for (size_t i = 0; i < whole.files().size(); ++i)
		TEST_EXPECT(stepped.files()[i].path == whole.files()[i].path &&
				stepped.files()[i].sha256 == whole.files()[i].sha256);

	// A file written a while ago is kept in the cache by its size and last write: rewritten with
	// both kept, a warm scan takes its SHA-256 from the cache unread (the cache's whole point, and
	// the one rewrite it cannot see, as git's).
	TEST_EXPECT(editor_test::backdate_tree(root, std::chrono::seconds(60)));
	TEST_EXPECT(editor::scan_content(paths, export_dir).problems().empty()); // the cache written
	const fs::path weapon = editor::system_path(root + "/defs/weapon.def");
	const auto stamp = fs::last_write_time(weapon);
	TEST_EXPECT(test_io::write_text(root + "/defs/weapon.def", "WEAPON\n"));
	fs::last_write_time(weapon, stamp);
	const editor::ContentScan warm = editor::scan_content(paths, export_dir);
	TEST_EXPECT(warm.files()[2].sha256 == sha_of("weapon\n"));
	// Its last write moved: read again.
	TEST_EXPECT(editor_test::backdate(root + "/defs/weapon.def", std::chrono::seconds(30)));
	TEST_EXPECT(editor::scan_content(paths, export_dir).files()[2].sha256 == sha_of("WEAPON\n"));

	// Two paths one file on Windows and macOS, where the file system holds both: the second is kept
	// out.
	TEST_EXPECT(test_io::write_text(root + "/defs/Weapon.def", "other"));
	std::error_code ec;
	size_t named = 0;
	for (const auto &entry : fs::directory_iterator(editor::system_path(root + "/defs"), ec)) {
		(void)entry;
		++named;
	}
	if (named == 2) {
		// The paths' bytes order them: "Weapon.def" (W, 0x57) before "weapon.def", which is kept
		// out.
		const editor::ContentScan folded = editor::scan_content(paths, export_dir);
		const std::vector<std::string> kept = { "art/gun.blend", "build/notes.txt",
			"defs/Weapon.def", "project.opennova" };
		TEST_EXPECT(folded.problems().size() == 1 &&
				folded.unread() == std::vector<std::string>{ "defs/weapon.def" } &&
				paths_of(folded.files()) == kept);
		std::printf("scan: two paths one file once folded kept out\n");
	}
	std::printf("scan: the project's files alone, stepped as in one call, the cache by size and "
				"last write\n");
	return 0;
}

static int test_plan() {
	// The base, the folder and the revision, a path for each case.
	const std::vector<ContentFile> base = { file("a", "1"), file("b", "1"), file("c", "1"),
		file("d", "1"), file("e", "1"), file("h", "1"), file("i", "1"), file("j", "1") };
	const std::vector<ContentFile> local = {
		file("a", "1"), // unchanged; the revision changed it: taken
		file("b", "2"), // only mine changed: kept
		file("c", "1"), // the revision deleted it: removed
						// d: only mine deleted it: kept
		file("e", "3"), // both changed, otherwise: a conflict
		file("f", "1"), // both added alike: nothing
						// h: mine deleted, the revision changed: a conflict
		file("i", "2"), // mine changed, the revision deleted: a conflict
						// j: unread
	};
	const std::vector<ContentFile> theirs = { file("a", "5"), file("b", "1"), file("d", "1"),
		file("e", "4"), file("f", "1"), file("g", "1"), file("h", "2"), file("j", "2") };
	const std::vector<std::string> unread = { "j" };

	const std::vector<editor::LocalChange> changes = editor::local_changes(base, local, unread);
	TEST_EXPECT(changes.size() == 6);
	const auto change = [&](const std::string &path) {
		for (const editor::LocalChange &c : changes)
			if (c.path == path)
				return std::string(editor::content_change_name(c.change));
		return std::string("none");
	};
	TEST_EXPECT(change("b") == "modified" && change("d") == "deleted" &&
			change("e") == "modified" && change("f") == "added" && change("h") == "deleted" &&
			change("i") == "modified" && change("a") == "none" && change("j") == "none");

	editor::PullPlan plan = editor::plan_pull(base, local, unread, theirs);
	const auto step = [&](const std::string &path) {
		const editor::PullItem *item = plan.find(path);
		if (!item)
			return std::string("none");
		switch (item->step) {
			case editor::PullStep::Write:
				return std::string("write");
			case editor::PullStep::Remove:
				return std::string("remove");
			case editor::PullStep::Conflict:
				return std::string("conflict");
		}
		return std::string("?");
	};
	TEST_EXPECT(step("a") == "write" && step("b") == "none" && step("c") == "remove" &&
			step("d") == "none" && step("e") == "conflict" && step("f") == "none" &&
			step("g") == "write" && step("h") == "conflict" && step("i") == "conflict" &&
			step("j") == "conflict");
	TEST_EXPECT(plan.conflicts() == 4 && plan.find("j")->unread && !plan.find("j")->mine);
	TEST_EXPECT(plan.find("h")->mine_change == editor::ContentChange::Deleted &&
			plan.find("h")->theirs_change == editor::ContentChange::Modified);
	TEST_EXPECT(plan.find("i")->theirs_change == editor::ContentChange::Deleted);
	TEST_EXPECT(plan.downloads().size() == 2); // a's and g's: "5" and "1"

	// Settled: e kept mine (out of the plan), h and i theirs (a write and a remove), j theirs.
	TEST_EXPECT(
			editor::resolve_conflict(plan, "e", editor::Resolution::KeepMine) && !plan.find("e"));
	TEST_EXPECT(editor::resolve_conflict(plan, "h", editor::Resolution::TakeTheirs) &&
			step("h") == "write");
	TEST_EXPECT(editor::resolve_conflict(plan, "i", editor::Resolution::TakeTheirs) &&
			step("i") == "remove");
	TEST_EXPECT(editor::resolve_conflict(plan, "j", editor::Resolution::TakeTheirs) &&
			step("j") == "write");
	TEST_EXPECT(!editor::resolve_conflict(
			plan, "a", editor::Resolution::KeepMine)); // no conflict there
	TEST_EXPECT(plan.conflicts() == 0);
	// A revision whose files are the folder's: nothing to do.
	TEST_EXPECT(editor::plan_pull(base, local, {}, local).items.empty());
	std::printf("plan: every case of the three, each conflict settled either way\n");
	return 0;
}

static int test_apply() {
	test_temp::TempDir dir("opennova_content_apply");
	const std::string root = dir.file("Project");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const editor::ContentPaths content_paths = editor::ContentPaths::for_project(paths);
	std::string error;

	// The folder at its base, revision 3: one file it changed since.
	const std::vector<ContentFile> base = { file("defs/a.def", "a1"), file("defs/b.def", "b1"),
		file("old/gone.txt", "gone"), file("project.opennova", "{}") };
	TEST_EXPECT(test_io::write_text(root + "/defs/a.def", "a1") &&
			test_io::write_text(root + "/defs/b.def", "b2") &&
			test_io::write_text(root + "/old/gone.txt", "gone") &&
			test_io::write_text(root + "/project.opennova", "{}"));
	TEST_EXPECT(editor::write_content_base(content_paths, base, error));
	TEST_EXPECT(editor::write_content_binding(
			content_paths, { "https://example.test", "JO", 3 }, error));

	// Revision 4: a.def changed, gone.txt deleted, a new file at two paths.
	const std::vector<ContentFile> theirs = { file("defs/a.def", "a2"), file("defs/b.def", "b1"),
		file("missions/m.bms", "same"), file("missions/n.bms", "same"),
		file("project.opennova", "{}") };
	editor::ContentScan scan = editor::scan_content(paths, "");
	std::vector<ContentFile> read_base;
	TEST_EXPECT(editor::read_content_base(content_paths, read_base, error) &&
			read_base.size() == base.size());
	editor::PullPlan plan = editor::plan_pull(read_base, scan.files(), scan.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 4 &&
			plan.downloads().size() == 2); // b.def kept: only mine changed it

	// Refused before anything moves: a download missing.
	editor::ApplyResult result;
	const editor::ContentBinding r4{ "https://example.test", "JO", 4 };
	TEST_EXPECT(!editor::apply_pull(paths, plan, theirs, r4, result, error) &&
			error.find("download") != std::string::npos);
	for (const ContentFile &download : plan.downloads())
		TEST_EXPECT(test_io::write_text(editor::incoming_path(content_paths, download.sha256),
				download.sha256 == sha_of("a2") ? "a2" : "same"));
	// A file changed since the plan: refused, nothing moved.
	TEST_EXPECT(test_io::write_text(root + "/old/gone.txt", "touched"));
	TEST_EXPECT(!editor::apply_pull(paths, plan, theirs, r4, result, error) &&
			result.changed_since == std::vector<std::string>{ "old/gone.txt" } &&
			read(root + "/defs/a.def") == "a1");
	TEST_EXPECT(test_io::write_text(root + "/old/gone.txt", "gone"));

	// Applied.
	TEST_EXPECT(editor::apply_pull(paths, plan, theirs, r4, result, error));
	TEST_EXPECT(result.written == 3 && result.removed == 1 && result.trash.id != 0);
	TEST_EXPECT(read(root + "/defs/a.def") == "a2" && read(root + "/defs/b.def") == "b2" &&
			read(root + "/missions/m.bms") == "same" && read(root + "/missions/n.bms") == "same");
	TEST_EXPECT(
			!exists(root + "/old/gone.txt") && !exists(root + "/old")); // the emptied folder gone
	TEST_EXPECT(!exists(content_paths.incoming_dir));
	// The replaced and removed files in the trash, put back by restoring it.
	TEST_EXPECT(result.trash.paths.size() == 2);
	std::vector<ContentFile> new_base;
	editor::ContentBinding binding;
	TEST_EXPECT(editor::read_content_base(content_paths, new_base, error) &&
			new_base.size() == theirs.size());
	TEST_EXPECT(editor::read_content_binding(content_paths, binding, error) &&
			binding.revision == 4 && binding.key == "JO");
	// The folder's own change kept, and the revision's files no change.
	const editor::ContentScan after = editor::scan_content(paths, "");
	const auto changes = editor::local_changes(new_base, after.files(), after.unread());
	TEST_EXPECT(changes.size() == 1 && changes[0].path == "defs/b.def");
	TEST_EXPECT(editor::plan_pull(new_base, after.files(), after.unread(), theirs).items.empty());

	// A conflict standing: refused.
	editor::PullPlan conflicted;
	editor::PullItem item;
	item.path = "defs/b.def";
	item.step = editor::PullStep::Conflict;
	conflicted.items.push_back(item);
	TEST_EXPECT(!editor::apply_pull(paths, conflicted, theirs, r4, result, error) &&
			error.find("conflict") != std::string::npos);
	std::printf("apply: the revision in place, the replaced in the trash, refused before anything "
				"moves\n");
	return 0;
}

static int test_resumed_pull() {
	test_temp::TempDir dir("opennova_content_resume");
	const std::string root = dir.file("Project");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const editor::ContentPaths content_paths = editor::ContentPaths::for_project(paths);
	std::string error;
	// A first pull into an empty folder (base none) cut short after one of its files was put in
	// place.
	const std::vector<ContentFile> theirs = { file("a.def", "a"), file("b.def", "b"),
		file("c.def", "c") };
	TEST_EXPECT(test_io::write_text(root + "/a.def", "a"));
	const editor::ContentScan scan = editor::scan_content(paths, "");
	editor::PullPlan plan = editor::plan_pull({}, scan.files(), scan.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 2 && !plan.find("a.def")); // the one in place needs nothing
	for (const ContentFile &download : plan.downloads())
		TEST_EXPECT(test_io::write_text(editor::incoming_path(content_paths, download.sha256),
				download.sha256 == sha_of("b") ? "b" : "c"));
	editor::ApplyResult result;
	TEST_EXPECT(editor::apply_pull(
			paths, plan, theirs, { "https://example.test", "JOX01", 1 }, result, error));
	TEST_EXPECT(
			read(root + "/b.def") == "b" && read(root + "/c.def") == "c" && result.trash.id == 0);
	std::printf("resume: a pull cut short finished by the next plan\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_skipped_files();
	failures += test_scan();
	failures += test_plan();
	failures += test_apply();
	failures += test_resumed_pull();
	if (failures == 0)
		std::printf("content_store: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
