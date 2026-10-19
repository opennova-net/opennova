#include "fleet_health.h"

#include "address_repository.h"

#include <net/novaworld/lobby_repository.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

// A span of seconds in words: "45 s", "22 min", "5 h", "3 days".
std::string age_text(int64_t seconds) {
	if (seconds < 0)
		seconds = 0;
	if (seconds < 120)
		return std::to_string(seconds) + " s";
	if (seconds < 120 * 60)
		return std::to_string(seconds / 60) + " min";
	if (seconds < 48 * 3600)
		return std::to_string(seconds / 3600) + " h";
	return std::to_string(seconds / 86400) + " days";
}

std::string percent_text(double percent) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.0f %%", percent);
	return buf;
}

std::string gib_text(uint64_t bytes) {
	char buf[32];
	std::snprintf(
			buf, sizeof(buf), "%.1f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
	return buf;
}

// An image reference as a reader tells two apart: its digest's first 12 hex
// digits (sha256:0123456789ab), else its name and tag after the registry.
std::string short_image(const std::string &ref) {
	const size_t digest = ref.find("@sha256:");
	if (digest != std::string::npos)
		return ref.substr(digest + 1, 7 + 12);
	const size_t slash = ref.rfind('/');
	return slash == std::string::npos ? ref : ref.substr(slash + 1);
}

std::string capitalized(std::string text) {
	if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
		text[0] = static_cast<char>(text[0] - 'a' + 'A');
	return text;
}

std::string machine_subject(const FleetMachineRow &m) {
	return "Machine " + std::to_string(m.id) + " (" + machine_kind_text(m.kind) + ")";
}

std::string machine_link(int64_t id) {
	return "/admin/fleet/machines/" + std::to_string(id);
}

std::string server_subject(int64_t id, const std::string &name) {
	return "Server " + std::to_string(id) + (name.empty() ? "" : " (" + name + ")");
}

std::string id_list(const std::vector<int64_t> &ids) {
	std::string out;
	for (const int64_t id : ids)
		out += (out.empty() ? "" : ", ") + std::to_string(id);
	return out;
}

int64_t epoch_at(const db::Row &r, size_t i) {
	return r.as_int(i).value_or(0);
}

// How long a machine in `state` may stay there, and what it waits on; 0 for a
// state with no bound.
struct StuckBound {
	int64_t seconds = 0;
	const char *waiting = "";
};

StuckBound stuck_bound(FleetMachineState state) {
	switch (state) {
		case FleetMachineState::Launching:
			return { kStuckBootS, "the cloud has not answered its launch" };
		case FleetMachineState::Booting:
			return { kStuckBootS, "its agent has not reported" };
		case FleetMachineState::Draining:
			return { kStuckDrainS, "its servers have not moved off it" };
		case FleetMachineState::Ending:
			return { kStuckEndS, "the cloud has not ended it" };
		case FleetMachineState::Lost:
			return { kStuckLostS, "it neither reports nor ends" };
		default:
			return {};
	}
}

int level_rank(HealthLevel level) {
	switch (level) {
		case HealthLevel::Danger:
			return 0;
		case HealthLevel::Warn:
			return 1;
		case HealthLevel::Info:
			return 2;
	}
	return 2;
}

} // namespace

const char *health_level_text(HealthLevel level) {
	switch (level) {
		case HealthLevel::Info:
			return "info";
		case HealthLevel::Warn:
			return "warn";
		case HealthLevel::Danger:
			return "danger";
	}
	return "warn";
}

std::optional<bool> machine_outdated(const FleetMachineRow &m, const ReleaseImages &release) {
	std::optional<bool> out;
	const auto compare = [&out](const std::string &own, const std::string &pinned) {
		if (!own.empty() && !pinned.empty())
			out = out.value_or(false) || own != pinned;
	};
	compare(m.image_id, release.image_id);
	compare(m.agent_image, release.agent_image);
	return out;
}

HealthInputs read_health(db::Database &db, const PlacementPolicy *policy) {
	db::ReadSnapshot snapshot(db);
	HealthInputs in;
	in.now = db.query("SELECT CAST(extract(epoch FROM now()) AS bigint);")
					 .front()
					 .as_int(0)
					 .value_or(0);

	// The live machines, with their times: the move into their state and the
	// moves of the last hour (fleet_machine_events; a note's to_state is its
	// from_state, the row's making's from_state null), and whether one carries
	// a static address its public address does not show (awaits_address).
	std::map<int64_t, HealthMachine> machines;
	for (FleetMachineRow &m : list_live_machines(db)) {
		const int64_t id = m.id;
		machines[id].row = std::move(m);
	}
	for (const db::Row &r : db.query(
				 "SELECT m.id, CAST(extract(epoch FROM m.last_report_at) AS bigint), "
				 "CAST(extract(epoch FROM m.stats_at) AS bigint), "
				 "CAST(extract(epoch FROM m.protocol_mismatch_since) AS bigint), "
				 "CAST(extract(epoch FROM (SELECT max(e.at) FROM fleet_machine_events e WHERE "
				 "e.machine_id = m.id AND e.to_state = m.state AND e.from_state IS DISTINCT FROM "
				 "e.to_state)) AS bigint), "
				 "(SELECT count(*) FROM fleet_machine_events e WHERE e.machine_id = m.id AND "
				 "e.from_state IS NOT NULL AND e.from_state <> e.to_state AND "
				 "e.at > now() - interval '1 hour'), "
				 "a.public_ip "
				 "FROM fleet_machines m LEFT JOIN hosted_addresses a ON a.id = m.address_id "
				 "WHERE m.state NOT IN ('ended', 'failed');")) {
		const auto it = machines.find(r.as_int(0).value_or(0));
		if (it == machines.end())
			continue;
		HealthMachine &h = it->second;
		h.last_report = epoch_at(r, 1);
		h.stats_at = epoch_at(r, 2);
		h.mismatch_since = epoch_at(r, 3);
		h.state_since = epoch_at(r, 4);
		h.moves_last_hour = static_cast<int>(r.as_int(5).value_or(0));
		h.address_wait = awaits_address(h.row, r.as_text(6).value_or(""));
	}

	// The live servers, each with its move into its state and its container's
	// ticks as its machine's agent last reported them.
	std::map<int64_t, int64_t> server_since;
	for (const db::Row &r : db.query(
				 "SELECT s.id, CAST(extract(epoch FROM max(e.at)) AS bigint) FROM hosted_servers s "
				 "JOIN hosted_server_events e ON e.server_id = s.id AND e.to_state = s.state AND "
				 "e.from_state <> e.to_state WHERE s.state NOT IN ('stopped', 'failed') "
				 "GROUP BY s.id;"))
		server_since[r.as_int(0).value_or(0)] = epoch_at(r, 1);
	std::map<std::pair<int64_t, std::string>, std::optional<TickHealth>> ticks;
	for (const auto &[id, m] : machines)
		for (const SlotReport &report : slot_reports(db, id))
			ticks[{ id, report.container }] = report.tick;
	for (const HostedServer &s : list_live(db)) {
		HealthServer h;
		h.id = s.id;
		h.name = s.settings.game_name;
		h.state = s.state;
		h.state_since = server_since.count(s.id) != 0 ? server_since[s.id] : 0;
		h.machine_id = s.machine_ref.value_or(0);
		if (const auto it = ticks.find({ h.machine_id, s.container }); it != ticks.end())
			h.tick = it->second;
		in.servers.push_back(std::move(h));
	}
	std::sort(in.servers.begin(), in.servers.end(),
			[](const HealthServer &a, const HealthServer &b) { return a.id < b.id; });
	for (auto &[id, m] : machines)
		in.machines.push_back(std::move(m));

	for (const db::Row &r :
			db.query("SELECT id, game_name, failure_reason, "
					 "CAST(extract(epoch FROM ended_at) AS bigint) FROM hosted_servers "
					 "WHERE state = 'failed' AND ended_at > now() - make_interval(secs => $1) "
					 "ORDER BY ended_at DESC, id DESC LIMIT 20;",
					{ BindValue(kFailedWindowS) }))
		in.failed.push_back({ r.as_int(0).value_or(0), r.as_text(1).value_or(""),
				r.as_text(2).value_or(""), epoch_at(r, 3) });

	// Each live server's newest starts that ended for want of their data, back to
	// its last start that ran or ended otherwise.
	std::map<int64_t, DataFailures> data_failures;
	for (const db::Row &r : db.query(
				 "SELECT st.server_id, count(*), CAST(extract(epoch FROM min(st.ended_at)) AS "
				 "bigint), (array_agg(st.reason ORDER BY st.generation DESC))[1], "
				 "(array_agg(st.base_tree_id ORDER BY st.generation DESC))[1], "
				 "(array_agg(st.expansion_tree_id ORDER BY st.generation DESC))[1], "
				 "COALESCE(d.display_name, '') "
				 "FROM hosted_server_starts st JOIN hosted_servers s ON s.id = st.server_id "
				 "LEFT JOIN data_sets d ON d.id = s.data_set_id "
				 "WHERE s.state NOT IN ('stopped', 'failed') AND st.ended_at IS NOT NULL AND "
				 "st.cause = 'data_unavailable' AND st.generation > COALESCE((SELECT "
				 "max(b.generation) FROM hosted_server_starts b WHERE b.server_id = st.server_id "
				 "AND (b.running_at IS NOT NULL OR (b.ended_at IS NOT NULL AND b.cause IS "
				 "DISTINCT FROM 'data_unavailable'))), 0) "
				 "GROUP BY st.server_id, d.display_name;")) {
		DataFailures d;
		d.count = static_cast<int>(r.as_int(1).value_or(0));
		d.since = epoch_at(r, 2);
		d.reason = r.as_text(3).value_or("");
		d.base_tree = r.as_int(4).value_or(0);
		d.expansion_tree = r.as_int(5);
		d.data_set = r.as_text(6).value_or("");
		data_failures[r.as_int(0).value_or(0)] = std::move(d);
	}
	for (HealthServer &s : in.servers)
		if (const auto it = data_failures.find(s.id); it != data_failures.end())
			s.data_failures = std::move(it->second);

	// Each kind's backoff while it holds (on the supervisor's clock, Unix
	// milliseconds), and the kind's newest failed machine: why it failed.
	for (const MachineKind kind : { MachineKind::Shared, MachineKind::Own }) {
		HeldLaunches held;
		held.kind = kind;
		held.backoff = launch_backoff(db, kind);
		if (held.backoff.not_before_ms <= in.now * 1000)
			continue;
		const auto rows =
				db.query("SELECT id, end_reason, CAST(extract(epoch FROM COALESCE(ended_at, "
						 "launched_at)) AS bigint) FROM fleet_machines WHERE kind = $1 AND "
						 "state = 'failed' ORDER BY id DESC LIMIT 1;",
						{ BindValue(std::string(machine_kind_text(kind))) });
		if (!rows.empty()) {
			held.machine_id = rows.front().as_int(0).value_or(0);
			held.cause = rows.front().as_text(1).value_or("");
			held.failed_at = epoch_at(rows.front(), 2);
		}
		in.held.push_back(std::move(held));
	}

	in.lobbies_live = lobbydb::live_lobbies(db).instances;
	for (const lobbydb::LobbyInstance &l : lobbydb::list_instances(db))
		in.lobby_last_seen = std::max(in.lobby_last_seen, l.last_seen_ms / 1000);

	for (const db::Row &r :
			db.query("SELECT id, name, reason, CAST(extract(epoch FROM updated_at) AS bigint), "
					 "retry_at IS NOT NULL FROM hosted_addresses WHERE state = 'failed' ORDER BY id;"))
		in.failed_addresses.push_back({ r.as_int(0).value_or(0), r.as_text(1).value_or(""),
				r.as_text(2).value_or(""), epoch_at(r, 3), r.as_int(4).value_or(0) != 0 });
	for (const db::Row &r :
			db.query("SELECT id, name, CAST(extract(epoch FROM updated_at) AS bigint) "
					 "FROM hosted_addresses WHERE state = 'releasing' ORDER BY id;"))
		in.releasing_addresses.push_back(
				{ r.as_int(0).value_or(0), r.as_text(1).value_or(""), epoch_at(r, 2) });
	in.held_addresses = count_held_addresses(db);
	in.addresses_on = policy != nullptr && policy->max_addresses > 0;
	for (const db::Row &r :
			db.query("SELECT allocation_id, public_ip, CAST(extract(epoch FROM seen_at) AS "
					 "bigint) FROM hosted_address_orphans ORDER BY seen_at, allocation_id;"))
		in.orphans.push_back(
				{ r.as_text(0).value_or(""), r.as_text(1).value_or(""), epoch_at(r, 2) });

	if (policy != nullptr) {
		in.sold = sold_now(db, in.now);
		in.sellable = sellable_of(*policy);
	}
	return in;
}

std::vector<Finding> fleet_health(const HealthInputs &in, const ReleaseImages &release) {
	std::vector<Finding> out;
	const auto add = [&out](HealthLevel level, std::string subject, std::string text, int64_t since,
							 std::string link, const char *source) {
		out.push_back(
				{ level, std::move(subject), std::move(text), since, std::move(link), source });
	};
	std::map<int64_t, const HealthMachine *> by_id;
	for (const HealthMachine &m : in.machines)
		by_id[m.row.id] = &m;

	// The servers' starts.
	for (const HealthServer &s : in.servers) {
		const bool starting = s.state == State::Requested || s.state == State::Provisioning ||
				s.state == State::Booting || s.state == State::Listing;
		if (!starting || s.state_since <= 0 || in.now - s.state_since <= kStalledStartS)
			continue;
		std::string text =
				capitalized(state_name(s.state)) + " for " + age_text(in.now - s.state_since);
		const auto machine = by_id.find(s.machine_id);
		if (machine != by_id.end() && machine->second->address_wait)
			text += ", waiting for its static address to reach its machine " +
					std::to_string(s.machine_id) + ".";
		else
			text += ": a start takes under " + age_text(kStalledStartS) + ".";
		add(HealthLevel::Warn, server_subject(s.id, s.name), std::move(text), s.state_since,
				"/admin/fleet/" + std::to_string(s.id), "stalled_start");
	}

	// The machines.
	std::vector<int64_t> old_image;
	for (const HealthMachine &h : in.machines) {
		const FleetMachineRow &m = h.row;
		const std::string subject = machine_subject(m);
		const std::string link = machine_link(m.id);
		const int64_t held_since = m.lost_held_since_ms / 1000;
		if (m.state == FleetMachineState::Lost && m.lost_held_since_ms > 0) {
			add(HealthLevel::Danger, subject,
					"Lost, and held from its end by the mass-lost brake: it ends when it reports "
					"again or an admin ends it (fleet end).",
					held_since, link, "lost_held");
		} else if (const StuckBound bound = stuck_bound(m.state);
				bound.seconds > 0 && h.state_since > 0 && in.now - h.state_since > bound.seconds) {
			add(HealthLevel::Warn, subject,
					capitalized(fleet_machine_state_text(m.state)) + " for " +
							age_text(in.now - h.state_since) + " (over " + age_text(bound.seconds) +
							"): " + bound.waiting + ".",
					h.state_since, link, "stuck_machine");
		}
		if (h.moves_last_hour > kMovesPerHour)
			add(HealthLevel::Warn, subject,
					"Moved state " + std::to_string(h.moves_last_hour) + " times in the last hour.",
					0, link, "flapping_machine");
		if (h.mismatch_since > 0)
			add(HealthLevel::Danger, subject,
					"Its agent speaks protocol " + std::to_string(m.agent_protocol) +
							", the service " + std::to_string(kAgentProtocol) +
							": held and acted on in nothing, it ends 10 min after it began "
							"(roll hostd and the fleet program to one release).",
					h.mismatch_since, link, "protocol_mismatch");
		if (!m.agent_image.empty() && !release.agent_image.empty() &&
				m.agent_image != release.agent_image)
			add(HealthLevel::Warn, subject,
					"Its hostd runs " + short_image(m.agent_image) + ", not the release's " +
							short_image(release.agent_image) +
							": app deploy rolls hostd (fleet status).",
					0, link, "outdated_agent");
		if (!m.image_id.empty() && !release.image_id.empty() && m.image_id != release.image_id)
			old_image.push_back(m.id);

		// Its reports: silent and not marked lost, or its use too old to judge;
		// else its use against the bounds.
		const bool reporting =
				m.state == FleetMachineState::Ready || m.state == FleetMachineState::Draining;
		if (!reporting)
			continue;
		if (h.last_report > 0 && in.now - h.last_report > kStaleReportS) {
			add(HealthLevel::Warn, subject,
					"Its last report is " + age_text(in.now - h.last_report) +
							" old and it is not marked lost: its state is unknown (is the "
							"fleet program passing?).",
					h.last_report, link, "unknown");
			continue;
		}
		const bool settled = h.state_since > 0 && in.now - h.state_since > kStaleStatsS;
		if (!m.stats || h.stats_at <= 0 || in.now - h.stats_at > kStaleStatsS) {
			if (settled)
				add(HealthLevel::Warn, subject,
						h.stats_at > 0
								? "Its use is unknown: its agent last reported it " +
										age_text(in.now - h.stats_at) + " ago."
								: std::string("Its use is unknown: its agent has not reported it."),
						h.stats_at, link, "unknown");
			continue;
		}
		const MachineStats &s = *m.stats;
		std::vector<std::string> high;
		if (s.cpu_percent > kCpuBusyPercent)
			high.push_back("CPU " + percent_text(s.cpu_percent));
		if (s.memory_total_bytes > 0) {
			const double used = 100.0 * static_cast<double>(s.memory_used_bytes) /
					static_cast<double>(s.memory_total_bytes);
			if (used > kMemoryUsedPercent)
				high.push_back(
						"memory " + percent_text(used) + " of " + gib_text(s.memory_total_bytes));
		}
		if (s.disk_total_bytes > 0) {
			const double free = 100.0 * static_cast<double>(s.disk_free_bytes) /
					static_cast<double>(s.disk_total_bytes);
			if (free < kDiskFreePercent)
				high.push_back(
						"disk " + percent_text(free) + " free of " + gib_text(s.disk_total_bytes));
		}
		if (!high.empty()) {
			std::string text;
			for (const std::string &part : high)
				text += (text.empty() ? "" : ", ") + part;
			add(HealthLevel::Warn, subject, capitalized(text) + ".", 0, link, "machine_pressure");
		}
	}
	if (!old_image.empty())
		add(HealthLevel::Info, "The fleet",
				std::string(old_image.size() == 1 ? "Machine " : "Machines ") + id_list(old_image) +
						(old_image.size() == 1 ? " runs" : " run") +
						" another image than the launch templates' " + release.image_id +
						": a fleet roll replaces " + (old_image.size() == 1 ? "it." : "them."),
				0, "/admin/fleet?tab=roll", "outdated_image");

	// Each machine's servers' ticks: late ones, and running ones with none.
	std::map<int64_t, std::vector<std::pair<int64_t, uint64_t>>> late;
	std::map<int64_t, std::vector<int64_t>> no_ticks;
	for (const HealthServer &s : in.servers) {
		if (s.state != State::Running || s.machine_id <= 0)
			continue;
		if (s.tick) {
			if (s.tick->late > static_cast<uint64_t>(kLateTicksPerMinute))
				late[s.machine_id].push_back({ s.id, s.tick->late });
		} else if (s.state_since > 0 && in.now - s.state_since > kStaleStatsS) {
			no_ticks[s.machine_id].push_back(s.id);
		}
	}
	const auto machine_named = [&by_id](int64_t id) {
		const auto it = by_id.find(id);
		return it != by_id.end() ? machine_subject(it->second->row)
								 : "Machine " + std::to_string(id);
	};
	for (const auto &[machine, servers] : late) {
		std::string list;
		for (const auto &[id, n] : servers)
			list += (list.empty() ? "" : ", ") + std::to_string(id) + " (" + std::to_string(n) +
					" late)";
		add(HealthLevel::Warn, machine_named(machine),
				std::string(servers.size() == 1 ? "Server " : "Servers ") + list +
						" ran over 1 % of the last minute's ticks late: the machine is "
						"short of CPU.",
				0, machine_link(machine), "late_ticks");
	}
	for (const auto &[machine, servers] : no_ticks)
		add(HealthLevel::Warn, machine_named(machine),
				"The tick health of " + std::string(servers.size() == 1 ? "server " : "servers ") +
						id_list(servers) +
						" is unknown: no fresh stats file in its agent's report.",
				0, machine_link(machine), "unknown");

	// The launches, the lobby, the failures, the addresses, the capacity.
	for (const HeldLaunches &h : in.held) {
		std::string text = "Held for another " + age_text(h.backoff.not_before_ms / 1000 - in.now) +
				" after " + std::to_string(h.backoff.failures) + " failed launch" +
				(h.backoff.failures == 1 ? "" : "es") + " in a row";
		text += h.machine_id > 0 ? "; the last, machine " + std::to_string(h.machine_id) + ": " +
						(h.cause.empty() ? std::string("no reason recorded") : h.cause) + "."
								 : std::string(".");
		add(HealthLevel::Warn, capitalized(machine_kind_text(h.kind)) + " launches",
				std::move(text), h.failed_at, "/admin/fleet", "launches_held");
	}
	if (in.lobbies_live == 0)
		add(HealthLevel::Danger, "The lobby",
				"No lobby instance takes clients: no player can log in and no hosted server starts "
				"(app status, app logs lobby).",
				in.lobby_last_seen, "/admin/infrastructure/programs/lobby", "no_lobby");
	for (const FailedServer &f : in.failed)
		add(HealthLevel::Warn, server_subject(f.id, f.name),
				"Failed: " + (f.reason.empty() ? std::string("no reason recorded") : f.reason) +
						".",
				f.ended_at, "/admin/fleet/" + std::to_string(f.id) + "/logs", "failed_server");
	// Servers whose data their machines cannot fetch, start after start: no
	// restart is counted for it, so nothing else ever ends them.
	for (const HealthServer &s : in.servers) {
		if (!s.data_failures)
			continue;
		const DataFailures &d = *s.data_failures;
		const int64_t failing = d.since > 0 ? in.now - d.since : 0;
		if (d.count < kDataFailuresInARow && failing <= kDataFailingS)
			continue;
		std::string trees = d.base_tree > 0 ? "tree " + std::to_string(d.base_tree)
											: std::string("no tree recorded");
		if (d.base_tree > 0 && d.expansion_tree)
			trees = "trees " + std::to_string(d.base_tree) + " and " +
					std::to_string(*d.expansion_tree);
		// hostd's reason past its own opening ("its data could not be fetched: ").
		std::string_view reason = d.reason;
		if (is_data_unavailable_reason(reason)) {
			reason.remove_prefix(kDataUnavailableReason.size());
			if (reason.substr(0, 2) == ": ")
				reason.remove_prefix(2);
		}
		std::string text = "Its data could not be fetched at its last " +
				(d.count == 1 ? std::string("start, ")
							  : std::to_string(d.count) + " starts, the first ") +
				age_text(failing) + " ago (" +
				(d.data_set.empty() ? std::string("its data set is gone") : d.data_set) + ", " +
				trees + ")";
		text += reason.empty() ? std::string(".")
							   : "; hostd's last reason: " + std::string(reason) + ".";
		add(failing > kDataFailingDangerS ? HealthLevel::Danger : HealthLevel::Warn,
				server_subject(s.id, s.name), std::move(text), d.since,
				"/admin/fleet/" + std::to_string(s.id), "data_unavailable");
	}
	for (const FailedAddress &a : in.failed_addresses)
		add(HealthLevel::Warn, "Address " + a.name,
				"The cloud refused it: " +
						(a.reason.empty() ? std::string("no reason recorded") : a.reason) +
						(a.retried ? ". It is tried again automatically; Retry tries it now, or release it."
								   : ". Retry it once the cause is fixed, or release it."),
				a.failed_at, "/admin/addresses", "failed_address");
	for (const ReleasingAddress &a : in.releasing_addresses) {
		if (a.last_step <= 0 || in.now - a.last_step <= kStuckReleaseS)
			continue;
		add(HealthLevel::Warn, "Address " + a.name,
				"Releasing, its release's last step " + age_text(in.now - a.last_step) +
						" ago (over " + age_text(kStuckReleaseS) +
						"): no worker runs its address.release (app logs worker).",
				a.last_step, "/admin/addresses", "stuck_address");
	}
	if (!in.addresses_on && in.held_addresses > 0)
		add(HealthLevel::Warn, "Static addresses",
				std::to_string(in.held_addresses) +
						(in.held_addresses == 1 ? " is" : " are") +
						" held while the add-on is off (ONNET_FLEET_MAX_ADDRESSES 0, or "
						"ONNET_HOSTED_ADDRESS_ZONE no zone name): only a worker whose address "
						"settings still read releases them. Release each before the add-on goes.",
				0, "/admin/addresses", "addresses_off");
	for (const OrphanAddress &o : in.orphans)
		add(HealthLevel::Warn, "Address " + o.public_ip,
				"The cloud holds " + o.allocation_id +
						" for this deployment and no address names it: release it by hand "
						"(the sweep never does).",
				o.seen_at, "/admin/addresses", "orphan_address");
	if (in.sold && sold_exceeds_capacity(*in.sold, in.sellable)) {
		std::string parts;
		const auto part = [&parts](const char *what, int64_t sold, int64_t sellable) {
			if (sold > sellable)
				parts += (parts.empty() ? "" : ", ") + std::string(what) + " " +
						std::to_string(sold) + " of " + std::to_string(sellable);
		};
		part("shared places", in.sold->shared, in.sellable.shared);
		part("dedicated machines", in.sold->own, in.sellable.own);
		part("static addresses", in.sold->addresses, in.sellable.addresses);
		add(HealthLevel::Warn, "Hosting capacity",
				"Sold past what the fleet holds: " + parts +
						". No new subscription of it sells until it is back under.",
				0, "/admin/billing", "oversold");
	}

	sort_findings(out);
	return out;
}

void sort_findings(std::vector<Finding> &findings) {
	std::stable_sort(findings.begin(), findings.end(), [](const Finding &a, const Finding &b) {
		if (level_rank(a.level) != level_rank(b.level))
			return level_rank(a.level) < level_rank(b.level);
		if ((a.since > 0) != (b.since > 0))
			return a.since > 0;
		return a.since < b.since;
	});
}

HealthGauges health_gauges(const HealthInputs &in, const std::vector<Finding> &findings) {
	HealthGauges g;
	std::set<std::string> stuck;
	for (const Finding &f : findings) {
		g.danger += f.level == HealthLevel::Danger ? 1 : 0;
		g.warn += f.level == HealthLevel::Warn ? 1 : 0;
		if (f.source == "stuck_machine" || f.source == "flapping_machine")
			stuck.insert(f.link);
	}
	g.machines_stuck = static_cast<int>(stuck.size());
	for (const HealthMachine &h : in.machines) {
		const FleetMachineRow &m = h.row;
		if ((m.state != FleetMachineState::Ready && m.state != FleetMachineState::Draining) ||
				!m.stats || m.stats->disk_total_bytes == 0)
			continue;
		// Use too old to judge is the unknown rule's, as in fleet_health: an old
		// figure would read healthy while the disk fills.
		if ((h.last_report > 0 && in.now - h.last_report > kStaleReportS) || h.stats_at <= 0 ||
				in.now - h.stats_at > kStaleStatsS)
			continue;
		const int percent =
				static_cast<int>(m.stats->disk_free_bytes * 100 / m.stats->disk_total_bytes);
		g.disk_free_min_percent =
				g.disk_free_min_percent ? std::min(*g.disk_free_min_percent, percent) : percent;
	}
	return g;
}

} // namespace opennova::novaworld_server::hosted
