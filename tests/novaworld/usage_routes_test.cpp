// What everything uses (apps/novaworld_web/infra/usage_routes.h), driven in-process: a
// database of the run's own, a Redis namespace of its own and two Crow HttpListeners on
// ports the OS picks: the local stack's (no cloud read) and a deployment's, whose ECS and
// CloudWatch reads go to stub CommandRunners (infra_aws_stub.h). Rows are planted as the
// fleet and the programs leave them: a hosted server's minutes and hours (cumulative
// network counters, one starting over, a gap; tick health), a machine's, and the programs'
// tasks (web's reporting one, one gone an hour ago, the worker's), in migration 0016's and
// 0018's rows.
//
//   - refused without an admin; a subject, range, id or task that is none 400 or 404;
//   - a server: its slot's limits, its minutes with the rates between them (none across a
//     gap or where a counter went back), its tick health, its newest minute; its hours'
//     rates over the hour;
//   - a machine: its memory's limit, its disk, its hours;
//   - a program: its tasks added up minute by minute (the CPU of their CPUs together), one
//     task alone, the reporting tasks' limits;
//   - an AWS resource: none on the local stack (and why), a deployment's metrics as
//     CloudWatch gives them;
//   - the programs: each in order, running from the tasks reporting (desired and release
//     unknown) on the local stack and from ECS on a deployment's, what the reporting tasks
//     use now, the tasks gone in 24 h, each task with its ECS facts.

#include "auth.h"
#include "hosted/fleet_repository.h"
#include "http_listener.h"
#include "infra/infra_status.h"
#include "infra/metrics_store.h"
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

#include <cmath>
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
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kAdminToken = "usage-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer usage-routes-admin-token";
constexpr const char *kTaskA = "4444dddd4444dddd4444dddd4444dddd";

struct Rows {
	int64_t server = 0;
	int64_t machine = 0;
};

struct Harness {
	uint16_t local_port = 0;
	uint16_t cloud_port = 0;
	Rows rows;

	net::HttpReply get(uint16_t port, const std::string &path, bool admin = true) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
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

bool near(double a, double b) {
	return std::fabs(a - b) < 1e-3;
}

// `minutes` whole minutes before the database's now, as SQL.
std::string ago(int minutes) {
	return "date_trunc('minute', now()) - interval '" + std::to_string(minutes) + " minutes'";
}

Rows plant(Database &db) {
	Rows r;
	r.server = db.query("INSERT INTO hosted_servers (game_name, max_players, time_limit, "
						"kill_limit, max_score, respawn, start_delay, missions, generation, "
						"state) VALUES ('Usage', 16, 30, 0, 0, 0, 0, 'm.bms', 1, 'running') "
						"RETURNING id;")
					   .front()
					   .as_int(0)
					   .value_or(0);
	// Its minutes: 6000 bytes in a minute, then the counter starting over at 20 minutes
	// back, then a gap of 10 minutes; the newest a minute ago.
	struct Minute {
		int ago;
		int64_t in;
		int64_t out;
	};
	for (const Minute &m : std::vector<Minute>{ { 23, 1000, 500 }, { 22, 7000, 1100 },
				 { 21, 13000, 1700 }, { 20, 400, 100 }, { 19, 6400, 700 }, { 9, 66400, 6700 },
				 { 1, 66400 + 48000, 6700 + 4800 } })
		db.exec("INSERT INTO hosted_server_samples (server_id, at, state, cpu_percent, "
				"memory_bytes, net_in_bytes, net_out_bytes, tick_p50_ms, tick_p99_ms, "
				"tick_max_ms, late_ticks) VALUES ($1, " +
						ago(m.ago) + ", 'running', 25, 300000000, $2, $3, 4, 9.5, 21, 2);",
				{ BindValue(r.server), BindValue(m.in), BindValue(m.out) });
	db.exec("INSERT INTO hosted_server_hourly (server_id, hour, avg_cpu, peak_memory_bytes, "
			"net_in_bytes, net_out_bytes, tick_p99_max_ms, late_ticks) VALUES ($1, "
			"date_trunc('hour', now()) - interval '5 hours', 30, 310000000, 3600000, 360000, "
			"12.5, 40);",
			{ BindValue(r.server) });

	hosted::NewMachine machine;
	machine.provider = "docker";
	machine.kind = hosted::MachineKind::Shared;
	machine.slots = 4;
	r.machine = hosted::create_machine(db, machine).id;
	db.exec("UPDATE fleet_machines SET memory_total_bytes = 4000000000, disk_total_bytes = "
			"30000000000 WHERE id = $1;",
			{ BindValue(r.machine) });
	for (int k = 3; k >= 1; --k)
		db.exec("INSERT INTO fleet_machine_samples (machine_id, at, cpu_percent, "
				"memory_used_bytes, "
				"memory_total_bytes, disk_free_bytes, net_in_bytes, net_out_bytes, servers) VALUES "
				"($1, " +
						ago(k) + ", 40, 2000000000, 4000000000, $2, $3, $4, 1);",
				{ BindValue(r.machine), BindValue(int64_t{ 20000000000 } - k),
						BindValue(int64_t{ 600000 } - 60000 * k),
						BindValue(int64_t{ 1200000 } - 120000 * k) });
	db.exec("INSERT INTO fleet_machine_hourly (machine_id, hour, cpu_avg, cpu_max, "
			"memory_peak_bytes, disk_free_min_bytes, net_in_bytes, net_out_bytes) VALUES ($1, "
			"date_trunc('hour', now()) - interval '2 hours', 35, 80, 2100000000, 19000000000, "
			"7200, 72000);",
			{ BindValue(r.machine) });

	// The programs' tasks: web's reporting one (a), web's gone an hour ago (b), sharing a
	// minute 70 minutes back; the worker's.
	const auto sample = [&db](const char *program, const std::string &task, int minutes, double cpu,
								double limit, int64_t memory, int64_t in) {
		db.exec("INSERT INTO program_samples (program, task_id, at, cpu_percent, cpu_limit, "
				"memory_used_bytes, memory_limit_bytes, net_in_bytes, net_out_bytes) VALUES ($1, "
				"$2, " + ago(minutes) +
						", $3, $4, $5, $6, $7, $7);",
				{ BindValue(std::string(program)), BindValue(task), BindValue(cpu),
						BindValue(limit), BindValue(memory),
						BindValue(static_cast<int64_t>(limit * 2 * 1024 * 1024 * 1024)),
						BindValue(in) });
	};
	sample("web", kTaskA, 71, 10, 0.5, 400000000, 0);
	sample("web", kTaskA, 70, 50, 0.5, 400000000, 6000);
	sample("web", kTaskA, 2, 40, 0.5, 450000000, 100000);
	sample("web", kTaskA, 1, 60, 0.5, 460000000, 106000);
	sample("web", "bbbb1111", 70, 100, 0.25, 200000000, 9000);
	sample("worker", "cccc2222", 1, 5, 0.5, 100000000, 3000);
	db.exec("INSERT INTO program_hourly (program, task_id, hour, cpu_avg, cpu_max, "
			"memory_peak_bytes, net_in_bytes, net_out_bytes) VALUES ('web', $1, "
			"date_trunc('hour', now()) - interval '3 hours', 20, 70, 500000000, 360000, 36000), "
			"('web', 'bbbb1111', date_trunc('hour', now()) - interval '3 hours', 40, 90, "
			"250000000, 3600, 3600);",
			{ BindValue(std::string(kTaskA)) });
	return r;
}

int test_refusals(Harness &h) {
	TEST_EXPECT(h.get(h.local_port, "/api/admin/usage?subject=server&id=1", false).code == 401);
	TEST_EXPECT(h.get(h.local_port, "/api/admin/programs", false).code == 401);
	for (const char *bad : { "subject=nope&id=1", "subject=server&id=1&range=7d" })
		TEST_EXPECT(h.get(h.local_port, std::string("/api/admin/usage?") + bad).code == 400);
	for (const char *missing :
			{ "subject=server&id=999999", "subject=machine&id=999999", "subject=program&id=nope",
					"subject=program&id=web&task=deadbeef", "subject=infra&id=s3" })
		TEST_EXPECT(h.get(h.local_port, std::string("/api/admin/usage?") + missing).code == 404);
	return 0;
}

int test_server(Harness &h) {
	const std::string id = std::to_string(h.rows.server);
	const auto reply = h.get(h.local_port, "/api/admin/usage?subject=server&id=" + id);
	bool no_store = false;
	for (const std::string &line : reply.headers)
		no_store = no_store || line == "Cache-Control: no-store";
	TEST_EXPECT(no_store);
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json && str(json["subject"]) == "server" && json["id"].i() == h.rows.server &&
			str(json["range"]) == "48h" && json["step_seconds"].i() == 60);
	TEST_EXPECT(near(json["limit"]["cpu"].d(), 0.5) &&
			json["limit"]["memory_bytes"].i() == int64_t{ 1024 } * 1024 * 1024);
	const auto &s = json["series"];
	TEST_EXPECT(s.size() == 7);
	// The first has no rate; then 6000 B a minute in; the counter going back none; the
	// gaps of 10 and 8 minutes none.
	TEST_EXPECT(is_null(s[0]["net_in_bps"]) && near(s[1]["net_in_bps"].d(), 100) &&
			near(s[1]["net_out_bps"].d(), 10) && near(s[2]["net_in_bps"].d(), 100));
	TEST_EXPECT(is_null(s[3]["net_in_bps"]) && near(s[4]["net_in_bps"].d(), 100));
	TEST_EXPECT(is_null(s[5]["net_in_bps"]) && is_null(s[6]["net_in_bps"]));
	TEST_EXPECT(near(s[6]["cpu_percent"].d(), 25) && s[6]["memory_bytes"].i() == 300000000 &&
			near(s[6]["tick_p99_ms"].d(), 9.5) && s[6]["late_ticks"].i() == 2 &&
			!s[6].has("disk_free_bytes"));
	TEST_EXPECT(str(s[6]["at"]).back() == 'Z' && json["now"]["late_ticks"].i() == 2);

	const auto hours = json_of(
			h.get(h.local_port, "/api/admin/usage?subject=server&id=" + id + "&range=400d"), 200);
	TEST_EXPECT(hours && hours["step_seconds"].i() == 3600 && hours["series"].size() == 1);
	const auto &hour = hours["series"][0];
	TEST_EXPECT(near(hour["cpu_percent"].d(), 30) && hour["memory_bytes"].i() == 310000000 &&
			near(hour["net_in_bps"].d(), 1000) && near(hour["net_out_bps"].d(), 100) &&
			near(hour["tick_p99_ms"].d(), 12.5) && hour["late_ticks"].i() == 40);
	// Its newest minute even so.
	TEST_EXPECT(!is_null(hours["now"]) && near(hours["now"]["cpu_percent"].d(), 25));
	return 0;
}

int test_machine(Harness &h) {
	const std::string id = std::to_string(h.rows.machine);
	const auto json =
			json_of(h.get(h.local_port, "/api/admin/usage?subject=machine&id=" + id), 200);
	TEST_EXPECT(json && is_null(json["limit"]["cpu"]) &&
			json["limit"]["memory_bytes"].i() == 4000000000 &&
			json["disk_total_bytes"].i() == int64_t{ 30000000000 });
	const auto &s = json["series"];
	TEST_EXPECT(s.size() == 3 && is_null(s[0]["net_in_bps"]) &&
			near(s[1]["net_in_bps"].d(), 1000) && near(s[1]["net_out_bps"].d(), 2000) &&
			s[2]["disk_free_bytes"].i() == int64_t{ 19999999999 } && !s[2].has("tick_p99_ms"));
	TEST_EXPECT(json["now"]["disk_free_bytes"].i() == int64_t{ 19999999999 });
	const auto hours = json_of(
			h.get(h.local_port, "/api/admin/usage?subject=machine&id=" + id + "&range=400d"), 200);
	TEST_EXPECT(hours && hours["series"].size() == 1 &&
			near(hours["series"][0]["net_in_bps"].d(), 2) &&
			hours["series"][0]["disk_free_bytes"].i() == int64_t{ 19000000000 });
	return 0;
}

int test_program(Harness &h) {
	const auto json = json_of(h.get(h.local_port, "/api/admin/usage?subject=program&id=web"), 200);
	TEST_EXPECT(json && str(json["id"]) == "web" && is_null(json["task"]));
	// The limits: the reporting task's (a: half a CPU, 1 GiB).
	TEST_EXPECT(near(json["limit"]["cpu"].d(), 0.5) &&
			json["limit"]["memory_bytes"].i() == int64_t{ 1024 } * 1024 * 1024);
	const auto &s = json["series"];
	TEST_EXPECT(s.size() == 4);
	// 70 minutes back both tasks: 50 % of 0.5 and 100 % of 0.25 is 66.7 % of 0.75 CPUs.
	TEST_EXPECT(near(s[1]["cpu_percent"].d(), 50.0 * 0.5 / 0.75 + 100.0 * 0.25 / 0.75) &&
			s[1]["memory_bytes"].i() == 600000000 && near(s[1]["net_in_bps"].d(), 100));
	TEST_EXPECT(near(s[3]["cpu_percent"].d(), 60) && near(s[3]["net_in_bps"].d(), 100) &&
			near(json["now"]["cpu_percent"].d(), 60));
	const auto one = json_of(h.get(h.local_port,
									 std::string("/api/admin/usage?subject=program&id="
												 "web&task=") +
											 kTaskA),
			200);
	TEST_EXPECT(one && str(one["task"]) == kTaskA && one["series"].size() == 4 &&
			near(one["series"][1]["cpu_percent"].d(), 50) &&
			one["series"][1]["memory_bytes"].i() == 400000000);
	const auto hours =
			json_of(h.get(h.local_port, "/api/admin/usage?subject=program&id=web&range=400d"), 200);
	TEST_EXPECT(hours && hours["series"].size() == 1 &&
			near(hours["series"][0]["cpu_percent"].d(), 30) &&
			hours["series"][0]["memory_bytes"].i() == 750000000 &&
			near(hours["series"][0]["net_in_bps"].d(), 101));
	return 0;
}

int test_infra(Harness &h) {
	const auto local = json_of(h.get(h.local_port, "/api/admin/usage?subject=infra&id=rds"), 200);
	TEST_EXPECT(local && !local["available"].b() &&
			str(local["unavailable"]).find("AWS is not read here") != std::string::npos &&
			local["metrics"].size() == 0);
	const auto cloud = json_of(h.get(h.cloud_port, "/api/admin/usage?subject=infra&id=rds"), 200);
	TEST_EXPECT(cloud && cloud["available"].b() && is_null(cloud["unavailable"]) &&
			is_null(cloud["error"]) && cloud["period_seconds"].i() == 60);
	TEST_EXPECT(str(cloud["statistics"]["CPUUtilization"]) == "Average" &&
			cloud["metrics"]["CPUUtilization"].size() == 1 &&
			near(cloud["metrics"]["CPUUtilization"][0]["value"].d(), 7.5) &&
			str(cloud["metrics"]["CPUUtilization"][0]["at"]) == "2026-10-18T11:59:00Z" &&
			cloud["metrics"]["WriteIOPS"].size() == 0);
	return 0;
}

const crow::json::rvalue *program_named(const crow::json::rvalue &list, const char *name) {
	for (const auto &p : list)
		if (str(p["name"]) == name)
			return &p;
	return nullptr;
}

int test_programs(Harness &h) {
	const auto local = json_of(h.get(h.local_port, "/api/admin/programs"), 200);
	TEST_EXPECT(
			local && !local["cloud"]["configured"].b() && !is_null(local["cloud"]["unavailable"]));
	const auto &programs = local["programs"];
	TEST_EXPECT(programs.size() == infra::InfraConfig().programs.size() &&
			str(programs[0]["name"]) == infra::InfraConfig().programs[0]);
	const auto *web = program_named(programs, "web");
	TEST_EXPECT(web != nullptr);
	TEST_EXPECT((*web)["running"].i() == 1 && is_null((*web)["desired"]) &&
			is_null((*web)["release"]) && (*web)["restarts_24h"].i() == 1);
	TEST_EXPECT(near((*web)["cpu_percent"].d(), 60) && near((*web)["cpu_limit"].d(), 0.5) &&
			(*web)["memory_bytes"].i() == 460000000 && near((*web)["net_in_bps"].d(), 100));
	TEST_EXPECT((*web)["tasks"].size() == 1 && str((*web)["tasks"][0]["id"]) == kTaskA &&
			(*web)["tasks"][0]["reporting"].b() && is_null((*web)["tasks"][0]["last_status"]));
	const auto *gate = program_named(programs, "gate");
	TEST_EXPECT(gate != nullptr && (*gate)["running"].i() == 0 && is_null((*gate)["cpu_percent"]) &&
			(*gate)["tasks"].size() == 0);

	// A deployment's: ECS's counts and revisions, its tasks beside the reporting one.
	const auto cloud = json_of(h.get(h.cloud_port, "/api/admin/programs"), 200);
	TEST_EXPECT(cloud && cloud["cloud"]["configured"].b() && is_null(cloud["cloud"]["error"]));
	const auto *cweb = program_named(cloud["programs"], "web");
	TEST_EXPECT(cweb != nullptr);
	TEST_EXPECT((*cweb)["running"].i() == 1 && (*cweb)["desired"].i() == 2 &&
			str((*cweb)["release"]) == "opennova-prod-web:43" && !(*cweb)["on_release"].b());
	const auto &tasks = (*cweb)["tasks"];
	TEST_EXPECT(tasks.size() == 2);
	TEST_EXPECT(str(tasks[0]["id"]) == kTaskA && tasks[0]["reporting"].b() &&
			str(tasks[0]["last_status"]) == "RUNNING" && str(tasks[0]["health"]) == "HEALTHY" &&
			str(tasks[0]["task_definition"]) == "opennova-prod-web:42");
	TEST_EXPECT(str(tasks[1]["id"]) == "3333cccc3333cccc3333cccc3333cccc" &&
			!tasks[1]["reporting"].b() && is_null(tasks[1]["cpu_percent"]) &&
			str(tasks[1]["last_status"]) == "PENDING");
	const auto *cgate = program_named(cloud["programs"], "gate");
	TEST_EXPECT(cgate != nullptr && (*cgate)["on_release"].b() && (*cgate)["desired"].i() == 1);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("usage_routes");
		test_temp::TempDir scratch("usage_routes_scratch");
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

		infra_stub::AwsStub ecs;
		ecs.replies = infra_stub::deployment_replies();
		infra::InfraStatus local_status(infra::InfraConfig(), ecs);
		infra::InfraConfig cloud_config;
		cloud_config.aws.region = "us-east-1";
		cloud_config.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
		cloud_config.aws.scratch_dir = scratch.root();
		cloud_config.cluster = "opennova-prod";
		cloud_config.release_parameter = "/opennova/prod/release";
		infra::InfraStatus cloud_status(cloud_config, ecs);

		infra::MetricsStore local_metrics(ecs, infra::MetricsConfig());
		infra_stub::AwsStub monitoring;
		monitoring.replies["credentials"] = infra_stub::answer(200, infra_stub::kCredentials);
		monitoring.replies["monitoring GetMetricData"] = infra_stub::answer(200,
				R"(<GetMetricDataResponse><GetMetricDataResult><MetricDataResults>)"
				R"(<member><Id>m1</Id><Timestamps><member>2026-10-18T11:59:00Z</member></Timestamps>)"
				R"(<Values><member>7.5</member></Values><StatusCode>Complete</StatusCode></member>)"
				R"(</MetricDataResults></GetMetricDataResult></GetMetricDataResponse>)");
		infra::MetricsConfig metrics_config;
		metrics_config.aws = cloud_config.aws;
		metrics_config.database = "opennova-prod-db";
		infra::MetricsStore cloud_metrics(monitoring, metrics_config);

		nws::HttpListener local(pool, redis.pool());
		local.set_infra(&local_status);
		local.set_metrics_store(&local_metrics);
		local.set_fleet_slots(1024, 0.5);
		TEST_EXPECT(local.start(config));
		nws::HttpListener cloud(pool, redis.pool());
		cloud.set_infra(&cloud_status);
		cloud.set_metrics_store(&cloud_metrics);
		cloud.set_fleet_slots(1024, 0.5);
		TEST_EXPECT(cloud.start(config));

		Harness harness;
		harness.local_port = local.bound_port();
		harness.cloud_port = cloud.bound_port();
		{
			auto db = pool.acquire();
			harness.rows = plant(*db);
		}

		rc = 0;
		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "refusals", test_refusals },
			{ "server", test_server },
			{ "machine", test_machine },
			{ "program", test_program },
			{ "infra", test_infra },
			{ "programs", test_programs },
		};
		for (const Case &c : cases) {
			std::printf("-- %s\n", c.name);
			if (rc == 0 && c.fn(harness) != 0) {
				std::fprintf(stderr, "FAIL: %s\n", c.name);
				rc = 1;
			}
		}
		cloud.stop();
		local.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: what everything uses\n");
	return rc;
}
