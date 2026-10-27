// The OpenNova Editor's NovaWorld client (engine/editor/novaworld, ADR 0069) against the
// service's own project routes (apps/novaworld_web/products/project_routes.h), so the two halves of
// the contract are one: the in-process HttpListener over a migrated database, a Redis namespace and
// a local object store, and the editor's pull and push jobs over a transport on apps/common's HTTP
// client. An admin signs in through the client's own login request; a folder is pushed as the
// first revision of a private project; a second folder gets it (the same files); the first pushes
// a change, the second's push is told its base is stale, pulls (the change taken, its own kept) and
// pushes both; the first pulls them; and a player's push is refused.

#include "core/local_object_store.h"
#include "http_listener.h"
#include "net_http.h"
#include "net_sockets.h"
#include "program/server_config.h"
#include "accounts/auth.h"

#include <base/io/file_io.h>
#include <base/io/sha256.h>
#include <editor/novaworld/novaworld_api.h>
#include <editor/novaworld/novaworld_jobs.h>
#include <editor/novaworld/novaworld_scan.h>
#include <editor/novaworld/novaworld_state.h>
#include <editor/project/project_document.h>
#include <net/novaworld/db/database.h>

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace editor = opennova::editor;
namespace io = opennova::io;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;

namespace {

// The editor's transport over apps/common's blocking HTTP client: each request made as it starts.
class HttpTransport : public editor::NovaWorldTransport {
public:
	uint64_t start(editor::NovaWorldRequest request) override {
		std::vector<std::string> headers;
		for (const auto &[name, value] : request.headers)
			headers.push_back(name + ": " + value);
		std::string body = request.body;
		std::string error;
		if (!request.body_file.empty())
			io::read_file_text(request.body_file, body, error);
		const net::HttpReply reply = net::http_exchange(request.method, request.url, headers, body,
				30000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
		editor::NovaWorldResponse out;
		out.transport_ok = reply.transport_ok;
		out.error = reply.error;
		out.status = reply.code;
		for (const std::string &line : reply.headers) {
			const auto colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			size_t value = colon + 1;
			while (value < line.size() && line[value] == ' ')
				++value;
			out.headers.emplace_back(line.substr(0, colon), line.substr(value));
		}
		const std::string bytes(reply.body.begin(), reply.body.end());
		if (request.response_file.empty()) {
			out.body = bytes;
		} else if (reply.transport_ok && reply.code == 200) {
			io::write_file_atomic(request.response_file, bytes, error);
			out.file_bytes = bytes.size();
			out.file_sha256 = io::sha256_hex(bytes.data(), bytes.size());
		} else {
			out.body = bytes;
		}
		const uint64_t id = next_++;
		ready_[id] = std::move(out);
		return id;
	}
	bool poll(uint64_t id, editor::NovaWorldResponse &out) override {
		const auto it = ready_.find(id);
		if (it == ready_.end())
			return false;
		out = std::move(it->second);
		ready_.erase(it);
		return true;
	}
	void cancel(uint64_t id) override { ready_.erase(id); }
	size_t parallel() const override { return 2; }

private:
	uint64_t next_ = 1;
	std::map<uint64_t, editor::NovaWorldResponse> ready_;
};

bool put(const std::string &path, const std::string &bytes) {
	return test_io::write_file(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

// The folder's working copy on the service at `server`: its state, else JO at no revision.
editor::NovaWorldWorkingCopy copy_of(const std::string &root, const std::string &server) {
	editor::NovaWorldWorkingCopy copy;
	std::string error;
	if (!editor::read_novaworld_state(
				editor::NovaWorldPaths::for_project(editor::ProjectPaths::for_root(root), server),
				copy, error))
		copy = { "JO", 0 };
	return copy;
}

editor::NovaWorldJobState run(editor::NovaWorldJob &job) {
	editor::NovaWorldJobState state;
	int steps = 0;
	while ((state = job.step(1 << 16)) == editor::NovaWorldJobState::Running && ++steps < 100000) {
	}
	if (state == editor::NovaWorldJobState::Failed)
		std::fprintf(stderr, "  the job failed: %s\n", job.error().c_str());
	return state;
}

std::map<std::string, std::string> files_of(const std::string &root) {
	std::map<std::string, std::string> out;
	const editor::NovaWorldScan scan = editor::novaworld_scan(editor::ProjectPaths::for_root(root), "");
	for (const auto &file : scan.files()) {
		std::string bytes, error;
		io::read_file_text(root + "/" + file.path, bytes, error);
		out[file.path] = bytes;
	}
	return out;
}

// An account with `role` signed in through the client's own login request.
bool signed_in(ConnectionPool &pool, HttpTransport &transport, const std::string &server,
		const std::string &username, const char *role, editor::NovaWorldAccount &account) {
	static int next = 0;
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", 0xc11e0000u + static_cast<unsigned>(next++));
	nws::CreateUserParams p;
	p.username = username;
	p.password = "content-client-pw1";
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = username;
	const auto made = nws::create_user(*pool.acquire(), p);
	if (!made.ok)
		return false;
	pool.acquire()->exec(
			"UPDATE players SET role = $1 WHERE id = $2;", { BindValue(role), BindValue(made.id) });
	editor::NovaWorldCall call(
			transport, editor::novaworld_api::login(server, username, "content-client-pw1"));
	if (!call.poll())
		return false;
	account.server = editor::novaworld_server(server);
	editor::NovaWorldApiError error;
	if (!editor::novaworld_api::read_login(call.reply(), account, error)) {
		std::fprintf(stderr, "  login: %s\n", error.line().c_str());
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 0;
	{
		test_temp::TempDir temp("novaworld_client_routes");
		test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
		ConnectionPool pool(database.options());
		test_redis::TestRedis redis;
		nws::LocalObjectStore store(temp.file("store"));

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.web_host = "127.0.0.1";
		config.admin_api_token = "content-client-routes-token";
		config.http_port = 0;
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);

		nws::HttpListener http(pool, redis.pool());
		http.set_object_store(&store);
		TEST_EXPECT(http.start(config));
		const std::string server = "http://127.0.0.1:" + std::to_string(http.bound_port());

		HttpTransport transport;
		editor::NovaWorldAccount admin, player;
		TEST_EXPECT(signed_in(pool, transport, server, "client_admin", "admin", admin) &&
				admin.admin());
		TEST_EXPECT(signed_in(pool, transport, server, "client_player", "player", player) &&
				!player.admin());

		const std::string first = temp.file("First"), second = temp.file("Second");
		const auto paths = [](const std::string &root) {
			return editor::ProjectPaths::for_root(root);
		};

		// The first push of a folder, then a second folder getting it.
		TEST_EXPECT(put(first + "/project.opennova", "{}") && put(first + "/defs/a.def", "a1") &&
				put(first + "/art/b.tga", std::string(70000, 'b')));
		{
			editor::NovaWorldPush job(
					transport, admin, paths(first), "", copy_of(first, server), "Stock import");
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done &&
					job.outcome() == editor::PushOutcome::Pushed && job.revision() == 1);
		}
		{
			editor::NovaWorldPull job(transport, admin, paths(second), "", { "JO", 0 });
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done && job.revision() == 1);
		}
		TEST_EXPECT(files_of(second) == files_of(first) && files_of(second).size() == 3);

		// A change each: the second's push stale until it pulls the first's.
		TEST_EXPECT(put(first + "/defs/a.def", "a2"));
		{
			editor::NovaWorldPush job(
					transport, admin, paths(first), "", copy_of(first, server), "a2");
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done && job.revision() == 2);
		}
		TEST_EXPECT(put(second + "/missions/c.bms", "c"));
		{
			editor::NovaWorldPush job(
					transport, admin, paths(second), "", copy_of(second, server), "c");
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done &&
					job.outcome() == editor::PushOutcome::Stale && job.head() == 2);
		}
		{
			editor::NovaWorldPull job(transport, admin, paths(second), "", copy_of(second, server));
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done && job.revision() == 2 &&
					job.applied().written == 1);
		}
		{
			editor::NovaWorldPush job(
					transport, admin, paths(second), "", copy_of(second, server), "c");
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done && job.revision() == 3);
		}
		{
			editor::NovaWorldPull job(transport, admin, paths(first), "", copy_of(first, server));
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Done && job.revision() == 3);
		}
		TEST_EXPECT(files_of(first) == files_of(second) &&
				files_of(first).at("defs/a.def") == "a2" &&
				files_of(first).at("missions/c.bms") == "c");

		// A player reads no private project, and pushes none.
		TEST_EXPECT(put(first + "/defs/a.def", "a3"));
		{
			editor::NovaWorldPush job(
					transport, player, paths(first), "", copy_of(first, server), "no");
			TEST_EXPECT(run(job) == editor::NovaWorldJobState::Failed);
		}
		std::printf("novaworld_client_routes: the editor's client and the service's routes agree\n");
		http.stop();
	}
	net::shutdown();
	return rc;
}
