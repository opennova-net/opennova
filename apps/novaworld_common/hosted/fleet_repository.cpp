#include "fleet_repository.h"

#include "hosted_settings.h"

#include <base/io/sha256.h>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <variant>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr const char *kMachineColumns =
		"id, provider, kind, state, cloud_id, instance_type, slots, private_address, "
		"public_address, agent_secret_hash <> '', agent_version, template_version, serve_image, "
		"COALESCE(reserved_for, 0), last_report_at, launched_at, ready_at, ended_at, end_reason, "
		"cloud_state, described_at, COALESCE(launched_for, 0), seen_state, seen_since_ms, "
		"idle_since_ms, "
		"drain_moving, was_draining, describe_asked_ms, launch_subnet, "
		"COALESCE(agent_protocol, 0), agent_image, protocol_mismatch_since, lost_held_since_ms, "
		"COALESCE(address_id, 0), image_id, agent_log_stream, cpu_percent, memory_used_bytes, "
		"memory_total_bytes, disk_free_bytes, disk_total_bytes, net_in_bytes, net_out_bytes, "
		"stats_at";
constexpr size_t kMachineColumnCount = 44;

// The SQL test for a live machine (a live hosted server's is hosted_repository.h's).
constexpr const char *kLiveMachineSql = "state NOT IN ('ended', 'failed')";

bool is_alnum_text(std::string_view text, size_t size) {
	if (text.size() != size)
		return false;
	for (const char c : text) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!ok)
			return false;
	}
	return true;
}

BindValue count_value(uint64_t count) {
	constexpr uint64_t kMax = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	return BindValue(static_cast<int64_t>(std::min(count, kMax)));
}

BindValue optional_int(const std::optional<int64_t> &value) {
	return value ? BindValue(*value) : BindValue(std::monostate{});
}

double as_double(const db::Row &row, size_t i) {
	if (i >= row.values.size())
		return 0.0;
	if (const auto *d = std::get_if<double>(&row.values[i]))
		return *d;
	if (const auto *n = std::get_if<int64_t>(&row.values[i]))
		return static_cast<double>(*n);
	return 0.0;
}

uint64_t as_count(const db::Row &row, size_t i) {
	const int64_t value = row.as_int(i).value_or(0);
	return value > 0 ? static_cast<uint64_t>(value) : 0;
}

FleetMachineRow row_to_machine(const db::Row &r) {
	FleetMachineRow m;
	m.id = r.as_int(0).value_or(0);
	m.provider = r.as_text(1).value_or("");
	m.kind = parse_machine_kind(r.as_text(2).value_or("")).value_or(MachineKind::Shared);
	m.state = parse_fleet_machine_state(r.as_text(3).value_or(""))
					  .value_or(FleetMachineState::Failed);
	m.cloud_id = r.as_text(4).value_or("");
	m.instance_type = r.as_text(5).value_or("");
	m.slots = static_cast<int>(r.as_int(6).value_or(0));
	m.private_address = r.as_text(7).value_or("");
	m.public_address = r.as_text(8).value_or("");
	m.bootstrapped = r.as_int(9).value_or(0) != 0;
	m.agent_version = r.as_text(10).value_or("");
	m.template_version = r.as_text(11).value_or("");
	m.serve_image = r.as_text(12).value_or("");
	m.reserved_for = r.as_int(13).value_or(0);
	m.last_report_at = r.as_text(14).value_or("");
	m.launched_at = r.as_text(15).value_or("");
	m.ready_at = r.as_text(16).value_or("");
	m.ended_at = r.as_text(17).value_or("");
	m.end_reason = r.as_text(18).value_or("");
	m.cloud_state = r.as_text(19).value_or("");
	m.described_at = r.as_text(20).value_or("");
	m.launched_for = r.as_int(21).value_or(0);
	m.seen_state = r.as_text(22).value_or("");
	m.seen_since_ms = r.as_int(23).value_or(0);
	m.idle_since_ms = r.as_int(24).value_or(0);
	m.drain_moving = r.as_int(25).value_or(0);
	m.was_draining = r.as_int(26).value_or(0) != 0;
	m.describe_asked_ms = r.as_int(27).value_or(0);
	m.launch_subnet = r.as_text(28).value_or("");
	m.agent_protocol = static_cast<int>(r.as_int(29).value_or(0));
	m.agent_image = r.as_text(30).value_or("");
	m.protocol_mismatch_since = r.as_text(31).value_or("");
	m.lost_held_since_ms = r.as_int(32).value_or(0);
	m.address_id = r.as_int(33).value_or(0);
	m.image_id = r.as_text(34).value_or("");
	m.agent_log_stream = r.as_text(35).value_or("");
	m.stats_at = r.as_text(43).value_or("");
	if (!m.stats_at.empty()) {
		MachineStats s;
		s.cpu_percent = as_double(r, 36);
		s.memory_used_bytes = as_count(r, 37);
		s.memory_total_bytes = as_count(r, 38);
		s.disk_free_bytes = as_count(r, 39);
		s.disk_total_bytes = as_count(r, 40);
		s.net_in_bytes = as_count(r, 41);
		s.net_out_bytes = as_count(r, 42);
		m.stats = s;
	}
	return m;
}

std::vector<FleetMachineRow> query_machines(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<FleetMachineRow> out;
	for (const db::Row &r : db.query(
				 std::string("SELECT ") + kMachineColumns + " FROM fleet_machines " + tail, binds))
		out.push_back(row_to_machine(r));
	return out;
}

// The event every move of a machine writes beside it: the tail of a statement
// whose `moved` CTE returns (id, from_state, to_state), the detail bound at
// `$<detail_param>`.
std::string event_insert(int detail_param) {
	return "INSERT INTO fleet_machine_events (machine_id, from_state, to_state, detail) "
		   "SELECT id, from_state, to_state, $" +
			std::to_string(detail_param) + "::text FROM moved";
}

// "'a', 'b'": the states' words, for an IN list (fixed words, never input).
std::string state_list(const std::vector<FleetMachineState> &states) {
	std::string out;
	for (const FleetMachineState state : states) {
		if (!out.empty())
			out += ", ";
		out += "'";
		out += fleet_machine_state_text(state);
		out += "'";
	}
	return out;
}

} // namespace

bool machine_is_live(FleetMachineState state) {
	return state != FleetMachineState::Ended && state != FleetMachineState::Failed;
}

const char *cloud_state_text(CloudState state) {
	switch (state) {
		case CloudState::Pending:
			return "pending";
		case CloudState::Running:
			return "running";
		case CloudState::Gone:
			return "gone";
		case CloudState::Unknown:
			return "unknown";
	}
	return "unknown";
}

std::optional<CloudState> parse_cloud_state(std::string_view text) {
	for (const CloudState state :
			{ CloudState::Pending, CloudState::Running, CloudState::Gone, CloudState::Unknown }) {
		if (text == cloud_state_text(state))
			return state;
	}
	return std::nullopt;
}

// --- the machines ---

CreatedMachine create_machine(
		db::Database &db, const NewMachine &machine, int64_t token_lifetime_s) {
	CreatedMachine out;
	out.bootstrap_token = random_secret(kMachineTokenChars);
	const std::string detail = machine.launched_for > 0
			? "launched for server " + std::to_string(machine.launched_for)
			: std::string();
	const auto rows = db.query(
			"WITH moved AS (INSERT INTO fleet_machines (provider, kind, slots, serve_image, "
			"reserved_for, bootstrap_token_hash, bootstrap_expires_at, launched_for, address_id) "
			"VALUES ($1, $2, $3, $4, $5, $6, now() + make_interval(secs => $7), $8, $9) "
			"RETURNING id, NULL::text AS from_state, state AS to_state), "
			"event AS (" +
					event_insert(10) + ") SELECT id FROM moved;",
			{ BindValue(machine.provider), BindValue(std::string(machine_kind_text(machine.kind))),
					BindValue(static_cast<int64_t>(machine.slots)), BindValue(machine.serve_image),
					machine.reserved_for > 0 ? BindValue(machine.reserved_for)
											 : BindValue(std::monostate{}),
					BindValue(agent_secret_digest(out.bootstrap_token)),
					BindValue(static_cast<double>(token_lifetime_s)),
					machine.launched_for > 0 ? BindValue(machine.launched_for)
											 : BindValue(std::monostate{}),
					machine.address_id > 0 ? BindValue(machine.address_id) : BindValue(std::monostate{}),
					BindValue(detail) });
	out.id = rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
	return out;
}

std::string carried_address_ip(db::Database &db, int64_t id) {
	const auto rows = db.query("SELECT a.public_ip FROM fleet_machines m JOIN hosted_addresses a ON a.id = m.address_id "
							   "WHERE m.id = $1;",
			{ BindValue(id) });
	return rows.empty() ? std::string() : rows.front().as_text(0).value_or("");
}

bool mark_launched(db::Database &db, int64_t id, const LaunchedMachine &launched) {
	db.exec("WITH moved AS (UPDATE fleet_machines SET state = 'booting', cloud_id = $2, "
			"private_address = $3, public_address = $4, instance_type = $5, template_version = $6, "
			"launched_at = now() WHERE id = $1 AND state = 'launching' "
			"RETURNING id, 'launching'::text AS from_state, state AS to_state) " +
					event_insert(7) + ";",
			{ BindValue(id), BindValue(launched.cloud_id), BindValue(launched.private_address),
					BindValue(launched.public_address), BindValue(launched.instance_type),
					BindValue(launched.template_version),
					BindValue("launched as " + launched.cloud_id +
							(launched.instance_type.empty()
											? ""
											: " (" + launched.instance_type + ")")) });
	return db.changes() > 0;
}

bool set_machine_addresses(db::Database &db, int64_t id, const std::string &private_address,
		const std::string &public_address) {
	db.exec(std::string("UPDATE fleet_machines SET "
						"private_address = CASE WHEN private_address = '' THEN $2 "
						"ELSE private_address END, "
						"public_address = $3 "
						"WHERE id = $1 AND ") +
					kLiveMachineSql +
					" AND ((private_address = '' AND $2 <> '') OR public_address <> $3);",
			{ BindValue(id), BindValue(private_address), BindValue(public_address) });
	return db.changes() > 0;
}

bool transition_machine(db::Database &db, int64_t id, const std::vector<FleetMachineState> &from,
		FleetMachineState to, const std::string &reason) {
	if (from.empty())
		return false;
	const std::string target = fleet_machine_state_text(to);
	const bool ready = to == FleetMachineState::Ready;
	const bool gone = to == FleetMachineState::Ended || to == FleetMachineState::Failed;
	const bool spends = gone || to == FleetMachineState::Ending;
	const bool takes_reason =
			!ready && to != FleetMachineState::Booting && to != FleetMachineState::Launching;
	// The row locked first, so the event names the state it moved from (a move
	// another made meanwhile is waited for and read); a move off lost lets go
	// of the mass-lost brake's hold.
	std::string sql =
			"WITH old AS (SELECT id, state FROM fleet_machines WHERE id = $1 AND state IN (" +
			state_list(from) + ") FOR UPDATE), moved AS (UPDATE fleet_machines m SET state = $2";
	if (to != FleetMachineState::Lost)
		sql += ", lost_held_since_ms = 0";
	if (ready)
		sql += ", ready_at = COALESCE(ready_at, now()), end_reason = ''";
	if (gone)
		sql += ", ended_at = now()";
	if (takes_reason)
		sql += ", end_reason = CASE WHEN $3::text <> '' THEN $3::text ELSE m.end_reason END";
	if (spends)
		sql += ", bootstrap_token_hash = '', bootstrap_expires_at = NULL, agent_secret_hash = ''";
	sql += " FROM old WHERE m.id = old.id RETURNING m.id, old.state AS from_state, m.state AS "
		   "to_state) ";
	db.exec(sql + event_insert(3) + ";", { BindValue(id), BindValue(target), BindValue(reason) });
	return db.changes() > 0;
}

bool transition_machine(db::Database &db, int64_t id, FleetMachineState from, FleetMachineState to,
		const std::string &reason) {
	return transition_machine(db, id, std::vector<FleetMachineState>{ from }, to, reason);
}

std::optional<FleetMachineRow> find_machine(db::Database &db, int64_t id) {
	auto rows = query_machines(db, "WHERE id = $1;", { BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return std::move(rows.front());
}

std::optional<FleetMachineRow> find_machine_by_cloud_id(
		db::Database &db, const std::string &provider, const std::string &cloud_id) {
	if (cloud_id.empty())
		return std::nullopt;
	auto rows = query_machines(db, "WHERE provider = $1 AND cloud_id = $2;",
			{ BindValue(provider), BindValue(cloud_id) });
	if (rows.empty())
		return std::nullopt;
	return std::move(rows.front());
}

std::vector<FleetMachineRow> live_machines_carrying(db::Database &db, int64_t address_id) {
	return query_machines(db, std::string("WHERE address_id = $1 AND ") + kLiveMachineSql + " ORDER BY id;",
			{ BindValue(address_id) });
}

std::vector<FleetMachineRow> list_live_machines(db::Database &db) {
	return query_machines(db, std::string("WHERE ") + kLiveMachineSql + " ORDER BY id;", {});
}

std::vector<FleetMachineRow> list_machines(db::Database &db, size_t ended_limit) {
	db::ReadSnapshot snapshot(db);
	std::vector<FleetMachineRow> out = list_live_machines(db);
	const auto ended =
			query_machines(db, "WHERE state IN ('ended', 'failed') ORDER BY id DESC LIMIT $1;",
					{ BindValue(static_cast<int64_t>(ended_limit)) });
	out.insert(out.end(), ended.begin(), ended.end());
	return out;
}

std::vector<FleetMachineRow> list_silent_machines(db::Database &db, int64_t seconds) {
	// A machine whose agent never reported (one drained while it booted) has
	// fallen silent from nothing: the boot timeout is its.
	return query_machines(db,
			"WHERE state IN ('ready', 'draining') AND last_report_at IS NOT NULL AND "
			"last_report_at < now() - make_interval(secs => $1) ORDER BY id;",
			{ BindValue(static_cast<double>(seconds)) });
}

bool machine_reported_within(db::Database &db, int64_t id, int64_t seconds) {
	return !db.query("SELECT 1 FROM fleet_machines WHERE id = $1 AND last_report_at IS NOT NULL AND "
					 "last_report_at >= now() - make_interval(secs => $2);",
						 { BindValue(id), BindValue(static_cast<double>(seconds)) })
					.empty();
}

std::vector<int64_t> drain_machines(
		db::Database &db, const std::vector<int64_t> &ids, const std::string &reason) {
	std::vector<int64_t> out;
	for (const int64_t id : ids) {
		const auto moved = db.query(
				"WITH old AS (SELECT id, state FROM fleet_machines WHERE id = $1 AND state IN "
				"('launching', 'booting', 'ready') FOR UPDATE), moved AS (UPDATE fleet_machines m "
				"SET state = 'draining', end_reason = $2 FROM old WHERE m.id = old.id RETURNING "
				"m.id, old.state AS from_state, m.state AS to_state), event AS (" +
						event_insert(2) + ") SELECT id FROM moved;",
				{ BindValue(id), BindValue(reason) });
		if (!moved.empty())
			out.push_back(id);
	}
	std::sort(out.begin(), out.end());
	return out;
}

size_t prune_ended_machines(db::Database &db, int64_t keep_s) {
	// An ended machine's row with no end time (none should be) ages from its launch.
	db.exec(std::string("DELETE FROM fleet_machines m WHERE m.state IN ('ended', 'failed') AND "
						"COALESCE(m.ended_at, m.launched_at) < now() - make_interval(secs => $1) "
						"AND NOT EXISTS (SELECT 1 FROM hosted_servers s WHERE s.machine_ref = m.id "
						"AND s.") +
					kLiveServerSql + ");",
			{ BindValue(static_cast<double>(keep_s)) });
	return static_cast<size_t>(db.changes());
}

int count_machines(db::Database &db, FleetMachineState state) {
	const auto rows = db.query("SELECT COUNT(*) FROM fleet_machines WHERE state = $1;",
			{ BindValue(std::string(fleet_machine_state_text(state))) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

int count_held_machines(db::Database &db) {
	const auto rows = db.query(
			"SELECT COUNT(*) FROM fleet_machines WHERE state = 'lost' AND lost_held_since_ms > 0;");
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

bool machine_launching_for(db::Database &db, int64_t server_id) {
	return !db.query("SELECT 1 FROM fleet_machines WHERE launched_for = $1 AND state IN "
					 "('launching', 'booting');",
					  { BindValue(server_id) })
					.empty();
}

// --- what the cloud answered ---

bool record_launch_subnet(db::Database &db, int64_t id, const std::string &subnet) {
	db.exec("UPDATE fleet_machines SET launch_subnet = $2 WHERE id = $1 AND state = 'launching';",
			{ BindValue(id), BindValue(subnet) });
	return db.changes() > 0;
}

std::optional<int64_t> launched_ago_ms(db::Database &db, int64_t id) {
	const auto rows = db.query("SELECT (EXTRACT(EPOCH FROM now() - launched_at) * 1000)::bigint "
							   "FROM fleet_machines WHERE id = $1;",
			{ BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return rows[0].as_int(0);
}

bool record_cloud_state(
		db::Database &db, int64_t id, const std::string &cloud_id, const CloudMachine &cloud) {
	// Addresses only from an answer that names the machine running or about to.
	const bool addressed =
			(cloud.state == CloudState::Pending || cloud.state == CloudState::Running) &&
			(!cloud.private_address.empty() || !cloud.public_address.empty());
	db.exec(std::string("UPDATE fleet_machines SET cloud_state = $3, described_at = now()") +
					(addressed ? ", private_address = CASE WHEN private_address = '' THEN $4 ELSE "
								 "private_address END, "
								 "public_address = CASE WHEN $5::text <> '' THEN $5::text ELSE "
								 "public_address END"
							   : "") +
					" WHERE id = $1 AND cloud_id = $2 AND " + kLiveMachineSql + ";",
			addressed ? std::vector<BindValue>{ BindValue(id), BindValue(cloud_id),
								BindValue(std::string(cloud_state_text(cloud.state))),
								BindValue(cloud.private_address), BindValue(cloud.public_address) }
					  : std::vector<BindValue>{ BindValue(id), BindValue(cloud_id),
								BindValue(std::string(cloud_state_text(cloud.state))) });
	return db.changes() > 0;
}

bool fail_refused_launch(db::Database &db, int64_t id, const std::string &reason) {
	db.exec("WITH moved AS (UPDATE fleet_machines SET state = 'failed', ended_at = now(), "
			"end_reason = $2, launch_refused = true, bootstrap_token_hash = '', "
			"bootstrap_expires_at = NULL, agent_secret_hash = '' "
			"WHERE id = $1 AND state = 'launching' "
			"RETURNING id, 'launching'::text AS from_state, state AS to_state) " +
					event_insert(2) + ";",
			{ BindValue(id), BindValue(reason) });
	return db.changes() > 0;
}

std::vector<RefusedLaunch> take_refused_launches(db::Database &db) {
	std::vector<RefusedLaunch> out;
	for (const db::Row &r :
			db.query("UPDATE fleet_machines SET launch_refused = false WHERE launch_refused "
					 "RETURNING id, kind, COALESCE(launched_for, 0), end_reason;")) {
		RefusedLaunch l;
		l.machine_id = r.as_int(0).value_or(0);
		l.kind = parse_machine_kind(r.as_text(1).value_or("")).value_or(MachineKind::Shared);
		l.launched_for = r.as_int(2).value_or(0);
		l.reason = r.as_text(3).value_or("");
		out.push_back(l);
	}
	std::sort(out.begin(), out.end(), [](const RefusedLaunch &a, const RefusedLaunch &b) {
		return a.machine_id < b.machine_id;
	});
	return out;
}

// --- what the supervisor remembers of a machine ---

void save_machine_runtime(db::Database &db, const FleetMachineRow &m) {
	db.exec("UPDATE fleet_machines SET seen_state = $2, seen_since_ms = $3, idle_since_ms = $4, "
			"drain_moving = $5, was_draining = $6 WHERE id = $1;",
			{ BindValue(m.id), BindValue(m.seen_state), BindValue(m.seen_since_ms),
					BindValue(m.idle_since_ms), BindValue(m.drain_moving),
					BindValue(db::Bool{ m.was_draining }) });
}

bool mark_machine_lost(db::Database &db, int64_t id, const std::string &reason, int64_t now_ms) {
	// was_draining names the state it left.
	db.exec("WITH moved AS (UPDATE fleet_machines SET was_draining = (state = 'draining'), "
			"state = 'lost', "
			"end_reason = CASE WHEN $2::text <> '' THEN $2::text ELSE end_reason END, "
			"seen_state = 'lost', seen_since_ms = $3, idle_since_ms = 0, drain_moving = 0, "
			"cloud_state = '', lost_held_since_ms = 0 "
			"WHERE id = $1 AND state IN ('ready', 'draining') RETURNING id, CASE WHEN was_draining "
			"THEN 'draining' ELSE 'ready' END AS from_state, state AS to_state) " +
					event_insert(2) + ";",
			{ BindValue(id), BindValue(reason), BindValue(now_ms) });
	return db.changes() > 0;
}

bool hold_lost_machine(db::Database &db, int64_t id, int64_t now_ms) {
	db.exec("UPDATE fleet_machines SET lost_held_since_ms = CASE WHEN lost_held_since_ms > 0 "
			"THEN lost_held_since_ms ELSE $2 END WHERE id = $1 AND state = 'lost';",
			{ BindValue(id), BindValue(now_ms) });
	return db.changes() > 0;
}

void note_describe_asked(db::Database &db, int64_t id, int64_t now_ms) {
	db.exec("UPDATE fleet_machines SET describe_asked_ms = $2 WHERE id = $1;",
			{ BindValue(id), BindValue(now_ms) });
}

// --- each kind's launch backoff ---

uint64_t launch_backoff_wait_ms(int64_t failures, uint64_t base_ms, uint64_t cap_ms) {
	uint64_t wait = std::min(base_ms, cap_ms);
	for (int64_t i = 1; i < failures && wait > 0 && wait < cap_ms; ++i)
		wait = std::min(wait * 2, cap_ms);
	return wait;
}

LaunchBackoff launch_backoff(db::Database &db, MachineKind kind) {
	LaunchBackoff out;
	const auto rows =
			db.query("SELECT failures, not_before_ms FROM fleet_launch_backoff WHERE kind = $1;",
					{ BindValue(std::string(machine_kind_text(kind))) });
	if (!rows.empty()) {
		out.failures = rows.front().as_int(0).value_or(0);
		out.not_before_ms = rows.front().as_int(1).value_or(0);
	}
	return out;
}

int64_t launch_not_before_ms(db::Database &db, MachineKind kind) {
	return launch_backoff(db, kind).not_before_ms;
}

std::optional<LaunchBackoff> note_launch_failure(
		db::Database &db, MachineKind kind, int64_t now_ms, uint64_t base_ms, uint64_t cap_ms) {
	const BindValue name(std::string(machine_kind_text(kind)));
	// The kind's row read and written under its lock: a boot's clear waits.
	db::Transaction tx(db);
	db.exec("INSERT INTO fleet_launch_backoff (kind, not_before_ms) VALUES ($1, 0) "
			"ON CONFLICT (kind) DO NOTHING;",
			{ name });
	const auto rows = db.query(
			"SELECT failures, not_before_ms FROM fleet_launch_backoff WHERE kind = $1 FOR UPDATE;",
			{ name });
	LaunchBackoff b;
	if (!rows.empty()) {
		b.failures = rows.front().as_int(0).value_or(0);
		b.not_before_ms = rows.front().as_int(1).value_or(0);
	}
	if (now_ms < b.not_before_ms)
		return std::nullopt;
	++b.failures;
	b.not_before_ms =
			now_ms + static_cast<int64_t>(launch_backoff_wait_ms(b.failures, base_ms, cap_ms));
	db.exec("UPDATE fleet_launch_backoff SET failures = $2, not_before_ms = $3 WHERE kind = $1;",
			{ name, BindValue(b.failures), BindValue(b.not_before_ms) });
	tx.commit();
	return b;
}

void clear_launch_backoff(db::Database &db, MachineKind kind) {
	db.exec("UPDATE fleet_launch_backoff SET failures = 0, not_before_ms = 0 WHERE kind = $1;",
			{ BindValue(std::string(machine_kind_text(kind))) });
}

// --- the agent's credentials ---

std::optional<FleetMachineRow> redeem_bootstrap_token(db::Database &db, int64_t id,
		std::string_view token, const std::string &secret_digest, const std::string &agent_version,
		std::string &why) {
	if (!is_alnum_text(token, kMachineTokenChars)) {
		why = "no token, or not one this service mints";
		return std::nullopt;
	}
	db::Transaction tx(db);
	const auto rows =
			db.query("SELECT bootstrap_token_hash, state, "
					 "(bootstrap_expires_at IS NOT NULL AND bootstrap_expires_at > now()) "
					 "FROM fleet_machines WHERE id = $1;",
					{ BindValue(id) });
	if (rows.empty()) {
		why = "no such machine";
		return std::nullopt;
	}
	const std::string stored = rows.front().as_text(0).value_or("");
	const std::string state = rows.front().as_text(1).value_or("");
	const bool live = rows.front().as_int(2).value_or(0) != 0;
	if (stored.empty()) {
		why = "a machine whose token is spent";
		return std::nullopt;
	}
	if (!io::constant_time_equal(agent_secret_digest(token), stored)) {
		why = "a token that is not the machine's";
		return std::nullopt;
	}
	if (!live) {
		why = "a lapsed token";
		return std::nullopt;
	}
	if (state != fleet_machine_state_text(FleetMachineState::Booting)) {
		why = "a machine that is " + state + ", not booting";
		return std::nullopt;
	}
	// The token stays until the agent's first sync spends it, so an agent
	// whose reply was lost redeems again: the new secret replaces the last.
	db.exec("UPDATE fleet_machines SET agent_secret_hash = $2, agent_version = $3 "
			"WHERE id = $1 AND bootstrap_token_hash = $4 AND state = 'booting';",
			{ BindValue(id), BindValue(secret_digest), BindValue(agent_version),
					BindValue(stored) });
	if (db.changes() == 0) {
		why = "a token spent at the same time";
		return std::nullopt;
	}
	auto row = find_machine(db, id);
	tx.commit();
	return row;
}

std::optional<FleetMachineRow> authenticate_agent(
		db::Database &db, int64_t id, std::string_view secret, std::string &why) {
	if (!is_alnum_text(secret, kAgentSecretChars)) {
		why = "no secret, or not one this service mints";
		return std::nullopt;
	}
	const auto rows = db.query(std::string("SELECT ") + kMachineColumns +
					", agent_secret_hash FROM fleet_machines WHERE id = $1;",
			{ BindValue(id) });
	if (rows.empty()) {
		why = "no such machine";
		return std::nullopt;
	}
	const std::string stored = rows.front().as_text(kMachineColumnCount).value_or("");
	if (stored.empty()) {
		why = "a machine with no agent secret";
		return std::nullopt;
	}
	if (!io::constant_time_equal(agent_secret_digest(secret), stored)) {
		why = "a secret that is not the machine's agent's";
		return std::nullopt;
	}
	FleetMachineRow machine = row_to_machine(rows.front());
	switch (machine.state) {
		case FleetMachineState::Booting:
		case FleetMachineState::Ready:
		case FleetMachineState::Draining:
		case FleetMachineState::Lost:
			return machine;
		default:
			why = std::string("a machine that is ") + fleet_machine_state_text(machine.state);
			return std::nullopt;
	}
}

// --- the agent's reports ---

std::optional<FleetMachineRow> record_sync(db::Database &db, int64_t id,
		const std::string &agent_version, const std::vector<ContainerReport> &containers,
		const std::string &agent_image) {
	db::Transaction tx(db);
	// A booting machine's row, locked until this sync moves it to ready: its
	// launch succeeded, so its kind's launch backoff ends with it.
	const auto booting = db.query(
			"SELECT kind FROM fleet_machines WHERE id = $1 AND state = 'booting' FOR UPDATE;",
			{ BindValue(id) });
	// The agent's first sync spends the bootstrap token (redeem_bootstrap_token);
	// a booting machine's (its row locked above) moves to ready with its event.
	const auto updated = db.query(
			"WITH moved AS (UPDATE fleet_machines SET last_report_at = now(), agent_version = $2, "
			"agent_image = $3, agent_protocol = $4, protocol_mismatch_since = NULL, "
			"bootstrap_token_hash = '', bootstrap_expires_at = NULL, "
			"ready_at = CASE WHEN state = 'booting' THEN now() ELSE ready_at END, "
			"end_reason = CASE WHEN state = 'booting' THEN '' ELSE end_reason END, "
			"state = CASE WHEN state = 'booting' THEN 'ready' ELSE state END "
			"WHERE id = $1 AND state IN ('booting', 'ready', 'draining', 'lost') "
			"RETURNING id, slots, 'booting'::text AS from_state, state AS to_state), "
			"event AS (INSERT INTO fleet_machine_events (machine_id, from_state, to_state, detail) "
			"SELECT id, from_state, to_state, 'its agent reports' FROM moved WHERE $5) "
			"SELECT slots FROM moved;",
			{ BindValue(id), BindValue(agent_version), BindValue(agent_image),
					BindValue(static_cast<int64_t>(kAgentProtocol)),
					BindValue(db::Bool{ !booting.empty() }) });
	if (updated.empty())
		return std::nullopt;
	if (!booting.empty()) {
		if (const auto kind = parse_machine_kind(booting.front().as_text(0).value_or("")))
			clear_launch_backoff(db, *kind);
	}
	const int slots = static_cast<int>(updated.front().as_int(0).value_or(0));
	// One report per slot: a running container wins its slot, else the last.
	std::map<int, const ContainerReport *> by_slot;
	for (const ContainerReport &c : containers) {
		if (c.slot < 0 || c.slot >= slots)
			continue;
		const ContainerReport *&held = by_slot[c.slot];
		if (held == nullptr || held->state != ContainerState::Running ||
				c.state == ContainerState::Running)
			held = &c;
	}
	db.exec("DELETE FROM fleet_slot_reports WHERE machine_id = $1;", { BindValue(id) });
	for (const auto &[slot, c] : by_slot) {
		const auto tick = [&c](double TickHealth::*field) {
			return c->tick ? BindValue((*c->tick).*field) : BindValue(std::monostate{});
		};
		db.exec("INSERT INTO fleet_slot_reports (machine_id, slot, container, state, waiting, "
				"exit_code, oom, started_at, reason, cpu_percent, memory_bytes, egress_bytes, "
				"net_in_bytes, tick_p50_ms, tick_p99_ms, tick_max_ms, late_ticks) "
				"VALUES ($1, $2, $3, $4, $5, $6, $7, to_timestamp($8::double precision), $9, $10, "
				"$11, $12, $13, $14, $15, $16, $17);",
				{ BindValue(id), BindValue(static_cast<int64_t>(slot)), BindValue(c->name),
						BindValue(std::string(container_state_text(c->state))),
						BindValue(std::string(container_wait_text(c->waiting))),
						optional_int(c->exit_code), BindValue(db::Bool{ c->oom }),
						optional_int(c->started_at), BindValue(c->reason),
						BindValue(c->cpu_percent), count_value(c->memory_bytes),
						count_value(c->egress_bytes), count_value(c->net_in_bytes),
						tick(&TickHealth::p50_ms), tick(&TickHealth::p99_ms),
						tick(&TickHealth::max_ms),
						c->tick ? count_value(c->tick->late) : BindValue(std::monostate{}) });
	}
	auto row = find_machine(db, id);
	tx.commit();
	return row;
}

std::optional<FleetMachineRow> record_unlisted_sync(db::Database &db, int64_t id,
		const std::string &agent_version, const std::string &agent_image) {
	db.exec("UPDATE fleet_machines SET last_report_at = now(), agent_version = $2, "
			"agent_image = $3, agent_protocol = $4, protocol_mismatch_since = NULL, "
			"bootstrap_token_hash = '', bootstrap_expires_at = NULL "
			"WHERE id = $1 AND state IN ('booting', 'ready', 'draining', 'lost');",
			{ BindValue(id), BindValue(agent_version), BindValue(agent_image),
					BindValue(static_cast<int64_t>(kAgentProtocol)) });
	if (db.changes() == 0)
		return std::nullopt;
	return find_machine(db, id);
}

std::optional<FleetMachineRow> record_held_sync(
		db::Database &db, int64_t id, const AgentEnvelope &envelope, int64_t &mismatch_s) {
	const auto updated = db.query(
			"UPDATE fleet_machines SET last_report_at = now(), agent_version = $2, "
			"agent_image = $3, agent_protocol = $4, "
			"protocol_mismatch_since = COALESCE(protocol_mismatch_since, now()), "
			"bootstrap_token_hash = '', bootstrap_expires_at = NULL "
			"WHERE id = $1 AND state IN ('booting', 'ready', 'draining', 'lost') "
			"RETURNING CAST(extract(epoch FROM now() - protocol_mismatch_since) AS bigint);",
			{ BindValue(id), BindValue(envelope.agent_version), BindValue(envelope.agent_image),
					BindValue(static_cast<int64_t>(envelope.protocol)) });
	if (updated.empty())
		return std::nullopt;
	mismatch_s = updated.front().as_int(0).value_or(0);
	return find_machine(db, id);
}

bool record_machine_report(db::Database &db, int64_t id, const SyncRequest &report) {
	db::Transaction tx(db);
	const MachineStats stats = report.machine.value_or(MachineStats{});
	const auto measured = [&report](uint64_t value) {
		return report.machine ? count_value(value) : BindValue(std::monostate{});
	};
	// A new log stream (an ECS task's restart names another): its own event, in
	// the statement that records it.
	const auto moved = db.query(
			"WITH before AS (SELECT agent_log_stream FROM fleet_machines WHERE id = $1), "
			"moved AS (UPDATE fleet_machines SET "
			"image_id = CASE WHEN $2::text <> '' THEN $2::text ELSE image_id END, "
			"agent_log_stream = CASE WHEN $3::text <> '' THEN $3::text ELSE agent_log_stream END, "
			"cpu_percent = CASE WHEN $4 THEN $5 ELSE cpu_percent END, "
			"memory_used_bytes = CASE WHEN $4 THEN $6 ELSE memory_used_bytes END, "
			"memory_total_bytes = CASE WHEN $4 THEN $7 ELSE memory_total_bytes END, "
			"disk_free_bytes = CASE WHEN $4 THEN $8 ELSE disk_free_bytes END, "
			"disk_total_bytes = CASE WHEN $4 THEN $9 ELSE disk_total_bytes END, "
			"net_in_bytes = CASE WHEN $4 THEN $10 ELSE net_in_bytes END, "
			"net_out_bytes = CASE WHEN $4 THEN $11 ELSE net_out_bytes END, "
			"stats_at = CASE WHEN $4 THEN now() ELSE stats_at END "
			"WHERE id = $1 AND state IN ('booting', 'ready', 'draining', 'lost') "
			"RETURNING id, state), "
			"event AS (INSERT INTO fleet_machine_events (machine_id, from_state, to_state, detail) "
			"SELECT moved.id, moved.state, moved.state, 'log stream ' || $3::text "
			"FROM moved, before WHERE $3::text <> '' AND before.agent_log_stream <> $3::text) "
			"SELECT id FROM moved;",
			{ BindValue(id), BindValue(report.image_id), BindValue(report.log_stream),
					BindValue(db::Bool{ report.machine.has_value() }),
					report.machine ? BindValue(stats.cpu_percent) : BindValue(std::monostate{}),
					measured(stats.memory_used_bytes), measured(stats.memory_total_bytes),
					measured(stats.disk_free_bytes), measured(stats.disk_total_bytes),
					measured(stats.net_in_bytes), measured(stats.net_out_bytes) });
	if (moved.empty())
		return false;
	db.exec("DELETE FROM fleet_machine_trees WHERE machine_id = $1;", { BindValue(id) });
	for (const auto &[tree_id, tree] : report.trees) {
		db.exec("INSERT INTO fleet_machine_trees (machine_id, tree_id, state, bytes, tries, error) "
				"VALUES ($1, $2, $3, $4, $5, $6);",
				{ BindValue(id), BindValue(tree_id),
						BindValue(std::string(tree_report_state_text(tree.state))),
						count_value(tree.bytes), BindValue(static_cast<int64_t>(tree.tries)),
						BindValue(tree.reason) });
	}
	tx.commit();
	return true;
}

std::vector<MachineTree> machine_trees(db::Database &db, int64_t machine_id) {
	std::vector<MachineTree> out;
	for (const db::Row &r :
			db.query("SELECT tree_id, state, bytes, tries, error, reported_at "
					 "FROM fleet_machine_trees WHERE machine_id = $1 ORDER BY tree_id;",
					{ BindValue(machine_id) })) {
		MachineTree t;
		t.tree_id = r.as_int(0).value_or(0);
		t.state = parse_tree_report_state(r.as_text(1).value_or(""))
						  .value_or(TreeReportState::Failed);
		t.bytes = as_count(r, 2);
		t.tries = static_cast<int>(r.as_int(3).value_or(0));
		t.error = r.as_text(4).value_or("");
		t.reported_at = r.as_text(5).value_or("");
		out.push_back(std::move(t));
	}
	return out;
}

std::vector<FleetMachineEvent> list_machine_events(
		db::Database &db, int64_t machine_id, size_t limit) {
	std::vector<FleetMachineEvent> out;
	for (const db::Row &r :
			db.query("SELECT id, machine_id, COALESCE(from_state, ''), to_state, "
					 "detail, at FROM (SELECT * FROM fleet_machine_events WHERE "
					 "machine_id = $1 ORDER BY id DESC LIMIT $2) newest ORDER BY id;",
					{ BindValue(machine_id), BindValue(static_cast<int64_t>(limit)) })) {
		FleetMachineEvent e;
		e.id = r.as_int(0).value_or(0);
		e.machine_id = r.as_int(1).value_or(0);
		e.from_state = r.as_text(2).value_or("");
		e.to_state = r.as_text(3).value_or("");
		e.detail = r.as_text(4).value_or("");
		e.at = r.as_text(5).value_or("");
		out.push_back(std::move(e));
	}
	return out;
}

namespace {

std::vector<SlotReport> query_slot_reports(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<SlotReport> out;
	for (const db::Row &r :
			db.query("SELECT r.slot, r.container, r.state, r.exit_code, r.oom, "
					 "CAST(extract(epoch FROM r.started_at) AS bigint), r.reason, r.cpu_percent, "
					 "r.memory_bytes, r.egress_bytes, r.reported_at, r.waiting, r.net_in_bytes, "
					 "r.tick_p50_ms, r.tick_p99_ms, r.tick_max_ms, r.late_ticks "
					 "FROM fleet_slot_reports r " +
							tail,
					binds)) {
		SlotReport s;
		s.slot = static_cast<int>(r.as_int(0).value_or(0));
		s.container = r.as_text(1).value_or("");
		s.state = parse_container_state(r.as_text(2).value_or("")).value_or(ContainerState::Exited);
		s.exit_code = r.as_int(3);
		s.oom = r.as_int(4).value_or(0) != 0;
		s.started_at = r.as_int(5);
		s.reason = r.as_text(6).value_or("");
		s.cpu_percent = as_double(r, 7);
		s.memory_bytes = as_count(r, 8);
		s.egress_bytes = as_count(r, 9);
		s.reported_at = r.as_text(10).value_or("");
		s.waiting = parse_container_wait(r.as_text(11).value_or("")).value_or(ContainerWait::None);
		s.net_in_bytes = as_count(r, 12);
		if (!r.is_null(13)) {
			TickHealth t;
			t.p50_ms = as_double(r, 13);
			t.p99_ms = as_double(r, 14);
			t.max_ms = as_double(r, 15);
			t.late = as_count(r, 16);
			s.tick = t;
		}
		out.push_back(std::move(s));
	}
	return out;
}

} // namespace

std::vector<SlotReport> slot_reports(db::Database &db, int64_t machine_id) {
	return query_slot_reports(
			db, "WHERE r.machine_id = $1 ORDER BY r.slot;", { BindValue(machine_id) });
}

std::optional<SlotReport> slot_report_of(db::Database &db, int64_t server_id) {
	auto rows = query_slot_reports(db,
			"JOIN hosted_servers s ON s.machine_ref = r.machine_id AND s.slot = r.slot "
			"WHERE s.id = $1;",
			{ BindValue(server_id) });
	if (rows.empty())
		return std::nullopt;
	return std::move(rows.front());
}

// --- the slots ---

std::vector<SlotHolder> slot_holders(db::Database &db, int64_t machine_id) {
	std::vector<SlotHolder> out;
	for (const db::Row &r : db.query(std::string("SELECT id, slot, game_port, generation, state "
												 "FROM hosted_servers WHERE machine_ref = $1 AND "
												 "slot IS NOT NULL AND ") +
						 kLiveServerSql + " ORDER BY slot;",
				 { BindValue(machine_id) })) {
		SlotHolder h;
		h.server_id = r.as_int(0).value_or(0);
		h.slot = static_cast<int>(r.as_int(1).value_or(0));
		h.game_port = static_cast<int>(r.as_int(2).value_or(0));
		h.generation = r.as_int(3).value_or(0);
		h.state = state_from_name(r.as_text(4).value_or("")).value_or(State::Failed);
		out.push_back(h);
	}
	return out;
}

std::vector<FleetMachine> placement_machines(db::Database &db) {
	db::ReadSnapshot snapshot(db);
	std::vector<FleetMachine> out;
	std::map<int64_t, size_t> index;
	for (const FleetMachineRow &row : list_live_machines(db)) {
		FleetMachine m;
		m.id = row.id;
		m.kind = row.kind;
		m.state = row.state;
		m.slots = row.slots;
		m.reserved_for = row.reserved_for;
		index[m.id] = out.size();
		out.push_back(std::move(m));
	}
	for (const db::Row &r : db.query(std::string("SELECT machine_ref, slot FROM hosted_servers "
												 "WHERE machine_ref IS NOT NULL AND slot IS NOT "
												 "NULL AND ") +
				 kLiveServerSql + " ORDER BY machine_ref, slot;")) {
		const auto it = index.find(r.as_int(0).value_or(0));
		if (it != index.end())
			out[it->second].used_slots.push_back(static_cast<int>(r.as_int(1).value_or(0)));
	}
	return out;
}

std::vector<PlacementRequest> waiting_servers(db::Database &db) {
	std::vector<PlacementRequest> out;
	for (const db::Row &r : db.query(std::string("SELECT id, own_machine FROM hosted_servers WHERE "
												 "machine_ref IS NULL AND ") +
					 kLiveServerSql + " AND state <> 'stopping' ORDER BY id;")) {
		PlacementRequest request;
		request.server_id = r.as_int(0).value_or(0);
		request.own_machine = r.as_int(1).value_or(0) != 0;
		out.push_back(request);
	}
	return out;
}

bool fleet_has_room(db::Database &db, const PlacementPolicy &policy, bool own_machine) {
	db::ReadSnapshot snapshot(db);
	std::vector<FleetMachine> machines = placement_machines(db);
	const std::vector<PlacementRequest> waiting = waiting_servers(db);
	PlacementRequest request;
	request.server_id = -1; // no row's (the start not yet written), no machine reserved for it
	request.own_machine = own_machine;
	return place_after(request, waiting, std::move(machines), policy).outcome != Placement::Outcome::Full;
}

std::optional<int64_t> assign_slot(
		db::Database &db, int64_t server_id, int64_t machine_id, int slot, int game_port) {
	if (!slot_valid(slot) || game_port < 0 || game_port > 65535)
		return std::nullopt;
	try {
		// Its own transaction (a savepoint inside the caller's), so a slot another
		// server holds undoes this write alone. The new generation is a start of
		// its own (hosted_server_starts), made in the same statement; one an
		// earlier start left open is ended there (the machine's cause).
		db::Transaction tx(db);
		const auto rows = db.query(std::string("WITH placed AS (UPDATE hosted_servers SET "
											   "machine_ref = $2, slot = $3, game_port = $4, "
											   "generation = generation + 1, updated_at = now() "
											   "WHERE id = $1 AND machine_ref IS NULL AND ") +
						kLiveServerSql +
						" AND EXISTS (SELECT 1 FROM fleet_machines WHERE id = $2 AND $3 < slots "
						"AND state IN ('launching', 'booting', 'ready')) "
						"RETURNING id, generation, machine_ref, slot), "
						"left_open AS (UPDATE hosted_server_starts SET ended_at = now(), "
						"cause = COALESCE(cause, 'fleet') WHERE server_id = $1 AND ended_at IS "
						"NULL "
						"AND EXISTS (SELECT 1 FROM placed)), "
						"started AS (INSERT INTO hosted_server_starts (server_id, generation, "
						"machine_id, slot) SELECT id, generation, machine_ref, slot FROM placed) "
						"SELECT generation FROM placed;",
				{ BindValue(server_id), BindValue(machine_id),
						BindValue(static_cast<int64_t>(slot)),
						BindValue(static_cast<int64_t>(game_port)) });
		if (rows.empty())
			return std::nullopt;
		tx.commit();
		return rows.front().as_int(0);
	} catch (const db::Error &e) {
		if (e.unique_violation())
			return std::nullopt;
		throw;
	}
}

bool release_slot(db::Database &db, int64_t server_id) {
	db.exec("UPDATE hosted_servers SET machine_ref = NULL, slot = NULL, game_port = 0, "
			"updated_at = now() WHERE id = $1 AND machine_ref IS NOT NULL;",
			{ BindValue(server_id) });
	return db.changes() > 0;
}

} // namespace opennova::novaworld_server::hosted
