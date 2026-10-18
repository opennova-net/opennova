// Every log on the site (apps/novaworld_web/infra/log_routes.h), driven in-process: a
// database of the run's own, a Redis namespace of its own and three Crow HttpListeners on
// ports the OS picks: the local stack's over a directory log store, a deployment's over
// CloudWatch behind a stub CommandRunner (infra_aws_stub.h), and one with no store. Rows are
// planted as the programs leave them: a hosted server with two starts (each naming its
// stream, hosted_server_starts), a machine whose hostd names its stream
// (fleet_machines.agent_log_stream), a program's sampled task.
//
//   - refused without an admin; every malformed parameter 400; no such server, start,
//     machine, program or task 404;
//   - the directory: a server's newest start and an earlier one by its number, the
//     machine's hostd, a program's task and every task of it, each line's time (with
//     milliseconds) or null, a page and the one before it by the cursor, a text filter, a
//     cursor of the other kind 400;
//   - CloudWatch: the stream the start's row names in its group, AWS's refusal a 502 in its
//     own words and no credential;
//   - no store: 503 no_log_store.

#include "auth.h"
#include "hosted/fleet_repository.h"
#include "http_listener.h"
#include "infra/log_store.h"
#include "net_http.h"
#include "net_sockets.h"
#include "server_config.h"

#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "infra_aws_stub.h"

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <base/io/file_io.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace infra = opennova::novaworld_server::infra;
namespace io = opennova::io;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kAdminToken = "log-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer log-routes-admin-token";

struct Rows {
	int64_t server = 0;
	int64_t machine = 0;
};

struct Harness {
	uint16_t local_port = 0;
	uint16_t cloud_port = 0;
	uint16_t none_port = 0;
	Rows rows;
	infra_stub::AwsStub *stub = nullptr;

	net::HttpReply get(uint16_t port, const std::string &query, bool admin = true) const {
		const std::string url =
				"http://127.0.0.1:" + std::to_string(port) + "/api/admin/logs?" + query;
		std::vector<std::string> headers;
		if (admin)
			headers.push_back(kBearer);
		return net::http_exchange(
				"GET", url, headers, {}, 15000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

crow::json::rvalue json_of(const net::HttpReply &reply, int code) {
	if (!reply.transport_ok || reply.code != code) {
		std::fprintf(stderr, "  HTTP %d %s, want %d\n", reply.code, body_text(reply).c_str(), code);
		return {};
	}
	return crow::json::load(body_text(reply));
}

std::string str(const crow::json::rvalue &v) {
	return std::string(v.s());
}

bool is_null(const crow::json::rvalue &v) {
	return v.t() == crow::json::type::Null;
}

bool put_lines(const std::string &path, const std::vector<std::string> &lines) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
	std::string text;
	for (const std::string &l : lines)
		text += l + "\n";
	std::string error;
	return io::write_file_atomic(path, text, error);
}

Rows plant(Database &db, const std::string &logs) {
	Rows r;
	r.server = db.query("INSERT INTO hosted_servers (game_name, max_players, time_limit, "
						"kill_limit, max_score, respawn, start_delay, missions, generation) "
						"VALUES ('Logs', 16, 30, 0, 0, 0, 0, 'm.bms', 2) RETURNING id;")
					   .front()
					   .as_int(0)
					   .value_or(0);
	for (int64_t g = 1; g <= 2; ++g)
		db.exec("INSERT INTO hosted_server_starts (server_id, generation, machine_id, slot, "
				"log_stream) VALUES ($1, $2, 1, 0, $3);",
				{ BindValue(r.server), BindValue(g),
						BindValue(std::to_string(r.server) + "/g" + std::to_string(g)) });
	hosted::NewMachine machine;
	machine.provider = "docker";
	machine.kind = hosted::MachineKind::Shared;
	machine.slots = 4;
	r.machine = hosted::create_machine(db, machine).id;
	db.exec("UPDATE fleet_machines SET agent_log_stream = 'hostd/hostd/abcd1234' WHERE id = $1;",
			{ BindValue(r.machine) });
	db.exec("INSERT INTO program_samples (program, task_id, at, cpu_limit) VALUES ('web', "
			"'c0ffee123456', now(), 1);");

	const std::string server_dir = logs + "/servers/" + std::to_string(r.server);
	put_lines(server_dir + "/g1.log", { "2026-10-18T11:00:00.000Z the first start" });
	std::vector<std::string> second;
	for (int i = 0; i < 30; ++i)
		second.push_back("2026-10-18T12:00:" + std::string(i < 10 ? "0" : "") + std::to_string(i) +
				".250Z " +
				(i == 29 ? "serving on UDP 32768" : "booting step " + std::to_string(i)));
	second.push_back("a line with no time");
	put_lines(server_dir + "/g2.log", second);
	put_lines(logs + "/machines/" + std::to_string(r.machine) + "/hostd.log",
			{ "2026-10-18T12:00:00.000Z [hostd] agent up" });
	put_lines(logs + "/programs/web/c0ffee123456.log",
			{ "2026-10-18T12:00:01.000Z [boot] ready.", "2026-10-18T12:00:03.000Z [http] up" });
	put_lines(logs + "/programs/web/0ddba1100000.log",
			{ "2026-10-18T12:00:02.000Z [boot] another task" });
	return r;
}

int test_refusals(Harness &h) {
	TEST_EXPECT(h.get(h.local_port, "subject=server&id=1", false).code == 401);
	for (const char *bad :
			{ "subject=nope&id=1", "subject=server&id=1&limit=0", "subject=server&id=1&limit=501",
					"subject=server&id=1&q=a%22b", "subject=server&id=1&generation=x" }) {
		const auto reply = h.get(h.local_port, bad);
		TEST_EXPECT(reply.code == 400 && str(json_of(reply, 400)["error"]) == "invalid_field");
	}
	const std::string server = std::to_string(h.rows.server);
	for (const std::string &missing : { std::string("subject=server&id=999999"),
				 "subject=server&id=" + server + "&generation=3",
				 std::string("subject=machine&id=999999"), std::string("subject=program&id=nope"),
				 std::string("subject=program&id=web&task=deadbeef"),
				 std::string("subject=program&id=web&task=..%2Fetc") }) {
		const auto reply = h.get(h.local_port, missing);
		TEST_EXPECT(reply.code == 404 && str(json_of(reply, 404)["error"]) == "not_found");
	}
	return 0;
}

int test_directory(Harness &h) {
	const std::string server = std::to_string(h.rows.server);
	const auto reply = h.get(h.local_port, "subject=server&id=" + server + "&limit=20");
	bool no_store = false;
	for (const std::string &line : reply.headers)
		no_store = no_store || line == "Cache-Control: no-store";
	TEST_EXPECT(no_store);
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json);
	TEST_EXPECT(str(json["subject"]) == "server" && json["id"].i() == h.rows.server &&
			json["generation"].i() == 2 && is_null(json["task"]));
	TEST_EXPECT(str(json["source"]) == "file" &&
			str(json["stream"]) == "servers/" + server + "/g2.log");
	const auto &lines = json["lines"];
	TEST_EXPECT(lines.size() == 20);
	TEST_EXPECT(str(lines[19]["text"]) == "a line with no time" && is_null(lines[19]["at"]));
	TEST_EXPECT(str(lines[18]["text"]) == "serving on UDP 32768" &&
			str(lines[18]["at"]) == "2026-10-18T12:00:29.250Z");
	TEST_EXPECT(!is_null(json["before"]));
	// The page before it.
	const auto older = json_of(
			h.get(h.local_port,
					"subject=server&id=" + server + "&limit=20&before=" + str(json["before"])),
			200);
	TEST_EXPECT(older && older["lines"].size() == 11 && is_null(older["before"]) &&
			str(older["lines"][0]["text"]) == "booting step 0");
	// An earlier start, a filter, a cursor of the other kind.
	const auto first =
			json_of(h.get(h.local_port, "subject=server&id=" + server + "&generation=1"), 200);
	TEST_EXPECT(first && first["generation"].i() == 1 && first["lines"].size() == 1 &&
			str(first["lines"][0]["text"]) == "the first start");
	const auto found = json_of(h.get(h.local_port, "subject=server&id=" + server + "&q=UDP"), 200);
	TEST_EXPECT(found && found["lines"].size() == 1);
	TEST_EXPECT(h.get(h.local_port, "subject=server&id=" + server + "&before=t%3A5").code == 400);

	const auto machine = json_of(
			h.get(h.local_port, "subject=machine&id=" + std::to_string(h.rows.machine)), 200);
	TEST_EXPECT(machine && machine["lines"].size() == 1 &&
			str(machine["lines"][0]["text"]) == "[hostd] agent up");

	const auto task = json_of(h.get(h.local_port, "subject=program&id=web&task=c0ffee123456"), 200);
	TEST_EXPECT(task && str(task["task"]) == "c0ffee123456" && task["lines"].size() == 2 &&
			str(task["stream"]) == "programs/web/c0ffee123456.log");
	const auto every = json_of(h.get(h.local_port, "subject=program&id=web"), 200);
	TEST_EXPECT(every && every["lines"].size() == 3 && is_null(every["task"]) &&
			str(every["lines"][1]["text"]) == "[boot] another task");
	// A program with nothing written yet.
	const auto quiet = json_of(h.get(h.local_port, "subject=program&id=worker"), 200);
	TEST_EXPECT(quiet && quiet["lines"].size() == 0 && is_null(quiet["before"]));
	return 0;
}

int test_cloudwatch(Harness &h) {
	h.stub->clear();
	const std::string server = std::to_string(h.rows.server);
	const auto json = json_of(h.get(h.cloud_port, "subject=server&id=" + server), 200);
	TEST_EXPECT(json && str(json["source"]) == "cloudwatch" &&
			str(json["group"]) == "/opennova/prod/servers" &&
			str(json["stream"]) == server + "/g2");
	TEST_EXPECT(json["lines"].size() == 1 && str(json["lines"][0]["text"]) == "serving" &&
			str(json["lines"][0]["at"]) == "2026-10-18T12:00:00.500Z" &&
			str(json["before"]) == "g:b/7/s");
	bool named = false;
	for (const infra_stub::Call &c : h.stub->calls())
		if (c.key == "logs GetLogEvents")
			named = infra_stub::config_line(c.config, "data-raw")
							.find("\"logStreamName\":\"" + server + "/g2\"") != std::string::npos;
	TEST_EXPECT(named);

	h.stub->replies["logs GetLogEvents"] = infra_stub::answer(400,
			R"({"__type":"AccessDeniedException","message":"not authorized to perform: logs:GetLogEvents"})");
	const auto denied = h.get(h.cloud_port, "subject=machine&id=" + std::to_string(h.rows.machine));
	const auto refused = json_of(denied, 502);
	TEST_EXPECT(refused && str(refused["error"]) == "logs_unavailable" &&
			str(refused["message"]).find("AccessDeniedException") != std::string::npos);
	TEST_EXPECT(body_text(denied).find(infra_stub::kSecret) == std::string::npos);

	const auto none = json_of(h.get(h.none_port, "subject=server&id=" + server), 503);
	TEST_EXPECT(none && str(none["error"]) == "no_log_store" &&
			str(none["message"]).find("ONNET_LOG_STORE is unset") != std::string::npos);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("log_routes");
		test_temp::TempDir logs("log_routes_store");
		test_temp::TempDir scratch("log_routes_scratch");
		test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
		ConnectionPool pool(database.options());
		test_redis::TestRedis redis;

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.web_host = "127.0.0.1";
		config.admin_api_token = kAdminToken;
		config.http_port = 0;
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);

		infra::DirectoryLogStore directory(logs.root());
		infra_stub::AwsStub stub;
		stub.replies["credentials"] = infra_stub::answer(200, infra_stub::kCredentials);
		stub.replies["logs GetLogEvents"] = infra_stub::answer(200,
				R"({"events":[{"timestamp":1792324800500,"message":"serving\n","ingestionTime":1}],)"
				R"("nextForwardToken":"f/8/s","nextBackwardToken":"b/7/s"})");
		infra::AwsApiConfig aws;
		aws.region = "us-east-1";
		aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
		aws.scratch_dir = scratch.root();
		infra::CloudWatchLogStore cloudwatch(stub, aws, "/opennova/prod");

		nws::HttpListener local(pool, redis.pool());
		local.set_log_store(&directory, {});
		TEST_EXPECT(local.start(config));
		nws::HttpListener cloud(pool, redis.pool());
		cloud.set_log_store(&cloudwatch, {});
		TEST_EXPECT(cloud.start(config));
		nws::HttpListener none(pool, redis.pool());
		none.set_log_store(nullptr, "ONNET_LOG_STORE is unset");
		TEST_EXPECT(none.start(config));

		Harness harness;
		harness.local_port = local.bound_port();
		harness.cloud_port = cloud.bound_port();
		harness.none_port = none.bound_port();
		harness.stub = &stub;
		{
			auto db = pool.acquire();
			harness.rows = plant(*db, logs.root());
		}

		rc = 0;
		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "refusals", test_refusals },
			{ "directory", test_directory },
			{ "cloudwatch", test_cloudwatch },
		};
		for (const Case &c : cases) {
			std::printf("-- %s\n", c.name);
			if (rc == 0 && c.fn(harness) != 0) {
				std::fprintf(stderr, "FAIL: %s\n", c.name);
				rc = 1;
			}
		}
		none.stop();
		cloud.stop();
		local.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: every log on the site\n");
	return rc;
}
