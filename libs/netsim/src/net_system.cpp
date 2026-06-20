#include "netsim/net_system.h"

#include <utility>
#include <vector>

namespace opennova::netsim {

void NetSystem::tick(world::World & /*world*/, const world::TickContext & /*ctx*/) {
	// Phase 1 seam: no C2S uplink exists yet, so the top-of-tick drain is a no-op.
	// Phase 2 drains queued PlayerIntent here and applies it to the owned pool-0
	// entity BEFORE WAC/BMS/AI run [orig: net-before-logic, Game_ProcessMainFrame
	// @ 0x5263f0].
	(void)channel_;
}

void NetSystem::emit_s2c(const world::World &w, const PlayerReplicationState &anchor) {
	std::vector<GameEntitySnapshot> ents = snapshot_world(w);
	std::vector<uint8_t> body = build_tag_0a_world_reference(anchor, ents);
	channel_.host_send(kTag0aFrameUpdate, std::move(body));
}

} // namespace opennova::netsim
