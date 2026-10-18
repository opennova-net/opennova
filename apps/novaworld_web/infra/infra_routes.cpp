#include "infra_routes.h"

#include "hosted/fleet_repository.h"
#include "hosted/stats_repository.h"
#include "http_json.h"

#include <net/novaworld/lobby_repository.h>

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

namespace {

// The database's UTC "YYYY-MM-DD HH:MM:SS" (fractions dropped) as ISO 8601; ""
// stays "".
std::string iso(const std::string &text) {
	if (text.size() < 19 || text[10] != ' ')
		return text;
	return text.substr(0, 10) + "T" + text.substr(11, 8) + "Z";
}

void iso_or_null(crow::json::wvalue &out, const char *key, const std::string &text) {
	text_or_null(out, key, iso(text));
}

std::string short_arn(const std::string &arn) {
	const size_t slash = arn.rfind('/');
	return slash == std::string::npos ? arn : arn.substr(slash + 1);
}

// Whether `arn` is the revision the release names for `program`; null when the
// release names none (none recorded, or it was not read).
void on_release(crow::json::wvalue &out, const CloudSnapshot &cloud, const std::string &program,
		const std::string &arn) {
	if (!cloud.release || program.empty()) {
		out["on_release"] = nullptr;
		return;
	}
	const auto it = cloud.release->task_definitions.find(program);
	if (it == cloud.release->task_definitions.end())
		out["on_release"] = nullptr;
	else
		out["on_release"] = it->second == arn;
}

crow::json::wvalue task_json(const CloudSnapshot &cloud, const EcsTask &t) {
	crow::json::wvalue out;
	out["id"] = t.id;
	text_or_null(out, "program", t.program);
	out["group"] = t.group;
	out["task_definition"] = short_arn(t.task_definition_arn);
	on_release(out, cloud, t.program, t.task_definition_arn);
	out["last_status"] = t.last_status;
	out["desired_status"] = t.desired_status;
	out["health"] = t.health.empty() ? std::string("UNKNOWN") : t.health;
	text_or_null(out, "created_at", t.created_at);
	text_or_null(out, "started_at", t.started_at);
	text_or_null(out, "stopping_at", t.stopping_at);
	text_or_null(out, "stopped_at", t.stopped_at);
	text_or_null(out, "stopped_reason", t.stopped_reason);
	text_or_null(out, "stop_code", t.stop_code);
	text_or_null(out, "zone", t.zone);
	text_or_null(out, "private_ip", t.private_ip);
	text_or_null(out, "cpu", t.cpu);
	text_or_null(out, "memory", t.memory);
	std::vector<crow::json::wvalue> containers;
	for (const EcsContainer &c : t.containers) {
		crow::json::wvalue j;
		j["name"] = c.name;
		j["image"] = c.image;
		text_or_null(j, "image_digest", c.image_digest);
		j["last_status"] = c.last_status;
		j["health"] = c.health.empty() ? std::string("UNKNOWN") : c.health;
		if (c.exit_code)
			j["exit_code"] = *c.exit_code;
		else
			j["exit_code"] = nullptr;
		text_or_null(j, "reason", c.reason);
		containers.push_back(std::move(j));
	}
	out["containers"] = std::move(containers);
	return out;
}

bool is_stopped(const EcsTask &t) {
	return t.desired_status == "STOPPED";
}

// The tasks of `program` ("" for the one-off ones): the running ones, then the
// newest kStoppedShown stopped ones (the snapshot lists them so).
std::pair<std::vector<crow::json::wvalue>, std::vector<crow::json::wvalue>> tasks_of(
		const CloudSnapshot &cloud, const std::string &program) {
	std::vector<crow::json::wvalue> running;
	std::vector<crow::json::wvalue> stopped;
	for (const EcsTask &t : cloud.tasks) {
		if (t.program != program)
			continue;
		if (!is_stopped(t))
			running.push_back(task_json(cloud, t));
		else if (stopped.size() < InfraRoutes::kStoppedShown)
			stopped.push_back(task_json(cloud, t));
	}
	return { std::move(running), std::move(stopped) };
}

crow::json::wvalue service_json(const CloudSnapshot &cloud, const EcsService &s) {
	crow::json::wvalue out;
	out["name"] = s.name;
	out["status"] = s.status;
	out["desired"] = s.desired;
	out["running"] = s.running;
	out["pending"] = s.pending;
	out["task_definition"] = short_arn(s.task_definition_arn);
	on_release(out, cloud, s.name, s.task_definition_arn);
	std::vector<crow::json::wvalue> deployments;
	for (const EcsDeployment &d : s.deployments) {
		crow::json::wvalue j;
		j["id"] = d.id;
		j["status"] = d.status;
		text_or_null(j, "rollout_state", d.rollout_state);
		text_or_null(j, "rollout_reason", d.rollout_reason);
		j["task_definition"] = short_arn(d.task_definition_arn);
		j["desired"] = d.desired;
		j["running"] = d.running;
		j["pending"] = d.pending;
		j["failed"] = d.failed;
		text_or_null(j, "created_at", d.created_at);
		text_or_null(j, "updated_at", d.updated_at);
		deployments.push_back(std::move(j));
	}
	out["deployments"] = std::move(deployments);
	std::vector<crow::json::wvalue> events;
	for (const EcsEvent &e : s.events) {
		crow::json::wvalue j;
		text_or_null(j, "at", e.at);
		j["message"] = e.message;
		events.push_back(std::move(j));
	}
	out["events"] = std::move(events);
	auto [running, stopped] = tasks_of(cloud, s.name);
	out["tasks"] = std::move(running);
	out["stopped_tasks"] = std::move(stopped);
	return out;
}

crow::json::wvalue cloud_json(const CloudSnapshot *cloud, const char *no_reader) {
	crow::json::wvalue out;
	if (cloud == nullptr || !cloud->unavailable.empty()) {
		out["configured"] = false;
		out["unavailable"] = cloud == nullptr ? std::string(no_reader) : cloud->unavailable;
		return out;
	}
	const CloudSnapshot &c = *cloud;
	out["configured"] = true;
	out["unavailable"] = nullptr;
	out["read_at"] = hosted::iso_utc(c.read_ms / 1000);
	out["cluster"] = c.cluster;
	out["region"] = c.region;
	text_or_null(out, "error", c.error);

	if (c.release) {
		crow::json::wvalue r;
		r["id"] = c.release->id;
		text_or_null(r, "created", c.release->created);
		text_or_null(r, "revision", c.release->revision);
		out["release"] = std::move(r);
	} else {
		out["release"] = nullptr;
	}
	out["release_none"] = c.release_none;
	text_or_null(out, "release_error", c.release_error);

	std::vector<crow::json::wvalue> services;
	for (const EcsService &s : c.services)
		services.push_back(service_json(c, s));
	out["services"] = std::move(services);
	std::vector<crow::json::wvalue> missing;
	for (const std::string &m : c.missing_services)
		missing.emplace_back(m);
	out["missing_services"] = std::move(missing);
	text_or_null(out, "services_error", c.services_error);
	auto [one_off, one_off_stopped] = tasks_of(c, "");
	out["one_off_tasks"] = std::move(one_off);
	out["one_off_stopped_tasks"] = std::move(one_off_stopped);
	text_or_null(out, "tasks_error", c.tasks_error);

	std::vector<crow::json::wvalue> groups;
	for (const TargetGroupHealth &g : c.target_groups) {
		crow::json::wvalue j;
		j["key"] = g.key;
		j["name"] = short_arn(g.arn.substr(0, g.arn.rfind('/')));
		std::vector<crow::json::wvalue> targets;
		for (const Target &t : g.targets) {
			crow::json::wvalue tj;
			tj["id"] = t.id;
			tj["port"] = t.port;
			text_or_null(tj, "zone", t.zone);
			tj["state"] = t.state;
			text_or_null(tj, "reason", t.reason);
			text_or_null(tj, "description", t.description);
			targets.push_back(std::move(tj));
		}
		j["targets"] = std::move(targets);
		text_or_null(j, "error", g.error);
		groups.push_back(std::move(j));
	}
	out["target_groups"] = std::move(groups);

	if (c.database) {
		const DatabaseStatus &d = *c.database;
		crow::json::wvalue j;
		j["identifier"] = d.identifier;
		j["status"] = d.status;
		j["engine"] = d.engine;
		j["engine_version"] = d.engine_version;
		j["instance_class"] = d.instance_class;
		j["multi_az"] = d.multi_az;
		text_or_null(j, "zone", d.zone);
		text_or_null(j, "secondary_zone", d.secondary_zone);
		text_or_null(j, "storage_type", d.storage_type);
		j["storage_gb"] = d.storage_gb;
		out["database"] = std::move(j);
	} else {
		out["database"] = nullptr;
	}
	text_or_null(out, "database_error", c.database_error);

	if (c.cache) {
		const CacheStatus &k = *c.cache;
		crow::json::wvalue j;
		j["id"] = k.id;
		j["status"] = k.status;
		j["multi_az"] = k.multi_az == "enabled";
		text_or_null(j, "automatic_failover", k.automatic_failover);
		text_or_null(j, "node_type", k.node_type);
		text_or_null(j, "engine", k.engine);
		std::vector<crow::json::wvalue> nodes;
		for (const CacheNode &n : k.nodes) {
			crow::json::wvalue nj;
			nj["id"] = n.id;
			text_or_null(nj, "role", n.role);
			text_or_null(nj, "zone", n.zone);
			nodes.push_back(std::move(nj));
		}
		j["nodes"] = std::move(nodes);
		out["cache"] = std::move(j);
	} else {
		out["cache"] = nullptr;
	}
	text_or_null(out, "cache_error", c.cache_error);
	text_or_null(out, "machines_error", c.machines_error);
	return out;
}

} // namespace

void InfraRoutes::configure(InfraStatus *status, bool hosting, std::string provider) {
	status_ = status;
	hosting_ = hosting;
	provider_ = std::move(provider);
}

void InfraRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/infrastructure")
			.methods("GET"_method)([this](const crow::request &req) { return overview(req); });
}

// GET /api/admin/infrastructure — {"at" (the database's now), "cloud", "lobbies",
// "fleet", "hosting", "machines"}; README "Infrastructure" spells out each.
// The database's sections from one snapshot, then the cloud (no lease held
// while AWS answers). No-store.
crow::response InfraRoutes::overview(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	crow::json::wvalue out;
	std::vector<hosted::FleetMachineRow> machines;
	std::map<int64_t, int64_t> slots_used;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["at"] = iso(db_conn->query("SELECT now()::timestamp(0)::text;")
						.front()
						.as_text(0)
						.value_or(""));

		std::map<int64_t, int64_t> sessions;
		for (const auto &r : db_conn->query("SELECT lobby_id, COUNT(*) FROM lobby_sessions "
											"WHERE lobby_id IS NOT NULL GROUP BY lobby_id;"))
			sessions[r.as_int(0).value_or(0)] = r.as_int(1).value_or(0);
		std::map<int64_t, int64_t> listings;
		for (const auto &r : db_conn->query("SELECT lobby_id, COUNT(*) FROM active_hosts "
											"WHERE lobby_id IS NOT NULL GROUP BY lobby_id;"))
			listings[r.as_int(0).value_or(0)] = r.as_int(1).value_or(0);
		std::vector<crow::json::wvalue> lobbies;
		for (const lobbydb::LobbyInstance &i : lobbydb::list_instances(*db_conn)) {
			crow::json::wvalue e;
			e["id"] = i.id;
			e["endpoint"] = i.endpoint;
			e["state"] = i.state;
			e["started_at"] = hosted::iso_utc(i.started_ms / 1000);
			e["last_seen_at"] = hosted::iso_utc(i.last_seen_ms / 1000);
			e["seen_ago_seconds"] = i.seen_ago_ms / 1000;
			e["live"] = i.seen_ago_ms <= lobbydb::kInstanceLiveSeconds * 1000;
			const auto s = sessions.find(i.id);
			e["sessions"] = s == sessions.end() ? int64_t{ 0 } : s->second;
			const auto l = listings.find(i.id);
			e["listings"] = l == listings.end() ? int64_t{ 0 } : l->second;
			lobbies.push_back(std::move(e));
		}
		out["lobbies"] = std::move(lobbies);

		crow::json::wvalue fleet;
		fleet["leader"] = nullptr;
		fleet["generation"] = 0;
		fleet["elected_at"] = nullptr;
		const auto leader =
				db_conn->query("SELECT holder, generation, elected_at::timestamp(0)::text FROM "
							   "fleet_leader WHERE id = 1;");
		if (!leader.empty()) {
			text_or_null(fleet, "leader", leader.front().as_text(0).value_or(""));
			fleet["generation"] = leader.front().as_int(1).value_or(0);
			iso_or_null(fleet, "elected_at", leader.front().as_text(2).value_or(""));
		}
		out["fleet"] = std::move(fleet);

		machines = hosted::list_machines(*db_conn, kEndedMachinesShown);
		for (const hosted::FleetMachineRow &m : machines)
			if (hosted::machine_is_live(m.state))
				slots_used[m.id] =
						static_cast<int64_t>(hosted::slot_holders(*db_conn, m.id).size());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/infrastructure failed: %s\n", e.what());
		return json_error(500, "db_error");
	}

	std::vector<std::string> instance_ids;
	for (const hosted::FleetMachineRow &m : machines)
		if (!m.cloud_id.empty())
			instance_ids.push_back(m.cloud_id);
	const std::shared_ptr<const CloudSnapshot> cloud =
			status_ ? status_->snapshot(instance_ids) : nullptr;
	out["cloud"] = cloud_json(
			cloud.get(), "AWS is not read here: this web process has no infrastructure reader");

	crow::json::wvalue hosting;
	hosting["enabled"] = hosting_;
	hosting["provider"] = provider_;
	out["hosting"] = std::move(hosting);

	std::vector<crow::json::wvalue> rows;
	for (const hosted::FleetMachineRow &m : machines) {
		crow::json::wvalue j;
		j["id"] = m.id;
		j["provider"] = m.provider;
		j["kind"] = hosted::machine_kind_text(m.kind);
		j["state"] = hosted::fleet_machine_state_text(m.state);
		j["slots"] = m.slots;
		const auto used = slots_used.find(m.id);
		j["slots_used"] = used == slots_used.end() ? int64_t{ 0 } : used->second;
		text_or_null(j, "cloud_id", m.cloud_id);
		text_or_null(j, "instance_type", m.instance_type);
		text_or_null(j, "cloud_state", m.cloud_state);
		iso_or_null(j, "launched_at", m.launched_at);
		iso_or_null(j, "ready_at", m.ready_at);
		iso_or_null(j, "ended_at", m.ended_at);
		text_or_null(j, "end_reason", m.end_reason);
		// EC2's own view, when the cloud was read and listed it.
		const MachineInstance *ec2 = nullptr;
		if (cloud) {
			const auto it = cloud->machines.find(m.cloud_id);
			if (it != cloud->machines.end())
				ec2 = &it->second;
		}
		if (ec2 != nullptr) {
			text_or_null(j, "zone", ec2->zone);
			text_or_null(j, "ec2_state", ec2->state);
		} else {
			j["zone"] = nullptr;
			j["ec2_state"] = nullptr;
		}
		rows.push_back(std::move(j));
	}
	out["machines"] = std::move(rows);
	return no_store(json_reply(200, out));
}

} // namespace opennova::novaworld_server::infra
