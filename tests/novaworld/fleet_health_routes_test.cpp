// The fleet's health on the admin's pages, driven in-process: a database of the run's own,
// a Redis namespace of its own and a Crow HttpListener on a port the OS picks, the release
// pinning a launch template image and a hostd image. Rows are planted as the fleet leaves
// them: machines made, launched and synced (fleet_repository.h), one reporting its use,
// image, log stream and trees, its minutes (fleet_machine_samples), a static address it
// carries, a server running on its slot with its ticks.
//
//   - the machine JSON (hosted/fleet_routes.h machine_json) in the fleet view: its image,
//     log stream, use with the network's rates between its two newest minutes (none
//     without a pair), the release's images and whether it runs others (true, false,
//     null), its static address, the live servers on its slots;
//   - GET /api/admin/fleet/machines/<id>: refused without an admin, 404 for no machine, the
//     machine, its moves oldest first (the row's making's `from` null, a log stream's note),
//     its trees with their manifests' totals (null for a tree the service does not hold);
//   - a server's `tick` in the admin's fleet view, never in its owner's JSON.

#include "auth.h"
#include "hosted/fleet_repository.h"
#include "hosted/hosted_routes.h"
#include "http_listener.h"
#include "net_http.h"
#include "net_sockets.h"
#include "server_config.h"

#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kAdminToken = "fleet-health-admin-token";
constexpr const char *kBearer = "Authorization: Bearer fleet-health-admin-token";
constexpr const char *kAmiNew = "ami-0aaaaaaaaaaaaaaa1";
constexpr const char *kAmiOld = "ami-0bbbbbbbbbbbbbbb2";
constexpr const char *kHostd = "ghcr.io/opennova-net/opennova-hostd@sha256:"
							   "1111111111111111111111111111111111111111111111111111111111111111";

struct Rows {
	int64_t reporting = 0; // ready, its use reported, on an old image, carrying an address
	int64_t current = 0; // ready, on the release's images, one minute sampled
	int64_t launching = 0; // nothing reported
	int64_t address = 0;
	int64_t tree = 0;
	int64_t server = 0;
};

struct Harness {
	uint16_t port = 0;
	Rows rows;

	net::HttpReply get(const std::string &path, bool admin = true) const {
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

bool no_store(const net::HttpReply &reply) {
	for (const std::string &line : reply.headers)
		if (line == "Cache-Control: no-store")
			return true;
	return false;
}

std::string str(const crow::json::rvalue &v) {
	return std::string(v.s());
}

bool is_null(const crow::json::rvalue &v) {
	return v.t() == crow::json::type::Null;
}

const crow::json::rvalue *by_id(const crow::json::rvalue &list, int64_t id) {
	for (const auto &item : list)
		if (item["id"].i() == id)
			return &item;
	return nullptr;
}

// A machine launched and synced once (ready), its agent on `agent_image`.
int64_t ready_machine(Database &db, const std::string &cloud_id, const std::string &agent_image) {
	hosted::NewMachine machine;
	machine.provider = "docker";
	machine.kind = hosted::MachineKind::Shared;
	machine.slots = 4;
	const int64_t id = hosted::create_machine(db, machine).id;
	hosted::LaunchedMachine launched;
	launched.cloud_id = cloud_id;
	launched.private_address = "10.0.1.7";
	launched.public_address = "198.51.100.7";
	launched.instance_type = "t3a.medium";
	if (!hosted::mark_launched(db, id, launched) ||
			!hosted::record_sync(db, id, "test-agent", {}, agent_image))
		return 0;
	return id;
}

Rows plant(Database &db) {
	Rows r;
	r.reporting = ready_machine(db, "local-health-1", kHostd);
	r.current = ready_machine(db, "local-health-2", kHostd);
	hosted::NewMachine machine;
	machine.provider = "docker";
	machine.kind = hosted::MachineKind::Own;
	machine.slots = 1;
	r.launching = hosted::create_machine(db, machine).id;

	// A tree the service holds (its manifest: 1,250 bytes), one it does not.
	r.tree = db.query("INSERT INTO data_set_trees (data_set_id, state, source, manifest) VALUES (1, "
					  "'ready', 'baseline', "
					  "'{\"files\":[{\"name\":\"resource.pff\",\"size\":1000},"
					  "{\"name\":\"localres.pff\",\"size\":250}]}'::jsonb) RETURNING id;")
					 .front()
					 .as_int(0)
					 .value_or(0);
	hosted::SyncRequest report;
	report.image_id = kAmiOld;
	report.log_stream = "hostd/hostd/0f1e2d3c";
	hosted::MachineStats stats;
	stats.cpu_percent = 41.5;
	stats.memory_used_bytes = 1000;
	stats.memory_total_bytes = 4000;
	stats.disk_free_bytes = 30;
	stats.disk_total_bytes = 100;
	stats.net_in_bytes = 61000;
	stats.net_out_bytes = 17000;
	report.machine = stats;
	report.trees[r.tree] = hosted::TreeReport{ hosted::TreeReportState::Ready, 1250, 1, "" };
	report.trees[999999] =
			hosted::TreeReport{ hosted::TreeReportState::Fetching, 10, 2, "timed out" };
	hosted::record_machine_report(db, r.reporting, report);
	hosted::SyncRequest current;
	current.image_id = kAmiNew;
	current.machine = stats;
	hosted::record_machine_report(db, r.current, current);
	// Their minutes: two a minute apart for the reporting one, one for the other.
	db.exec("INSERT INTO fleet_machine_samples (machine_id, at, net_in_bytes, net_out_bytes) "
			"VALUES "
			"($1, date_trunc('minute', now()) - interval '2 minutes', 1000, 5000), "
			"($1, date_trunc('minute', now()) - interval '1 minute', 61000, 17000), "
			"($2, date_trunc('minute', now()) - interval '1 minute', 61000, 17000);",
			{ BindValue(r.reporting), BindValue(r.current) });

	r.address = db.query("INSERT INTO hosted_addresses (name, source, state, public_ip) VALUES "
						 "('smoke', 'comp', 'ready', '198.51.100.7') RETURNING id;")
						.front()
						.as_int(0)
						.value_or(0);
	db.exec("UPDATE fleet_machines SET address_id = $2 WHERE id = $1;",
			{ BindValue(r.reporting), BindValue(r.address) });

	// A server running on the reporting machine's slot 0, its agent reporting its ticks.
	r.server = db.query("INSERT INTO hosted_servers (state, game_name, max_players, time_limit, "
						"kill_limit, max_score, respawn, start_delay, missions, machine_ref, slot, "
						"game_port, generation, provider, machine_id) VALUES ('running', 'Ticks', "
						"16, 30, 0, 0, 0, 0, 'm.bms', $1, 0, 32768, 1, 'docker', 'srv-ticks-g1') "
						"RETURNING id;",
						 { BindValue(r.reporting) })
					   .front()
					   .as_int(0)
					   .value_or(0);
	hosted::ContainerReport running;
	running.name = "srv-ticks-g1";
	running.slot = 0;
	running.state = hosted::ContainerState::Running;
	running.tick = hosted::TickHealth{ 2.5, 14.25, 30.0, 42 };
	hosted::record_sync(db, r.reporting, "test-agent", { running }, kHostd);
	return r;
}

int test_fleet_view(Harness &h) {
	const auto reply = h.get("/api/admin/hosted");
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json && no_store(reply));
	const auto *reporting = by_id(json["machines"], h.rows.reporting);
	const auto *current = by_id(json["machines"], h.rows.current);
	const auto *launching = by_id(json["machines"], h.rows.launching);
	TEST_EXPECT(reporting && current && launching);

	// What its agent said of it, and its network's rates over its two newest minutes.
	const auto &m = *reporting;
	TEST_EXPECT(
			str(m["image_id"]) == kAmiOld && str(m["agent_log_stream"]) == "hostd/hostd/0f1e2d3c");
	TEST_EXPECT(str(m["stats_at"]).size() == 20 && str(m["stats_at"])[10] == 'T' &&
			str(m["stats_at"]).back() == 'Z');
	const auto &s = m["stats"];
	TEST_EXPECT(s["cpu_percent"].d() == 41.5 && s["memory_used_bytes"].i() == 1000 &&
			s["memory_total_bytes"].i() == 4000 && s["disk_free_bytes"].i() == 30 &&
			s["disk_total_bytes"].i() == 100 && s["net_in_bytes"].i() == 61000 &&
			s["net_out_bytes"].i() == 17000);
	TEST_EXPECT(s["net_in_bps"].d() == 1000.0 && s["net_out_bps"].d() == 200.0);
	TEST_EXPECT(
			str(m["release"]["image_id"]) == kAmiNew && str(m["release"]["agent_image"]) == kHostd);
	// Its image is not the launch templates': outdated, though its hostd is the release's.
	TEST_EXPECT(m["outdated"].t() == crow::json::type::True);
	TEST_EXPECT(m["address"]["id"].i() == h.rows.address && str(m["address"]["name"]) == "smoke");
	// The live servers on its slots, whatever the view's server cap.
	TEST_EXPECT(m["slots"].i() == 4 && m["slots_used"].i() == 1 && m["slot_holders"].size() == 1 &&
			m["slot_holders"][0]["slot"].i() == 0 &&
			m["slot_holders"][0]["server_id"].i() == h.rows.server &&
			str(m["slot_holders"][0]["state"]) == "running");
	// Its servers' names and ticks are the server list's here, its own page's alone.
	TEST_EXPECT(!m["slot_holders"][0].has("name") && !m["slot_holders"][0].has("tick"));
	TEST_EXPECT((*current)["slots_used"].i() == 0 && (*current)["slot_holders"].size() == 0);

	// One minute alone: no rate. Every image the release's: not outdated.
	TEST_EXPECT(is_null((*current)["stats"]["net_in_bps"]) &&
			is_null((*current)["stats"]["net_out_bps"]));
	TEST_EXPECT((*current)["outdated"].t() == crow::json::type::False &&
			is_null((*current)["address"]));
	// Nothing reported: no use, no image, outdated unknown.
	TEST_EXPECT(is_null((*launching)["stats"]) && is_null((*launching)["stats_at"]) &&
			is_null((*launching)["image_id"]) && is_null((*launching)["agent_log_stream"]) &&
			is_null((*launching)["outdated"]));

	// The server's last minute of ticks, in the admin's view alone.
	const auto *server = by_id(json["servers"], h.rows.server);
	TEST_EXPECT(server && (*server)["tick"]["p99_ms"].d() == 14.25 &&
			(*server)["tick"]["late"].i() == 42);
	return 0;
}

int test_owner_view(Harness &h, ConnectionPool &pool) {
	auto db = pool.acquire();
	const auto server = hosted::find_server(*db, h.rows.server);
	TEST_EXPECT(server.has_value());
	const auto owner = crow::json::load(hosted::server_json(*db, *server, false).dump());
	const auto admin = crow::json::load(hosted::server_json(*db, *server, true).dump());
	TEST_EXPECT(owner && admin && !owner.has("tick") && admin.has("tick"));
	return 0;
}

int test_machine_page(Harness &h) {
	const std::string path = "/api/admin/fleet/machines/" + std::to_string(h.rows.reporting);
	TEST_EXPECT(h.get(path, false).code == 401);
	const auto missing = json_of(h.get("/api/admin/fleet/machines/999999"), 404);
	TEST_EXPECT(missing && str(missing["error"]) == "not_found");

	const auto reply = h.get(path);
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json && no_store(reply));
	TEST_EXPECT(json["machine"]["id"].i() == h.rows.reporting &&
			str(json["machine"]["state"]) == "ready" &&
			json["machine"]["stats"]["net_in_bps"].d() == 1000.0);
	// Its slot holders with their servers' names, ports and last minute of ticks: the page
	// reads no fleet view.
	const auto &holders = json["machine"]["slot_holders"];
	TEST_EXPECT(holders.size() == 1 && holders[0]["server_id"].i() == h.rows.server &&
			str(holders[0]["name"]) == "Ticks" && holders[0]["game_port"].i() == 32768 &&
			holders[0]["tick"]["p99_ms"].d() == 14.25 && holders[0]["tick"]["late"].i() == 42);
	// Its moves, oldest first: its making, its launch, its first sync, its log stream.
	const auto &events = json["events"];
	TEST_EXPECT(events.size() == 4);
	TEST_EXPECT(is_null(events[0]["from"]) && str(events[0]["to"]) == "launching");
	TEST_EXPECT(str(events[1]["from"]) == "launching" && str(events[1]["to"]) == "booting");
	TEST_EXPECT(str(events[2]["from"]) == "booting" && str(events[2]["to"]) == "ready");
	TEST_EXPECT(str(events[3]["from"]) == "ready" && str(events[3]["to"]) == "ready" &&
			str(events[3]["detail"]) == "log stream hostd/hostd/0f1e2d3c");
	// Its trees by id: the one the service holds with its manifest's total, then one it
	// does not.
	const auto &trees = json["trees"];
	TEST_EXPECT(trees.size() == 2);
	TEST_EXPECT(trees[0]["tree_id"].i() == h.rows.tree && str(trees[0]["state"]) == "ready" &&
			trees[0]["bytes"].i() == 1250 && trees[0]["total_bytes"].i() == 1250 &&
			trees[0]["tries"].i() == 1 && is_null(trees[0]["error"]) &&
			!str(trees[0]["reported_at"]).empty());
	TEST_EXPECT(trees[1]["tree_id"].i() == 999999 && str(trees[1]["state"]) == "fetching" &&
			trees[1]["bytes"].i() == 10 && is_null(trees[1]["total_bytes"]) &&
			trees[1]["tries"].i() == 2 && str(trees[1]["error"]) == "timed out");

	// A machine with no move but its making and no tree.
	const auto launching =
			json_of(h.get("/api/admin/fleet/machines/" + std::to_string(h.rows.launching)), 200);
	TEST_EXPECT(launching && launching["events"].size() == 1 && launching["trees"].size() == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("fleet_health_routes");
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

		nws::HttpListener http(pool, redis.pool());
		http.set_fleet_release({ kAmiNew, kHostd });
		TEST_EXPECT(http.start(config));

		Harness harness;
		harness.port = http.bound_port();
		{
			auto db = pool.acquire();
			harness.rows = plant(*db);
		}
		TEST_EXPECT(
				harness.rows.reporting > 0 && harness.rows.current > 0 && harness.rows.server > 0);

		rc = 0;
		std::printf("-- fleet_view\n");
		if (test_fleet_view(harness) != 0)
			rc = 1;
		std::printf("-- owner_view\n");
		if (rc == 0 && test_owner_view(harness, pool) != 0)
			rc = 1;
		std::printf("-- machine_page\n");
		if (rc == 0 && test_machine_page(harness) != 0)
			rc = 1;
		if (rc != 0)
			std::fprintf(stderr, "FAIL\n");
		http.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: the fleet's health on the admin's pages\n");
	return rc;
}
