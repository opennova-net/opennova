// The hosted servers' stats (apps/novaworld_fleet/hosted/stats_sampler.h,
// stats_repository.h; migration 0004_server_stats.sql) on a migrated database
// and a hand-stepped wall clock: the sampler's one sample a minute of each live
// server (its listing's players, slots and mission, its admin link's scene,
// its agent's CPU as a share of the slot's CPUs, its memory, and its slot's
// egress as the counter's differences, a first sample counting none and a
// counter that started over counted from 0; its bytes in and out as counters
// since its first sample, the differences summed; its server's ticks), the
// unique players counted from the roster and never stored, the missions started
// in the game scene, the hourly rollup (its peak memory, bytes in and out, worst
// p99 and late ticks among the rest), the machine's monthly egress, every live
// machine's minute from its agent's report, and the prune (the stats.prune job,
// the pass's sample never, which takes the fleet's machines ended 14 days ago
// too); the machines' minutes rolled up by the hour (the counters' differences,
// one that started over) and pruned at 48 hours and 400 days, a stale report
// no minute; then the reads the
// stats routes answer from, over hand-written samples: the summary (live and
// running time, availability, the peak and the average players, the
// player-hours, the egress, the restarts, the month's egress) and the series'
// buckets for each range (1h a minute, 24h five minutes, 7d an hour from the
// rollups, 30d four hours), for one server, an account's servers together and
// the fleet, the restarts counted from the starts its crashes and timeouts
// ended; and the ISO timestamps. Compiles the hosted sources directly.

#include "hosted_fleet_rig.h"

#include "hosted/stats_jobs.h"
#include "hosted/stats_repository.h"
#include "hosted/stats_sampler.h"
#include "jobs/job_queue.h"
#include "jobs/worker_pool.h"

#include "common/test_expect.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace hosted_rig;

namespace {

bool near(std::optional<double> value, double want) {
	return value && std::fabs(*value - want) < 1e-6;
}

std::optional<double> double_of(const opennova::db::Row &row, size_t i) {
	if (const auto *d = std::get_if<double>(&row.values[i])) return *d;
	return std::nullopt;
}

int64_t count_rows(Database &db, const std::string &sql) {
	const auto rows = db.query(sql);
	return rows.empty() ? 0 : rows[0].as_int(0).value_or(0);
}

// A live server of `owner`'s in `state` (written by hand), on Joint Operations' data set
// (seeded with a ready tree at the first call).
int64_t make_server(Database &db, int64_t owner, const char *state) {
	int64_t jo = hosted_rig::seeded_data_set(db, "Joint Operations");
	if (hosted::offered_data_sets(db).empty()) jo = hosted_rig::seed_data(db).jo;
	const auto r = hosted::request_server(db, owner, valid_settings(jo), hosted::offered_data_sets(db), 100, kWall);
	if (r.outcome != hosted::RequestOutcome::Created) return 0;
	db.exec("UPDATE hosted_servers SET state = $1 WHERE id = $2;", { BindValue(std::string(state)), BindValue(r.id) });
	return r.id;
}

// The listing server `id`'s service account holds: RID `rid`, `players`, a
// mission, its roster's names.
void list_server(Database &db, int64_t id, uint32_t rid, int players, const std::string &mission,
		const std::vector<std::string> &names) {
	auto row = hosted::find_server(db, id);
	if (!row->service_user_id) {
		const auto account = nws::upsert_service_account(db, nws::service_username(id), "pw-stats");
		hosted::set_service_account(db, id, account.id);
		row = hosted::find_server(db, id);
	}
	if (!hostdb::find_host_by_rid(db, rid)) list_as(db, *row->service_user_id, rid);
	auto host = *hostdb::find_host_by_rid(db, rid);
	host.player_count = players;
	host.max_players = 32;
	host.mission_name = mission;
	hostdb::upsert_host(db, host);
	std::vector<opennova::HostRosterSlot> roster;
	int slot = 1;
	for (const std::string &name : names) {
		opennova::HostRosterSlot s;
		s.slot = slot++;
		s.player_name = name;
		roster.push_back(s);
	}
	hostdb::replace_roster(db, rid, roster);
}

// What machine `machine`'s agent reports of slot 0: server `id`'s container.
void report(Database &db, int64_t machine, int64_t id, double cpu, uint64_t memory, uint64_t egress,
		uint64_t net_in = 0, std::optional<hosted::TickHealth> tick = std::nullopt) {
	hosted::ContainerReport c;
	c.name = hosted::hosted_container_name(id, 1);
	c.slot = 0;
	c.state = hosted::ContainerState::Running;
	c.cpu_percent = cpu;
	c.memory_bytes = memory;
	c.egress_bytes = egress;
	c.net_in_bytes = net_in;
	c.tick = tick;
	hosted::record_sync(db, machine, "stats-agent", { c });
}

// Server `id`'s minute `at`'s network counters and ticks.
struct NetTick {
	std::optional<int64_t> in, out, late;
	std::optional<double> p99;
};
NetTick net_tick_at(Database &db, int64_t id, int64_t at) {
	NetTick out;
	const auto rows = db.query("SELECT net_in_bytes, net_out_bytes, tick_p99_ms, late_ticks FROM hosted_server_samples "
							   "WHERE server_id = $1 AND at = to_timestamp($2::double precision);",
			{ BindValue(id), BindValue(at) });
	if (rows.empty()) return out;
	out.in = rows[0].as_int(0);
	out.out = rows[0].as_int(1);
	if (const auto *d = std::get_if<double>(&rows[0].values[2])) out.p99 = *d;
	out.late = rows[0].as_int(3);
	return out;
}

// Server `id`'s sample of the minute `at`.
std::optional<hosted::ServerSample> sample_at(Database &db, int64_t id, int64_t at) {
	const auto rows = db.query("SELECT state, players, max_players, COALESCE(mission, ''), COALESCE(scene, ''), "
							   "link_up, cpu_percent, memory_bytes, egress_bytes, mission_started "
							   "FROM hosted_server_samples WHERE server_id = $1 AND at = to_timestamp($2::double precision);",
			{ BindValue(id), BindValue(at) });
	if (rows.empty()) return std::nullopt;
	const auto &r = rows[0];
	hosted::ServerSample s;
	s.server_id = id;
	s.at = at;
	s.state = r.as_text(0).value_or("");
	s.players = r.as_int(1);
	s.max_players = r.as_int(2);
	s.mission = r.as_text(3).value_or("");
	s.scene = r.as_text(4).value_or("");
	s.link_up = r.as_int(5).value_or(0) != 0;
	if (const auto *d = std::get_if<double>(&r.values[6])) s.cpu_percent = *d;
	s.memory_bytes = r.as_int(7);
	s.egress_bytes = r.as_int(8).value_or(0);
	s.mission_started = r.as_int(9).value_or(0) != 0;
	return s;
}

int test_sampler() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	const int64_t owner = make_player(db, "stats_owner", 3);
	const int64_t id = make_server(db, owner, "running");
	const int64_t waiting = make_server(db, owner, "provisioning");
	TEST_EXPECT(id > 0 && waiting > 0);
	// Its machine and slot, and its container as the row names it.
	const auto machine = hosted::create_machine(db, { "fake", hosted::MachineKind::Shared, 4, kServeImage, 0 });
	hosted::LaunchedMachine launched;
	launched.cloud_id = "fake-stats";
	launched.private_address = "10.0.1.9";
	TEST_EXPECT(hosted::mark_launched(db, machine.id, launched));
	TEST_EXPECT(hosted::assign_slot(db, id, machine.id, 0, 32768) == 1);
	hosted::set_machine(db, id, "fake", hosted::hosted_container_name(id, 1), "10.0.1.9");
	list_server(db, id, 4101, 3, "first.bms", { "alice", "bob", "carol" });
	report(db, machine.id, id, 50.0, 100 << 20, 1000, 300);
	// The machine's own use, as its agent reported it.
	hosted::SyncRequest machine_report;
	machine_report.agent_version = "stats-agent";
	hosted::MachineStats use;
	use.cpu_percent = 37.5;
	use.memory_used_bytes = 3ull << 30;
	use.memory_total_bytes = 4ull << 30;
	use.disk_free_bytes = 9ull << 30;
	use.net_in_bytes = 1234;
	use.net_out_bytes = 5678;
	machine_report.machine = use;
	TEST_EXPECT(hosted::record_machine_report(db, machine.id, machine_report));

	hosted::StatsSampler sampler(1.0);
	std::string scene = "Game";
	const hosted::StatsSampler::LinkLookup links = [&scene, id](int64_t server) -> std::optional<hosted::StatsSampler::LinkView> {
		if (server != id) return std::nullopt;
		return hosted::StatsSampler::LinkView{ scene, true };
	};
	const int64_t t0 = 1760000040; // a whole minute
	TEST_EXPECT(sampler.sample(db, t0 + 5, links));
	TEST_EXPECT(!sampler.sample(db, t0 + 50, links)); // the same minute: once
	auto s = sample_at(db, id, t0);
	TEST_EXPECT(s && s->state == "running" && s->players == 3 && s->max_players == 32);
	TEST_EXPECT(s->mission == "first.bms" && s->scene == "game" && s->link_up);
	TEST_EXPECT(near(s->cpu_percent, 50.0) && s->memory_bytes == (100 << 20));
	TEST_EXPECT(s->egress_bytes == 0 && s->mission_started); // the first sample counts no egress
	const auto w = sample_at(db, waiting, t0);
	TEST_EXPECT(w && w->state == "provisioning" && !w->players && !w->cpu_percent && w->scene.empty() && !w->link_up);
	// Its network counters start at 0; one with no report has none.
	TEST_EXPECT(net_tick_at(db, id, t0).in == 0 && net_tick_at(db, id, t0).out == 0 && !net_tick_at(db, id, t0).p99);
	TEST_EXPECT(!net_tick_at(db, waiting, t0).in && !net_tick_at(db, waiting, t0).out);
	// The machine's minute: its use, and the one live server holding a slot on it.
	{
		const auto m = db.query("SELECT cpu_percent, memory_used_bytes, memory_total_bytes, disk_free_bytes, "
								"net_in_bytes, net_out_bytes, servers FROM fleet_machine_samples WHERE machine_id = $1 "
								"AND at = to_timestamp($2::double precision);",
				{ BindValue(machine.id), BindValue(t0) });
		TEST_EXPECT(m.size() == 1);
		if (m.size() == 1) {
			TEST_EXPECT(std::get<double>(m[0].values[0]) == 37.5 && m[0].as_int(1) == int64_t(3ll << 30) &&
					m[0].as_int(2) == int64_t(4ll << 30) && m[0].as_int(3) == int64_t(9ll << 30));
			TEST_EXPECT(m[0].as_int(4) == 1234 && m[0].as_int(5) == 5678 && m[0].as_int(6) == 1);
		}
	}

	// The next minutes: the counter's differences, a counter that started over
	// counted from 0, a new mission in the game scene counted, one in the menus not.
	report(db, machine.id, id, 50.0, 100 << 20, 1500, 700, hosted::TickHealth{ 1.0, 16.0, 30.0, 2 });
	TEST_EXPECT(sampler.sample(db, t0 + 60, links));
	s = sample_at(db, id, t0 + 60);
	TEST_EXPECT(s && s->egress_bytes == 500 && !s->mission_started);
	TEST_EXPECT(net_tick_at(db, id, t0 + 60).in == 400 && net_tick_at(db, id, t0 + 60).out == 500 &&
			net_tick_at(db, id, t0 + 60).p99 == 16.0 && net_tick_at(db, id, t0 + 60).late == 2);
	list_server(db, id, 4101, 4, "second.bms", { "alice", "bob", "dave" });
	report(db, machine.id, id, 50.0, 100 << 20, 200, 100, hosted::TickHealth{ 1.0, 20.0, 45.0, 3 });
	TEST_EXPECT(sampler.sample(db, t0 + 120, links));
	s = sample_at(db, id, t0 + 120);
	TEST_EXPECT(s && s->egress_bytes == 200 && s->mission_started && s->players == 4);
	// The agent's counters started over: the server's own carry on, counted from 0.
	TEST_EXPECT(net_tick_at(db, id, t0 + 120).in == 500 && net_tick_at(db, id, t0 + 120).out == 700);
	scene = "Menus";
	list_server(db, id, 4101, 0, "third.bms", {});
	report(db, machine.id, id, 50.0, 100 << 20, 300, 250);
	TEST_EXPECT(sampler.sample(db, t0 + 180, links));
	s = sample_at(db, id, t0 + 180);
	TEST_EXPECT(s && s->egress_bytes == 100 && !s->mission_started && s->scene == "menus");
	TEST_EXPECT(net_tick_at(db, id, t0 + 180).in == 650 && net_tick_at(db, id, t0 + 180).out == 800 &&
			!net_tick_at(db, id, t0 + 180).p99);
	// Another container's report on its slot is not this server's.
	hosted::set_machine(db, id, "fake", hosted::hosted_container_name(id, 2), "10.0.1.9");
	TEST_EXPECT(sampler.sample(db, t0 + 240, links));
	s = sample_at(db, id, t0 + 240);
	TEST_EXPECT(s && !s->cpu_percent && s->egress_bytes == 0);
	hosted::set_machine(db, id, "fake", hosted::hosted_container_name(id, 1), "10.0.1.9");

	// The hour so far, rolled up: five minutes, the distinct names (alice, bob,
	// carol, dave), two missions, the bytes, the mean CPU; the peak memory, the
	// bytes in (the counters' differences) and out, the worst p99, the late ticks.
	const auto hour = db.query("SELECT live_minutes, running_minutes, player_minutes, peak_players, max_players, "
							   "unique_players, missions, egress_bytes, avg_cpu, peak_memory_bytes, net_in_bytes, "
							   "net_out_bytes, tick_p99_max_ms, late_ticks FROM hosted_server_hourly "
							   "WHERE server_id = $1;",
			{ BindValue(id) });
	TEST_EXPECT(hour.size() == 1);
	if (hour.size() == 1) {
		TEST_EXPECT(hour[0].as_int(0) == 5 && hour[0].as_int(1) == 5 && hour[0].as_int(2) == 3 + 3 + 4 + 0 + 0);
		TEST_EXPECT(hour[0].as_int(3) == 4 && hour[0].as_int(4) == 32 && hour[0].as_int(5) == 4);
		TEST_EXPECT(hour[0].as_int(6) == 2 && hour[0].as_int(7) == 800);
		TEST_EXPECT(hour[0].as_int(9) == (100 << 20) && hour[0].as_int(10) == 650 && hour[0].as_int(11) == 800);
		TEST_EXPECT(std::get<double>(hour[0].values[12]) == 20.0 && hour[0].as_int(13) == 5);
	}
	// No name is stored anywhere.
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM hosted_server_samples WHERE mission LIKE '%alice%';") == 0);
	// The machine's month.
	TEST_EXPECT(hosted::machine_egress_month(db, machine.id, t0 + 240) == 800);
	TEST_EXPECT(hosted::machine_egress_month(db, machine.id, t0 + 40 * 86400) == 0);

	// The CPU as a share of the slot's CPUs (two here), within 0..100.
	hosted::StatsSampler two_cpus(2.0);
	report(db, machine.id, id, 150.0, 1, 300);
	TEST_EXPECT(two_cpus.sample(db, t0 + 300, links));
	s = sample_at(db, id, t0 + 300);
	TEST_EXPECT(s && near(s->cpu_percent, 75.0) && s->egress_bytes == 0);
	report(db, machine.id, id, 500.0, 1, 300);
	TEST_EXPECT(two_cpus.sample(db, t0 + 360, links));
	TEST_EXPECT(near(sample_at(db, id, t0 + 360)->cpu_percent, 100.0));

	// The prune is the hourly stats.prune job's, never the pass's: the first
	// sample of an hour prunes nothing; the job, at its clock's hour, takes the
	// samples past 48 h and the hours past 400 days.
	hosted::ServerSample old;
	old.server_id = id;
	old.at = t0 - 49 * 3600;
	old.state = "running";
	hosted::insert_sample(db, old);
	hosted::rollup_hour(db, id, t0 - 401 * 86400, 0);
	hosted::rollup_hour(db, id, t0 - 91 * 86400, 0); // kept: hours live 400 days now
	const std::string old_hours = "SELECT COUNT(*) FROM hosted_server_hourly WHERE hour < to_timestamp(" +
			std::to_string(t0 - 400 * 86400) + ");";
	TEST_EXPECT(sample_at(db, id, old.at).has_value() && count_rows(db, old_hours) == 1);
	TEST_EXPECT(two_cpus.sample(db, t0 + 4000, links)); // a new hour's first sample
	TEST_EXPECT(sample_at(db, id, old.at).has_value() && count_rows(db, old_hours) == 1);
	jobs::JobRegistry registry;
	hosted::register_stats_jobs(registry, [t0] { return t0 + 4000; });
	const auto schedule = hosted::stats_schedule();
	TEST_EXPECT(schedule.size() == 1 && schedule[0].kind == hosted::kJobStatsPrune &&
			schedule[0].period_ms == 60 * 60 * 1000);
	// The same job takes the fleet's machines ended more than 14 days ago.
	const auto gone =
			hosted::create_machine(db, { "fake", hosted::MachineKind::Own, 1, kServeImage, 0 });
	TEST_EXPECT(hosted::fail_refused_launch(db, gone.id, "the launch failed"));
	db.exec("UPDATE fleet_machines SET ended_at = now() - interval '15 days' WHERE id = $1;",
			{ BindValue(gone.id) });
	jobs::NewJob prune;
	prune.kind = hosted::kJobStatsPrune;
	TEST_EXPECT(jobs::enqueue(db, prune) > 0);
	TEST_EXPECT(jobs::run_due(pool, registry) == 1);
	TEST_EXPECT(!sample_at(db, id, old.at) && count_rows(db, old_hours) == 0);
	TEST_EXPECT(sample_at(db, id, t0).has_value()); // within 48 h: kept
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM hosted_server_hourly WHERE hour = to_timestamp(" +
							std::to_string((t0 - 91 * 86400) - (t0 - 91 * 86400) % 3600) + ");") == 1);
	TEST_EXPECT(!hosted::find_machine(db, gone.id) && hosted::find_machine(db, machine.id));
	return 0;
}

// A hand-written minute of `id`'s.
void put(Database &db, int64_t id, int64_t at, const char *state, std::optional<int64_t> players, int64_t egress,
		std::optional<double> cpu) {
	hosted::ServerSample s;
	s.server_id = id;
	s.at = at;
	s.state = state;
	s.players = players;
	s.max_players = players ? std::optional<int64_t>(32) : std::nullopt;
	s.egress_bytes = egress;
	s.cpu_percent = cpu;
	hosted::insert_sample(db, s);
}

int test_reads() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	const int64_t x = make_player(db, "reads_x", 3);
	const int64_t y = make_player(db, "reads_y", 3);
	const int64_t a = make_server(db, x, "running");
	const int64_t b = make_server(db, x, "running");
	const int64_t c = make_server(db, y, "running");
	TEST_EXPECT(a > 0 && b > 0 && c > 0);
	// now: half a minute into the minute 1760004000 (an hour's first).
	const int64_t m0 = 1760004000;
	const int64_t now = m0 + 30;
	// A: ten minutes, the last eight running with 2 players and 100 bytes each.
	for (int k = 0; k < 10; ++k) {
		const bool running = k < 8;
		put(db, a, m0 - 60 * k, running ? "running" : "provisioning", running ? std::optional<int64_t>(2) : std::nullopt,
				running ? 100 : 0, running ? std::optional<double>(40.0) : std::nullopt);
	}
	// B: five minutes running with 3 players; C: one with 1.
	for (int k = 0; k < 5; ++k) put(db, b, m0 - 60 * k, "running", 3, 50, 20.0);
	put(db, c, m0, "running", 1, 10, 10.0);
	// The rollups (unique players as the sampler would count them).
	for (const int64_t id : { a, b, c }) {
		hosted::rollup_hour(db, id, m0, id == a ? 1 : 0);
		hosted::rollup_hour(db, id, m0 - 60, id == a ? 2 : 1);
	}
	// A's restarts, its starts that its own cause ended: two in the range (a crash,
	// a start that timed out), one before it, and a move and a stop that count none.
	int64_t generation = 0;
	for (const auto &[at, cause] : std::vector<std::pair<int64_t, std::string>>{
				 { m0 - 120, "crash" }, { m0 - 60, "timeout" }, { m0 - 7200, "crash" }, { m0 - 180, "fleet" },
				 { m0 - 30, "stop" } })
		db.exec("INSERT INTO hosted_server_starts (server_id, generation, machine_id, slot, placed_at, ended_at, "
				"cause) VALUES ($1, $2, 99, 0, to_timestamp($3::double precision) - interval '10 minutes', "
				"to_timestamp($3::double precision), $4);",
				{ BindValue(a), BindValue(++generation), BindValue(at), BindValue(cause) });

	const auto range = [](const char *name) { return *hosted::parse_stats_range(name); };
	TEST_EXPECT(!hosted::parse_stats_range("2h") && !hosted::parse_stats_range(""));
	hosted::StatsScope one;
	one.server_id = a;
	hosted::StatsScope account;
	account.owner_user_id = x;
	const hosted::StatsScope fleet;

	// 1h from the samples.
	auto s = hosted::stats_summary(db, one, range("1h"), now);
	TEST_EXPECT(s.live_seconds == 600 && s.running_seconds == 480 && s.peak_players == 2);
	TEST_EXPECT(near(s.avg_players, 2.0) && std::fabs(s.player_hours - 16.0 / 60) < 1e-9);
	TEST_EXPECT(s.egress_bytes == 800 && s.missions == 0 && s.restarts == 2 && s.unique_players == 3);
	TEST_EXPECT(s.egress_bytes_month == 800);
	s = hosted::stats_summary(db, account, range("1h"), now);
	TEST_EXPECT(s.live_seconds == 900 && s.running_seconds == 780 && s.peak_players == 5);
	TEST_EXPECT(near(s.avg_players, 31.0 / 8) && s.egress_bytes == 1050 && s.restarts == 2);
	s = hosted::stats_summary(db, fleet, range("1h"), now);
	TEST_EXPECT(s.live_seconds == 960 && s.running_seconds == 840 && s.peak_players == 6);
	TEST_EXPECT(near(s.avg_players, 32.0 / 8) && s.egress_bytes == 1060);
	// Another account's server is not the account's.
	hosted::StatsScope other;
	other.owner_user_id = y;
	TEST_EXPECT(hosted::stats_summary(db, other, range("1h"), now).live_seconds == 60);

	// The 1h series: a point a minute, the newest last.
	auto points = hosted::stats_series(db, one, range("1h"), now);
	TEST_EXPECT(points.size() == 60);
	if (points.size() == 60) {
		TEST_EXPECT(points[59].t == m0 && near(points[59].players, 2.0) && points[59].running);
		TEST_EXPECT(points[59].egress_bytes == 100 && near(points[59].cpu_percent, 40.0) && points[59].max_players == 32);
		TEST_EXPECT(points[51].t == m0 - 480 && !points[51].players && !points[51].running && points[51].egress_bytes == 0);
		TEST_EXPECT(!points[0].players && !points[0].egress_bytes && !points[0].running && !points[0].cpu_percent);
	}
	// The 24h series of the account's servers: five-minute buckets, the players
	// each server averaged, added up.
	points = hosted::stats_series(db, account, range("24h"), now);
	TEST_EXPECT(points.size() == 288);
	if (points.size() == 288) {
		TEST_EXPECT(points[287].t == m0 && near(points[287].players, 5.0) && points[287].egress_bytes == 150);
		TEST_EXPECT(points[286].t == m0 - 300 && near(points[286].players, 5.0) && points[286].egress_bytes == 700);
		TEST_EXPECT(points[286].max_players == 64 && points[286].running);
		TEST_EXPECT(points[285].t == m0 - 600 && near(points[285].players, 2.0) && points[285].egress_bytes == 200);
	}

	// 7d and 30d from the rollups.
	s = hosted::stats_summary(db, one, range("7d"), now);
	TEST_EXPECT(s.live_seconds == 600 && s.running_seconds == 480 && s.egress_bytes == 800);
	TEST_EXPECT(s.peak_players == 2 && near(s.avg_players, 2.0) && s.unique_players == 3 && s.restarts == 3);
	points = hosted::stats_series(db, one, range("7d"), now);
	TEST_EXPECT(points.size() == 168);
	if (points.size() == 168) {
		TEST_EXPECT(points[167].t == m0 && near(points[167].players, 2.0) && points[167].egress_bytes == 100);
		TEST_EXPECT(points[166].t == m0 - 3600 && near(points[166].players, 14.0 / 9) && points[166].running);
		TEST_EXPECT(points[166].egress_bytes == 700 && points[166].max_players == 32 && !points[0].running);
	}
	points = hosted::stats_series(db, fleet, range("30d"), now);
	TEST_EXPECT(points.size() == 180);
	if (points.size() == 180) {
		// One four-hour bucket holds both hours: each server's players per live
		// minute, added up.
		TEST_EXPECT(near(points[179].players, 16.0 / 10 + 15.0 / 5 + 1.0) && points[179].egress_bytes == 1060);
		TEST_EXPECT(points[179].t % (4 * 3600) == 0 && points[178].t == points[179].t - 4 * 3600);
	}
	s = hosted::stats_summary(db, fleet, range("30d"), now);
	TEST_EXPECT(s.live_seconds == 960 && s.egress_bytes == 1060 && s.unique_players == 5);

	// The ISO timestamps.
	TEST_EXPECT(hosted::iso_utc(0) == "1970-01-01T00:00:00Z");
	TEST_EXPECT(hosted::iso_utc(1760000000) == "2025-10-09T08:53:20Z");
	TEST_EXPECT(hosted::iso_utc(951782400) == "2000-02-29T00:00:00Z");
	return 0;
}

// The machines' minutes and hours: a minute from each live machine's fresh
// report alone, the hours rolled up from the minutes (the CPU's mean and peak,
// the memory's peak, the least disk room, the counters' differences, one that
// started over counted from 0), both pruned by their ages; the history outlives
// the machine's row.
int test_machine_stats() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	const auto fresh = hosted::create_machine(db, { "fake", hosted::MachineKind::Shared, 4, kServeImage, 0 });
	const auto stale = hosted::create_machine(db, { "fake", hosted::MachineKind::Shared, 4, kServeImage, 0 });
	for (const int64_t id : { fresh.id, stale.id }) {
		hosted::LaunchedMachine launched;
		launched.cloud_id = "fake-" + std::to_string(id);
		launched.private_address = "10.0.1." + std::to_string(id);
		TEST_EXPECT(hosted::mark_launched(db, id, launched));
		TEST_EXPECT(hosted::record_sync(db, id, "v1", {}));
		hosted::SyncRequest report;
		report.agent_version = "v1";
		report.machine = hosted::MachineStats{ 10.0, 1, 2, 3, 4, 5, 6 };
		TEST_EXPECT(hosted::record_machine_report(db, id, report));
	}
	db.exec("UPDATE fleet_machines SET stats_at = now() - interval '3 minutes' WHERE id = $1;",
			{ BindValue(stale.id) });
	const int64_t minute = 1760004000;
	TEST_EXPECT(hosted::sample_machines(db, minute) == 1);
	TEST_EXPECT(hosted::sample_machines(db, minute) == 0); // once a minute
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM fleet_machine_samples WHERE machine_id = " +
							std::to_string(stale.id) + ";") == 0);

	// Two hours of a machine's minutes, written by hand: the counters grow, then
	// start over (its agent's interface reset), then grow again.
	const int64_t machine = 77; // no row: the history outlives it
	const int64_t h0 = 1760000400; // an hour's start
	struct Minute {
		int64_t at;
		double cpu;
		int64_t memory, disk, in, out;
	};
	const std::vector<Minute> minutes = { { h0 - 60, 5, 10, 900, 1000, 100 }, { h0, 10, 20, 800, 1500, 300 },
		{ h0 + 60, 30, 50, 700, 1800, 400 }, { h0 + 3600, 20, 40, 600, 200, 50 }, { h0 + 3660, 40, 30, 650, 500, 90 } };
	for (const Minute &m : minutes)
		db.exec("INSERT INTO fleet_machine_samples (machine_id, at, cpu_percent, memory_used_bytes, disk_free_bytes, "
				"net_in_bytes, net_out_bytes) VALUES ($1, to_timestamp($2::double precision), $3, $4, $5, $6, $7);",
				{ BindValue(machine), BindValue(m.at), BindValue(m.cpu), BindValue(m.memory), BindValue(m.disk),
						BindValue(m.in), BindValue(m.out) });
	// The roll-up at the second hour's minute 2 writes the first hour and the second.
	hosted::rollup_machine_hours(db, h0 + 3600 + 120);
	const auto hours = db.query("SELECT CAST(extract(epoch FROM hour) AS bigint), cpu_avg, cpu_max, memory_peak_bytes, "
								"disk_free_min_bytes, net_in_bytes, net_out_bytes FROM fleet_machine_hourly "
								"WHERE machine_id = $1 ORDER BY hour;",
			{ BindValue(machine) });
	TEST_EXPECT(hours.size() == 2);
	if (hours.size() == 2) {
		// The first: two minutes, each counted from the one before (the hour
		// before's last among them).
		TEST_EXPECT(hours[0].as_int(0) == h0 && near(double_of(hours[0], 1), 20.0) && near(double_of(hours[0], 2), 30.0));
		TEST_EXPECT(hours[0].as_int(3) == 50 && hours[0].as_int(4) == 700 && hours[0].as_int(5) == 800 &&
				hours[0].as_int(6) == 300);
		// The second: the counters started over (counted from 0), then grew.
		TEST_EXPECT(hours[1].as_int(0) == h0 + 3600 && near(double_of(hours[1], 1), 30.0) &&
				hours[1].as_int(3) == 40 && hours[1].as_int(4) == 600);
		TEST_EXPECT(hours[1].as_int(5) == 200 + 300 && hours[1].as_int(6) == 50 + 40);
	}
	// Again: the same hours rewritten, never doubled.
	hosted::rollup_machine_hours(db, h0 + 3600 + 600);
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM fleet_machine_hourly WHERE machine_id = 77;") == 2);

	// The prune: minutes past 48 hours, hours past 400 days.
	hosted::prune_stats(db, h0 + 3 * 86400);
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM fleet_machine_samples WHERE machine_id = 77;") == 0);
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM fleet_machine_hourly WHERE machine_id = 77;") == 2);
	hosted::prune_stats(db, h0 + 401 * 86400);
	TEST_EXPECT(count_rows(db, "SELECT COUNT(*) FROM fleet_machine_hourly WHERE machine_id = 77;") == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "sampler", test_sampler },
		{ "reads", test_reads },
		{ "machine_stats", test_machine_stats },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the hosted servers' stats\n");
	return 0;
}
