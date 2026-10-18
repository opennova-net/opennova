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
//     machine with its slot holders' servers named, their ports and ticks (the fleet view's
//     holders without), its moves oldest first (the row's making's `from` null, a log
//     stream's note), its trees with their manifests' totals (null for a tree the service
//     does not hold);
//   - a server's `tick` in the admin's fleet view, never in its owner's JSON;
//   - GET /api/admin/attention: refused without an admin; the findings over these rows in
//     their order with their links (no lobby the danger until one registers; a server
//     whose data could not be fetched at its last starts, until one runs), no alarm
//     read on the local stack; a deployment's (a second listener over CloudWatch behind
//     a stub CommandRunner, infra_aws_stub.h): DescribeAlarms' form (the prefix, ALARM),
//     the firing alarms by name with their times, the 20 s cache, AWS's refusal a warn
//     finding in its own words and no credential.

#include "accounts/auth.h"
#include "hosted/hosted_routes.h"
#include "hosted/model/fleet_repository.h"
#include "http_listener.h"
#include "infra/alarm_store.h"
#include "net_http.h"
#include "net_sockets.h"
#include "program/server_config.h"

#include <net/novaworld/db/database.h>

#include <crow/json.h>
#include <net/novaworld/lobby_repository.h>

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

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace infra = opennova::novaworld_server::infra;
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
	uint16_t port = 0; // the local stack's: no alarm read
	uint16_t cloud_port = 0; // a deployment's: its alarms read through the stub
	Rows rows;
	infra_stub::AwsStub *stub = nullptr;
	int64_t *steady_ms = nullptr; // the cloud's alarms' cache clock

	net::HttpReply get(const std::string &path, bool admin = true) const {
		return get_at(port, path, admin);
	}
	net::HttpReply get_at(uint16_t at, const std::string &path, bool admin = true) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(at) + path;
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

// Two alarms in ALARM as DescribeAlarms answers them: one moved into ALARM before its
// reason's last update, one naming only its update.
constexpr const char *kAlarms =
		R"(<DescribeAlarmsResponse xmlns="http://monitoring.amazonaws.com/doc/2010-08-01/">)"
		R"(<DescribeAlarmsResult><MetricAlarms>)"
		R"(<member><AlarmName>opennova-prod-machines-stuck</AlarmName>)"
		R"(<AlarmDescription>A machine is stuck (the admin Attention list).</AlarmDescription>)"
		R"(<StateValue>ALARM</StateValue>)"
		R"(<StateTransitionedTimestamp>2026-10-18T12:00:00.250Z</StateTransitionedTimestamp>)"
		R"(<StateUpdatedTimestamp>2026-10-18T12:05:00.000Z</StateUpdatedTimestamp></member>)"
		R"(<member><AlarmName>opennova-prod-db-cpu</AlarmName><StateValue>ALARM</StateValue>)"
		R"(<StateUpdatedTimestamp>2026-10-18T11:00:00Z</StateUpdatedTimestamp></member>)"
		R"(</MetricAlarms></DescribeAlarmsResult>)"
		R"(<ResponseMetadata><RequestId>0f1e</RequestId></ResponseMetadata>)"
		R"(</DescribeAlarmsResponse>)";

int test_attention(Harness &h, ConnectionPool &pool) {
	TEST_EXPECT(h.get("/api/admin/attention", false).code == 401);
	auto reply = h.get("/api/admin/attention");
	auto json = json_of(reply, 200);
	TEST_EXPECT(json && no_store(reply));
	// No lobby takes clients (danger), a server on the reporting machine late (warn), a
	// machine on an older image (info, the fleet's).
	const auto &findings = json["findings"];
	TEST_EXPECT(findings.size() == 3);
	TEST_EXPECT(str(findings[0]["level"]) == "danger" && str(findings[0]["source"]) == "no_lobby" &&
			str(findings[0]["subject"]) == "The lobby" &&
			str(findings[0]["link"]) == "/admin/infrastructure/programs/lobby" &&
			is_null(findings[0]["since"]));
	TEST_EXPECT(str(findings[1]["level"]) == "warn" && str(findings[1]["source"]) == "late_ticks" &&
			str(findings[1]["link"]) ==
					"/admin/fleet/machines/" + std::to_string(h.rows.reporting) &&
			str(findings[1]["text"])
							.find("Server " + std::to_string(h.rows.server) + " (42 late)") !=
					std::string::npos);
	TEST_EXPECT(str(findings[2]["level"]) == "info" &&
			str(findings[2]["source"]) == "outdated_image" &&
			str(findings[2]["link"]) == "/admin/fleet?tab=roll");
	TEST_EXPECT(json["alarms_read"].t() == crow::json::type::False && json["alarms"].size() == 0);
	TEST_EXPECT(str(json["checked_at"]).size() == 20 && str(json["checked_at"]).back() == 'Z');

	// A lobby registers: no danger left.
	{
		auto db = pool.acquire();
		opennova::lobbydb::register_instance(*db, "127.0.0.1:7598");
	}
	json = json_of(h.get("/api/admin/attention"), 200);
	TEST_EXPECT(json && json["findings"].size() == 2);
	for (const auto &f : json["findings"])
		TEST_EXPECT(str(f["level"]) != "danger");

	// A server whose data could not be fetched at its last three starts, the first
	// 20 min ago (a start that ran before them ends the run back; one under way
	// after them breaks nothing): a warn on the server with its data set, the
	// trees its last start pinned and hostd's last reason. Once a start runs, none.
	int64_t unfetched = 0;
	{
		auto db = pool.acquire();
		unfetched = db->query("INSERT INTO hosted_servers (state, game_name, max_players, "
							  "time_limit, kill_limit, max_score, respawn, start_delay, missions, "
							  "generation, provider, data_set_id) VALUES ('provisioning', "
							  "'Unfetched', 16, 30, 0, 0, 0, 0, 'm.bms', 5, 'docker', (SELECT id "
							  "FROM data_sets WHERE display_name = 'Joint Operations' ORDER BY id "
							  "LIMIT 1)) RETURNING id;")
							.front()
							.as_int(0)
							.value_or(0);
		db->exec("INSERT INTO hosted_server_starts (server_id, generation, machine_id, slot, "
				 "base_tree_id, expansion_tree_id, running_at, ended_at, exit_code, cause, "
				 "reason) VALUES "
				 "($1, 1, 99, 0, 41, NULL, now() - interval '39 minutes', now() - interval '30 "
				 "minutes', 3, 'crash', ''), "
				 "($1, 2, 99, 0, 41, NULL, NULL, now() - interval '20 minutes', 125, "
				 "'data_unavailable', 'its data could not be fetched: first'), "
				 "($1, 3, 99, 1, 41, NULL, NULL, now() - interval '12 minutes', 125, "
				 "'data_unavailable', 'its data could not be fetched: second'), "
				 "($1, 4, 99, 2, 41, 42, NULL, now() - interval '5 minutes', 125, "
				 "'data_unavailable', 'its data could not be fetched: aws s3 sync exited 1: "
				 "AccessDenied'), "
				 "($1, 5, 99, 0, 41, 42, NULL, NULL, NULL, NULL, '');",
				{ BindValue(unfetched) });
	}
	json = json_of(h.get("/api/admin/attention"), 200);
	bool warned = false;
	for (const auto &f : json["findings"])
		warned = warned ||
				(str(f["source"]) == "data_unavailable" && str(f["level"]) == "warn" &&
						str(f["subject"]) == "Server " + std::to_string(unfetched) + " (Unfetched)" &&
						str(f["link"]) == "/admin/fleet/" + std::to_string(unfetched) &&
						str(f["text"]) ==
								"Its data could not be fetched at its last 3 starts, the first 20 "
								"min ago (Joint Operations, trees 41 and 42); hostd's last reason: "
								"aws s3 sync exited 1: AccessDenied.");
	TEST_EXPECT(json && warned);
	{
		auto db = pool.acquire();
		db->exec("UPDATE hosted_server_starts SET running_at = now() WHERE server_id = $1 AND "
				 "generation = 5;",
				{ BindValue(unfetched) });
	}
	json = json_of(h.get("/api/admin/attention"), 200);
	TEST_EXPECT(json && json["findings"].size() == 2);
	{
		auto db = pool.acquire();
		db->exec("DELETE FROM hosted_servers WHERE id = $1;", { BindValue(unfetched) });
	}
	return 0;
}

int test_alarms(Harness &h) {
	h.stub->clear();
	auto reply = h.get_at(h.cloud_port, "/api/admin/attention");
	auto json = json_of(reply, 200);
	TEST_EXPECT(json && json["alarms_read"].t() == crow::json::type::True);
	const auto &alarms = json["alarms"];
	TEST_EXPECT(alarms.size() == 2);
	TEST_EXPECT(str(alarms[0]["name"]) == "opennova-prod-db-cpu" &&
			str(alarms[0]["state_since"]) == "2026-10-18T11:00:00Z" &&
			str(alarms[0]["description"]).empty());
	TEST_EXPECT(str(alarms[1]["name"]) == "opennova-prod-machines-stuck" &&
			str(alarms[1]["state_since"]) == "2026-10-18T12:00:00Z" &&
			str(alarms[1]["description"]) == "A machine is stuck (the admin Attention list).");
	// The form: this deployment's alarms by their prefix, the firing ones alone.
	bool asked = false;
	for (const infra_stub::Call &c : h.stub->calls())
		if (c.key == "monitoring DescribeAlarms") {
			const std::string form = infra_stub::config_line(c.config, "data-raw");
			asked = form.find("Action=DescribeAlarms") != std::string::npos &&
					form.find("AlarmNamePrefix=opennova-prod-") != std::string::npos &&
					form.find("StateValue=ALARM") != std::string::npos &&
					form.find("AlarmTypes.member.1=MetricAlarm") != std::string::npos;
		}
	TEST_EXPECT(asked && h.stub->count("monitoring DescribeAlarms") == 1);
	// Within 20 s the read is answered again; past it, AWS is asked again.
	TEST_EXPECT(json_of(h.get_at(h.cloud_port, "/api/admin/attention"), 200));
	TEST_EXPECT(h.stub->count("monitoring DescribeAlarms") == 1);
	*h.steady_ms += infra::AlarmStore::kCacheMs + 1;
	h.stub->replies["monitoring DescribeAlarms"] = infra_stub::answer(403,
			R"(<ErrorResponse><Error><Type>Sender</Type><Code>AccessDenied</Code>)"
			R"(<Message>User is not authorized to perform: cloudwatch:DescribeAlarms</Message>)"
			R"(</Error><RequestId>1a2b</RequestId></ErrorResponse>)");
	reply = h.get_at(h.cloud_port, "/api/admin/attention");
	json = json_of(reply, 200);
	TEST_EXPECT(json && json["alarms_read"].t() == crow::json::type::False &&
			json["alarms"].size() == 0);
	TEST_EXPECT(h.stub->count("monitoring DescribeAlarms") == 2);
	bool told = false;
	for (const auto &f : json["findings"])
		told = told ||
				(str(f["subject"]) == "CloudWatch alarms" && str(f["level"]) == "warn" &&
						str(f["source"]) == "unknown" && is_null(f["link"]) &&
						str(f["text"]).find("AccessDenied") != std::string::npos);
	TEST_EXPECT(told);
	TEST_EXPECT(body_text(reply).find(infra_stub::kSecret) == std::string::npos &&
			body_text(reply).find(infra_stub::kToken) == std::string::npos);
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
		test_temp::TempDir scratch("fleet_health_routes_scratch");
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

		// The local stack's: no AWS, no alarm read.
		infra_stub::AwsStub stub;
		infra::AlarmStore local_alarms(stub, infra::AlarmConfig{});
		nws::HttpListener http(pool, redis.pool());
		http.set_fleet_release({ kAmiNew, kHostd });
		http.set_alarm_store(&local_alarms);
		TEST_EXPECT(http.start(config));
		// A deployment's: its alarms through the stub, on a clock the test steps.
		stub.replies["credentials"] = infra_stub::answer(200, infra_stub::kCredentials);
		stub.replies["monitoring DescribeAlarms"] = infra_stub::answer(200, kAlarms);
		infra::AlarmConfig alarm_config;
		alarm_config.aws.region = "us-east-1";
		alarm_config.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
		alarm_config.aws.scratch_dir = scratch.root();
		alarm_config.prefix = "opennova-prod-";
		int64_t steady_ms = 1000;
		infra::AlarmStore cloud_alarms(stub, alarm_config, [&steady_ms] { return steady_ms; });
		TEST_EXPECT(cloud_alarms.unavailable().empty() && !local_alarms.unavailable().empty());
		nws::HttpListener cloud(pool, redis.pool());
		cloud.set_fleet_release({ kAmiNew, kHostd });
		cloud.set_alarm_store(&cloud_alarms);
		TEST_EXPECT(cloud.start(config));

		Harness harness;
		harness.port = http.bound_port();
		harness.cloud_port = cloud.bound_port();
		harness.stub = &stub;
		harness.steady_ms = &steady_ms;
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
		std::printf("-- attention\n");
		if (rc == 0 && test_attention(harness, pool) != 0)
			rc = 1;
		std::printf("-- alarms\n");
		if (rc == 0 && test_alarms(harness) != 0)
			rc = 1;
		if (rc != 0)
			std::fprintf(stderr, "FAIL\n");
		cloud.stop();
		http.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: the fleet's health on the admin's pages\n");
	return rc;
}
