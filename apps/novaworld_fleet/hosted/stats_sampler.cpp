#include "stats_sampler.h"

#include "hosted/model/stats_repository.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <net/novaworld/host_repository.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

namespace opennova::novaworld_server::hosted {

bool StatsSampler::sample(db::Database &db, int64_t now, const LinkLookup &links) {
	const int64_t minute = now - now % 60;
	if (minute == last_minute_) return false;
	last_minute_ = minute;
	const int64_t hour = minute - minute % 3600;

	std::set<int64_t> seen;
	for (const HostedServer &s : list_live(db)) {
		seen.insert(s.id);
		ServerSample sample;
		sample.server_id = s.id;
		sample.at = minute;
		sample.state = state_name(s.state);

		// The listing its service account holds, and its roster's names.
		std::optional<hostdb::HostRow> listing;
		if (s.service_user_id) {
			if (const auto rid = listing_rid_of(db, *s.service_user_id)) listing = hostdb::find_host_by_rid(db, *rid);
		}
		std::set<std::string> &names = players_[{ s.id, hour }];
		if (listing) {
			sample.players = listing->player_count;
			sample.max_players = listing->max_players;
			sample.mission = retail_text_to_utf8(listing->mission_name);
			for (const HostRosterSlot &slot : hostdb::list_roster(db, listing->rid)) {
				if (!slot.player_name.empty()) names.insert(slot.player_name);
			}
		}

		// Its admin link's scene, and a mission started in the game scene.
		if (links) {
			if (const auto link = links(s.id)) {
				sample.scene = strutil::to_lower(link->scene);
				sample.link_up = link->up;
			}
		}
		// What the next sample counts from moves only once this one is stored (a
		// sample that failed is counted by the next one instead).
		std::optional<std::string> mission_mark;
		if (sample.scene == "game" && !sample.mission.empty()) {
			const auto last = last_mission_.find(s.id);
			sample.mission_started = last == last_mission_.end() || last->second != sample.mission;
			mission_mark = sample.mission;
		}

		// Its container's CPU, memory, ticks and its slot's network, as the agent
		// last reported them (only its own container's).
		std::optional<SlotMark> egress_mark;
		const auto report = slot_report_of(db, s.id);
		if (report && s.machine_ref && report->container == s.container) {
			sample.cpu_percent = std::clamp(report->cpu_percent / slot_cpus_, 0.0, 100.0);
			sample.memory_bytes = static_cast<int64_t>(report->memory_bytes);
			if (report->tick) {
				sample.tick_p50_ms = report->tick->p50_ms;
				sample.tick_p99_ms = report->tick->p99_ms;
				sample.tick_max_ms = report->tick->max_ms;
				sample.late_ticks = static_cast<int64_t>(report->tick->late);
			}
			// A count below the last one is a counter that started over.
			const auto delta = [](uint64_t now, uint64_t last) { return now >= last ? now - last : now; };
			SlotMark mark{ *s.machine_ref, s.slot, report->egress_bytes, report->net_in_bytes, 0, 0 };
			const auto known = egress_.find(s.id);
			if (known != egress_.end() && known->second.machine_id == *s.machine_ref &&
					known->second.slot == s.slot) {
				const uint64_t sent = delta(report->egress_bytes, known->second.egress);
				sample.egress_bytes = static_cast<int64_t>(sent);
				mark.out_total = known->second.out_total + sent;
				mark.in_total = known->second.in_total + delta(report->net_in_bytes, known->second.ingress);
			} else if (known != egress_.end()) {
				// Moved to another slot: its counters carry on from where they were.
				mark.out_total = known->second.out_total;
				mark.in_total = known->second.in_total;
			}
			sample.net_out_bytes = static_cast<int64_t>(mark.out_total);
			sample.net_in_bytes = static_cast<int64_t>(mark.in_total);
			egress_mark = mark;
		}

		insert_sample(db, sample);
		if (sample.egress_bytes > 0 && s.machine_ref) add_machine_egress(db, *s.machine_ref, minute, sample.egress_bytes);
		if (egress_mark) egress_[s.id] = *egress_mark;
		if (mission_mark) last_mission_[s.id] = *mission_mark;
		rollup_hour(db, s.id, minute, static_cast<int64_t>(names.size()));
	}

	// Every live machine's minute, from its agent's last report.
	sample_machines(db, minute);

	// What it keeps of the servers that ended, and of the hours gone.
	for (auto it = egress_.begin(); it != egress_.end();)
		it = seen.count(it->first) == 0 ? egress_.erase(it) : std::next(it);
	for (auto it = last_mission_.begin(); it != last_mission_.end();)
		it = seen.count(it->first) == 0 ? last_mission_.erase(it) : std::next(it);
	for (auto it = players_.begin(); it != players_.end();)
		it = it->first.second != hour || seen.count(it->first.first) == 0 ? players_.erase(it) : std::next(it);
	return true;
}

} // namespace opennova::novaworld_server::hosted
