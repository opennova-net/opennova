#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include <npwire/replay_timeline.h>
#include <npwire/wire_capture.h>

namespace opennova::netsim {

class ClientReplicaPipeline;

// Optional, incremental time-series projection over the canonical client
// replica fold. Live play reads ClientReplicaPipeline::state() directly;
// replay/spectate attaches this history and therefore does not own a second
// entity decoder. Messages must be supplied in decode order.
class ReplicaHistory {
public:
	void reset();

	// Fold one decoded message. Server messages first update `pipeline`; observer
	// consequences such as the remote-Person reload dip are applied there before
	// the resulting replica edges are journaled. Client uplinks and journal-only
	// events contribute history without mutating the client replica state.
	// Returns true when the public history projection changed. A current-replica
	// destroy also counts because topology_revision advances even though the
	// historical entity track is intentionally retained.
	bool apply(const InGameMessage &message, ClientReplicaPipeline &pipeline);

	const ReplayTimeline &timeline() const noexcept { return timeline_; }
	// Monotonic invalidation edges for history consumers. topology_revision is
	// narrower: it changes only when the current replica/history row topology
	// changes, including a current-row destroy retained in the timeline.
	std::uint64_t revision() const noexcept { return revision_; }
	std::uint64_t topology_revision() const noexcept {
		return topology_revision_;
	}

private:
	ReplayEntity &entity(uint16_t handle, uint16_t type_id);
	bool capture_replica_edges(int frame_index, ClientReplicaPipeline &pipeline);
	bool capture_environment(int frame_index, const ClientReplicaPipeline &pipeline);
	void mark_killed(uint16_t handle);
	void retire_current_generation(uint16_t handle);

	ReplayTimeline timeline_;
	std::unordered_map<uint16_t, std::size_t> entity_index_;
	std::unordered_map<uint16_t, uint32_t> compact_revisions_;
	std::unordered_map<uint16_t, uint32_t> respawn_revisions_;
	std::unordered_set<uint16_t> spawn_recorded_;
	std::unordered_set<uint16_t> killed_;
	std::unordered_map<uint16_t, uint16_t> zone_states_;
	uint32_t environment_revision_ = 0;
	bool any_frame_ = false;
	std::uint64_t revision_ = 0;
	std::uint64_t topology_revision_ = 0;
};

} // namespace opennova::netsim
