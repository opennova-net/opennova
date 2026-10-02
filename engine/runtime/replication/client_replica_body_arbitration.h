#pragma once

#include <runtime/replication/client_state.h>

#include <cstdint>

// Internal (src-local) seam: the D-NET-209 per-record receive-side body-state
// arbitration, split out of client_replica_pipeline.cpp's fold (W3-7 TU-size
// rule). Declared here for the fold; the channel half (transition insert +
// deferred promotion) stays inside row_root_motion_tick.

namespace opennova::replication {

// Per-record receive-side body-state arbitration: retail applies each decoded
// record's anim byte to the entity FSM pair (+0x2BC current / +0x2B8 pending)
// AS IT DECODES [orig: player @0x4c1153, infantry @0x4c0600..0x4c0641] —
// running it per folded record (not per present frame) closes the
// snapshot-coalescing divergence the anim_state_pulse seam papered over
// (docs/net/novaworld-net-re.md D-NET-209).
void apply_record_body_arbitration(ClientEntityState &es, uint8_t decoded,
		uint8_t ratio_byte, bool is_player, bool wire_dead, bool row_was_dead,
		bool respawned_this_record);

// The organic movers' death edge on a remote row: Health is zero and Flags bit
// 2 is not latched yet. Latches the bit (dropping the 0xC0 climb/mount bits),
// consumes the parked deathAnimStateId and returns the body state the edge
// commits; the caller stores it as the current state after this tick's body
// pass, so the channel takes it on the next tick as retail's top-of-pass
// AnimMap update does.
int16_t replica_death_edge(ClientEntityState &es);

} // namespace opennova::replication
