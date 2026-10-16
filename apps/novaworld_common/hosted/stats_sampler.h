#pragma once

// The hosted servers' stats sampler: once a minute, on the supervisor's pass
// (Supervisor::tick, on its wall clock), one sample of every live server
// (stats_repository.h): its state; its listing's players, slots and mission
// (the lobby's row its service account holds); its admin link's scene and
// whether the link is up; its container's CPU (as a share of the slot's CPUs,
// 0..100) and memory as its machine's agent last reported them; and the bytes
// its slot sent since the last sample, the difference of the agent's per-slot
// counter (counted since the machine booted; a count below the last one is a
// counter that started over, counted from 0; a server's first sample on a slot
// counts none). Each sample also rewrites its server's hour (rollup_hour) and
// adds the machine's egress to its month; the old rows go with the hourly
// stats.prune job (stats_jobs.h), never on the pass. Unique players are
// counted from the listing's roster names, held in this process's memory for
// the hour and never stored (a service restart loses an hour's names: the
// count is an approximation); a mission is counted when a sample in the game
// scene sees a mission other than the last such sample's (one that starts and
// ends between two samples is missed). Service policy, not a port.

#include "fleet_repository.h"
#include "hosted_repository.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace opennova::novaworld_server::hosted {

class StatsSampler {
public:
	// A server's admin link as the sampler reads it: its scene ("game",
	// "menus", "unknown"; "" before a poll) and whether its session is up.
	struct LinkView {
		std::string scene;
		bool up = false;
	};
	// Server `server_id`'s admin link; nullopt with none.
	using LinkLookup = std::function<std::optional<LinkView>(int64_t server_id)>;

	// `slot_cpus`: each container's CPUs (ONNET_FLEET_SLOT_CPUS), the share
	// the reported CPU is a percent of.
	explicit StatsSampler(double slot_cpus = 1.0) : slot_cpus_(slot_cpus > 0 ? slot_cpus : 1.0) {}

	// One sample of every live server at `now` (Unix seconds), the first call in
	// each wall-clock minute alone: true when it sampled.
	bool sample(db::Database &db, int64_t now, const LinkLookup &links = {});

private:
	struct EgressMark {
		int64_t machine_id = 0;
		int slot = -1;
		uint64_t cumulative = 0;
	};

	double slot_cpus_;
	int64_t last_minute_ = -1;
	std::map<int64_t, EgressMark> egress_; // by server
	std::map<int64_t, std::string> last_mission_; // by server: its last game-scene sample's
	std::map<std::pair<int64_t, int64_t>, std::set<std::string>> players_; // (server, hour) -> names
};

} // namespace opennova::novaworld_server::hosted
