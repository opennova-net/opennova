// The content store's client (engine/editor/content_store/content_jobs.h, content_api.h, ADR 0069)
// over an in-memory service behind the transport seam, which keeps the routes' contract
// (apps/novaworld_web/content/content_routes.h: the head, the manifests, the files by SHA-256, the
// missing files with their uploads, the push on its parent), driving real folders: an admin's
// first push; a second folder getting the project, the same files; each pushing a change, the
// second told its base is stale, then pulling (the first's change taken, its own kept) and pushing
// both; a conflict settled by keeping one's own, pushed, and pulled by the other; a folder at its
// base with nothing to push; a player refused; a renewed session kept by the account; an ended
// one signing it out; a store reset since the folder's revision, a pull and a push each told to
// bind again; and a folder the scan could not read whole, refused by a push and a pull, its check
// counting no deletion. The routes themselves answer the same client over HTTP in
// tests/novaworld/content_client_routes_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <editor/content_store/content_account.h>
#include <editor/content_store/content_api.h>
#include <editor/content_store/content_jobs.h>
#include <editor/content_store/content_state.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

#include "common/test_expect.h"
#include "editor/content_fake_service.h"
#include "editor/editor_test_support.h"

namespace editor = opennova::editor;
namespace io = opennova::io;
using opennova::content::ContentFile;
using namespace content_fake;

namespace {

editor::ContentAccount account(const char *cookie) {
	editor::ContentAccount a;
	a.server = kServer;
	a.username = "taylor";
	a.role = "admin";
	a.cookie = cookie;
	return a;
}

editor::ContentBinding binding_of(const std::string &root) {
	editor::ContentBinding binding;
	std::string error;
	if (!editor::read_content_binding(
				editor::ContentPaths::for_project(editor::ProjectPaths::for_root(root)), binding,
				error))
		binding = { kServer, "MOD", 0 };
	return binding;
}

editor::ContentJobState run(editor::ContentJob &job) {
	editor::ContentJobState state;
	int steps = 0;
	while ((state = job.step(1 << 16)) == editor::ContentJobState::Running && ++steps < 100000) {
	}
	return state;
}

editor::PushOutcome push(FakeService &service, const std::string &root, const char *message,
		const char *cookie = kAdminCookie, std::string *error = nullptr) {
	editor::ContentPush job(service, account(cookie), editor::ProjectPaths::for_root(root), "",
			binding_of(root), message);
	const editor::ContentJobState state = run(job);
	if (state != editor::ContentJobState::Done) {
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
	const editor::ContentScan scan = editor::scan_content(editor::ProjectPaths::for_root(root), "");
	for (const ContentFile &file : scan.files()) {
		std::string bytes, error;
		io::read_file_text(root + "/" + file.path, bytes, error);
		out[file.path] = bytes;
	}
	return out;
}

} // namespace

static int test_share_and_pull() {
	test_temp::TempDir dir("opennova_content_client");
	const std::string first = dir.file("First"), second = dir.file("Second");
	FakeService service;

	// The first push of a folder.
	TEST_EXPECT(put(first + "/project.opennova", "{}") && put(first + "/defs/a.def", "a1") &&
			put(first + "/art/b.tga", "b1") && put(first + "/missions/c.bms", "c1"));
	TEST_EXPECT(push(service, first, "First") == editor::PushOutcome::Pushed);
	TEST_EXPECT(service.head == 1 && service.uploads == 4 && binding_of(first).revision == 1);
	TEST_EXPECT(push(service, first, "Again") == editor::PushOutcome::NothingToPush);

	// Got into an empty folder: the same files.
	{
		editor::ContentPull pull(service, account(""), editor::ProjectPaths::for_root(second), "",
				{ kServer, "MOD", 0 });
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && pull.revision() == 1 &&
				!pull.up_to_date());
		TEST_EXPECT(files_of(second) == files_of(first) && service.downloads == 4);
	}

	// Each changes a file; the second's push is stale until it pulls the first's.
	TEST_EXPECT(put(first + "/defs/a.def", "a2"));
	TEST_EXPECT(push(service, first, "a2") == editor::PushOutcome::Pushed && service.head == 2);
	TEST_EXPECT(put(second + "/missions/c.bms", "c2"));
	TEST_EXPECT(push(service, second, "c2") == editor::PushOutcome::Stale);
	{
		editor::ContentPull pull(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(second), "", binding_of(second));
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && pull.revision() == 2);
		TEST_EXPECT(pull.applied().written == 1);
	}
	TEST_EXPECT(files_of(second).at("defs/a.def") == "a2" &&
			files_of(second).at("missions/c.bms") == "c2");
	TEST_EXPECT(push(service, second, "c2 over a2") == editor::PushOutcome::Pushed &&
			service.head == 3);
	{
		editor::ContentPull pull(
				service, account(""), editor::ProjectPaths::for_root(first), "", binding_of(first));
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && pull.revision() == 3);
	}
	TEST_EXPECT(files_of(first) == files_of(second));

	// Both change one file: the second keeps its own over the first's, pushes it, and the first
	// takes it.
	TEST_EXPECT(put(first + "/art/b.tga", "b-first"));
	TEST_EXPECT(
			push(service, first, "b first") == editor::PushOutcome::Pushed && service.head == 4);
	TEST_EXPECT(put(second + "/art/b.tga", "b-second"));
	{
		editor::ContentPull pull(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(second), "", binding_of(second));
		TEST_EXPECT(run(pull) == editor::ContentJobState::NeedsResolution &&
				pull.plan().conflicts() == 1);
		TEST_EXPECT(pull.plan().items.front().path == "art/b.tga");
		TEST_EXPECT(pull.resolve("art/b.tga", editor::Resolution::KeepMine));
		pull.resume();
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && pull.revision() == 4);
	}
	TEST_EXPECT(files_of(second).at("art/b.tga") == "b-second");
	TEST_EXPECT(
			push(service, second, "b second") == editor::PushOutcome::Pushed && service.head == 5);
	{
		editor::ContentPull pull(
				service, account(""), editor::ProjectPaths::for_root(first), "", binding_of(first));
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && pull.revision() == 5);
	}
	TEST_EXPECT(files_of(first).at("art/b.tga") == "b-second");
	// At the head: a pull changes nothing.
	{
		editor::ContentPull pull(
				service, account(""), editor::ProjectPaths::for_root(first), "", binding_of(first));
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && pull.up_to_date());
	}
	std::printf("share: a first push, a get, a stale push pulled and pushed, a conflict kept and "
				"taken\n");
	return 0;
}

// A restore: changes discarded (the base's file back, the folder's own new file gone, a deleted one
// back), and a file put back as an older revision has it, the base and the binding untouched (a
// change to push); every file it replaced in the project's trash.
// The signed URLs a batch's transfers wait on (ContentJob::kSignedBatchBytes): a push asks which of a batch's files the
// store lacks, uploads them, then asks of the next; a pull asks where a batch is, downloads it, then asks of the next.
// So no URL the store signed for an hour waits past its own batch, where every URL was signed before the first
// transfer. Bounded here at 25 bytes: the 10-byte files go two a batch, the 30-byte one alone; bounded at one file,
// each its own ask.
static int test_signed_batches() {
	test_temp::TempDir dir("opennova_content_batches");
	const std::string first = dir.file("First"), second = dir.file("Second");
	FakeService service;
	TEST_EXPECT(put(first + "/project.opennova", "{}") && put(first + "/a.def", "aaaaaaaaaa") &&
			put(first + "/b.def", "bbbbbbbbbb") && put(first + "/c.def", std::string(30, 'c')) &&
			put(first + "/d.def", "dddddddddd"));
	{
		editor::ContentPush job(service, account(kAdminCookie), editor::ProjectPaths::for_root(first), "",
				binding_of(first), "First");
		job.limit_batches(0, 25);
		TEST_EXPECT(run(job) == editor::ContentJobState::Done && job.outcome() == editor::PushOutcome::Pushed);
	}
	// The scan's order (by path): a.def, b.def | c.def | d.def, project.opennova.
	const std::vector<std::string> pushed = { "lacks:2", "put", "put", "lacks:1", "put", "lacks:2", "put", "put" };
	TEST_EXPECT(service.transfers == pushed);
	if (service.transfers != pushed)
		for (const std::string &call : service.transfers) std::fprintf(stderr, "  push call %s\n", call.c_str());
	service.transfers.clear();
	{
		editor::ContentPull pull(service, account(""), editor::ProjectPaths::for_root(second), "", { kServer, "MOD", 0 });
		pull.limit_batches(0, 25);
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done && files_of(second) == files_of(first));
	}
	const std::vector<std::string> pulled = { "where:2", "get", "get", "where:1", "get", "where:2", "get", "get" };
	TEST_EXPECT(service.transfers == pulled);
	if (service.transfers != pulled)
		for (const std::string &call : service.transfers) std::fprintf(stderr, "  pull call %s\n", call.c_str());
	// A batch of one file at most: five asks, each before its download.
	service.transfers.clear();
	const std::string third = dir.file("Third");
	{
		editor::ContentPull pull(service, account(""), editor::ProjectPaths::for_root(third), "", { kServer, "MOD", 0 });
		pull.limit_batches(1, editor::ContentJob::kSignedBatchBytes);
		TEST_EXPECT(run(pull) == editor::ContentJobState::Done);
	}
	const std::vector<std::string> single = { "where:1", "get", "where:1", "get", "where:1", "get", "where:1", "get",
		"where:1", "get" };
	TEST_EXPECT(service.transfers == single);
	return 0;
}

static int test_restore() {
	test_temp::TempDir dir("opennova_content_client_restore");
	const std::string root = dir.file("Folder");
	FakeService service;
	TEST_EXPECT(put(root + "/project.opennova", "{}") && put(root + "/defs/a.def", "a1") &&
			put(root + "/defs/b.def", "b1"));
	TEST_EXPECT(push(service, root, "r1") == editor::PushOutcome::Pushed);
	TEST_EXPECT(put(root + "/defs/a.def", "a2"));
	TEST_EXPECT(push(service, root, "r2") == editor::PushOutcome::Pushed && service.head == 2);
	const auto restore = [&](std::vector<std::string> paths, int64_t revision) {
		editor::ContentPull job(service, account(kAdminCookie), editor::ProjectPaths::for_root(root),
				"", binding_of(root), editor::ContentRestore{ std::move(paths), revision });
		const editor::ContentJobState state = run(job);
		if (state != editor::ContentJobState::Done)
			std::fprintf(stderr, "restore: %s\n", job.error().c_str());
		return state == editor::ContentJobState::Done ? job.applied().trash.id : uint64_t(-1);
	};

	// Discard: a's change undone, the new c gone, the deleted b back.
	TEST_EXPECT(put(root + "/defs/a.def", "a3") && put(root + "/defs/c.def", "c"));
	std::error_code ec;
	std::filesystem::remove(io::os_path(root + "/defs/b.def"), ec);
	const uint64_t trashed = restore({ "defs/a.def", "defs/b.def", "defs/c.def" }, 0);
	TEST_EXPECT(trashed != uint64_t(-1) && trashed != 0);
	std::map<std::string, std::string> files = files_of(root);
	TEST_EXPECT(files["defs/a.def"] == "a2" && files["defs/b.def"] == "b1" && !files.count("defs/c.def"));
	TEST_EXPECT(binding_of(root).revision == 2);

	// a as revision 1 has it: a change to push, the base still r2's.
	TEST_EXPECT(restore({ "defs/a.def" }, 1) != uint64_t(-1));
	files = files_of(root);
	TEST_EXPECT(files["defs/a.def"] == "a1" && binding_of(root).revision == 2);
	TEST_EXPECT(push(service, root, "a back") == editor::PushOutcome::Pushed && service.head == 3);
	// Nothing to put back: no trash batch.
	TEST_EXPECT(restore({ "defs/a.def" }, 0) == 0);
	std::printf("restore: changes discarded, a file back as revision 1 has it, then pushed\n");
	return 0;
}

static int test_accounts() {
	test_temp::TempDir dir("opennova_content_client_accounts");
	const std::string root = dir.file("Folder");
	FakeService service;
	TEST_EXPECT(put(root + "/defs/a.def", "a"));

	// A player may read but not push.
	std::string error;
	push(service, root, "x", kPlayerCookie, &error);
	TEST_EXPECT(error.find("may not push") != std::string::npos && service.head == 0);

	// A renewed session: kept by the account the job leaves.
	service.renew_next = true;
	editor::ContentPush renewed(service, account(kAdminCookie),
			editor::ProjectPaths::for_root(root), "", binding_of(root), "renewed");
	TEST_EXPECT(run(renewed) == editor::ContentJobState::Done &&
			renewed.outcome() == editor::PushOutcome::Pushed);
	const editor::ContentAccountUpdate update = renewed.take_account();
	TEST_EXPECT(update.changed && update.account.cookie == kRenewedCookie &&
			update.account.signed_in());
	TEST_EXPECT(!renewed.take_account().changed); // taken once

	// An ended session: the job fails and the account is signed out.
	TEST_EXPECT(put(root + "/defs/a.def", "b"));
	editor::ContentPush expired(service, account(kExpiredCookie),
			editor::ProjectPaths::for_root(root), "", binding_of(root), "expired");
	TEST_EXPECT(run(expired) == editor::ContentJobState::Failed &&
			expired.error().find("sign in again") != std::string::npos);
	const editor::ContentAccountUpdate ended = expired.take_account();
	TEST_EXPECT(ended.changed && !ended.account.signed_in() && ended.account.server == kServer);

	// The kept account's file: with none kept, the build's service alone; once kept, the account.
	editor::FileContentAccountStore store(dir.file("content_account.json"), "staging.test/");
	TEST_EXPECT(store.load().server == "https://staging.test" && !store.load().signed_in());
	TEST_EXPECT(store.save(update.account, error));
	const editor::ContentAccount kept = store.load();
	TEST_EXPECT(kept.server == kServer && kept.cookie == kRenewedCookie &&
			kept.username == update.account.username);
	std::printf("accounts: a player refused, a renewed session kept, an ended one signed out, the "
				"account's file\n");
	return 0;
}

static int test_bind_and_check() {
	test_temp::TempDir dir("opennova_content_client_bind");
	const std::string first = dir.file("First"), kept = dir.file("Kept");
	FakeService service;
	TEST_EXPECT(put(first + "/defs/a.def", "a1") && put(first + "/defs/b.def", "b1"));
	TEST_EXPECT(push(service, first, "r1") == editor::PushOutcome::Pushed);
	TEST_EXPECT(put(first + "/defs/a.def", "a2"));
	TEST_EXPECT(push(service, first, "r2") == editor::PushOutcome::Pushed && service.head == 2);

	// A folder kept before the store, holding revision 1's files and one of its own: bound at the
	// head, its files untouched, what differs from the head its change.
	TEST_EXPECT(put(kept + "/defs/a.def", "a1") && put(kept + "/defs/b.def", "b1") &&
			put(kept + "/defs/c.def", "c1"));
	{
		editor::ContentBind bind(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(kept), { kServer, "MOD", 0 });
		TEST_EXPECT(run(bind) == editor::ContentJobState::Done && bind.binding().revision == 2);
	}
	TEST_EXPECT(binding_of(kept).revision == 2 && files_of(kept).at("defs/a.def") == "a1");
	{
		editor::ContentCheck check(
				service, account(""), editor::ProjectPaths::for_root(kept), "", binding_of(kept));
		TEST_EXPECT(run(check) == editor::ContentJobState::Done && check.remote_known() &&
				check.remote().head == 2);
		const auto &changes = check.changes();
		TEST_EXPECT(changes.size() == 2 && changes[0].path == "defs/a.def" &&
				changes[0].change == editor::ContentChange::Modified &&
				changes[1].path == "defs/c.def" &&
				changes[1].change == editor::ContentChange::Added);
	}
	// Pushed from there: revision 3 is the kept folder's.
	TEST_EXPECT(push(service, kept, "kept") == editor::PushOutcome::Pushed && service.head == 3);
	// The store reset (a NovaWorld database reset: its head 0, the folder still at revision 3): a
	// pull and a push each say to bind again, neither moving anything.
	{
		FakeService reset;
		editor::ContentPull pull(
				reset, account(""), editor::ProjectPaths::for_root(kept), "", binding_of(kept));
		TEST_EXPECT(run(pull) == editor::ContentJobState::Failed &&
				pull.error().find("bind the project again") != std::string::npos);
		std::string error;
		push(reset, kept, "after the reset", kAdminCookie, &error);
		TEST_EXPECT(error.find("bind the project again") != std::string::npos && reset.head == 0 &&
				binding_of(kept).revision == 3);
	}

	// A check whose service will not answer still reads the folder's changes.
	TEST_EXPECT(put(kept + "/defs/b.def", "b2"));
	{
		editor::ContentCheck check(service, account(kExpiredCookie),
				editor::ProjectPaths::for_root(kept), "", binding_of(kept));
		TEST_EXPECT(run(check) == editor::ContentJobState::Done && !check.remote_known() &&
				!check.remote_error().empty() && check.changes().size() == 1);
	}
	// A project with no revision yet: bound at none, every file the first push's.
	FakeService empty;
	const std::string fresh = dir.file("Fresh");
	TEST_EXPECT(put(fresh + "/defs/a.def", "a"));
	{
		editor::ContentBind bind(empty, account(kAdminCookie),
				editor::ProjectPaths::for_root(fresh), { kServer, "MOD", 0 });
		TEST_EXPECT(run(bind) == editor::ContentJobState::Done && bind.binding().revision == 0);
	}
	TEST_EXPECT(push(empty, fresh, "first") == editor::PushOutcome::Pushed && empty.head == 1);
	std::printf("bind and check: a kept folder bound at the head, its changes read, offline too\n");
	return 0;
}

// A push sends every file, so it refuses a folder it could not read whole, NovaWorld's head unmoved:
// one its walk could not list (the folder gone, as a drive taken away; a folder the walk may list
// but not look into) and one holding a file it could not read (two paths one file on Windows and
// macOS, where the file system holds both) (F-2). Over the folder not listed whole, a pull refuses
// too and a check counts no file the list lacks as deleted (C-11). The cases a system cannot make
// (case-insensitive names; no search permission enforced, as for root or on Windows) say so.
static int test_incomplete_folder() {
	test_temp::TempDir dir("opennova_content_client_incomplete");
	const std::string root = dir.file("Project");
	FakeService service;
	TEST_EXPECT(put(root + "/project.opennova", "{}") && put(root + "/defs/a.def", "a1") &&
			put(root + "/locked/c.def", "c1"));
	TEST_EXPECT(push(service, root, "r1") == editor::PushOutcome::Pushed && service.head == 1);
	const editor::ContentBinding bound = binding_of(root);
	TEST_EXPECT(bound.revision == 1);
	std::string error;

	{
		editor::ContentPush gone(service, account(kAdminCookie),
				editor::ProjectPaths::for_root(dir.file("Gone")), "", bound, "gone");
		TEST_EXPECT(run(gone) == editor::ContentJobState::Failed &&
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
			editor::ContentCheck check(
					service, account(""), editor::ProjectPaths::for_root(root), "", bound);
			TEST_EXPECT(run(check) == editor::ContentJobState::Done && !check.problems().empty());
			TEST_EXPECT(check.changes().size() == 1 && check.changes()[0].path == "defs/a.def" &&
					check.changes()[0].change == editor::ContentChange::Modified);
		}
		{
			// A pull from before the head, over the same folder.
			editor::ContentPull pull(service, account(""), editor::ProjectPaths::for_root(root), "",
					{ kServer, "MOD", 0 });
			TEST_EXPECT(run(pull) == editor::ContentJobState::Failed &&
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
	TEST_EXPECT(editor::content_server(" opennova.net/ ") == "https://opennova.net");
	TEST_EXPECT(editor::content_server("http://127.0.0.1:8080") == "http://127.0.0.1:8080");
	const editor::ContentRequest login = editor::content_api::login("opennova.net", "taylor", "pw");
	TEST_EXPECT(login.url == "https://opennova.net/api/login" && login.method == "POST");
	editor::ContentResponse reply = json(200, io::JsonValue::make_object());
	reply.body = "{\"id\": 7, \"username\": \"taylor\", \"role\": \"admin\"}";
	reply.headers = { { "Set-Cookie", "other=1; Path=/" },
		{ "set-cookie", "__Host-opennova_session=tok; Path=/; Secure; HttpOnly" } };
	editor::ContentAccount signed_in;
	signed_in.server = "https://opennova.net";
	editor::ContentApiError error;
	TEST_EXPECT(editor::content_api::read_login(reply, signed_in, error) &&
			signed_in.cookie == "__Host-opennova_session=tok" && signed_in.admin());
	// An upload the service's route takes carries the session; one to the store's own URL does not.
	editor::ContentResponse missing = json(200, io::JsonValue::make_object());
	missing.body = "{\"missing\": [{\"sha256\": \"" + std::string(64, 'a') +
			"\", \"size\": 3, \"upload\": {\"method\": \"PUT\", \"url\": "
			"\"/api/content/projects/JO/blobs/x\", "
			"\"headers\": {}}}, {\"sha256\": \"" +
			std::string(64, 'b') +
			"\", \"size\": 4, \"upload\": {\"method\": \"PUT\", \"url\": "
			"\"https://bucket.s3/key?sig=1\", "
			"\"headers\": {\"x-amz-checksum-sha256\": \"abc\"}}}]}";
	std::vector<editor::UploadTarget> targets;
	TEST_EXPECT(editor::content_api::read_missing(missing, signed_in, targets, error) &&
			targets.size() == 2);
	const auto has = [](const editor::ContentRequest &r, const std::string &name) {
		for (const auto &[key, value] : r.headers)
			if (key == name)
				return true;
		return false;
	};
	TEST_EXPECT(targets[0].request.url == "https://opennova.net/api/content/projects/JO/blobs/x" &&
			has(targets[0].request, "Cookie") && has(targets[0].request, "X-OpenNova-Request"));
	TEST_EXPECT(targets[1].request.url == "https://bucket.s3/key?sig=1" &&
			!has(targets[1].request, "Cookie") && has(targets[1].request, "x-amz-checksum-sha256"));
	std::printf("api: the server's spelling, the login's session, an upload's headers by whose URL "
				"it is\n");
	return 0;
}

// The Shell's transport's rules (godot/src/authoring/content_http, over editor::content_address and
// content_goes_again): the URLs it reaches, and which request goes again on a fresh connection.
static int test_transport_rules() {
	editor::ContentAddress at;
	TEST_EXPECT(editor::content_address("https://opennova.net/api/content/projects?x=1#top", at) &&
			at.tls && at.host == "opennova.net" && at.port == 443 &&
			at.target == "/api/content/projects?x=1" && at.origin() == "https://opennova.net:443");
	TEST_EXPECT(editor::content_address("http://127.0.0.1:8080", at) && !at.tls &&
			at.host == "127.0.0.1" && at.port == 8080 && at.target == "/");
	TEST_EXPECT(editor::content_address("https://bucket.s3?sig=1", at) && at.target == "/?sig=1");
	TEST_EXPECT(editor::content_address("https://host#only", at) && at.target == "/");
	for (const char *refused : { "ftp://host/x", "opennova.net/api", "https://user@host/",
				 "http://[::1]:80/", "http://host:0/", "http://host:65536/", "http://host:x/",
				 "https:///path", "" })
		TEST_EXPECT(!editor::content_address(refused, at));

	// A GET, PUT or DELETE goes again after a broken connection or a 5xx, three more times; a POST
	// (a push, a sign-in) never does; a reply below 500, or a transfer that stalled, is the answer.
	TEST_EXPECT(editor::kContentRetries == 3);
	TEST_EXPECT(editor::content_goes_again("GET", 0, false, 0, true));
	TEST_EXPECT(editor::content_goes_again("PUT", 2, true, 503, false));
	TEST_EXPECT(editor::content_goes_again("DELETE", 1, true, 500, false));
	TEST_EXPECT(!editor::content_goes_again("PUT", 3, true, 503, false));
	TEST_EXPECT(!editor::content_goes_again("GET", 3, false, 0, true));
	TEST_EXPECT(!editor::content_goes_again("POST", 0, false, 0, true));
	TEST_EXPECT(!editor::content_goes_again("POST", 0, true, 503, false));
	TEST_EXPECT(!editor::content_goes_again("GET", 0, true, 200, false));
	TEST_EXPECT(!editor::content_goes_again("GET", 0, true, 404, false));
	TEST_EXPECT(!editor::content_goes_again("GET", 0, false, 0, false)); // stalled, given up
	TEST_EXPECT(!editor::content_goes_again("GET", 0, false, 503, false)); // no whole reply
	std::printf("transport: the URLs it reaches, a request that may go again\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_share_and_pull();
	failures += test_signed_batches();
	failures += test_restore();
	failures += test_accounts();
	failures += test_bind_and_check();
	failures += test_incomplete_folder();
	failures += test_api_shapes();
	failures += test_transport_rules();
	if (failures == 0)
		std::printf("content_client: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
