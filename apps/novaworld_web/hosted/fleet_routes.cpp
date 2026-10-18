#include "fleet_routes.h"

#include "accounts/auth.h"
#include "billing/entitlement.h"
#include "data_sets/data_set_repository.h"
#include "hosted/address_repository.h"
#include "hosted/hosted_repository.h"
#include "hosted/stats_repository.h"
#include "hosted_routes.h"
#include "http_json.h"
#include "infra/usage_routes.h"

#include <climits>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

crow::response machine_reply(
		int code, db::Database &db, int64_t id, const ReleaseImages &release) {
	crow::json::wvalue out;
	if (const auto machine = find_machine(db, id))
		out["machine"] = machine_json(db, *machine, release);
	else
		out["machine"] = nullptr;
	return json_reply(code, out);
}

// The database's "YYYY-MM-DD HH:MM:SS" (UTC) as ISO 8601 ("...T...Z"); "" stays "".
std::string iso_of(const std::string &text) {
	if (text.size() != 19 || text[10] != ' ')
		return text;
	return text.substr(0, 10) + "T" + text.substr(11) + "Z";
}

void optional_number(crow::json::wvalue &out, const char *key, const std::optional<double> &value) {
	if (value)
		out[key] = *value;
	else
		out[key] = nullptr;
}

// A slot holder's server, for its machine's page: its name, its game port (null
// before its first placement) and its last minute of ticks from its slot's report
// of its own container (`reports`, the machine's; server_json's "tick"), null
// with none.
void put_holder_server(crow::json::wvalue &j, db::Database &db, const SlotHolder &h,
		const std::vector<SlotReport> &reports) {
	const auto s = find_server(db, h.server_id);
	j["name"] = s ? s->settings.game_name : std::string();
	if (h.game_port > 0)
		j["game_port"] = h.game_port;
	else
		j["game_port"] = nullptr;
	const SlotReport *report = nullptr;
	for (const SlotReport &r : reports)
		if (r.slot == h.slot)
			report = &r;
	if (s && report && report->container == s->container && report->tick) {
		crow::json::wvalue t;
		t["p99_ms"] = report->tick->p99_ms;
		t["late"] = report->tick->late;
		j["tick"] = std::move(t);
	} else {
		j["tick"] = nullptr;
	}
}

} // namespace

crow::json::wvalue machine_json(db::Database &db, const FleetMachineRow &m,
		const ReleaseImages &release, bool holders_named) {
	crow::json::wvalue out;
	out["id"] = m.id;
	out["provider"] = m.provider;
	out["kind"] = machine_kind_text(m.kind);
	out["state"] = fleet_machine_state_text(m.state);
	text_or_null(out, "cloud_id", m.cloud_id);
	text_or_null(out, "instance_type", m.instance_type);
	out["slots"] = m.slots;
	// The live servers on its slots, by slot (none on a machine that ended).
	const std::vector<SlotHolder> holders =
			machine_is_live(m.state) ? slot_holders(db, m.id) : std::vector<SlotHolder>{};
	out["slots_used"] = static_cast<int64_t>(holders.size());
	const std::vector<SlotReport> reports =
			holders_named && !holders.empty() ? slot_reports(db, m.id) : std::vector<SlotReport>{};
	std::vector<crow::json::wvalue> held;
	for (const SlotHolder &h : holders) {
		crow::json::wvalue j;
		j["slot"] = h.slot;
		j["server_id"] = h.server_id;
		j["state"] = state_name(h.state);
		if (holders_named)
			put_holder_server(j, db, h, reports);
		held.push_back(std::move(j));
	}
	out["slot_holders"] = std::move(held);
	text_or_null(out, "private_address", m.private_address);
	text_or_null(out, "public_address", m.public_address);
	out["bootstrapped"] = m.bootstrapped;
	text_or_null(out, "agent_version", m.agent_version);
	if (m.agent_protocol > 0)
		out["agent_protocol"] = m.agent_protocol;
	else
		out["agent_protocol"] = nullptr;
	text_or_null(out, "agent_image", m.agent_image);
	text_or_null(out, "agent_log_stream", m.agent_log_stream);
	text_or_null(out, "protocol_mismatch_since", m.protocol_mismatch_since);
	out["lost_held"] = m.lost_held_since_ms > 0;
	text_or_null(out, "template_version", m.template_version);
	text_or_null(out, "serve_image", m.serve_image);
	if (m.reserved_for > 0)
		out["reserved_for"] = m.reserved_for;
	else
		out["reserved_for"] = nullptr;
	text_or_null(out, "last_report_at", m.last_report_at);
	out["launched_at"] = m.launched_at;
	text_or_null(out, "ready_at", m.ready_at);
	text_or_null(out, "ended_at", m.ended_at);
	text_or_null(out, "end_reason", m.end_reason);
	// Its slots' egress this calendar month, as the stats' samples counted it.
	out["egress_bytes_month"] = machine_egress_month(db, m.id, billing::unix_now());
	// Its image and its use as its agent last reported them, the network's rates
	// between its two newest minutes.
	text_or_null(out, "image_id", m.image_id);
	text_or_null(out, "stats_at", iso_of(m.stats_at));
	if (m.stats) {
		crow::json::wvalue s;
		s["cpu_percent"] = m.stats->cpu_percent;
		s["memory_used_bytes"] = m.stats->memory_used_bytes;
		s["memory_total_bytes"] = m.stats->memory_total_bytes;
		s["disk_free_bytes"] = m.stats->disk_free_bytes;
		s["disk_total_bytes"] = m.stats->disk_total_bytes;
		s["net_in_bytes"] = m.stats->net_in_bytes;
		s["net_out_bytes"] = m.stats->net_out_bytes;
		const infra::NetRates rates = infra::machine_net_rates(db, m.id);
		optional_number(s, "net_in_bps", rates.in_bps);
		optional_number(s, "net_out_bps", rates.out_bps);
		out["stats"] = std::move(s);
	} else {
		out["stats"] = nullptr;
	}
	// The images the release pins, and whether it runs others.
	crow::json::wvalue r;
	text_or_null(r, "image_id", release.image_id);
	text_or_null(r, "agent_image", release.agent_image);
	out["release"] = std::move(r);
	if (const auto outdated = machine_outdated(m, release))
		out["outdated"] = *outdated;
	else
		out["outdated"] = nullptr;
	// The static address it carries.
	const auto address = m.address_id > 0 ? find_address(db, m.address_id) : std::nullopt;
	if (address) {
		crow::json::wvalue a;
		a["id"] = address->id;
		a["name"] = address->name;
		out["address"] = std::move(a);
	} else {
		out["address"] = nullptr;
	}
	return out;
}

void FleetRoutes::configure(bool hosting_on, std::string provider, int cap, const PlacementPolicy &policy) {
	hosting_on_ = hosting_on;
	provider_ = std::move(provider);
	cap_ = cap;
	policy_ = policy;
}

void FleetRoutes::configure_roll(ReleaseImages release, infra::InfraStatus *infra) {
	release_ = std::move(release);
	infra_ = infra;
}

void FleetRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/hosted")
			.methods("GET"_method)([this](const crow::request &req) { return fleet(req); });
	CROW_ROUTE(app, "/api/admin/hosted/<uint>")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) { return server(req, id); });
	CROW_ROUTE(app, "/api/admin/fleet/machines/<uint>")
			.methods("GET"_method)(
					[this](const crow::request &req, uint64_t id) { return machine(req, id); });
	CROW_ROUTE(app, "/api/admin/fleet/machines/<uint>/drain")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) { return drain(req, id); });
	CROW_ROUTE(app, "/api/admin/fleet/machines/<uint>/end")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) { return end(req, id); });
	CROW_ROUTE(app, "/api/admin/fleet/roll")
			.methods("GET"_method, "POST"_method)([this](const crow::request &req) {
				return req.method == crow::HTTPMethod::Get ? roll_plan(req) : roll(req);
			});
	CROW_ROUTE(app, "/api/admin/hosted/update-all")
			.methods("POST"_method)([this](const crow::request &req) { return update_all(req); });
}

// GET /api/admin/hosted[?owner=<username>][&state=live|ended|all] — the fleet:
// {"enabled" (a machine driver and at least one offered data set), "provider",
// "cap", "live" (how many servers are live), "shared" and "own" ({live, room}:
// the live servers of each machine kind, and the most the placement policy's
// machines of that kind hold, ONNET_FLEET_MAX_SHARED x ONNET_FLEET_SHARED_SLOTS
// and ONNET_FLEET_MAX_OWN), "machines" (the live machines by
// id, then the newest kEndedMachinesShown ended or failed ones, newest first;
// machine_json, with the trees each holds), "servers"} (the newest
// kServersShown the filter takes, live first: `owner` an account's username
// (none by that name: no server), `state` live, ended or all (the default);
// each as GET /api/me/servers' plus `owner` ({id, username, tier: its granting
// support tier or null} or null), `service_account` ({id, username} or null),
// `provider`, `container` (its container's name), `machine_address`, `admin_port`,
// `machine_ref` and `slot` (null with none; an ended server's where it last
// ran), `game_port` (null before its first placement), `generation`,
// `serve_image`, `base_tree_id` and `expansion_tree_id` (its pins, null before
// its first start), `outdated` (newer data than its pins, while live),
// `update_requested` and `players` (its listing's, null when not listed)). 400
// invalid_field for another state. Never the admin password; no-store. The
// background jobs (the machines' launches and ends among them) are the jobs
// routes' (jobs/job_routes.h).
crow::response FleetRoutes::fleet(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	ServerFilter filter = ServerFilter::All;
	if (const char *state = req.url_params.get("state")) {
		const std::string which = state;
		if (which == "live")
			filter = ServerFilter::Live;
		else if (which == "ended")
			filter = ServerFilter::Ended;
		else if (which != "all")
			return json_error(400, "invalid_field", "state is live, ended or all");
	}
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["enabled"] = hosting_on_ && !offered_data_sets(*db_conn).empty();
		out["provider"] = provider_;
		out["cap"] = cap_;
		const int live = count_live(*db_conn);
		const int own_live = count_own_live(*db_conn);
		out["live"] = live;
		out["shared"]["live"] = live - own_live;
		out["shared"]["room"] = policy_.max_shared * policy_.shared_slots;
		out["own"]["live"] = own_live;
		out["own"]["room"] = policy_.max_own;
		std::vector<crow::json::wvalue> machines;
		for (const FleetMachineRow &m : list_machines(*db_conn, kEndedMachinesShown))
			machines.push_back(machine_json(*db_conn, m, release_));
		out["machines"] = std::move(machines);
		std::optional<int64_t> owner;
		bool no_such_owner = false;
		if (const char *name = req.url_params.get("owner")) {
			const auto user = get_user_by_username(*db_conn, name);
			if (user)
				owner = user->id;
			else
				no_such_owner = true;
		}
		std::vector<crow::json::wvalue> servers;
		if (!no_such_owner) {
			for (const HostedServer &s : list_fleet_servers(*db_conn, owner, filter, kServersShown))
				servers.push_back(server_json(*db_conn, s, true));
		}
		out["servers"] = std::move(servers);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosted failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// GET /api/admin/hosted/<id> — one server in any state with its machine:
// {"server" (as the fleet view's, plus `phase` and `stopped_because` as its
// owner reads them with the agent's reason a start never ran besides,
// hosted_routes.h put_phase, and `events`, the newest
// kEventsShown transitions oldest first), "machine" (machine_json of the
// machine it is on, or last ran on; null for none)}; 404 not_found; no-store.
crow::response FleetRoutes::server(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto s = find_server(*db_conn, path_id(id));
		if (!s) return json_error(404, "not_found", "no hosted server has this id");
		crow::json::wvalue e = server_json(*db_conn, *s, true);
		put_phase(e, *db_conn, *s, billing::unix_now() * 1000, true);
		std::vector<crow::json::wvalue> events;
		for (const HostedEvent &ev : list_events(*db_conn, s->id, kEventsShown)) {
			crow::json::wvalue j;
			j["from"] = ev.from_state;
			j["to"] = ev.to_state;
			j["detail"] = ev.detail;
			j["at"] = ev.at;
			events.push_back(std::move(j));
		}
		e["events"] = std::move(events);
		out["server"] = std::move(e);
		const auto machine = s->machine_ref ? find_machine(*db_conn, *s->machine_ref) : std::nullopt;
		if (machine)
			out["machine"] = machine_json(*db_conn, *machine, release_);
		else
			out["machine"] = nullptr;
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosted/%llu failed: %s\n", static_cast<unsigned long long>(id),
				e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// GET /api/admin/fleet/machines/<id> — one machine in any state for its page:
// {"machine" (machine_json), "events" (its newest kMachineEventsShown moves,
// oldest first, each {from (null at the row's making), to, detail, at}),
// "trees" (each tree its agent last said it holds: {tree_id, state (fetching,
// ready or failed), bytes (on disk, the fetch's so far), total_bytes (its
// manifest's; null when not known), tries, error (null for none),
// reported_at})}; 404 not_found; no-store. Its slot holders name their
// servers, game ports and ticks (machine_json's holders_named), so the page
// reads no fleet view.
crow::response FleetRoutes::machine(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const auto m = find_machine(*db_conn, path_id(id));
		if (!m) return json_error(404, "not_found", "no fleet machine has this id");
		out["machine"] = machine_json(*db_conn, *m, release_, /*holders_named=*/true);
		std::vector<crow::json::wvalue> events;
		for (const FleetMachineEvent &ev :
				list_machine_events(*db_conn, m->id, kMachineEventsShown)) {
			crow::json::wvalue j;
			text_or_null(j, "from", ev.from_state);
			j["to"] = ev.to_state;
			j["detail"] = ev.detail;
			j["at"] = ev.at;
			events.push_back(std::move(j));
		}
		out["events"] = std::move(events);
		std::vector<crow::json::wvalue> trees;
		for (const MachineTree &t : machine_trees(*db_conn, m->id)) {
			crow::json::wvalue j;
			j["tree_id"] = t.tree_id;
			j["state"] = tree_report_state_text(t.state);
			j["bytes"] = t.bytes;
			const auto tree = find_tree(*db_conn, t.tree_id);
			uint64_t total = 0;
			if (tree)
				for (const TreeFile &f : tree->files) total += f.size;
			if (tree && !tree->files.empty())
				j["total_bytes"] = total;
			else
				j["total_bytes"] = nullptr;
			j["tries"] = t.tries;
			text_or_null(j, "error", t.error);
			j["reported_at"] = t.reported_at;
			trees.push_back(std::move(j));
		}
		out["trees"] = std::move(trees);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/fleet/machines/%llu failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// POST /api/admin/fleet/machines/<id>/drain — the machine takes no new server
// and the supervisor moves its servers off it, one at a time, no restart
// counted, then ends it. 404 not_found; 409 not_live for one ending or ended;
// else 202 {"machine"} (one already draining included).
crow::response FleetRoutes::drain(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const int64_t machine_id = path_id(id);
		const auto machine = find_machine(*db_conn, machine_id);
		if (!machine) return json_error(404, "not_found", "no fleet machine has this id");
		if (machine->state == FleetMachineState::Draining)
			return machine_reply(202, *db_conn, machine_id, release_);
		if (transition_machine(*db_conn, machine_id,
					{ FleetMachineState::Launching, FleetMachineState::Booting, FleetMachineState::Ready,
							FleetMachineState::Lost },
					FleetMachineState::Draining, "drained by an admin")) {
			std::printf("[fleet] machine %lld: %s -> draining (drained by an admin)\n",
					static_cast<long long>(machine_id), fleet_machine_state_text(machine->state));
			return machine_reply(202, *db_conn, machine_id, release_);
		}
		return json_error(409, "not_live", "this machine is ending or has ended");
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/fleet/machines/%llu/drain failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/admin/fleet/machines/<id>/end — the machine ends now: the next
// supervisor pass terminates it and places its servers again elsewhere, no
// restart counted (a stopping one is stopped). Its agent is refused from now
// on. 404 not_found; 409 not_live for one ended or failed; else 202
// {"machine"} (one already ending included).
crow::response FleetRoutes::end(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	try {
		auto db_conn = pool_.acquire();
		const int64_t machine_id = path_id(id);
		const auto machine = find_machine(*db_conn, machine_id);
		if (!machine) return json_error(404, "not_found", "no fleet machine has this id");
		if (machine->state == FleetMachineState::Ending)
			return machine_reply(202, *db_conn, machine_id, release_);
		if (transition_machine(*db_conn, machine_id,
					{ FleetMachineState::Launching, FleetMachineState::Booting, FleetMachineState::Ready,
							FleetMachineState::Draining, FleetMachineState::Lost },
					FleetMachineState::Ending, "ended by an admin")) {
			std::printf("[fleet] machine %lld: %s -> ending (ended by an admin)\n", static_cast<long long>(machine_id),
					fleet_machine_state_text(machine->state));
			return machine_reply(202, *db_conn, machine_id, release_);
		}
		return json_error(409, "not_live", "this machine has ended");
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/fleet/machines/%llu/end failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/admin/hosted/update-all — the force of every live server whose
// pins are behind its data sets' current trees onto them (data_set_repository.h
// request_update_all): body {"data_set_id"} (optional: the servers of that data
// set, as base or expansion, alone), each marked update_requested for the
// supervisor's next pass (placed again on the current trees, no restart
// counted). 400 invalid_json / invalid_field; 202 {"updating": how many}.
crow::response FleetRoutes::update_all(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	std::optional<int64_t> data_set_id;
	if (!req.body.empty()) {
		auto body = JsonBody::parse(req.body);
		if (!body) return json_error(400, "invalid_json");
		data_set_id = body->integer("data_set_id");
		if (body->wrong_type()) return json_error(400, "invalid_field");
	}
	try {
		const size_t updating = request_update_all(*pool_.acquire(), data_set_id);
		std::printf("[hosted] update all: %zu server(s) to their data sets' current data\n", updating);
		crow::json::wvalue out;
		out["updating"] = updating;
		return json_reply(202, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/hosted/update-all failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

std::vector<RollStep> FleetRoutes::plan_roll(db::Database &db) {
	std::vector<FleetMachineRow> machines = list_live_machines(db);
	std::map<int64_t, int> servers;
	for (const FleetMachineRow &m : machines)
		servers[m.id] = static_cast<int>(slot_holders(db, m.id).size());
	// Each machine's image as EC2 has it (the infrastructure view's reader,
	// cached), no lease held while AWS answers.
	std::map<int64_t, std::string> images;
	if (infra_ != nullptr && infra_->configured()) {
		std::vector<std::string> ids;
		for (const FleetMachineRow &m : machines)
			if (!m.cloud_id.empty())
				ids.push_back(m.cloud_id);
		if (const auto cloud = infra_->snapshot(ids))
			for (const FleetMachineRow &m : machines)
				if (const auto it = cloud->machines.find(m.cloud_id); it != cloud->machines.end())
					images[m.id] = it->second.image_id;
	}
	return plan_fleet_roll(machines, images, servers, release_.image_id);
}

// GET /api/admin/fleet/roll — a fleet roll's preview (hosted/fleet_roll.h):
// {"image_id" (the image the launch templates pin, null for none), "machines"
// (each live machine that takes servers: {id, kind, state, image_id (its own,
// null when not known), servers, action: "replace" or "keep", why}), "replace"
// (the ids to POST)}. No-store.
crow::response FleetRoutes::roll_plan(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	try {
		const std::vector<RollStep> plan = plan_roll(*pool_.acquire());
		crow::json::wvalue out;
		text_or_null(out, "image_id", release_.image_id);
		std::vector<crow::json::wvalue> machines;
		std::vector<crow::json::wvalue> replace;
		for (const RollStep &step : plan) {
			crow::json::wvalue j;
			j["id"] = step.machine_id;
			j["kind"] = machine_kind_text(step.kind);
			j["state"] = fleet_machine_state_text(step.state);
			text_or_null(j, "image_id", step.image_id);
			j["servers"] = step.servers;
			j["action"] = step.replace ? "replace" : "keep";
			j["why"] = step.why;
			machines.push_back(std::move(j));
			if (step.replace) {
				crow::json::wvalue id;
				id = step.machine_id;
				replace.push_back(std::move(id));
			}
		}
		out["machines"] = std::move(machines);
		out["replace"] = std::move(replace);
		crow::response res = json_reply(200, out);
		res.set_header("Cache-Control", "no-store");
		return res;
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/fleet/roll failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/admin/fleet/roll {"machines": [<id>, ...]} — a fleet roll of the
// machines named (the preview's "replace", or the admin's own choice): each
// drains, so its servers move, one at a time per machine and no restart
// counted, to machines launched for them from the templates as they are now
// (a new image, template or user-data reaches those). 202 {"draining": how
// many, "machines": their ids}; 400 invalid_field for a body that names no
// machine, or one that is no live machine taking servers.
crow::response FleetRoutes::roll(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	const auto body = crow::json::load(req.body);
	if (!body || body.t() != crow::json::type::Object || !body.has("machines") ||
			body["machines"].t() != crow::json::type::List || body["machines"].size() == 0 ||
			body["machines"].size() > 1000)
		return json_error(400, "invalid_field", "machines is a list of machine ids");
	std::vector<int64_t> ids;
	for (const auto &v : body["machines"]) {
		if (v.t() != crow::json::type::Number || v.nt() != crow::json::num_type::Unsigned_integer)
			return json_error(400, "invalid_field", "machines is a list of machine ids");
		ids.push_back(static_cast<int64_t>(v.u()));
	}
	try {
		auto lease = pool_.acquire();
		db::Database &db = *lease;
		for (const int64_t id : ids) {
			const auto machine = find_machine(db, id);
			if (!machine || !machine_rollable(machine->state))
				return json_error(400, "invalid_field",
						"machine " + std::to_string(id) + " is no machine taking servers");
		}
		const std::vector<int64_t> drained = drain_machines(db, ids, "rolled by an admin");
		std::printf("[fleet] roll: %zu machine(s) draining\n", drained.size());
		crow::json::wvalue out;
		out["draining"] = drained.size();
		std::vector<crow::json::wvalue> ids;
		for (const int64_t machine : drained) {
			crow::json::wvalue v;
			v = machine;
			ids.push_back(std::move(v));
		}
		out["machines"] = std::move(ids);
		return json_reply(202, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/fleet/roll failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

} // namespace opennova::novaworld_server::hosted
