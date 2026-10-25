// The content store's client (engine/editor/content_store/content_jobs.h, content_api.h, ADR 0069)
// over an in-memory service behind the transport seam, which keeps the routes' contract
// (apps/novaworld_web/content/content_routes.h: the head, the manifests, the files by SHA-256, the
// missing files with their uploads, the push on its parent), driving real folders: an admin's
// first push; a second folder getting the project, the same files; each pushing a change, the
// second told its base is stale, then pulling (the first's change taken, its own kept) and pushing
// both; a conflict settled by keeping one's own, pushed, and pulled by the other; a folder at its
// base with nothing to push; a player refused; a renewed session kept by the account; and an ended
// one signing it out. The routes themselves answer the same client over HTTP in
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
#include <base/io/sha256.h>
#include <editor/content_store/content_api.h>
#include <editor/content_store/content_jobs.h>
#include <editor/content_store/content_state.h>
#include <editor/project/project_document.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

namespace editor = opennova::editor;
namespace io = opennova::io;
using opennova::content::ContentFile;

namespace {

constexpr const char *kServer = "https://content.test";
constexpr const char *kAdminCookie = "opennova_session=admin";
constexpr const char *kRenewedCookie = "opennova_session=renewed";
constexpr const char *kPlayerCookie = "opennova_session=player";
constexpr const char *kExpiredCookie = "opennova_session=expired";

// `bytes` written at `path` as they are (test_io::write_text stages a .def's line ends as CRLF),
// its folders made.
bool put(const std::string &path, const std::string &bytes) {
	return test_io::write_file(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

editor::ContentResponse json(int status, const io::JsonValue &body) {
	editor::ContentResponse r;
	r.transport_ok = true;
	r.status = status;
	r.body = io::json_write(body);
	return r;
}

editor::ContentResponse refusal(int status, const char *code) {
	io::JsonValue body = io::JsonValue::make_object();
	body.set("error", io::json_string(code));
	return json(status, body);
}

// The service, in memory: one project, its manifests and files.
class FakeService : public editor::ContentTransport {
public:
	int64_t head = 0;
	std::map<int64_t, std::string> manifests; // by revision
	std::map<std::string, std::string> blobs; // by SHA-256
	bool renew_next = false; // the next reply renews the admin's session
	size_t uploads = 0, downloads = 0;

	uint64_t start(editor::ContentRequest request) override {
		const uint64_t id = next_++;
		ready_[id] = handle(request);
		return id;
	}
	bool poll(uint64_t id, editor::ContentResponse &out) override {
		const auto it = ready_.find(id);
		if (it == ready_.end())
			return false;
		out = std::move(it->second);
		ready_.erase(it);
		return true;
	}
	void cancel(uint64_t id) override { ready_.erase(id); }
	size_t parallel() const override { return 3; }

private:
	editor::ContentResponse handle(const editor::ContentRequest &request) {
		std::string cookie;
		for (const auto &[name, value] : request.headers)
			if (name == "Cookie")
				cookie = value;
		if (cookie == kExpiredCookie)
			return refusal(401, "unauthorized");
		const bool admin = cookie == kAdminCookie || cookie == kRenewedCookie;
		editor::ContentResponse reply = route(request, admin);
		if (renew_next && admin) {
			reply.headers.emplace_back(
					"Set-Cookie", std::string(kRenewedCookie) + "; Path=/; HttpOnly");
			renew_next = false;
		}
		return reply;
	}

	editor::ContentResponse route(const editor::ContentRequest &request, bool admin) {
		const std::string prefix = std::string(kServer) + "/api/content/projects/MOD";
		if (request.url.rfind(prefix, 0) != 0)
			return refusal(404, "not_found");
		const std::string rest = request.url.substr(prefix.size());
		if (request.method == "GET" && rest.empty()) {
			io::JsonValue project = io::JsonValue::make_object();
			project.set("key", io::json_string("MOD"));
			project.set("title", io::json_string("A mod"));
			project.set("visibility", io::json_string("public"));
			project.set("head", io::json_number(double(head)));
			project.set("can_push", io::JsonValue::make_bool(admin));
			io::JsonValue body = io::JsonValue::make_object();
			body.set("project", project);
			if (head > 0) {
				io::JsonValue revision = io::JsonValue::make_object();
				revision.set("number", io::json_number(double(head)));
				revision.set("manifest_sha256",
						io::json_string(opennova::content::manifest_sha256(manifests[head])));
				body.set("head_revision", revision);
			}
			return json(200, body);
		}
		if (request.method == "GET" && rest.rfind("/revisions/", 0) == 0) {
			const int64_t number = std::strtoll(rest.c_str() + 11, nullptr, 10);
			editor::ContentResponse r;
			r.transport_ok = true;
			r.status = manifests.count(number) ? 200 : 404;
			r.body = manifests[number];
			return r;
		}
		if (request.method == "GET" && rest.rfind("/blobs/", 0) == 0) {
			const auto blob = blobs.find(rest.substr(7));
			if (blob == blobs.end())
				return refusal(404, "not_found");
			editor::ContentResponse r;
			r.transport_ok = true;
			r.status = 200;
			std::string error;
			io::write_file_atomic(request.response_file, blob->second, error);
			r.file_bytes = blob->second.size();
			r.file_sha256 = io::sha256_hex(blob->second.data(), blob->second.size());
			++downloads;
			return r;
		}
		if (!admin)
			return refusal(403, "forbidden");
		io::JsonValue body;
		std::string error;
		if (request.method == "POST" && rest == "/missing" &&
				io::json_parse(request.body, body, error)) {
			io::JsonValue list = io::JsonValue::make_array();
			for (const io::JsonValue &item : body.get("blobs")->array) {
				const std::string sha = item.get_string("sha256", "");
				if (blobs.count(sha))
					continue;
				io::JsonValue upload = io::JsonValue::make_object();
				upload.set("method", io::json_string("PUT"));
				upload.set("url", io::json_string("/api/content/projects/MOD/blobs/" + sha));
				io::JsonValue entry = io::JsonValue::make_object();
				entry.set("sha256", io::json_string(sha));
				entry.set("size", *item.get("size"));
				entry.set("upload", upload);
				list.push(entry);
			}
			io::JsonValue out = io::JsonValue::make_object();
			out.set("missing", list);
			return json(200, out);
		}
		if (request.method == "PUT" && rest.rfind("/blobs/", 0) == 0) {
			std::string bytes;
			if (!io::read_file_text(request.body_file, bytes, error))
				return refusal(400, "unreadable");
			if (io::sha256_hex(bytes.data(), bytes.size()) != rest.substr(7))
				return refusal(400, "sha256_mismatch");
			blobs[rest.substr(7)] = bytes;
			++uploads;
			return json(200, io::JsonValue::make_object());
		}
		if (request.method == "POST" && rest == "/revisions" &&
				io::json_parse(request.body, body, error)) {
			const int64_t parent = int64_t(body.get_number("parent", -1));
			const std::string text = body.get_string("manifest", "");
			if (parent != head) {
				io::JsonValue stale = io::JsonValue::make_object();
				stale.set("error", io::json_string("stale"));
				stale.set("head", io::json_number(double(head)));
				return json(409, stale);
			}
			std::vector<ContentFile> files;
			if (!opennova::content::decode_manifest(text, files, error))
				return refusal(400, "invalid_manifest");
			for (const ContentFile &file : files)
				if (!blobs.count(file.sha256))
					return refusal(400, "missing_blobs");
			manifests[++head] = text;
			io::JsonValue out = io::JsonValue::make_object();
			out.set("head", io::json_number(double(head)));
			return json(201, out);
		}
		return refusal(404, "not_found");
	}

	uint64_t next_ = 1;
	std::map<uint64_t, editor::ContentResponse> ready_;
};

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
	std::printf("accounts: a player refused, a renewed session kept, an ended one signed out\n");
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

int main() {
	int failures = 0;
	failures += test_share_and_pull();
	failures += test_accounts();
	failures += test_api_shapes();
	if (failures == 0)
		std::printf("content_client: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
