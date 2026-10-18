// The admin's infrastructure view (apps/novaworld_web/infra/infra_routes.h),
// driven in-process: a database of the run's own (common/test_database.h), a
// Redis namespace of its own (common/test_redis.h), and two Crow HttpListeners
// from opennova_novaworld_web_core on ports the OS picks: the local stack's,
// whose InfraStatus reads no cloud, and a deployment's, whose reads go to a stub
// CommandRunner answering as AWS would mid-deploy (infra_aws_stub.h). Rows are
// planted as the programs would leave them: two lobby instances (one live with a
// session, one gone quiet and draining), the fleet's election, a ready
// fleet machine and an ended one. The cases: the route refused without an
// admin (401, 403 for a player); the local stack's view (the cloud unavailable,
// saying why, nothing called; the lobbies, the fleet leader and the machines from
// the database); the deployment's (every program with its counts, rollout,
// revision and whether it is the release's, its running and stopped tasks and
// events; the one-off task; the target groups' targets; the database and the
// cache; the machines' zones and EC2 states), read once for the polls within
// the cache window; and no reply carrying any credential.

#include "auth.h"
#include "hosted/fleet_repository.h"
#include "http_listener.h"
#include "infra/infra_status.h"
#include "net_http.h"
#include "net_sockets.h"
#include "program/server_config.h"
#include "web_session.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/lobby_repository.h>

#include <crow/json.h>

#include "infra_aws_stub.h"

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

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
namespace lobbydb = opennova::lobbydb;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kAdminToken = "infra-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer infra-routes-admin-token";

struct Harness {
	ConnectionPool &pool;
	uint16_t local_port = 0;
	uint16_t cloud_port = 0;
	std::string player_cookie;
	infra_stub::AwsStub *stub = nullptr;

	net::HttpReply get(uint16_t port, const std::vector<std::string> &headers) const {
		const std::string url =
				"http://127.0.0.1:" + std::to_string(port) + "/api/admin/infrastructure";
		return net::http_exchange(
				"GET", url, headers, {}, 10000, [](const std::string &host, net::Endpoint &out) {
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

bool holds_credential(const std::string &text) {
	return text.find(infra_stub::kSecret) != std::string::npos ||
			text.find(infra_stub::kToken) != std::string::npos ||
			text.find(infra_stub::kKeyId) != std::string::npos;
}

int64_t make_account(Database &db, const std::string &name, const char *role) {
	nws::CreateUserParams p;
	p.username = name;
	p.password = "pw-" + name;
	p.pcid = "1f0a0001";
	p.nwh = "1";
	p.nwhandle = name;
	const auto made = nws::create_user(db, p);
	if (!made.ok)
		return 0;
	db.exec("UPDATE players SET role = $1 WHERE id = $2;",
			{ BindValue(std::string(role)), BindValue(made.id) });
	return made.id;
}

std::string session_cookie(Database &db, int64_t id) {
	const auto user = nws::get_user_by_id(db, id);
	if (!user)
		return {};
	const auto token = nws::create_web_session(db, id, user->credentials_gen, "10.9.0.1", "test");
	return token ? std::string("Cookie: ") + nws::kWebSessionCookie + "=" + *token : std::string();
}

// The rows the programs would have left: two lobby instances, the fleet's
// election, a ready machine and an ended one. The live lobby's id.
int64_t plant(Database &db) {
	const int64_t live = lobbydb::register_instance(db, "54.162.79.147:64206");
	const int64_t quiet = lobbydb::register_instance(db, "54.162.79.147:64206");
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '2 minutes', state = "
			"'draining' WHERE id = $1;",
			{ BindValue(quiet) });
	lobbydb::SessionRow s;
	s.lobby_id = live;
	s.connection_id = 7;
	s.peer_ip = "10.1.2.3";
	s.peer_port = 40001;
	s.state = "active";
	s.pn = "NOVAWORLDUDP";
	s.identity = "jop:cus2";
	lobbydb::insert_session(db, s);
	db.exec("UPDATE fleet_leader SET holder = 'fleet-1234abcd', generation = 3, "
			"elected_at = now() - "
			"interval '1 hour' WHERE id = 1;");

	hosted::NewMachine shared;
	shared.provider = "ec2";
	shared.kind = hosted::MachineKind::Shared;
	shared.slots = 12;
	const int64_t ready = hosted::create_machine(db, shared).id;
	hosted::LaunchedMachine launched;
	launched.cloud_id = "i-0aaaaaaaaaaaaaaa1";
	launched.private_address = "10.40.1.40";
	launched.public_address = "3.3.3.40";
	launched.instance_type = "t3a.large";
	launched.template_version = "lt-0123456789abcdef0:4";
	hosted::mark_launched(db, ready, launched);
	hosted::transition_machine(
			db, ready, hosted::FleetMachineState::Booting, hosted::FleetMachineState::Ready, "");
	const int64_t ended = hosted::create_machine(db, shared).id;
	launched.cloud_id = "i-0bbbbbbbbbbbbbbb2";
	hosted::mark_launched(db, ended, launched);
	hosted::transition_machine(db, ended, hosted::FleetMachineState::Booting,
			hosted::FleetMachineState::Ended, "drained");
	return live;
}

const crow::json::rvalue *service_named(const crow::json::rvalue &services, const char *name) {
	for (const auto &s : services)
		if (str(s["name"]) == name)
			return &s;
	return nullptr;
}

int test_access(Harness &h) {
	TEST_EXPECT(h.get(h.local_port, {}).code == 401);
	TEST_EXPECT(h.get(h.cloud_port, { h.player_cookie }).code == 403);
	// Refused before AWS is asked anything.
	TEST_EXPECT(h.stub->calls().empty());
	return 0;
}

int test_local(Harness &h, int64_t live_lobby) {
	const auto reply = h.get(h.local_port, { kBearer });
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json);
	bool no_store = false;
	for (const std::string &line : reply.headers)
		no_store = no_store || line == "Cache-Control: no-store";
	TEST_EXPECT(no_store);
	TEST_EXPECT(json["at"].t() == crow::json::type::String && str(json["at"]).size() == 20);

	const auto &cloud = json["cloud"];
	TEST_EXPECT(!cloud["configured"].b());
	TEST_EXPECT(
			str(cloud["unavailable"]).find("ONNET_INFRA_CLUSTER is unset") != std::string::npos);
	TEST_EXPECT(!cloud.has("services") && !cloud.has("target_groups"));

	// The lobbies by id: the live one with its session, the quiet one draining.
	const auto &lobbies = json["lobbies"];
	TEST_EXPECT(lobbies.size() == 2);
	TEST_EXPECT(lobbies[0]["id"].i() == live_lobby &&
			str(lobbies[0]["endpoint"]) == "54.162.79.147:64206");
	TEST_EXPECT(str(lobbies[0]["state"]) == "active" && lobbies[0]["live"].b() &&
			lobbies[0]["sessions"].i() == 1 && lobbies[0]["listings"].i() == 0);
	TEST_EXPECT(str(lobbies[1]["state"]) == "draining" && !lobbies[1]["live"].b() &&
			lobbies[1]["seen_ago_seconds"].i() >= 110 && lobbies[1]["sessions"].i() == 0);
	TEST_EXPECT(str(lobbies[0]["last_seen_at"]).back() == 'Z');

	const auto &fleet = json["fleet"];
	TEST_EXPECT(str(fleet["leader"]) == "fleet-1234abcd" && fleet["generation"].i() == 3);
	TEST_EXPECT(str(fleet["elected_at"]).size() == 20);

	TEST_EXPECT(!json["hosting"]["enabled"].b() && str(json["hosting"]["provider"]) == "none");
	// The machines: the ready one, then the ended one; no zone without the cloud.
	const auto &machines = json["machines"];
	TEST_EXPECT(machines.size() == 2);
	TEST_EXPECT(str(machines[0]["state"]) == "ready" && str(machines[0]["kind"]) == "shared" &&
			machines[0]["slots"].i() == 12 && machines[0]["slots_used"].i() == 0 &&
			str(machines[0]["cloud_id"]) == "i-0aaaaaaaaaaaaaaa1" &&
			str(machines[0]["instance_type"]) == "t3a.large");
	TEST_EXPECT(str(machines[0]["launched_at"]).back() == 'Z' && is_null(machines[0]["ended_at"]));
	TEST_EXPECT(is_null(machines[0]["zone"]) && is_null(machines[0]["ec2_state"]));
	TEST_EXPECT(str(machines[1]["state"]) == "ended" &&
			str(machines[1]["end_reason"]) == "drained" && !is_null(machines[1]["ended_at"]));
	TEST_EXPECT(h.stub->calls().empty());
	return 0;
}

int test_cloud(Harness &h) {
	h.stub->clear();
	const auto reply = h.get(h.cloud_port, { kBearer });
	const std::string body = body_text(reply);
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json);
	TEST_EXPECT(!holds_credential(body));
	const auto &cloud = json["cloud"];
	TEST_EXPECT(
			cloud["configured"].b() && is_null(cloud["unavailable"]) && is_null(cloud["error"]));
	TEST_EXPECT(str(cloud["cluster"]) == "opennova-prod" && str(cloud["region"]) == "us-east-1");
	TEST_EXPECT(str(cloud["read_at"]).back() == 'Z');

	TEST_EXPECT(str(cloud["release"]["id"]) == "20261017T120000Z-abc1234" &&
			str(cloud["release"]["revision"]) == "abc1234def56" && !cloud["release_none"].b() &&
			is_null(cloud["release_error"]));

	const auto &services = cloud["services"];
	TEST_EXPECT(services.size() == 6 && is_null(cloud["services_error"]) &&
			cloud["missing_services"].size() == 0);
	const auto *gate = service_named(services, "gate");
	TEST_EXPECT(gate != nullptr);
	TEST_EXPECT((*gate)["desired"].i() == 1 && (*gate)["running"].i() == 1 &&
			(*gate)["pending"].i() == 0);
	TEST_EXPECT(
			str((*gate)["task_definition"]) == "opennova-prod-gate:7" && (*gate)["on_release"].b());
	TEST_EXPECT((*gate)["events"].size() == 3 &&
			str((*gate)["events"][0]["message"]) == "(service gate) has reached a steady state.");
	TEST_EXPECT((*gate)["tasks"].size() == 1 && (*gate)["stopped_tasks"].size() == 0);
	const auto &gate_task = (*gate)["tasks"][0];
	TEST_EXPECT(str(gate_task["id"]) == "1111aaaa1111aaaa1111aaaa1111aaaa" &&
			str(gate_task["health"]) == "HEALTHY" && str(gate_task["zone"]) == "us-east-1a" &&
			str(gate_task["private_ip"]) == "10.40.0.11" && str(gate_task["cpu"]) == "256" &&
			str(gate_task["memory"]) == "512" && gate_task["on_release"].b() &&
			str(gate_task["started_at"]) == "2026-10-17T10:40:00Z");
	TEST_EXPECT(gate_task["containers"].size() == 1 &&
			str(gate_task["containers"][0]["image_digest"]).rfind("sha256:0123", 0) == 0 &&
			is_null(gate_task["containers"][0]["exit_code"]));

	// Web mid-rollout: its new revision is not the release's yet.
	const auto *web = service_named(services, "web");
	TEST_EXPECT(web != nullptr);
	TEST_EXPECT(
			str((*web)["task_definition"]) == "opennova-prod-web:43" && !(*web)["on_release"].b());
	TEST_EXPECT((*web)["deployments"].size() == 2 &&
			str((*web)["deployments"][0]["status"]) == "PRIMARY" &&
			str((*web)["deployments"][0]["rollout_state"]) == "IN_PROGRESS" &&
			str((*web)["deployments"][0]["task_definition"]) == "opennova-prod-web:43" &&
			(*web)["deployments"][0]["failed"].i() == 1);
	TEST_EXPECT((*web)["tasks"].size() == 2);
	TEST_EXPECT(str((*web)["tasks"][0]["task_definition"]) == "opennova-prod-web:42" &&
			(*web)["tasks"][0]["on_release"].b());
	TEST_EXPECT(str((*web)["tasks"][1]["last_status"]) == "PENDING" &&
			!(*web)["tasks"][1]["on_release"].b() && is_null((*web)["tasks"][1]["started_at"]) &&
			str((*web)["tasks"][1]["health"]) == "UNKNOWN");
	TEST_EXPECT((*web)["stopped_tasks"].size() == 1);
	const auto &stopped = (*web)["stopped_tasks"][0];
	TEST_EXPECT(str(stopped["stopped_reason"]) == "Essential container in task exited" &&
			str(stopped["stop_code"]) == "EssentialContainerExited" &&
			stopped["containers"][0]["exit_code"].i() == 1);

	// The nightly backup, a one-off task.
	TEST_EXPECT(cloud["one_off_tasks"].size() == 1 &&
			str(cloud["one_off_tasks"][0]["group"]) == "family:opennova-prod-backup" &&
			is_null(cloud["one_off_tasks"][0]["program"]) &&
			is_null(cloud["one_off_tasks"][0]["on_release"]));
	TEST_EXPECT(cloud["one_off_stopped_tasks"].size() == 0 && is_null(cloud["tasks_error"]));

	// The balancer, in its order.
	const auto &groups = cloud["target_groups"];
	TEST_EXPECT(groups.size() == 4);
	TEST_EXPECT(str(groups[0]["key"]) == "gate" && str(groups[0]["name"]) == "opennova-prod-gate" &&
			groups[0]["targets"].size() == 1 &&
			str(groups[0]["targets"][0]["state"]) == "healthy" &&
			groups[0]["targets"][0]["port"].i() == 7597 && is_null(groups[0]["error"]));
	TEST_EXPECT(str(groups[3]["key"]) == "https" && groups[3]["targets"].size() == 2 &&
			str(groups[3]["targets"][1]["state"]) == "unhealthy" &&
			str(groups[3]["targets"][1]["reason"]) == "Target.FailedHealthChecks" &&
			is_null(groups[3]["targets"][0]["reason"]));

	TEST_EXPECT(str(cloud["database"]["status"]) == "available" &&
			cloud["database"]["multi_az"].b() &&
			str(cloud["database"]["instance_class"]) == "db.t4g.small" &&
			cloud["database"]["storage_gb"].i() == 20 &&
			str(cloud["database"]["secondary_zone"]) == "us-east-1b" &&
			is_null(cloud["database_error"]));
	TEST_EXPECT(str(cloud["cache"]["status"]) == "available" && cloud["cache"]["multi_az"].b() &&
			str(cloud["cache"]["automatic_failover"]) == "enabled" &&
			cloud["cache"]["nodes"].size() == 2 &&
			str(cloud["cache"]["nodes"][1]["role"]) == "replica" && is_null(cloud["cache_error"]));

	// The machines with EC2's view: the ended one's instance EC2 no longer lists.
	const auto &machines = json["machines"];
	TEST_EXPECT(machines.size() == 2);
	TEST_EXPECT(str(machines[0]["zone"]) == "us-east-1b" &&
			str(machines[0]["ec2_state"]) == "running" &&
			str(machines[0]["image_id"]) == "ami-0123456789abcdef0");
	TEST_EXPECT(is_null(machines[1]["zone"]) && is_null(machines[1]["ec2_state"]) &&
			is_null(machines[1]["image_id"]));
	TEST_EXPECT(is_null(cloud["machines_error"]));
	// No fleet cluster named (hosting off): no fleet section.
	TEST_EXPECT(is_null(cloud["fleet"]));

	// One read, every call once; both machines' ids asked about.
	TEST_EXPECT(h.stub->count("credentials") == 1 && h.stub->count("ecs DescribeServices") == 1);
	bool asked = false;
	for (const infra_stub::Call &c : h.stub->calls())
		if (c.key == "ec2 DescribeInstances")
			asked = c.config.find("Filter.1.Value.1=i-0aaaaaaaaaaaaaaa1&Filter.1.Value.2=i-"
								  "0bbbbbbbbbbbbbbb2") != std::string::npos;
	TEST_EXPECT(asked);

	// The page's next polls within the window ask AWS nothing.
	h.stub->clear();
	for (int i = 0; i < 3; ++i) {
		const auto again = h.get(h.cloud_port, { kBearer });
		TEST_EXPECT(again.code == 200 && !holds_credential(body_text(again)));
	}
	TEST_EXPECT(h.stub->calls().empty());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("infra_routes");
		test_temp::TempDir scratch("infra_routes_scratch");
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

		// The local stack's view: no cluster, so nothing is ever called.
		infra_stub::AwsStub stub;
		stub.replies = infra_stub::deployment_replies();
		infra::InfraStatus local_status(infra::InfraConfig(), stub);
		// A deployment's.
		infra::InfraConfig cloud_config;
		cloud_config.aws.region = "us-east-1";
		cloud_config.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
		cloud_config.aws.scratch_dir = scratch.root();
		cloud_config.cluster = "opennova-prod";
		const std::string tg = infra_stub::kTargetGroupArn;
		cloud_config.target_groups = { { "gate", tg + "opennova-prod-gate/0123456789abcdef" },
			{ "lobby", tg + "opennova-prod-lobby/0123456789abcdef" },
			{ "http", tg + "opennova-prod-http/0123456789abcdef" },
			{ "https", tg + "opennova-prod-https/0123456789abcdef" } };
		cloud_config.database = "opennova-prod-db";
		cloud_config.cache = "opennova-prod-cache";
		cloud_config.release_parameter = "/opennova/prod/release";
		cloud_config.cache_ms = 60 * 1000;
		infra::InfraStatus cloud_status(cloud_config, stub);

		nws::HttpListener local(pool, redis.pool());
		local.set_infra(&local_status);
		TEST_EXPECT(local.start(config));
		nws::HttpListener cloud(pool, redis.pool());
		cloud.set_infra(&cloud_status);
		TEST_EXPECT(cloud.start(config));

		Harness harness{ pool, local.bound_port(), cloud.bound_port(), {}, &stub };
		int64_t live_lobby = 0;
		{
			auto db = pool.acquire();
			const int64_t player = make_account(*db, "infra_player", "player");
			TEST_EXPECT(player > 0);
			harness.player_cookie = session_cookie(*db, player);
			TEST_EXPECT(!harness.player_cookie.empty());
			live_lobby = plant(*db);
		}

		rc = 0;
		std::printf("-- access\n");
		if (test_access(harness) != 0) {
			std::fprintf(stderr, "FAIL: access\n");
			rc = 1;
		}
		std::printf("-- local\n");
		if (rc == 0 && test_local(harness, live_lobby) != 0) {
			std::fprintf(stderr, "FAIL: local\n");
			rc = 1;
		}
		std::printf("-- cloud\n");
		if (rc == 0 && test_cloud(harness) != 0) {
			std::fprintf(stderr, "FAIL: cloud\n");
			rc = 1;
		}
		cloud.stop();
		local.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: the admin's infrastructure view\n");
	return rc;
}
