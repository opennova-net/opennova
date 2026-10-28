// A project folder as a working copy of its NovaWorld project (engine/editor/novaworld, ADR
// 0069): the files no store keeps (a machine's beside a project's); a folder scanned as the store
// keeps it (the project file in, the machine's own folder, the export folder, other dot-folders and
// a Blender backup out, each file's SHA-256 the bytes'), stepped at the smallest budget the same as
// in one call, and a warm scan taking a settled file's SHA-256 from the cache by its size and last
// write alone; the changes since the base; a pull planned over the base, the folder and the
// revision for every case (only theirs changed, only mine, both alike, both otherwise, a delete
// against a change either way, a file the scan could not read) and each conflict settled either
// way; and a pull applied into the folder (the revision's files in place, the replaced and removed
// ones in the trash, an emptied folder gone, the base and state written), refused while a
// conflict stands, a download is missing or a file changed since the plan, and finished by a pull
// planned again after one cut short; a change of a file's case, a folder replaced by a file and a
// file by a folder, each applied; a rename into place that does not go, the files it would
// replace back out of the trash; and the folder's state, each service's under its host, read back
// as written and refused where it is no state the editor writes.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <editor/novaworld/novaworld_apply.h>
#include <editor/novaworld/novaworld_plan.h>
#include <editor/novaworld/novaworld_scan.h>
#include <editor/novaworld/novaworld_state.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project/project_trash.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

namespace fs = std::filesystem;
namespace editor = opennova::editor;
using opennova::project::ProjectFile;

namespace {

constexpr const char *kServer = "https://example.test";

// `bytes` written at `path` as they are (test_io::write_text stages a .def's line ends as CRLF),
// its folders made.
bool put(const std::string &path, const std::string &bytes) {
	return test_io::write_file(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

std::string sha_of(const std::string &bytes) {
	return opennova::io::sha256_hex(bytes.data(), bytes.size());
}

ProjectFile file(const std::string &path, const std::string &bytes) {
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

std::vector<std::string> paths_of(const std::vector<ProjectFile> &files) {
	std::vector<std::string> out;
	for (const ProjectFile &f : files)
		out.push_back(f.path);
	return out;
}

// A small project folder: its project file, its own files, and what a machine keeps beside them.
bool make_folder(const std::string &root) {
	return put(root + "/project.opennova", "{}\n") && put(root + "/defs/weapon.def", "weapon\n") &&
			put(root + "/art/gun.blend", "blend scene") &&
			put(root + "/art/gun.blend1", "blender's backup") &&
			put(root + "/art/Thumbs.db", "windows' thumbnails") &&
			put(root + "/GT.SSC", "old lobby override") &&
			put(root + "/expansion/nifty/Gt.SsC", "old expansion override") &&
			put(root + "/.opennova/local.json", "{\"game_install\": \"C:/JO\"}") &&
			put(root + "/.git/config", "[core]") &&
			put(root + "/build/export/resource.pff", "an export") &&
			put(root + "/build/export.tmp/resource.pff", "an export cut short") &&
			put(root + "/build/notes.txt", "a file of the project's under build/");
}

} // namespace

static int test_skipped_files() {
	TEST_EXPECT(editor::novaworld_skips_file("GT.SSC") && editor::novaworld_skips_file("gt.ssc"));
	TEST_EXPECT(editor::novaworld_skips_file("Thumbs.db") &&
			editor::novaworld_skips_file("DESKTOP.INI") && editor::novaworld_skips_file(".DS_Store"));
	TEST_EXPECT(editor::novaworld_skips_file("gun.blend1") &&
			editor::novaworld_skips_file("gun.blend12") &&
			editor::novaworld_skips_file("weapon.def.tmp"));
	TEST_EXPECT(!editor::novaworld_skips_file("gun.blend") &&
			!editor::novaworld_skips_file("gun.blendx") &&
			!editor::novaworld_skips_file("thumbs.dbx") && !editor::novaworld_skips_file("weapon.def"));
	std::printf("skipped: a machine's files beside a project's, Blender's backups, an atomic "
				"save's leftover\n");
	return 0;
}

static int test_scan() {
	test_temp::TempDir dir("opennova_novaworld_scan");
	const std::string root = dir.file("Project");
	TEST_EXPECT(make_folder(root));
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const std::string export_dir = root + "/build/export";

	editor::NovaWorldScan whole = editor::novaworld_scan(paths, export_dir);
	TEST_EXPECT(whole.done() && whole.problems().empty());
	const std::vector<std::string> expected = { "art/gun.blend", "build/notes.txt",
		"defs/weapon.def", "project.opennova" };
	TEST_EXPECT(paths_of(whole.files()) == expected);
	TEST_EXPECT(whole.files()[2].sha256 == sha_of("weapon\n") && whole.files()[2].size == 7);

	// Stepped at the smallest budget: the same files.
	editor::NovaWorldScan stepped(paths, export_dir);
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
	TEST_EXPECT(editor::novaworld_scan(paths, export_dir).problems().empty()); // the cache written
	const fs::path weapon = editor::system_path(root + "/defs/weapon.def");
	const auto stamp = fs::last_write_time(weapon);
	TEST_EXPECT(put(root + "/defs/weapon.def", "WEAPON\n"));
	fs::last_write_time(weapon, stamp);
	const editor::NovaWorldScan warm = editor::novaworld_scan(paths, export_dir);
	TEST_EXPECT(warm.files()[2].sha256 == sha_of("weapon\n"));
	// Its last write moved: read again.
	TEST_EXPECT(editor_test::backdate(root + "/defs/weapon.def", std::chrono::seconds(30)));
	TEST_EXPECT(editor::novaworld_scan(paths, export_dir).files()[2].sha256 == sha_of("WEAPON\n"));

	// Two paths one file on Windows and macOS, where the file system holds both: the second is kept
	// out.
	TEST_EXPECT(put(root + "/defs/Weapon.def", "other"));
	std::error_code ec;
	size_t named = 0;
	for (const auto &entry : fs::directory_iterator(editor::system_path(root + "/defs"), ec)) {
		(void)entry;
		++named;
	}
	if (named == 2) {
		// The paths' bytes order them: "Weapon.def" (W, 0x57) before "weapon.def", which is kept
		// out.
		const editor::NovaWorldScan folded = editor::novaworld_scan(paths, export_dir);
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

// A folder the walk may list but not look into (read, no search permission): its entries' places
// and sizes cannot be told, so the scan says it is incomplete and lacks the file, and a status over
// it counts the file as no deletion (C-11). Where the system does not enforce the permission (a
// run as root, Windows' ACLs), there is nothing to see and the case says so.
static int test_scan_unlisted() {
#ifndef _WIN32
	test_temp::TempDir dir("opennova_novaworld_scan_unlisted");
	const std::string root = dir.file("Project");
	TEST_EXPECT(put(root + "/project.opennova", "{}\n") && put(root + "/defs/a.def", "a\n") &&
			put(root + "/locked/held.def", "held\n"));
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const fs::path locked = editor::system_path(root + "/locked");
	std::error_code ec;
	fs::permissions(locked, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
			fs::perm_options::replace, ec);
	TEST_EXPECT(!ec);
	std::error_code probe;
	fs::file_size(locked / "held.def", probe);
	if (probe) {
		const editor::NovaWorldScan scan = editor::novaworld_scan(paths, "");
		TEST_EXPECT(scan.done() && !scan.incomplete().empty() &&
				scan.incomplete().find("held.def") != std::string::npos);
		TEST_EXPECT(paths_of(scan.files()) ==
				(std::vector<std::string>{ "defs/a.def", "project.opennova" }));
		const std::vector<ProjectFile> base = { file("defs/a.def", "a\n"),
			file("locked/held.def", "held\n"), file("project.opennova", "{}\n") };
		TEST_EXPECT(editor::local_changes(base, scan.files(), scan.unread(), false).empty());
		std::printf("scan: a folder listed but not looked into, the scan incomplete\n");
	} else {
		std::printf("scan: the system enforces no search permission here: the unlisted case not run\n");
	}
	fs::permissions(locked, fs::perms::owner_all, fs::perm_options::add, ec);
#endif
	return 0;
}

static int test_plan() {
	// The base, the folder and the revision, a path for each case.
	const std::vector<ProjectFile> base = { file("a", "1"), file("b", "1"), file("c", "1"),
		file("d", "1"), file("e", "1"), file("h", "1"), file("i", "1"), file("j", "1") };
	const std::vector<ProjectFile> local = {
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
	const std::vector<ProjectFile> theirs = { file("a", "5"), file("b", "1"), file("d", "1"),
		file("e", "4"), file("f", "1"), file("g", "1"), file("h", "2"), file("j", "2") };
	const std::vector<std::string> unread = { "j" };

	const std::vector<editor::LocalChange> changes = editor::local_changes(base, local, unread);
	TEST_EXPECT(changes.size() == 6);
	const auto change = [&](const std::string &path) {
		for (const editor::LocalChange &c : changes)
			if (c.path == path)
				return std::string(editor::novaworld_change_name(c.change));
		return std::string("none");
	};
	TEST_EXPECT(change("b") == "modified" && change("d") == "deleted" &&
			change("e") == "modified" && change("f") == "added" && change("h") == "deleted" &&
			change("i") == "modified" && change("a") == "none" && change("j") == "none");
	// A folder not listed whole (NovaWorldScan::incomplete): the files it read are changes, none it
	// lacks is taken for deleted.
	const std::vector<editor::LocalChange> listed =
			editor::local_changes(base, local, unread, /*listed_whole=*/false);
	TEST_EXPECT(listed.size() == 4);
	for (const editor::LocalChange &c : listed)
		TEST_EXPECT(c.change != editor::NovaWorldChange::Deleted && c.path != "d" && c.path != "h");

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
	TEST_EXPECT(plan.find("h")->mine_change == editor::NovaWorldChange::Deleted &&
			plan.find("h")->theirs_change == editor::NovaWorldChange::Modified);
	TEST_EXPECT(plan.find("i")->theirs_change == editor::NovaWorldChange::Deleted);
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
	test_temp::TempDir dir("opennova_novaworld_apply");
	const std::string root = dir.file("Project");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const editor::NovaWorldPaths novaworld_paths =
			editor::NovaWorldPaths::for_project(paths, kServer);
	std::string error;

	// The folder at its base, revision 3: one file it changed since.
	const std::vector<ProjectFile> base = { file("defs/a.def", "a1"), file("defs/b.def", "b1"),
		file("old/gone.txt", "gone"), file("project.opennova", "{}") };
	TEST_EXPECT(put(root + "/defs/a.def", "a1") && put(root + "/defs/b.def", "b2") &&
			put(root + "/old/gone.txt", "gone") && put(root + "/project.opennova", "{}"));
	TEST_EXPECT(editor::write_novaworld_base(novaworld_paths, base, error));
	TEST_EXPECT(editor::write_novaworld_state(novaworld_paths, { "JO", 3 }, error));

	// Revision 4: a.def changed, gone.txt deleted, a new file at two paths.
	const std::vector<ProjectFile> theirs = { file("defs/a.def", "a2"), file("defs/b.def", "b1"),
		file("missions/m.bms", "same"), file("missions/n.bms", "same"),
		file("project.opennova", "{}") };
	editor::NovaWorldScan scan = editor::novaworld_scan(paths, "");
	std::vector<ProjectFile> read_base;
	TEST_EXPECT(editor::read_novaworld_base(novaworld_paths, read_base, error) &&
			read_base.size() == base.size());
	editor::PullPlan plan = editor::plan_pull(read_base, scan.files(), scan.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 4 &&
			plan.downloads().size() == 2); // b.def kept: only mine changed it

	// Refused before anything moves: a download missing.
	editor::ApplyResult result;
	const editor::NovaWorldWorkingCopy r4{ "JO", 4 };
	TEST_EXPECT(!editor::apply_pull(paths, novaworld_paths, plan, theirs, r4, result, error) &&
			error.find("download") != std::string::npos);
	for (const ProjectFile &download : plan.downloads())
		TEST_EXPECT(put(editor::incoming_path(novaworld_paths, download.sha256),
				download.sha256 == sha_of("a2") ? "a2" : "same"));
	// A file changed since the plan: refused, nothing moved.
	TEST_EXPECT(put(root + "/old/gone.txt", "touched"));
	TEST_EXPECT(!editor::apply_pull(paths, novaworld_paths, plan, theirs, r4, result, error) &&
			result.changed_since == std::vector<std::string>{ "old/gone.txt" } &&
			read(root + "/defs/a.def") == "a1");
	TEST_EXPECT(put(root + "/old/gone.txt", "gone"));

	// Applied.
	TEST_EXPECT(editor::apply_pull(paths, novaworld_paths, plan, theirs, r4, result, error));
	TEST_EXPECT(result.written == 3 && result.removed == 1 && result.trash.id != 0);
	TEST_EXPECT(read(root + "/defs/a.def") == "a2" && read(root + "/defs/b.def") == "b2" &&
			read(root + "/missions/m.bms") == "same" && read(root + "/missions/n.bms") == "same");
	TEST_EXPECT(
			!exists(root + "/old/gone.txt") && !exists(root + "/old")); // the emptied folder gone
	TEST_EXPECT(!exists(novaworld_paths.incoming_dir));
	// The replaced and removed files in the trash, put back by restoring it.
	TEST_EXPECT(result.trash.paths.size() == 2);
	std::vector<ProjectFile> new_base;
	editor::NovaWorldWorkingCopy copy;
	TEST_EXPECT(editor::read_novaworld_base(novaworld_paths, new_base, error) &&
			new_base.size() == theirs.size());
	TEST_EXPECT(editor::read_novaworld_state(novaworld_paths, copy, error) && copy.revision == 4 &&
			copy.key == "JO");
	// The folder's own change kept, and the revision's files no change.
	const editor::NovaWorldScan after = editor::novaworld_scan(paths, "");
	const auto changes = editor::local_changes(new_base, after.files(), after.unread());
	TEST_EXPECT(changes.size() == 1 && changes[0].path == "defs/b.def");
	TEST_EXPECT(editor::plan_pull(new_base, after.files(), after.unread(), theirs).items.empty());

	// A conflict standing: refused.
	editor::PullPlan conflicted;
	editor::PullItem item;
	item.path = "defs/b.def";
	item.step = editor::PullStep::Conflict;
	conflicted.items.push_back(item);
	TEST_EXPECT(
			!editor::apply_pull(paths, novaworld_paths, conflicted, theirs, r4, result, error) &&
			error.find("conflict") != std::string::npos);
	std::printf("apply: the revision in place, the replaced in the trash, refused before anything "
				"moves\n");
	return 0;
}

// A revision that changes a file's letters' case alone (one file on Windows and macOS, never a
// change since the plan there), replaces a folder by a file of its name and a file by a folder of
// its name: each applied, the folder then the revision's.
static int test_apply_renames() {
	test_temp::TempDir dir("opennova_novaworld_apply_renames");
	const std::string root = dir.file("Project");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const editor::NovaWorldPaths novaworld_paths =
			editor::NovaWorldPaths::for_project(paths, kServer);
	std::string error;
	const std::vector<ProjectFile> base = { file("bar", "bar"), file("defs/a.def", "a"),
		file("foo/x.txt", "x") };
	TEST_EXPECT(put(root + "/bar", "bar") && put(root + "/defs/a.def", "a") &&
			put(root + "/foo/x.txt", "x"));
	const std::vector<ProjectFile> theirs = { file("bar/y.txt", "y"), file("defs/A.def", "a"),
		file("foo", "foo") };
	const editor::NovaWorldScan scan = editor::novaworld_scan(paths, "");
	const editor::PullPlan plan = editor::plan_pull(base, scan.files(), scan.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 6 && plan.conflicts() == 0);
	for (const std::string &bytes : { std::string("y"), std::string("a"), std::string("foo") })
		TEST_EXPECT(put(editor::incoming_path(novaworld_paths, sha_of(bytes)), bytes));

	editor::ApplyResult result;
	TEST_EXPECT(
			editor::apply_pull(paths, novaworld_paths, plan, theirs, { "JO", 2 }, result, error));
	TEST_EXPECT(result.written == 3 && result.removed == 3 && result.trash.paths.size() == 3);
	TEST_EXPECT(read(root + "/bar/y.txt") == "y" && read(root + "/foo") == "foo" &&
			read(root + "/defs/A.def") == "a");
	const editor::NovaWorldScan after = editor::novaworld_scan(paths, "");
	TEST_EXPECT(paths_of(after.files()) ==
			(std::vector<std::string>{ "bar/y.txt", "defs/A.def", "foo" }));
	TEST_EXPECT(editor::plan_pull(theirs, after.files(), after.unread(), theirs).items.empty());
	std::printf("apply: a change of case, a folder replaced by a file and a file by a folder\n");
	return 0;
}

// A rename into place that does not go (a folder that is not empty where the revision puts a
// file): the file it and the writes after it would replace back out of the trash, the ones written
// before it kept, the base the old one; a pull planned again finishes once the folder is empty.
static int test_apply_put_back() {
	test_temp::TempDir dir("opennova_novaworld_apply_put_back");
	const std::string root = dir.file("Project");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const editor::NovaWorldPaths novaworld_paths =
			editor::NovaWorldPaths::for_project(paths, kServer);
	std::string error;
	const std::vector<ProjectFile> base = { file("a.txt", "a1"), file("z.txt", "z1") };
	TEST_EXPECT(put(root + "/a.txt", "a1") && put(root + "/z.txt", "z1") &&
			put(root + "/q/Thumbs.db", "windows' thumbnails"));
	TEST_EXPECT(editor::write_novaworld_base(novaworld_paths, base, error));
	const std::vector<ProjectFile> theirs = { file("a.txt", "a2"), file("q", "q"),
		file("z.txt", "z2") };
	for (const std::string &bytes : { std::string("a2"), std::string("q"), std::string("z2") })
		TEST_EXPECT(put(editor::incoming_path(novaworld_paths, sha_of(bytes)), bytes));
	const editor::NovaWorldWorkingCopy r2{ "JO", 2 };

	const editor::NovaWorldScan scan = editor::novaworld_scan(paths, "");
	editor::PullPlan plan = editor::plan_pull(base, scan.files(), scan.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 3);
	editor::ApplyResult result;
	TEST_EXPECT(!editor::apply_pull(paths, novaworld_paths, plan, theirs, r2, result, error));
	TEST_EXPECT(read(root + "/a.txt") == "a2" && read(root + "/z.txt") == "z1" &&
			result.trash.paths == std::vector<std::string>{ "a.txt" });
	TEST_EXPECT(!exists(novaworld_paths.incoming_dir + "/staged") &&
			!exists(novaworld_paths.state_file));
	std::vector<ProjectFile> kept;
	TEST_EXPECT(editor::read_novaworld_base(novaworld_paths, kept, error) && kept.size() == 2);

	// The folder emptied: planned again, the rest put in place, the empty folder giving way.
	std::error_code ec;
	fs::remove(editor::system_path(root + "/q/Thumbs.db"), ec);
	const editor::NovaWorldScan emptied = editor::novaworld_scan(paths, "");
	plan = editor::plan_pull(base, emptied.files(), emptied.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 2 && !plan.find("a.txt"));
	TEST_EXPECT(editor::apply_pull(paths, novaworld_paths, plan, theirs, r2, result, error));
	TEST_EXPECT(read(root + "/q") == "q" && read(root + "/z.txt") == "z2");
	std::printf("apply: a rename refused, the files it would replace back from the trash, then "
				"finished\n");
	return 0;
}

static int test_resumed_pull() {
	test_temp::TempDir dir("opennova_novaworld_resume");
	const std::string root = dir.file("Project");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(root);
	const editor::NovaWorldPaths novaworld_paths =
			editor::NovaWorldPaths::for_project(paths, kServer);
	std::string error;
	// A first pull into an empty folder (base none) cut short after one of its files was put in
	// place.
	const std::vector<ProjectFile> theirs = { file("a.def", "a"), file("b.def", "b"),
		file("c.def", "c") };
	TEST_EXPECT(put(root + "/a.def", "a"));
	const editor::NovaWorldScan scan = editor::novaworld_scan(paths, "");
	editor::PullPlan plan = editor::plan_pull({}, scan.files(), scan.unread(), theirs);
	TEST_EXPECT(plan.items.size() == 2 && !plan.find("a.def")); // the one in place needs nothing
	for (const ProjectFile &download : plan.downloads())
		TEST_EXPECT(put(editor::incoming_path(novaworld_paths, download.sha256),
				download.sha256 == sha_of("b") ? "b" : "c"));
	editor::ApplyResult result;
	TEST_EXPECT(editor::apply_pull(
			paths, novaworld_paths, plan, theirs, { "JOX01", 1 }, result, error));
	TEST_EXPECT(
			read(root + "/b.def") == "b" && read(root + "/c.def") == "c" && result.trash.id == 0);
	std::printf("resume: a pull cut short finished by the next plan\n");
	return 0;
}

// The folder's state on a service: under the service's host (its port too), each service's apart
// and the hash cache and the downloads every service's; read back as written, none where it has
// none, and refused where it is no state this editor writes.
static int test_state() {
	TEST_EXPECT(editor::novaworld_service_folder("https://opennova.net") == "opennova.net");
	TEST_EXPECT(editor::novaworld_service_folder("https://Staging.OpenNova.net/") ==
			"staging.opennova.net");
	TEST_EXPECT(editor::novaworld_service_folder("http://localhost:8080") == "localhost_8080");
	TEST_EXPECT(editor::novaworld_service_folder("") == "");
	test_temp::TempDir dir("opennova_novaworld_state");
	const editor::ProjectPaths paths = editor::ProjectPaths::for_root(dir.file("Project"));
	const editor::NovaWorldPaths live =
			editor::NovaWorldPaths::for_project(paths, "https://opennova.net");
	const editor::NovaWorldPaths staging =
			editor::NovaWorldPaths::for_project(paths, "https://staging.opennova.net");
	TEST_EXPECT(live.state_file == paths.cache_dir + "/novaworld/opennova.net/state.json" &&
			live.base_file == paths.cache_dir + "/novaworld/opennova.net/base.json" &&
			live.hashes_file == staging.hashes_file && live.incoming_dir == staging.incoming_dir);
	TEST_EXPECT(editor::NovaWorldPaths::for_project(paths, "").state_file.empty());
	editor::NovaWorldWorkingCopy copy;
	std::string error;
	TEST_EXPECT(!editor::read_novaworld_state(live, copy, error) && error.empty());
	TEST_EXPECT(editor::write_novaworld_state(live, { "ACIDGAMBIT", 41 }, error));
	TEST_EXPECT(editor::read_novaworld_state(live, copy, error) && copy.key == "ACIDGAMBIT" &&
			copy.revision == 41);
	TEST_EXPECT(!editor::read_novaworld_state(staging, copy, error) &&
			error.empty()); // another service's: none
	// Another schema, a key that is none, no revision, binding.json's old form, no JSON.
	const char *const refused_states[] = { "{\"schema\": 2, \"key\": \"JO\", \"revision\": 1}",
		"{\"schema\": 1, \"key\": \"jo\", \"revision\": 1}", "{\"schema\": 1, \"key\": \"JO\"}",
		"{\"schema_version\": 1, \"key\": \"JO\", \"revision\": 1}", "not json" };
	for (const char *refused : refused_states) {
		TEST_EXPECT(put(live.state_file, refused));
		TEST_EXPECT(!editor::read_novaworld_state(live, copy, error) && !error.empty() &&
				copy.key.empty());
	}
	std::printf("state: each service's under its host, read back as written, refused where it "
				"is no state\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_skipped_files();
	failures += test_scan();
	failures += test_scan_unlisted();
	failures += test_plan();
	failures += test_apply();
	failures += test_apply_renames();
	failures += test_apply_put_back();
	failures += test_resumed_pull();
	failures += test_state();
	if (failures == 0)
		std::printf("novaworld: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
