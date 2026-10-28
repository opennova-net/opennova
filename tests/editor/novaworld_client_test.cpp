// NovaWorld's client (engine/editor/novaworld/novaworld_jobs.h, novaworld_api.h, ADR 0069)
// over an in-memory service behind the transport seam, which keeps the routes' contract
// (apps/novaworld_web/products/project_routes.h: the head, the manifests, the files by SHA-256, the
// missing files with their uploads, the push on its parent), driving real folders: an admin's
// first push; a second folder getting the project, the same files; each pushing a change, the
// second told its base is stale, then pulling (the first's change taken, its own kept) and pushing
// both; a conflict settled by keeping one's own, pushed, and pulled by the other; a folder at its
// base with nothing to push; a player refused another's project, and pushing its own (made through
// POST /api/products, private to it and the admins); a renewed session kept by the account; an
// ended one signing it out; a folder kept before the store, whose files are not the head's, set up
// at no revision and pulled before it pushes, and one that is the head's set up at it; a
// store reset since the folder's revision, a check, a pull and a push each ending on it, moving
// nothing; and a folder the scan could not read whole, refused by a push and a pull, its check
// counting no deletion. The routes themselves answer the same client over HTTP in
// tests/novaworld/novaworld_client_routes_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/base64.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_api.h>
#include <editor/novaworld/novaworld_jobs.h>
#include <editor/novaworld/novaworld_state.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

#include "common/test_expect.h"
#include "editor/novaworld_fake_service.h"
#include "editor/editor_test_support.h"

namespace editor = opennova::editor;
namespace io = opennova::io;
using opennova::project::ProjectFile;
using namespace novaworld_fake;

namespace {

editor::NovaWorldAccount account(const char *cookie) {
	editor::NovaWorldAccount a;
	a.server = kServer;
	a.username = "taylor";
	a.role = "admin";
	a.cookie = cookie;
	return a;
}

// The folder's working copy on the service: its state, else the project `key` at no revision.
editor::NovaWorldWorkingCopy copy_of(const std::string &root, const char *key = "MOD") {
	editor::NovaWorldWorkingCopy copy;
	std::string error;
	if (!editor::read_novaworld_state(
				editor::NovaWorldPaths::for_project(editor::ProjectPaths::for_root(root), kServer),
				copy, error))
		copy = { key, 0 };
	return copy;
}

editor::NovaWorldJobState run(editor::NovaWorldJob &job) {
	editor::NovaWorldJobState state;
	int steps = 0;
	while ((state = job.step(1 << 16)) == editor::NovaWorldJobState::Running && ++steps < 100000) {
	}
	return state;
}

editor::PushOutcome push(FakeService &service, const std::string &root, const char *message,
		const char *cookie = kAdminCookie, std::string *error = nullptr, const char *key = "MOD") {
	editor::NovaWorldPush job(service, account(cookie), editor::ProjectPaths::for_root(root), "",
			copy_of(root, key), message);
	const editor::NovaWorldJobState state = run(job);
	if (state != editor::NovaWorldJobState::Done) {
		if (error)
			*error = job.error();
		else
			std::fprintf(stderr, "push from %s: %s\n", root.c_str(), job.error().c_str());
		return editor::PushOutcome::NothingToPush;
	}
	return job.outcome();
}

// The folder's own files and their bytes, the machine's folder aside.
std::map<std::string, std::string> files_of(const std::string &root) {
	std::map<std::string, std::string> out;
	const editor::NovaWorldScan scan = editor::novaworld_scan(editor::ProjectPaths::for_root(root), "");
	for (const ProjectFile &file : scan.files()) {
		std::string bytes, error;
		io::read_file_text(root + "/" + file.path, bytes, error);
		out[file.path] = bytes;
	}
	return out;
}

} // namespace

static int test_share_and_pull() {
	test_temp::TempDir dir("opennova_novaworld_client");
	const std::string first = dir.file("First"), second = dir.file("Second");
	FakeService service;

	// The first push of a folder.
	TEST_EXPECT(put(first + "/project.opennova", "{}") && put(first + "/defs/a.def", "a1") &&
			put(first + "/art/b.tga", "b1") && put(first + "/missions/c.bms", "c1"));
	TEST_EXPECT(push(service, first, "First") == editor::PushOutcome::Pushed);
	TEST_EXPECT(service.head == 1 && service.uploads == 4 && copy_of(first).revision == 1);
	TEST_EXPECT(push(service, first, "Again") == editor::PushOutcome::NothingToPush);

	// Got into an empty folder: the same files.
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(second), "", { "MOD", 0 });
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.revision() == 1 &&
				!pull.up_to_date());
		TEST_EXPECT(files_of(second) == files_of(first) && service.downloads == 4);
	}

	// Each changes a file; the second's push is stale until it pulls the first's.
	TEST_EXPECT(put(first + "/defs/a.def", "a2"));
	TEST_EXPECT(push(service, first, "a2") == editor::PushOutcome::Pushed && service.head == 2);
	TEST_EXPECT(put(second + "/missions/c.bms", "c2"));
	TEST_EXPECT(push(service, second, "c2") == editor::PushOutcome::Stale);
	{
		editor::NovaWorldPull pull(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(second), "", copy_of(second));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.revision() == 2);
		TEST_EXPECT(pull.applied().written == 1);
	}
	TEST_EXPECT(files_of(second).at("defs/a.def") == "a2" &&
			files_of(second).at("missions/c.bms") == "c2");
	TEST_EXPECT(push(service, second, "c2 over a2") == editor::PushOutcome::Pushed &&
			service.head == 3);
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(first), "", copy_of(first));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.revision() == 3);
	}
	TEST_EXPECT(files_of(first) == files_of(second));

	// Both change one file: the second keeps its own over the first's, pushes it, and the first
	// takes it.
	TEST_EXPECT(put(first + "/art/b.tga", "b-first"));
	TEST_EXPECT(
			push(service, first, "b first") == editor::PushOutcome::Pushed && service.head == 4);
	TEST_EXPECT(put(second + "/art/b.tga", "b-second"));
	{
		editor::NovaWorldPull pull(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(second), "", copy_of(second));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::NeedsResolution &&
				pull.plan().conflicts() == 1);
		TEST_EXPECT(pull.plan().items.front().path == "art/b.tga");
		TEST_EXPECT(pull.resolve("art/b.tga", editor::Resolution::KeepMine));
		pull.resume();
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.revision() == 4);
	}
	TEST_EXPECT(files_of(second).at("art/b.tga") == "b-second");
	TEST_EXPECT(
			push(service, second, "b second") == editor::PushOutcome::Pushed && service.head == 5);
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(first), "", copy_of(first));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.revision() == 5);
	}
	TEST_EXPECT(files_of(first).at("art/b.tga") == "b-second");
	// At the head: a pull changes nothing.
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(first), "", copy_of(first));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.up_to_date());
	}
	std::printf("share: a first push, a get, a stale push pulled and pushed, a conflict kept and "
				"taken\n");
	return 0;
}

// A restore: changes discarded (the base's file back, the folder's own new file gone, a deleted one
// back), and a file put back as an older revision has it, the base and the state untouched (a
// change to push); every file it replaced in the project's trash.
// The signed URLs a batch's transfers wait on (NovaWorldJob::kSignedBatchBytes): a push asks which
// of a batch's files the store lacks, uploads them, then asks of the next; a pull asks where a
// batch is, downloads it, then asks of the next. So no URL the store signed for an hour waits past
// its own batch, where every URL was signed before the first transfer. Bounded here at 25 bytes:
// the 10-byte files go two a batch, the 30-byte one alone; bounded at one file, each its own ask.
static int test_signed_batches() {
	test_temp::TempDir dir("opennova_novaworld_batches");
	const std::string first = dir.file("First"), second = dir.file("Second");
	FakeService service;
	TEST_EXPECT(put(first + "/project.opennova", "{}") && put(first + "/a.def", "aaaaaaaaaa") &&
			put(first + "/b.def", "bbbbbbbbbb") && put(first + "/c.def", std::string(30, 'c')) &&
			put(first + "/d.def", "dddddddddd"));
	{
		editor::NovaWorldPush job(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(first), "", copy_of(first), "First");
		job.limit_batches(0, 25);
		TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done && job.outcome() == editor::PushOutcome::Pushed);
	}
	// The scan's order (by path): a.def, b.def | c.def | d.def, project.opennova.
	const std::vector<std::string> pushed = { "lacks:2", "put", "put", "lacks:1", "put", "lacks:2", "put", "put" };
	TEST_EXPECT(service.transfers == pushed);
	if (service.transfers != pushed)
		for (const std::string &call : service.transfers) std::fprintf(stderr, "  push call %s\n", call.c_str());
	service.transfers.clear();
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(second), "", { "MOD", 0 });
		pull.limit_batches(0, 25);
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && files_of(second) == files_of(first));
	}
	const std::vector<std::string> pulled = { "where:2", "get", "get", "where:1", "get", "where:2", "get", "get" };
	TEST_EXPECT(service.transfers == pulled);
	if (service.transfers != pulled)
		for (const std::string &call : service.transfers) std::fprintf(stderr, "  pull call %s\n", call.c_str());
	// A batch of one file at most: five asks, each before its download.
	service.transfers.clear();
	const std::string third = dir.file("Third");
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(third), "", { "MOD", 0 });
		pull.limit_batches(1, editor::NovaWorldJob::kSignedBatchBytes);
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done);
	}
	const std::vector<std::string> single = { "where:1", "get", "where:1", "get", "where:1", "get", "where:1", "get",
		"where:1", "get" };
	TEST_EXPECT(service.transfers == single);
	return 0;
}

static int test_restore() {
	test_temp::TempDir dir("opennova_novaworld_client_restore");
	const std::string root = dir.file("Folder");
	FakeService service;
	TEST_EXPECT(put(root + "/project.opennova", "{}") && put(root + "/defs/a.def", "a1") &&
			put(root + "/defs/b.def", "b1"));
	TEST_EXPECT(push(service, root, "r1") == editor::PushOutcome::Pushed);
	TEST_EXPECT(put(root + "/defs/a.def", "a2"));
	TEST_EXPECT(push(service, root, "r2") == editor::PushOutcome::Pushed && service.head == 2);
	const auto restore = [&](std::vector<std::string> paths, int64_t revision) {
		editor::NovaWorldPull job(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(root), "", copy_of(root),
				editor::NovaWorldRestore{ std::move(paths), revision });
		const editor::NovaWorldJobState state = run(job);
		if (state != editor::NovaWorldJobState::Done)
			std::fprintf(stderr, "restore: %s\n", job.error().c_str());
		return state == editor::NovaWorldJobState::Done ? job.applied().trash.id : uint64_t(-1);
	};

	// Discard: a's change undone, the new c gone, the deleted b back.
	TEST_EXPECT(put(root + "/defs/a.def", "a3") && put(root + "/defs/c.def", "c"));
	std::error_code ec;
	std::filesystem::remove(io::os_path(root + "/defs/b.def"), ec);
	const uint64_t trashed = restore({ "defs/a.def", "defs/b.def", "defs/c.def" }, 0);
	TEST_EXPECT(trashed != uint64_t(-1) && trashed != 0);
	std::map<std::string, std::string> files = files_of(root);
	TEST_EXPECT(files["defs/a.def"] == "a2" && files["defs/b.def"] == "b1" && !files.count("defs/c.def"));
	TEST_EXPECT(copy_of(root).revision == 2);

	// a as revision 1 has it: a change to push, the base still r2's.
	TEST_EXPECT(restore({ "defs/a.def" }, 1) != uint64_t(-1));
	files = files_of(root);
	TEST_EXPECT(files["defs/a.def"] == "a1" && copy_of(root).revision == 2);
	TEST_EXPECT(push(service, root, "a back") == editor::PushOutcome::Pushed && service.head == 3);
	// Nothing to put back: no trash batch.
	TEST_EXPECT(restore({ "defs/a.def" }, 0) == 0);
	std::printf("restore: changes discarded, a file back as revision 1 has it, then pushed\n");
	return 0;
}

static int test_accounts() {
	test_temp::TempDir dir("opennova_novaworld_client_accounts");
	const std::string root = dir.file("Folder");
	FakeService service;
	TEST_EXPECT(put(root + "/defs/a.def", "a"));

	// A player may read another's project but not push it.
	std::string error;
	push(service, root, "x", kPlayerCookie, &error);
	TEST_EXPECT(error == editor::novaworld_push_refusal("MOD") && service.head == 0);

	// A renewed session: kept by the account the job leaves.
	service.renew_next = true;
	editor::NovaWorldPush renewed(service, account(kAdminCookie),
			editor::ProjectPaths::for_root(root), "", copy_of(root), "renewed");
	TEST_EXPECT(run(renewed) == editor::NovaWorldJobState::Done &&
			renewed.outcome() == editor::PushOutcome::Pushed);
	const editor::NovaWorldAccountUpdate update = renewed.take_account();
	TEST_EXPECT(update.changed && update.account.cookie == kRenewedCookie &&
			update.account.signed_in());
	TEST_EXPECT(!renewed.take_account().changed); // taken once

	// An ended session: the job fails and the account is signed out.
	TEST_EXPECT(put(root + "/defs/a.def", "b"));
	editor::NovaWorldPush expired(service, account(kExpiredCookie),
			editor::ProjectPaths::for_root(root), "", copy_of(root), "expired");
	TEST_EXPECT(run(expired) == editor::NovaWorldJobState::Failed &&
			expired.error().find("sign in again") != std::string::npos);
	const editor::NovaWorldAccountUpdate ended = expired.take_account();
	TEST_EXPECT(ended.changed && !ended.account.signed_in() && ended.account.server == kServer);

	// The kept account's file: with none kept, the build's service alone; once kept, the account.
	editor::FileNovaWorldAccountStore store(dir.file("novaworld_account.json"), "staging.test/");
	TEST_EXPECT(store.load().server == "https://staging.test" && !store.load().signed_in());
	TEST_EXPECT(store.save(update.account, error));
	const editor::NovaWorldAccount kept = store.load();
	TEST_EXPECT(kept.server == kServer && kept.cookie == kRenewedCookie &&
			kept.username == update.account.username);
	std::printf("accounts: a player refused, a renewed session kept, an ended one signed out, the "
				"account's file\n");
	return 0;
}

static int test_track_and_check() {
	test_temp::TempDir dir("opennova_novaworld_client_track");
	const std::string first = dir.file("First"), kept = dir.file("Kept");
	FakeService service;
	TEST_EXPECT(put(first + "/defs/a.def", "a1") && put(first + "/defs/b.def", "b1"));
	TEST_EXPECT(push(service, first, "r1") == editor::PushOutcome::Pushed);
	TEST_EXPECT(put(first + "/defs/a.def", "a2"));
	TEST_EXPECT(push(service, first, "r2") == editor::PushOutcome::Pushed && service.head == 2);

	// A folder kept before the store, holding revision 1's files and one of its own: its files are not
	// the head's, so it is set up at no revision (no base), its files untouched, and taking the head for
	// its base would have pushed revision 2's change undone. Its push waits for a pull, which takes the
	// head's files and stops on the one both hold otherwise; kept as the folder has it, it is a change.
	TEST_EXPECT(put(kept + "/defs/a.def", "a1") && put(kept + "/defs/b.def", "b1") &&
			put(kept + "/defs/c.def", "c1"));
	{
		editor::NovaWorldTrack track(
				service, account(""), editor::ProjectPaths::for_root(kept), "", "MOD");
		TEST_EXPECT(run(track) == editor::NovaWorldJobState::Done && track.differs() &&
				track.copy().revision == 0 && track.copy().key == "MOD" && track.remote_known() &&
				track.remote().head == 2);
	}
	TEST_EXPECT(copy_of(kept).revision == 0 && files_of(kept).at("defs/a.def") == "a1");
	TEST_EXPECT(push(service, kept, "kept") == editor::PushOutcome::Stale && service.head == 2);
	{
		editor::NovaWorldPull pull(
				service, account(""), editor::ProjectPaths::for_root(kept), "", copy_of(kept));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::NeedsResolution &&
				pull.plan().conflicts() == 1 &&
				pull.plan().find("defs/a.def")->step == editor::PullStep::Conflict);
		TEST_EXPECT(pull.resolve("defs/a.def", editor::Resolution::KeepMine));
		pull.resume();
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.revision() == 2);
	}
	TEST_EXPECT(copy_of(kept).revision == 2 && files_of(kept).at("defs/a.def") == "a1" &&
			files_of(kept).at("defs/c.def") == "c1");
	{
		editor::NovaWorldCheck check(
				service, account(""), editor::ProjectPaths::for_root(kept), "", copy_of(kept));
		TEST_EXPECT(run(check) == editor::NovaWorldJobState::Done && check.remote_known() &&
				check.remote().head == 2 && !check.reset());
		const auto &changes = check.changes();
		TEST_EXPECT(changes.size() == 2 && changes[0].path == "defs/a.def" &&
				changes[0].change == editor::NovaWorldChange::Modified &&
				changes[1].path == "defs/c.def" &&
				changes[1].change == editor::NovaWorldChange::Added);
	}
	// Pushed from there: revision 3 is the kept folder's.
	TEST_EXPECT(push(service, kept, "kept") == editor::PushOutcome::Pushed && service.head == 3);
	// The store reset (a NovaWorld database reset: its head 0, the folder still at revision 3): a
	// check, a pull and a push each end on it at once, moving nothing; set up again, the folder is
	// a working copy at none, every file the first push's.
	{
		FakeService reset;
		editor::NovaWorldCheck check(
				reset, account(""), editor::ProjectPaths::for_root(kept), "", copy_of(kept));
		TEST_EXPECT(run(check) == editor::NovaWorldJobState::Done && check.reset() &&
				check.changes().empty());
		editor::NovaWorldPull pull(
				reset, account(""), editor::ProjectPaths::for_root(kept), "", copy_of(kept));
		TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Done && pull.reset() &&
				pull.remote().head == 0);
		editor::NovaWorldPush pushed(reset, account(kAdminCookie),
				editor::ProjectPaths::for_root(kept), "", copy_of(kept), "after the reset");
		TEST_EXPECT(run(pushed) == editor::NovaWorldJobState::Done && pushed.reset() &&
				reset.head == 0 && copy_of(kept).revision == 3);
		editor::NovaWorldTrack track(
				reset, account(kAdminCookie), editor::ProjectPaths::for_root(kept), "", "MOD");
		TEST_EXPECT(run(track) == editor::NovaWorldJobState::Done && track.copy().revision == 0 &&
				!track.differs());
		TEST_EXPECT(push(reset, kept, "again") == editor::PushOutcome::Pushed && reset.head == 1);
	}
	// Set up again over the first service, its files the head's: at its head.
	{
		editor::NovaWorldTrack track(
				service, account(""), editor::ProjectPaths::for_root(kept), "", "MOD");
		TEST_EXPECT(run(track) == editor::NovaWorldJobState::Done && track.copy().revision == 3 &&
				!track.differs());
	}

	// A check whose service will not answer still reads the folder's changes.
	TEST_EXPECT(put(kept + "/defs/b.def", "b2"));
	{
		editor::NovaWorldCheck check(service, account(kExpiredCookie),
				editor::ProjectPaths::for_root(kept), "", copy_of(kept));
		TEST_EXPECT(run(check) == editor::NovaWorldJobState::Done && !check.remote_known() &&
				!check.remote_error().empty() && !check.not_found() && check.changes().size() == 1);
	}
	// A project the service does not hold: its setup fails, told so (a 404).
	{
		editor::NovaWorldTrack track(
				service, account(""), editor::ProjectPaths::for_root(kept), "", "NOPE");
		TEST_EXPECT(run(track) == editor::NovaWorldJobState::Failed && track.not_found() &&
				!track.remote_known());
	}
	// A project with no revision yet: set up at none, every file the first push's.
	FakeService empty;
	const std::string fresh = dir.file("Fresh");
	TEST_EXPECT(put(fresh + "/defs/a.def", "a"));
	{
		editor::NovaWorldTrack track(
				empty, account(kAdminCookie), editor::ProjectPaths::for_root(fresh), "", "MOD");
		TEST_EXPECT(run(track) == editor::NovaWorldJobState::Done && track.copy().revision == 0);
	}
	TEST_EXPECT(push(empty, fresh, "first") == editor::PushOutcome::Pushed && empty.head == 1);
	std::printf("track and check: a kept folder that is not the head set up at no revision and "
				"pulled first, its changes read, offline too; the store's reset ending a check, a pull "
				"and a push; a folder that is the head set up at it\n");
	return 0;
}

// A player's own project (ADR 0069: any account makes its own and pushes it): made through POST
// /api/products as the client asks for it (create_project, read_created), the same key again
// refused by its code, private to it and the admins, pushed by it and by an admin, a push past its
// storage said so plainly (413 quota_exceeded); the games it may make a mod of (GET
// /api/me/projects); signed out, not found.
static int test_owned_project() {
	test_temp::TempDir dir("opennova_novaworld_client_owned");
	const std::string root = dir.file("Mine");
	FakeService service;
	editor::NovaWorldAccount player = account(kPlayerCookie);
	player.role = "player";
	editor::NewRemoteProject asked;
	asked.key = "MYMOD";
	asked.title = "My mod";
	asked.kind = "expansion";
	asked.base = "MOD";
	asked.expansion_code = "mymod";
	asked.visibility = "private";
	editor::NovaWorldCall call(service, editor::novaworld_api::create_project(player, asked));
	editor::RemoteProject made;
	editor::NovaWorldApiError error;
	TEST_EXPECT(call.poll() && editor::novaworld_api::read_created(call.reply(), made, error));
	TEST_EXPECT(made.key == "MYMOD" && made.title == "My mod" && made.kind == "expansion" &&
			made.base == "MOD" && made.expansion_code == "mymod" && made.game == "jo" &&
			made.owner == kPlayer && !made.is_public && made.can_push && made.can_manage &&
			made.head == 0);
	editor::NovaWorldCall again(service, editor::novaworld_api::create_project(player, asked));
	TEST_EXPECT(again.poll() && !editor::novaworld_api::read_created(again.reply(), made, error) &&
			error.code == "duplicate_key" && error.status == 409);
	// Pushed by its owner, then by an admin.
	TEST_EXPECT(put(root + "/project.opennova", "{}") && put(root + "/defs/a.def", "a"));
	TEST_EXPECT(push(service, root, "First revision", kPlayerCookie, nullptr, "MYMOD") ==
					editor::PushOutcome::Pushed &&
			service.made["MYMOD"].head == 1);
	TEST_EXPECT(put(root + "/defs/a.def", "b"));
	TEST_EXPECT(push(service, root, "By an admin", kAdminCookie, nullptr, "MYMOD") ==
					editor::PushOutcome::Pushed &&
			service.made["MYMOD"].head == 2);
	// The games it may make a mod of: the service's own MOD (MYMOD is a mod, no game).
	editor::NovaWorldCall mine(service, editor::novaworld_api::my_projects(player));
	std::vector<editor::RemoteGame> games;
	TEST_EXPECT(mine.poll() && editor::novaworld_api::read_my_games(mine.reply(), games, error) &&
			games.size() == 1 && games[0].key == "MOD" && games[0].game == "jo");
	// Past its storage: the push says so plainly, the head unmoved.
	service.quota = service.stored + 1;
	TEST_EXPECT(put(root + "/defs/big.def", "more than one byte"));
	std::string why;
	push(service, root, "Too much", kPlayerCookie, &why, "MYMOD");
	TEST_EXPECT(why.find("this push would pass it") != std::string::npos &&
			why.find("NovaWorld keeps") != std::string::npos && service.made["MYMOD"].head == 2);
	// No one else reads a private project: signed out, it is not found.
	{
		editor::NovaWorldCheck check(service, account(""), editor::ProjectPaths::for_root(root), "",
				copy_of(root, "MYMOD"));
		TEST_EXPECT(run(check) == editor::NovaWorldJobState::Done && !check.remote_known() &&
				check.not_found());
	}
	std::printf("owned: a player's own project made, refused again, pushed by it and an admin, "
				"private\n");
	return 0;
}

// A push sends every file, so it refuses a folder it could not read whole, NovaWorld's head unmoved:
// one its walk could not list (the folder gone, as a drive taken away; a folder the walk may list
// but not look into) and one holding a file it could not read (two paths one file on Windows and
// macOS, where the file system holds both) (F-2). Over the folder not listed whole, a pull refuses
// too and a check counts no file the list lacks as deleted (C-11). The cases a system cannot make
// (case-insensitive names; no search permission enforced, as for root or on Windows) say so.
static int test_incomplete_folder() {
	test_temp::TempDir dir("opennova_novaworld_client_incomplete");
	const std::string root = dir.file("Project");
	FakeService service;
	TEST_EXPECT(put(root + "/project.opennova", "{}") && put(root + "/defs/a.def", "a1") &&
			put(root + "/locked/c.def", "c1"));
	TEST_EXPECT(push(service, root, "r1") == editor::PushOutcome::Pushed && service.head == 1);
	const editor::NovaWorldWorkingCopy bound = copy_of(root);
	TEST_EXPECT(bound.revision == 1);
	std::string error;

	{
		editor::NovaWorldPush gone(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(dir.file("Gone")), "", bound, "gone");
		TEST_EXPECT(run(gone) == editor::NovaWorldJobState::Failed &&
				gone.error().find("could not all be listed") != std::string::npos);
		TEST_EXPECT(service.head == 1);
	}

	TEST_EXPECT(put(root + "/defs/A.def", "other"));
	size_t named = 0;
	std::error_code ec;
	for (const auto &entry :
			std::filesystem::directory_iterator(editor::system_path(root + "/defs"), ec)) {
		(void)entry;
		++named;
	}
	if (named == 2) {
		push(service, root, "folded", kAdminCookie, &error);
		TEST_EXPECT(error.find("could not be read; a push sends every file") != std::string::npos &&
				service.head == 1);
		std::printf("incomplete: a file the scan could not read refuses the push\n");
	} else {
		std::printf("incomplete: names fold here: the unread case not run\n");
	}
	std::filesystem::remove(editor::system_path(root + "/defs/A.def"), ec);
	if (named != 2)
		TEST_EXPECT(put(root + "/defs/a.def", "a1"));

#ifndef _WIN32
	const std::filesystem::path locked = editor::system_path(root + "/locked");
	std::filesystem::permissions(locked,
			std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
					std::filesystem::perms::others_read,
			std::filesystem::perm_options::replace, ec);
	TEST_EXPECT(!ec);
	std::error_code probe;
	std::filesystem::file_size(locked / "c.def", probe);
	if (probe) {
		TEST_EXPECT(put(root + "/defs/a.def", "a2"));
		error.clear();
		push(service, root, "locked", kAdminCookie, &error);
		TEST_EXPECT(error.find("could not all be listed") != std::string::npos && service.head == 1);
		{
			editor::NovaWorldCheck check(
					service, account(""), editor::ProjectPaths::for_root(root), "", bound);
			TEST_EXPECT(run(check) == editor::NovaWorldJobState::Done && !check.problems().empty());
			TEST_EXPECT(check.changes().size() == 1 && check.changes()[0].path == "defs/a.def" &&
					check.changes()[0].change == editor::NovaWorldChange::Modified);
		}
		{
			// A pull from before the head, over the same folder.
			editor::NovaWorldPull pull(
					service, account(""), editor::ProjectPaths::for_root(root), "", { "MOD", 0 });
			TEST_EXPECT(run(pull) == editor::NovaWorldJobState::Failed &&
					pull.error().find("a pull compares every file") != std::string::npos);
		}
		std::printf("incomplete: a folder listed but not looked into refuses the push and the "
					"pull, its check counts no deletion\n");
	} else {
		std::printf("incomplete: the system enforces no search permission here: the case not run\n");
	}
	std::filesystem::permissions(
			locked, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
#endif
	std::printf("incomplete: a folder gone refuses the push\n");
	return 0;
}

static int test_api_shapes() {
	// The server as the client keeps it, and the session cookie a login reply signs in.
	TEST_EXPECT(editor::novaworld_server(" opennova.net/ ") == "https://opennova.net");
	TEST_EXPECT(editor::novaworld_server("http://127.0.0.1:8080") == "http://127.0.0.1:8080");
	const editor::NovaWorldRequest login = editor::novaworld_api::login("opennova.net", "taylor", "pw");
	TEST_EXPECT(login.url == "https://opennova.net/api/login" && login.method == "POST");
	editor::NovaWorldResponse reply = json(200, io::JsonValue::make_object());
	reply.body = "{\"id\": 7, \"username\": \"taylor\", \"role\": \"admin\"}";
	reply.headers = { { "Set-Cookie", "other=1; Path=/" },
		{ "set-cookie", "__Host-opennova_session=tok; Path=/; Secure; HttpOnly" } };
	editor::NovaWorldAccount signed_in;
	signed_in.server = "https://opennova.net";
	editor::NovaWorldApiError error;
	TEST_EXPECT(editor::novaworld_api::read_login(reply, signed_in, error) &&
			signed_in.cookie == "__Host-opennova_session=tok" && signed_in.admin());
	// An upload the service's route takes carries the session; one to the store's own URL does not.
	editor::NovaWorldResponse missing = json(200, io::JsonValue::make_object());
	missing.body = "{\"missing\": [{\"sha256\": \"" + std::string(64, 'a') +
			"\", \"size\": 3, \"upload\": {\"method\": \"PUT\", \"url\": "
			"\"/api/products/JO/blobs/x\", "
			"\"headers\": {}}}, {\"sha256\": \"" +
			std::string(64, 'b') +
			"\", \"size\": 4, \"upload\": {\"method\": \"PUT\", \"url\": "
			"\"https://bucket.s3/key?sig=1\", "
			"\"headers\": {\"x-amz-checksum-sha256\": \"abc\"}}}]}";
	std::vector<editor::UploadTarget> targets;
	TEST_EXPECT(editor::novaworld_api::read_missing(missing, signed_in, targets, error) &&
			targets.size() == 2);
	const auto has = [](const editor::NovaWorldRequest &r, const std::string &name) {
		for (const auto &[key, value] : r.headers)
			if (key == name)
				return true;
		return false;
	};
	TEST_EXPECT(targets[0].request.url == "https://opennova.net/api/products/JO/blobs/x" &&
			has(targets[0].request, "Cookie") && has(targets[0].request, "X-OpenNova-Request"));
	TEST_EXPECT(targets[1].request.url == "https://bucket.s3/key?sig=1" &&
			!has(targets[1].request, "Cookie") && has(targets[1].request, "x-amz-checksum-sha256"));
	// A release is asked of the project's own route (its owner's or an admin's), and a project says
	// whether the account releases it and which release players get.
	const editor::NovaWorldRequest release =
			editor::novaworld_api::release(signed_in, "MYMOD", "0.2.0", 4);
	TEST_EXPECT(release.method == "POST" &&
			release.url == "https://opennova.net/api/products/MYMOD/releases" &&
			release.body.find("\"revision\": 4") != std::string::npos);
	editor::NovaWorldResponse answered = json(200, io::JsonValue::make_object());
	answered.body = "{\"project\": {\"key\": \"MYMOD\", \"can_release\": true, "
					"\"release\": {\"id\": 9, \"version\": \"0.1.0\"}}}";
	editor::RemoteProject project;
	std::optional<editor::RemoteRevision> head;
	TEST_EXPECT(editor::novaworld_api::read_project(answered, project, head, error) &&
			project.can_release && project.release_id == 9 && project.release_version == "0.1.0");
	answered.body = "{\"project\": {\"key\": \"MYMOD\", \"release\": null}}";
	TEST_EXPECT(editor::novaworld_api::read_project(answered, project, head, error) &&
			!project.can_release && project.release_id == 0 && project.release_version.empty());
	// A refusal's field is the error's.
	editor::NovaWorldResponse taken = json(409, io::JsonValue::make_object());
	taken.body = "{\"error\": \"duplicate_expansion\", \"field\": \"expansion_code\"}";
	TEST_EXPECT(!editor::novaworld_api::read_created(taken, project, error) &&
			error.code == "duplicate_expansion" && error.field == "expansion_code");
	std::printf("api: the server's spelling, the login's session, an upload's headers by whose URL "
				"it is, a release's route, a project's release rights and release, a refusal's field\n");
	return 0;
}

// The Shell's transport's rules (godot/src/authoring/novaworld_http, over editor::novaworld_address and
// novaworld_goes_again): the URLs it reaches, and which request goes again on a fresh connection.
static int test_service_lobby_cache() {
	test_temp::TempDir dir("opennova_editor_lobby_cache");
	const auto project_paths = editor::ProjectPaths::for_root(dir.file("Game"));
	const auto paths = editor::NovaWorldPaths::for_project(project_paths, kServer);
	editor::RemoteLobby lobby;
	lobby.slug = "acidgambit_0_consumer";
	lobby.display_name = "Acid Gambit";
	lobby.title = "jo";
	const std::vector<uint8_t> bytes = { 0, 1, 2, 255, 128, 13, 10 };
	lobby.gate_tag_file = io::base64_encode(bytes);
	std::string error;
	TEST_EXPECT(editor::cache_novaworld_lobby(paths, "MYGAME", lobby, error));
	const std::string original = editor::cached_novaworld_gate_tag(paths, "MYGAME", "jo");
	std::vector<uint8_t> read;
	TEST_EXPECT(!original.empty() && io::read_file_bytes(original, read, error) && read == bytes);
	TEST_EXPECT(editor::cached_novaworld_gate_tag(paths, "OTHER", "jo").empty());
	TEST_EXPECT(editor::cached_novaworld_gate_tag(paths, "MYGAME", "dfx2").empty());
	TEST_EXPECT(editor::cached_novaworld_gate_tag(
			editor::NovaWorldPaths::for_project(project_paths, "https://another.test"), "MYGAME",
			"jo")
					.empty());
	lobby.gate_tag_file += "garbage";
	TEST_EXPECT(!editor::cache_novaworld_lobby(paths, "MYGAME", lobby, error));
	TEST_EXPECT(editor::cached_novaworld_gate_tag(paths, "MYGAME", "jo") == original);
	lobby.gate_tag_file = io::base64_encode(bytes);
	TEST_EXPECT(put(original, "damaged"));
	TEST_EXPECT(editor::cached_novaworld_gate_tag(paths, "MYGAME", "jo").empty());
	TEST_EXPECT(editor::cache_novaworld_lobby(paths, "MYGAME", lobby, error));
	TEST_EXPECT(editor::cached_novaworld_gate_tag(paths, "MYGAME", "jo") == original);
	lobby.gate_tag_file = io::base64_encode(std::vector<uint8_t>{ 9, 8, 7 });
	TEST_EXPECT(editor::cache_novaworld_lobby(paths, "MYGAME", lobby, error));
	TEST_EXPECT(editor::cached_novaworld_gate_tag(paths, "MYGAME", "jo") != original);
	TEST_EXPECT(io::read_file_bytes(original, read, error) && read == bytes);
	TEST_EXPECT(!std::filesystem::exists(editor::system_path(project_paths.root + "/gt.ssc")));

	// Historical manifests retain their original hash. Filtering happens only after checking it.
	const std::vector<ProjectFile> old = {
		{ "GT.SSC", std::string(64, 'a'), 3 },
		{ "config/gT.sSc", std::string(64, 'b'), 4 },
		{ "defs/a.def", std::string(64, 'c'), 5 },
	};
	editor::NovaWorldResponse response;
	response.transport_ok = true;
	response.status = 200;
	response.body = opennova::project::encode_manifest(old);
	editor::NovaWorldApiError api_error;
	std::vector<ProjectFile> files;
	TEST_EXPECT(editor::novaworld_api::read_manifest(
			response, opennova::project::manifest_sha256(response.body), files, api_error));
	TEST_EXPECT(files.size() == 1 && files.front().path == "defs/a.def");
	TEST_EXPECT(!editor::novaworld_api::read_manifest(
			response, std::string(64, '0'), files, api_error));
	TEST_EXPECT(editor::write_novaworld_base(paths, old, error));
	TEST_EXPECT(editor::read_novaworld_base(paths, files, error) && files.size() == 1 &&
			files.front().path == "defs/a.def");

	FakeService service;
	const auto who = account(kAdminCookie);
	const uint64_t id = service.start(editor::novaworld_api::project(who, "MOD"));
	TEST_EXPECT(service.poll(id, response));
	editor::RemoteProject remote;
	std::optional<editor::RemoteRevision> head;
	TEST_EXPECT(editor::novaworld_api::read_project(response, remote, head, api_error));
	TEST_EXPECT(remote.lobby && remote.lobby->slug == "mod_lobby" && !remote.lobby_locked &&
			!remote.lobby->gate_tag_file.empty());
	const uint64_t my = service.start(editor::novaworld_api::my_projects(who));
	TEST_EXPECT(service.poll(my, response));
	std::vector<editor::RemoteLobby> offered;
	TEST_EXPECT(editor::novaworld_api::read_my_lobbies(response, offered, api_error) &&
			offered.size() == 2 && offered[0].slug == "jo_lobby" && offered[0].title == "jo");
	editor::NewRemoteProject created;
	created.key = "MYGAME";
	created.kind = "game";
	created.title = "My game";
	created.game = "jo";
	created.lobby = "jo_lobby";
	io::JsonValue body;
	TEST_EXPECT(
			io::json_parse(editor::novaworld_api::create_project(who, created).body, body, error));
	TEST_EXPECT(body.get_string("lobby", "") == "jo_lobby" && !body.get("new_lobby_name"));
	created.lobby.clear();
	created.new_lobby_name = "My separate lobby";
	TEST_EXPECT(
			io::json_parse(editor::novaworld_api::create_project(who, created).body, body, error));
	TEST_EXPECT(body.get_string("new_lobby_name", "") == "My separate lobby" && !body.get("lobby"));
	std::printf("lobby: service artifact cache binds service, project and title; old source "
				"manifests filter after verification\n");
	return 0;
}

static int test_transport_rules() {
	editor::NovaWorldAddress at;
	TEST_EXPECT(editor::novaworld_address("https://opennova.net/api/products?x=1#top", at) &&
			at.tls && at.host == "opennova.net" && at.port == 443 &&
			at.target == "/api/products?x=1" && at.origin() == "https://opennova.net:443");
	TEST_EXPECT(editor::novaworld_address("http://127.0.0.1:8080", at) && !at.tls &&
			at.host == "127.0.0.1" && at.port == 8080 && at.target == "/");
	TEST_EXPECT(editor::novaworld_address("https://bucket.s3?sig=1", at) && at.target == "/?sig=1");
	TEST_EXPECT(editor::novaworld_address("https://host#only", at) && at.target == "/");
	for (const char *refused : { "ftp://host/x", "opennova.net/api", "https://user@host/",
				 "http://[::1]:80/", "http://host:0/", "http://host:65536/", "http://host:x/",
				 "https:///path", "" })
		TEST_EXPECT(!editor::novaworld_address(refused, at));

	// A GET, PUT or DELETE goes again after a broken connection or a 5xx, three more times; a POST
	// (a push, a sign-in) never does; a reply below 500, or a transfer that stalled, is the answer.
	TEST_EXPECT(editor::kNovaWorldRetries == 3);
	TEST_EXPECT(editor::novaworld_goes_again("GET", 0, false, 0, true));
	TEST_EXPECT(editor::novaworld_goes_again("PUT", 2, true, 503, false));
	TEST_EXPECT(editor::novaworld_goes_again("DELETE", 1, true, 500, false));
	TEST_EXPECT(!editor::novaworld_goes_again("PUT", 3, true, 503, false));
	TEST_EXPECT(!editor::novaworld_goes_again("GET", 3, false, 0, true));
	TEST_EXPECT(!editor::novaworld_goes_again("POST", 0, false, 0, true));
	TEST_EXPECT(!editor::novaworld_goes_again("POST", 0, true, 503, false));
	TEST_EXPECT(!editor::novaworld_goes_again("GET", 0, true, 200, false));
	TEST_EXPECT(!editor::novaworld_goes_again("GET", 0, true, 404, false));
	TEST_EXPECT(!editor::novaworld_goes_again("GET", 0, false, 0, false)); // stalled, given up
	TEST_EXPECT(!editor::novaworld_goes_again("GET", 0, false, 503, false)); // no whole reply
	std::printf("transport: the URLs it reaches, a request that may go again\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_share_and_pull();
	failures += test_signed_batches();
	failures += test_restore();
	failures += test_accounts();
	failures += test_track_and_check();
	failures += test_owned_project();
	failures += test_incomplete_folder();
	failures += test_api_shapes();
	failures += test_service_lobby_cache();
	failures += test_transport_rules();
	if (failures == 0)
		std::printf("novaworld_client: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
