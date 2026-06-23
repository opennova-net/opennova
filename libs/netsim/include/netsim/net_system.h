#pragma once

#include <cstdint>

#include <novaworld/replication_min.h> // PlayerReplicationState, GameEntitySnapshot
#include <world/world.h>

#include "netsim/entity_wire_bridge.h"
#include "netsim/session_transport.h"

namespace opennova::netsim {

// S2C in-match message tags carried on the loopback (the inner-message tag, not the
// session opcode). Phase 1 emits only the per-frame world reference.
inline constexpr uint8_t kTag0aFrameUpdate = 0x0A;

// The in-match net seam as a World ISystem (ADR 0009 Decision 1 / ADR 0011),
// registered AHEAD of WAC so it sits where the original's net step does. Faithful
// frame order [orig: Game_ProcessMainFrame @ 0x5263f0]:
//
//   input -> NetSystem::tick (drain C2S) -> World::run_logic_tick (WAC/BMS/AI)
//         -> NetSystem::emit_s2c -> present
//
// tick() runs INSIDE the authoritative system loop (it only fires under is_authority,
// which the SP host always is). emit_s2c() is called by the host AFTER
// run_logic_tick — deliberately NOT an ISystem hook — so the outbound serialize
// happens post-logic, mirroring the original's net-before-logic / serialize-after
// order without tripping the run_logic_tick authority guard.
class NetSystem : public world::ISystem {
public:
	explicit NetSystem(ISessionTransport &channel) : channel_(channel) {}

	const char *name() const override { return "net"; }
	void tick(world::World &world, const world::TickContext &ctx) override;

	// Serialize the live world into one S2C 0x0A frame onto the loopback. `anchor`
	// is the subject (local player) identity/spawn the frame is built around — its
	// world position is the frame anchor each compact record compresses against.
	void emit_s2c(const world::World &w, const PlayerReplicationState &anchor);

private:
	ISessionTransport &channel_;
};

} // namespace opennova::netsim
